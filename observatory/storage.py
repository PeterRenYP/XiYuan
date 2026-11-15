"""项目存储统计、低空间保护及仅针对自有缓存的清理。"""
from pathlib import Path
import json
import os
import shutil
import uuid

from .protocol import ProtocolError

MIB = 1024**2


def directory_bytes(root):
    """只累计普通文件，不跟随目录链接。"""
    total = 0
    for current, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = [name for name in dirs if not (Path(current) / name).is_symlink()]
        for name in files:
            path = Path(current) / name
            if not path.is_symlink():
                total += path.stat().st_size
    return total


def atomic_json(path, value):
    """同目录原子提交；失败时保留上一次有效现场。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, allow_nan=False)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


class ProjectBudget:
    """统计项目写入量，仅按真实磁盘剩余空间保护采集。"""

    def __init__(self, project, trace_root, per_run=128 * MIB):
        self.root = Path(project).resolve()
        self.trace_root = Path(trace_root).resolve()
        if not self.trace_root.is_relative_to(self.root) or self.trace_root == self.root:
            raise ProtocolError("INVALID_PROJECT", "运行记录必须位于当前项目内")
        config = json.loads((self.root / "project.json").read_text(encoding="utf-8"))
        settings = config.get("storage", {})
        # 兼容旧项目字段，但旧正数配额不再限制采集或触发缓存删除。
        self.limit = self.per_run = 0
        self.reserve = max(256 * MIB, int(settings.get("reserve_mib", 1024)) * MIB)
        self.used = directory_bytes(self.root)
        # 为前端现场的原子替换、清单和日志留出空间，避免张量吃满实际磁盘空间。
        self.write_reserve = 32 * MIB
        if settings.get("auto_clean", True):
            self.prune()
        if not self.can_write(self.write_reserve):
            raise ProtocolError("PROJECT_FULL", "磁盘剩余空间不足，请打开存储管理清理记录或释放磁盘空间")

    def prune(self):
        """只删除本工具标记的、已结束且未手动保留的缓存；从不删除会话和模型。"""
        candidates = []
        for path in (self.root / "sessions").glob("*/records/*/manifest.json"):
            if path.is_symlink() or not path.resolve().is_relative_to(self.root):
                continue
            try:
                value = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if value.get("managed_by") == "observatory-project" and not value.get("pinned") and value.get("status") != "running":
                candidates.append(path)
        for path in sorted(candidates, key=lambda p: p.stat().st_mtime):
            # 无限采集无法预先保留整轮空间，仅为下一次写入保留必要空间。
            upcoming = self.per_run or self.write_reserve
            if (not self.limit or self.used + upcoming + self.write_reserve <= self.limit) and shutil.disk_usage(self.root).free >= self.reserve + upcoming + self.write_reserve:
                break
            target = path.parent.resolve()
            if target.is_relative_to(self.root / "sessions") and not path.parent.is_symlink():
                size = directory_bytes(target)
                shutil.rmtree(target)
                self.used -= size

    def can_write(self, size):
        """仅检查真实磁盘空闲量，不检查历史采集额度。"""
        return shutil.disk_usage(self.root).free >= self.reserve + size

    def charge(self, size):
        """逐次累计写入开销，SQLite 页面预留按保守值计入。"""
        self.used += size
