"""独立异常监控的有界采样与异步传输；此处不进行异常判定。"""
from __future__ import annotations

from collections import deque
import hashlib
import json
import math
import select
import socket
import threading
import time


class AnomalySampler:
    """复用真实前向结果，最多暂存四份小型 GPU 快照，网络不阻塞推理。"""

    def __init__(self, endpoint, metadata, controller=None):
        """仅本机随机端口与会话密钥有效；连接失败时自动关闭采样。"""
        self.enabled = bool(endpoint.get("port") and endpoint.get("secret"))
        self.endpoint, self.metadata, self.controller = endpoint, metadata, controller
        self.pending = deque()
        self.lock = threading.Lock()
        self.closed = False
        self.capture_disabled = False
        self.dropped = self.sampled = self.eligible = 0
        self.dropped_tokens = self.dropped_tensors = 0
        self.step, self.layer, self.operation = -1, -1, ""
        self.last_capture = 0.0
        self.last_token_at = time.monotonic()
        self.interval = 16
        self.overhead = 0.0
        self.run_started = time.monotonic()
        # 各轮生成使用不同起点，短回答也能逐轮覆盖深层与不同注意力头。
        offset = int.from_bytes(hashlib.sha256(metadata["run_id"].encode("utf-8")).digest()[:4], "little")
        self.layer_offset = offset % max(1, metadata["layers"])
        self.head_offset = offset // max(1, metadata["layers"])
        self.last_heartbeat = 0.0
        self.connected = False
        self.connection = None
        self.wire = bytearray()
        if self.enabled:
            # 非阻塞本机连接不产生常驻 Python 线程，避免与逐算子推理争用解释器锁。
            try:
                self.connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.connection.setblocking(False)
                result = self.connection.connect_ex(("127.0.0.1", int(endpoint["port"])))
                self.connected = result == 0
                self._queue({"kind": "start", "secret": endpoint["secret"], **metadata})
            except (OSError, ValueError):
                self._disable()

    def progress(self, step, layer, operation):
        """更新普通标量，不查询 GPU，也不读取模型张量。"""
        self.step, self.layer, self.operation = step, layer, operation
        # 每层残差或前向入口才检查时钟，不为每个小算子执行网络操作。
        if self.enabled and operation in ("embed", "residual"):
            now = time.monotonic()
            if now - self.last_heartbeat >= .5:
                self._queue({"kind": "progress", "step": self.step, "layer": self.layer, "operation": self.operation,
                             "waiting": bool(self.controller and self.controller.waiting), "dropped": self.dropped,
                             "sampled": self.sampled, "eligible": self.eligible, "interval": self.interval,
                             "capture_disabled": self.capture_disabled, "dropped_tokens": self.dropped_tokens,
                             "dropped_tensors": self.dropped_tensors})
                self.last_heartbeat = now

    def token(self, token_id, channel, piece, thinking_complete):
        """保留Token顺序及有界文本片段；间隔使用生成侧单调时钟。"""
        if self.enabled:
            self._collect()
        now = time.monotonic()
        self._queue({"kind": "token", "step": self.step, "id": token_id, "channel": channel,
                     "piece": piece[:2048], "thinking_complete": thinking_complete,
                     "elapsed_ms": (now - self.last_token_at) * 1000,
                     "input_tokens": self.metadata.get("input_tokens", 0) + self.step})
        self.last_token_at = now

    def _collect(self):
        """在选词已读取 GPU 标量之后回收就绪快照，发送线程不访问 CUDA。"""
        complete = []
        with self.lock:
            while self.pending and (self.pending[0][2] is None or self.pending[0][2].query()):
                complete.append(self.pending.popleft())
        for message, host, _ in complete:
            values = host.tolist()
            if message.get("raw"):
                values = [value if math.isfinite(value) else None for value in values]
            message["values"] = values
            self._queue(message)

    def _queue(self, message):
        """至多缓存六十四 KiB；通信不可写时丢弃新样本，不等待接收方。"""
        if not self.enabled:
            return
        message.update(run_id=self.metadata["run_id"], branch_id=self.metadata["branch_id"])
        encoded = (json.dumps(message, ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(self.wire) + len(encoded) > 65536:
            self.dropped += 1
            if message["kind"] == "token":
                self.dropped_tokens += 1
            else:
                self.dropped_tensors += 1
        else:
            self.wire.extend(encoded)
        self._pump()

    def _pump(self):
        """单次非阻塞发送有界字节，所有连接与写入异常只停用监控。"""
        if not self.enabled or self.connection is None:
            return
        try:
            if not self.connected:
                if not select.select([], [self.connection], [], 0)[1]:
                    if time.monotonic() - self.run_started > .5:
                        self._disable()
                    return
                if self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR):
                    self._disable()
                    return
                self.connected = True
            if self.wire:
                sent = self.connection.send(self.wire[:32768])
                if sent:
                    del self.wire[:sent]
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self._disable()

    def _disable(self):
        """关闭的只是监控连接，不调用生成控制器。"""
        self.enabled = False
        if self.connection is not None:
            self.connection.close()
            self.connection = None
        self.wire.clear()

    def capture(self, layer, operation, result, context, positions):
        """限频轮换采集激活、残差与注意力的固定大小摘要，不保留整块张量。"""
        if not self.enabled or self.closed or self.capture_disabled or layer < 0 or operation not in ("mlp", "residual", "attention", "linear_delta"):
            return
        step = context["generation_step"]
        if step != self.step:
            return
        if operation in ("mlp", "residual"):
            self.eligible += 1
        layers = self.metadata["layers"]
        # 每十六步检查一个轮换层，避免同一步突发复制多层；超预算时进一步降频。
        if step % self.interval or layer != (self.layer_offset + step // self.interval) % layers:
            return
        now = time.monotonic()
        if operation == "attention" and now - self.last_capture < .15:
            return
        with self.lock:
            if len(self.pending) >= 4:
                self.dropped += 1
                self.dropped_tensors += 1
                return
        import torch
        started = time.perf_counter()
        with torch.inference_mode():
            primary = result[0] if isinstance(result, tuple) else result
            message = {"kind": "tensor", "layer": layer, "operation": operation, "step": step,
                       "phase": "prefill" if step == 0 else "decode", "position": positions[-1]}
            if operation == "attention":
                if not isinstance(result, tuple) or result[1] is None or result[1].ndim != 4:
                    return
                weights = result[1]
                heads, keys = weights.shape[1], weights.shape[-1]
                visit = step // (self.interval * layers)
                ids = sorted({(self.head_offset + visit + i * max(1, heads // 4)) % heads for i in range(min(4, heads))})
                # 最后一个查询位置没有未来键；逐头统计，不能先对头取平均。
                rows = weights[0, ids, -1, :].detach().float()
                valid = torch.isfinite(rows).all(-1)
                safe = torch.where(torch.isfinite(rows), rows, 0).clamp_min(0)
                safe = safe / safe.sum(-1, keepdim=True).clamp_min(1e-30)
                entropy = -(safe * safe.clamp_min(1e-30).log()).sum(-1) / max(1e-12, math.log(max(2, keys)))
                peak, index = safe.max(-1)
                bins = min(64, keys)
                # 连续区间求和保留总概率质量；头相似度明确是分桶近似。
                boundaries = [i * keys // bins for i in range(bins + 1)]
                cumulative = torch.nn.functional.pad(safe.cumsum(-1), (1, 0))
                histogram = cumulative[:, boundaries[1:]] - cumulative[:, boundaries[:-1]]
                packed = torch.cat([entropy[:, None], peak[:, None], index.float()[:, None],
                                    valid.float()[:, None], histogram], dim=-1).contiguous()
                message.update(heads=ids, keys=keys, bins=bins)
                self.last_capture = now
            else:
                value = result[1] if operation == "linear_delta" and isinstance(result, tuple) else primary
                if value is None:
                    return
                row = value.reshape(-1) if operation == "linear_delta" else value.reshape(-1, value.shape[-1])[-1]
                # 固定等距维度用于趋势与表示近似；不复制全量隐藏向量或递归状态。
                stride = max(1, (row.numel() + 127) // 128)
                packed = row[::stride][:128].detach().to(dtype=torch.float32, copy=True)
                # 均方根、峰值与近零比例交给独立 CPU 进程计算，减少推理流的小核启动。
                message["raw"] = True
                message["dimensions"] = row.numel()
            if packed.is_cuda:
                # 在原流排队复制到有界锁页内存，发送线程查询事件，不同步整个设备。
                host = torch.empty_like(packed, device="cpu", pin_memory=True)
                host.copy_(packed, non_blocking=True)
                ready = torch.cuda.Event()
                ready.record(torch.cuda.current_stream(packed.device))
            else:
                host, ready = packed.cpu(), None
            with self.lock:
                self.pending.append((message, host, ready))
            self.sampled += 1
        self.overhead += time.perf_counter() - started
        elapsed = time.monotonic() - self.run_started
        if elapsed > 1 and self.overhead / elapsed > .01:
            self.interval = min(128, self.interval * 2)
            self.overhead = 0.0
            self.run_started = time.monotonic()

    def finish(self, outcome, reason, thinking_complete):
        """尽力发送结束状态后关闭，不等待网络；未完成快照单独安全释放。"""
        if self.enabled:
            self._collect()
            self._queue({"kind": "finish", "outcome": outcome, "reason": reason,
                         "thinking_complete": thinking_complete, "step": self.step})
        self.closed = True
        self._disable()
        with self.lock:
            pending = list(self.pending)
            self.pending.clear()
        if pending:
            def release():
                """仅异常结束仍有在途拷贝时保留缓冲，生成线程不等待 CUDA。"""
                for _, _, ready in pending:
                    if ready is not None:
                        ready.synchronize()
            threading.Thread(target=release, name="监控快照安全释放", daemon=True).start()


def baseline_identity(adapter, generation, rules, mode, policy):
    """按模型、权重、精度、设备与实际采样配置隔离统计参考，防止跨条件污染。"""
    identity = {"model": adapter.info.get("model_revision"), "weights": adapter.weights_revision,
                "device": str(adapter.device), "dtype": str(next(adapter.model.parameters()).dtype),
                "generation": {k: v for k, v in generation.items() if k not in ("seed", "max_new_tokens", "requested_max_new_tokens")},
                "rules": rules, "mode": mode, "policy": policy, "schema": 1}
    return hashlib.sha256(json.dumps(identity, sort_keys=True, ensure_ascii=False).encode("utf-8")).hexdigest()
