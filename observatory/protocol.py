"""版本化 NDJSON 通信；标准输出只承载协议消息。"""
from __future__ import annotations

import datetime as dt
import base64
import json
import sys
import threading
import time
import uuid

MAX_LINE = 1024 * 1024


class ProtocolError(ValueError):
    """携带可供界面转换的错误代码。"""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def command(name, payload=None, **identity):
    """创建命令信封，保留请求、运行和分支身份。"""
    result = dict(protocol_version="1.0", producer_id="desktop", kind="command",
                name=name, message_id=uuid.uuid4().hex, request_id=uuid.uuid4().hex,
                run_id=None, branch_id=None, seq=0,
                timestamp_utc=dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
                monotonic_ns=time.monotonic_ns(), context=None, payload=payload or {})
    result.update(identity)
    return result


def parse_command(line):
    """校验输入边界，拒绝不兼容版本和非有限 JSON 数值。"""
    if len(line) > MAX_LINE:
        raise ProtocolError("MESSAGE_TOO_LARGE", "命令超过大小限制")
    try:
        value = json.loads(line, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
    except (ValueError, TypeError) as exc:
        raise ProtocolError("INVALID_JSON", "命令不是有效 JSON") from exc
    if not isinstance(value, dict) or value.get("protocol_version") != "1.0":
        raise ProtocolError("PROTOCOL_VERSION", "通信协议版本不兼容")
    if value.get("kind") != "command" or not isinstance(value.get("payload"), dict):
        raise ProtocolError("INVALID_COMMAND", "命令信封无效")
    if not isinstance(value.get("request_id"), str) or not value.get("request_id"):
        raise ProtocolError("INVALID_COMMAND", "缺少请求编号")
    return value


class Emitter:
    """线程安全地输出事件；每个生产者使用独立递增序号。"""

    def __init__(self, producer="runtime", stream=None):
        self.producer = producer
        self.stream = stream or sys.stdout
        self.lock = threading.RLock()
        self.seq = 0
        self.listener = None

    def send(self, name, payload=None, request=None, kind="event", context=None,
             run_id=None, branch_id=None):
        """输出一整行消息，避免多个线程交错写入。"""
        with self.lock:
            self.seq += 1
            msg = dict(protocol_version="1.0", producer_id=self.producer, kind=kind,
                       name=name, message_id=uuid.uuid4().hex,
                       request_id=request.get("request_id") if request else None,
                       run_id=run_id, branch_id=branch_id, seq=self.seq,
                       timestamp_utc=dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
                       monotonic_ns=time.monotonic_ns(), context=context, payload=payload or {})
            wire = json.dumps(msg, ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n"
            if len(wire.encode("utf-8")) > MAX_LINE:
                if kind == "result" and name in ("feature.result", "file.result"):
                    # 大报告分帧传输；仅完整重组后向界面提交最终结果。
                    raw = wire.encode("utf-8")
                    if len(raw) > 64*1024**2:
                        raise ProtocolError("MESSAGE_TOO_LARGE", "报告超过 64 MiB，请缩小采集范围")
                    chunks = [raw[i:i+384*1024] for i in range(0,len(raw),384*1024)]
                    transfer_id = uuid.uuid4().hex
                    for index, chunk in enumerate(chunks):
                        self.send("result.chunk", {"transfer_id":transfer_id,"index":index,"total":len(chunks),
                            "data":base64.b64encode(chunk).decode("ascii")}, request, "result")
                    return msg
                raise ProtocolError("MESSAGE_TOO_LARGE", "响应超过通信帧上限，请缩小切片")
            if self.listener:
                self.listener(msg)
            self.stream.write(wire)
            self.stream.flush()
            return msg

    def error(self, exc, request=None):
        """将异常转换为可读错误，不将回溯或日志混入标准输出。"""
        return self.send("request.failed", {"code": getattr(exc, "code", "INTERNAL_ERROR"),
                         "message": str(exc), "recoverable": True}, request or {"request_id": "unassociated"}, "error")
