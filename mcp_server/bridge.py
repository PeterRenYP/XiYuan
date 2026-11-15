"""发现本机客户端，并通过带令牌的回环连接调用控制桥。"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path

FRAME_LIMIT = 1024 * 1024


class BridgeError(RuntimeError):
    """保留机器可读错误码与中文说明，供 MCP 返回执行错误。"""

    def __init__(self, message: str, code: str = "BRIDGE_ERROR"):
        super().__init__(message)
        self.code = code


class BridgeClient:
    """同一 MCP 会话固定连接选中的客户端，多个实例时禁止猜测目标。"""

    def __init__(self, directory: str | Path | None = None):
        self.directory = Path(directory or os.environ.get("MODEL_OBSERVATORY_MCP_DIR") or
                              Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share")) /
                              "ModelObservatory/mcp")
        self.selected: dict | None = None
        self.selection_lock = asyncio.Lock()

    async def _request(self, descriptor: dict, method: str, params: dict, timeout: float = 25) -> dict:
        """限制帧长和等待时间；始终连接回环地址，不接受描述符提供的外部主机。"""
        port = descriptor.get("port")
        if not isinstance(port, int) or not 1 <= port <= 65535 or not isinstance(descriptor.get("token"), str):
            raise BridgeError("客户端连接信息无效", "INVALID_DESCRIPTOR")
        wire = json.dumps({"token": descriptor["token"], "method": method, "params": params},
                          ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
        if len(wire) > FRAME_LIMIT:
            raise BridgeError("请求过大，请缩小查询范围", "FRAME_TOO_LARGE")
        writer = None
        try:
            async with asyncio.timeout(timeout):
                reader, writer = await asyncio.open_connection("127.0.0.1", port, limit=FRAME_LIMIT)
                writer.write(wire)
                await writer.drain()
                line = await reader.readline()
                if not line.endswith(b"\n") or len(line) > FRAME_LIMIT:
                    raise BridgeError("客户端响应不完整或过大", "INVALID_RESPONSE")
                value = json.loads(line, parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
                if not isinstance(value, dict) or not isinstance(value.get("ok"), bool):
                    raise BridgeError("客户端响应格式错误", "INVALID_RESPONSE")
                if not value["ok"]:
                    error = value.get("error", {})
                    raise BridgeError(error.get("message", "操作失败"), error.get("code", "BRIDGE_ERROR"))
                return value.get("data", {})
        except TimeoutError as exc:
            raise BridgeError("客户端连接超时，请检查软件是否仍在运行", "TIMEOUT") from exc
        except (OSError, ValueError, UnicodeError) as exc:
            raise BridgeError("无法连接客户端，请启动支持 MCP 的析元", "CONNECTION_FAILED") from exc
        finally:
            if writer is not None:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass

    async def _discover(self) -> list[tuple[dict, dict]]:
        """忽略退出后残留和损坏的描述符，只接受身份与连接响应一致的实例。"""
        async def probe(path: Path):
            try:
                if path.stat().st_size > 16384:
                    return None
                descriptor = json.loads(path.read_text("utf-8"))
                if not isinstance(descriptor, dict) or descriptor.get("version") != 1:
                    return None
                state = await self._request(descriptor, "state", {}, timeout=1.5)
                if state.get("instance_id") != descriptor.get("instance_id") or state.get("pid") != descriptor.get("pid"):
                    return None
                return descriptor, state
            except (OSError, ValueError, BridgeError):
                return None

        paths = sorted(self.directory.glob("*.json"))[:64]
        results = await asyncio.gather(*(probe(path) for path in paths))
        return [result for result in results if result is not None]

    async def list_instances(self) -> dict:
        """列出客户端身份与状态，连接令牌始终留在服务内部。"""
        return {"instances": [state for _, state in await self._discover()],
                "selected_instance_id": self.selected.get("instance_id") if self.selected else None}

    async def select_instance(self, instance_id: str) -> dict:
        """显式选择客户端；选择失效后要求重新选择，避免误操作其他窗口。"""
        async with self.selection_lock:
            for descriptor, state in await self._discover():
                if descriptor["instance_id"] == instance_id:
                    self.selected = descriptor
                    return state
        raise BridgeError("指定客户端不存在，请先列出运行实例", "INSTANCE_NOT_FOUND")

    async def call(self, method: str, params: dict | None = None) -> dict:
        """单实例自动连接，多实例必须先选择；重启后不沿用旧事件游标。"""
        async with self.selection_lock:
            if self.selected is None:
                instances = await self._discover()
                if not instances:
                    raise BridgeError("未发现客户端，请先启动支持 MCP 的析元", "NO_INSTANCE")
                if len(instances) != 1:
                    raise BridgeError("发现多个客户端，请先调用 observatory_select_instance 选择目标", "MULTIPLE_INSTANCES")
                self.selected = instances[0][0]
            descriptor = self.selected
        return await self._request(descriptor, method, params or {})

    async def wait_events(self, after_cursor: int, names: list[str], timeout_seconds: float) -> dict:
        """异步等待事件，游标分页不会丢掉同一批中后续事件；环缓冲溢出显式返回。"""
        deadline = asyncio.get_running_loop().time() + timeout_seconds
        while True:
            page = await self.call("events", {"after_cursor": after_cursor, "limit": 100})
            matched = [event for event in page["items"] if not names or event["name"] in names]
            if matched or page["dropped"]:
                return {**page, "items": matched, "timed_out": False}
            after_cursor = page["next_cursor"]
            if asyncio.get_running_loop().time() >= deadline:
                return {**page, "items": [], "timed_out": True}
            # 积压事件先读完，随后让出事件循环，不阻塞停止命令或其他 MCP 调用。
            await asyncio.sleep(0 if after_cursor < page["latest_cursor"] else 0.25)
