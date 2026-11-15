"""可恢复的 SQLite 事件索引与 safetensors 数值存储。"""
from __future__ import annotations

import hashlib
import json
import os
import queue
import time
from pathlib import Path
import re
import shutil
import sqlite3
import threading
import uuid

from .protocol import ProtocolError


def safe_path(root, relative):
    """约束记录内路径，拒绝绝对路径、符号链接逃逸和路径穿越。"""
    root = Path(root).resolve()
    target = (root / relative).resolve()
    if Path(relative).is_absolute() or not target.is_relative_to(root):
        raise ProtocolError("INVALID_TRACE_PATH", "记录中的文件路径越界")
    return target


class TraceStore:
    """每次运行一个目录；先原子提交数值块，再发布可用引用。"""

    def __init__(self, root, readonly=False, budget=0, project_budget=None):
        self.root = Path(root).resolve()
        self.readonly = readonly
        self.budget = 0  # 旧调用传入的正数额度也按无限采集处理。
        self.lock = threading.RLock()
        self.bytes_written = 0
        self.incomplete = False
        self.project_budget = project_budget
        self.metadata_bytes = 0
        self.degraded_reason = ""
        self.pending_bytes = 0
        self.queue_limit = 64 * 1024**2
        self.jobs = queue.Queue(maxsize=4096)
        self.admission_lock = threading.RLock()
        self.write_error = None
        self.error_reported = False
        self.closed = False
        self.commit_count = 0
        if not readonly:
            self.root.mkdir(parents=True, exist_ok=True)
            (self.root / "tensors").mkdir(exist_ok=True)
        uri = (self.root / "index.sqlite").as_uri() + ("?mode=ro" if readonly else "?mode=rwc")
        self.db = sqlite3.connect(uri, uri=True, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        if not readonly:
            self.db.executescript("""
                PRAGMA journal_mode=WAL;
                PRAGMA synchronous=FULL;
                PRAGMA wal_autocheckpoint=128;
                CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY, data TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS tensors(ref TEXT PRIMARY KEY, path TEXT NOT NULL,
                    shape TEXT NOT NULL, dtype TEXT NOT NULL, nbytes INTEGER, sha256 TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS operations(branch TEXT, forward_id TEXT, layer INTEGER,
                    operation TEXT, context TEXT NOT NULL, refs TEXT NOT NULL,
                    PRIMARY KEY(branch,forward_id,layer,operation));
            """)
            self.db.commit()
        elif not (self.root / "manifest.json").is_file():
            raise ProtocolError("INVALID_TRACE", "记录缺少清单")
        self.manifest = self.read_manifest() if (self.root / "manifest.json").exists() else {}
        if readonly and self.manifest.get("status") == "running":
            # 崩溃后的临时块从未进入索引；已经提交的快照仍可安全读取。
            self.manifest["recovery_status"] = "incomplete"
        self.writer = None
        if not readonly:
            self.writer = threading.Thread(target=self._write_loop, name="观测批量写入", daemon=True)
            self.writer.start()

    def _enqueue(self, job, size=0, critical=False):
        """有界缓存后台写入；拥堵时等待已接纳数据落盘，不因额度丢弃采集。"""
        if job[0] == "sql":
            size = sum(len(str(value).encode("utf-8")) for value in job[2])
        # 队列仅限制同时驻留的内存；写入拥堵时先落盘，不能因队列满丢失采集。
        while True:
            with self.admission_lock:
                if self.write_error:
                    self.incomplete = True
                    return False
                # 大于队列预算的单块独占队列，不设置单块采集大小上限。
                if not self.jobs.full() and (self.pending_bytes == 0 or self.pending_bytes + size <= self.queue_limit):
                    self.pending_bytes += size
                    self.jobs.put_nowait((job, size))
                    return True
            self.flush()

    def _write_loop(self):
        """专用线程写数值块并批量提交索引，引用仅在文件原子落盘后发布到数据库。"""
        from safetensors.torch import save
        while True:
            first = self.jobs.get()
            batch = [first]
            if first[0][0] != "stop":
                # 短暂合批替代每个运算的独立数据库提交。
                deadline = time.monotonic() + .01
                while len(batch) < 32 and time.monotonic() < deadline:
                    try:
                        item = self.jobs.get(timeout=max(.0001, deadline - time.monotonic()))
                        batch.append(item)
                        if item[0][0] in ("barrier", "stop"):
                            break
                    except queue.Empty:
                        break
            stopping = False
            try:
                with self.lock:
                    for job, _ in batch:
                        kind, *args = job
                        if kind == "stop":
                            stopping = True
                        elif kind == "tensor" and not self.write_error:
                            ref, snapshot = args
                            relative = f"tensors/{ref}.safetensors"
                            target = self.root / relative
                            temporary = target.with_suffix(".tmp")
                            data = save({"value": snapshot})
                            temporary.write_bytes(data)
                            os.replace(temporary, target)
                            self.db.execute("INSERT INTO tensors VALUES(?,?,?,?,?,?)", (ref, relative,
                                json.dumps(list(snapshot.shape)), str(snapshot.dtype).removeprefix("torch."),
                                snapshot.numel() * snapshot.element_size(), hashlib.sha256(data).hexdigest()))
                        elif kind == "sql" and not self.write_error:
                            self.db.execute(*args)
                    self.db.commit()
                    self.commit_count += 1
            except Exception as exc:
                self.db.rollback()
                self.write_error = exc
                self.incomplete = True
                self.degraded_reason = "观测数据写入失败：" + str(exc)
            finally:
                for job, size in batch:
                    with self.admission_lock:
                        self.pending_bytes -= size
                    if job[0] == "barrier":
                        job[1].set()
                    self.jobs.task_done()
            if stopping:
                return

    def flush(self, cancel=None):
        """屏障只等待调用前已接纳的数据，查询不会等待后来不断产生的新任务。"""
        if self.readonly or self.closed:
            return
        ready = threading.Event()
        # 分析的取消只退出等待，已接纳的采集任务仍由写入线程完整落盘。
        deadline = time.monotonic() + 60
        while True:
            if cancel:
                cancel()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError("TRACE_WRITE_TIMEOUT", "等待观测数据写入超时")
            try:
                self.jobs.put((("barrier", ready), 0), timeout=min(.1, remaining))
                break
            except queue.Full:
                continue
        deadline = time.monotonic() + 60
        while not ready.wait(.1):
            if cancel:
                cancel()
            if time.monotonic() >= deadline:
                raise ProtocolError("TRACE_WRITE_TIMEOUT", "等待观测数据写入超时")
        if cancel:
            cancel()
        if self.write_error and not self.error_reported:
            self.error_reported = True
            raise ProtocolError("TRACE_WRITE_FAILED", self.degraded_reason)

    def read_manifest(self):
        """读取有大小上限的记录清单。"""
        path = self.root / "manifest.json"
        if path.stat().st_size > 8 * 1024**2:
            raise ProtocolError("INVALID_TRACE", "记录清单过大")
        return json.loads(path.read_text(encoding="utf-8"))

    def save_manifest(self, **changes):
        """原子替换清单，运行中保留可恢复状态。"""
        self.flush()
        with self.lock:
            if self.readonly:
                return
            self.manifest.update(changes)
            temporary = self.root / "manifest.tmp"
            temporary.write_text(json.dumps(self.manifest, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
            os.replace(temporary, self.root / "manifest.json")

    def event(self, message):
        """事件后台合批写入，结束事件返回前完成持久化屏障。"""
        if self.readonly:
            return
        data = json.dumps(message, ensure_ascii=False, allow_nan=False)
        # 磁盘空间不足时仍尝试提交结束事件，保留恢复状态。
        if not self.allow_metadata(len(data.encode("utf-8")) + 256) and message["name"] != "run.finished":
            return
        if message["name"] == "run.finished":
            self.flush()
        self._enqueue(("sql", "INSERT OR REPLACE INTO events VALUES(?,?)", (message["seq"], data)))
        if message["name"] == "run.finished":
            self.flush()

    def allow_metadata(self, size):
        """索引不设累计额度，仅保留真实磁盘空间保护。"""
        if (self.project_budget and not self.project_budget.can_write(size + self.project_budget.write_reserve)) or shutil.disk_usage(self.root).free < size + 256 * 1024**2:
            self.incomplete = True
            self.degraded_reason = self.degraded_reason or "磁盘剩余空间不足，后续仅保留对话与恢复点"
            return False
        self.metadata_bytes += size
        if self.project_budget:
            self.project_budget.charge(size)
        return True

    def tensor(self, value):
        """保存独立 CPU 快照，不设单块或累计采集额度。"""
        import torch
        nbytes = value.numel() * value.element_size()
        if (self.project_budget and not self.project_budget.can_write(nbytes + 8192 + self.project_budget.write_reserve)) or shutil.disk_usage(self.root).free < nbytes + 256 * 1024**2:
            self.incomplete = True
            self.degraded_reason = "磁盘保留空间不足，已停止张量采集，生成继续"
            return None
        # 复制前排空拥堵队列，避免慢盘导致快照累计或跳过后续采集。
        if self.jobs.full() or self.pending_bytes + nbytes > self.queue_limit:
            self.flush()
        snapshot = value.detach().to(device="cpu", copy=True).contiguous()
        ref = uuid.uuid4().hex
        if not self._enqueue(("tensor", ref, snapshot), nbytes):
            return None
        self.bytes_written += nbytes + 4096
        if self.project_budget:
            self.project_budget.charge(nbytes + 4096)
        return ref

    def operation(self, context, values):
        """把操作输出与真实执行身份关联，缺失张量仍保留操作记录。"""
        refs = {name: self.tensor(value) for name, value in values.items() if value is not None}
        if not self.allow_metadata(len(json.dumps(context)) + len(json.dumps(refs)) + 512):
            return refs
        self._enqueue(("sql", "INSERT INTO operations VALUES(?,?,?,?,?,?)", (
                context["branch_id"], context["forward_id"], context.get("layer_index", -1),
                context["operation_id"], json.dumps(context), json.dumps(refs))))
        return refs

    def get_operation(self, branch, forward_id, layer, operation):
        """按完整位置读取历史快照，查看不执行模型。"""
        self.flush()
        with self.lock:
            row = self.db.execute("SELECT context,refs FROM operations WHERE branch=? AND forward_id=? AND layer=? AND operation=?",
                                  (branch, forward_id, layer, operation)).fetchone()
        return (json.loads(row["context"]), json.loads(row["refs"])) if row else (None, {})

    def load(self, ref, cancel=None):
        """核对哈希、文件大小和形状后读取数值块。"""
        if not ref or not re.fullmatch(r"[0-9a-f]{32}", ref):
            return None
        if cancel:
            cancel()
        self.flush(cancel=cancel)
        with self.lock:
            row = self.db.execute("SELECT * FROM tensors WHERE ref=?", (ref,)).fetchone()
        if not row:
            return None
        target = safe_path(self.root, row["path"])
        # 分块计算摘要，不将完整大文件再次复制到内存。
        digest = hashlib.sha256()
        with target.open("rb") as handle:
            for block in iter(lambda: handle.read(1024 * 1024), b""):
                if cancel:
                    cancel()
                digest.update(block)
        if digest.hexdigest() != row["sha256"]:
            raise ProtocolError("TRACE_CORRUPT", "数值块校验失败")
        from safetensors.torch import load_file
        value = load_file(str(target), device="cpu")["value"]
        if cancel:
            cancel()
        if list(value.shape) != json.loads(row["shape"]) or value.numel() * value.element_size() != row["nbytes"]:
            raise ProtocolError("TRACE_CORRUPT", "数值块形状不符合索引")
        return value

    def catalog(self):
        """提供分支和前向目录，供离线视图导航。"""
        self.flush()
        with self.lock:
            rows = self.db.execute("SELECT DISTINCT branch,forward_id,context FROM operations WHERE operation='embed' ORDER BY rowid").fetchall()
        return [{"branch_id": r["branch"], "forward_id": r["forward_id"],
                 "positions": json.loads(r["context"]).get("positions", [])} for r in rows]

    def close(self):
        """收尾 WAL，使整个目录可以直接复制与离线打开。"""
        if self.closed:
            return
        failure = None
        try:
            self.flush()
        except Exception as exc:
            failure = exc
        finally:
            if self.writer:
                self.jobs.put((("stop",), 0))
                self.writer.join(60)
                if self.writer.is_alive():
                    raise ProtocolError("TRACE_WRITE_TIMEOUT", "观测写入线程未能安全退出")
        with self.lock:
            if not self.readonly:
                self.db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            self.db.close()
            self.closed = True
        if failure:
            raise failure
