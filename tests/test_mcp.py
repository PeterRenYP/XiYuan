"""MCP 连接隔离、身份验证、分页与错误传播回归，不依赖推理环境。"""
import asyncio
import json
from pathlib import Path
import tempfile
import unittest

from mcp_server.bridge import BridgeClient, BridgeError


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    """使用真实回环套接字模拟两个客户端，验证不会选错操作窗口。"""

    async def asyncSetUp(self):
        """每个测试独立描述符目录与服务列表。"""
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.servers = []

    async def asyncTearDown(self):
        """关闭本测试创建的全部服务器，不触及实际客户端。"""
        for server in self.servers:
            server.close()
            await server.wait_closed()
        self.temporary.cleanup()

    async def start_instance(self, identity="one", handler=None):
        """发布带令牌的测试描述符，任何错误令牌都返回执行错误。"""
        descriptor = {"version": 1, "instance_id": identity, "pid": len(self.servers) + 100, "token": "秘密令牌"}

        async def serve(reader, writer):
            """按与原生桥相同的单帧协议响应，保留中文数据。"""
            request = json.loads(await reader.readline())
            if request["token"] != descriptor["token"]:
                response = {"ok": False, "error": {"code": "UNAUTHORIZED", "message": "令牌错误"}}
            elif request["method"] == "state":
                response = {"ok": True, "data": {"instance_id": identity, "pid": descriptor["pid"]}}
            else:
                response = handler(request) if handler else {"ok": True, "data": {"text": "中文消息"}}
            writer.write(json.dumps(response, ensure_ascii=False).encode() + b"\n")
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        server = await asyncio.start_server(serve, "127.0.0.1", 0)
        self.servers.append(server)
        descriptor["port"] = server.sockets[0].getsockname()[1]
        (self.directory / (identity + ".json")).write_text(json.dumps(descriptor), "utf-8")
        return descriptor

    async def test_no_instance_is_actionable_error(self):
        """未启动软件时返回明确错误，不暗中创建新模型进程。"""
        with self.assertRaises(BridgeError) as caught:
            await BridgeClient(self.directory).call("state")
        self.assertEqual(caught.exception.code, "NO_INSTANCE")

    async def test_single_instance_and_token_redaction(self):
        """单实例自动连接，公开列表不泄露控制令牌。"""
        await self.start_instance()
        client = BridgeClient(self.directory)
        self.assertNotIn("秘密令牌", json.dumps(await client.list_instances(), ensure_ascii=False))
        self.assertEqual((await client.call("history"))["text"], "中文消息")

    async def test_multiple_instances_require_selection(self):
        """多个窗口必须选择，并且关闭后不会切到另一个窗口。"""
        await self.start_instance("one")
        await self.start_instance("two")
        client = BridgeClient(self.directory)
        with self.assertRaises(BridgeError) as caught:
            await client.call("state")
        self.assertEqual(caught.exception.code, "MULTIPLE_INSTANCES")
        await client.select_instance("two")
        self.assertEqual((await client.call("state"))["instance_id"], "two")
        self.servers[1].close()
        await self.servers[1].wait_closed()
        with self.assertRaises(BridgeError) as caught:
            await client.call("state")
        self.assertEqual(caught.exception.code, "CONNECTION_FAILED")

    async def test_stale_and_corrupt_descriptors_are_ignored(self):
        """损坏或身份不符的残留文件不能被认为是有效客户端。"""
        descriptor = await self.start_instance()
        descriptor["instance_id"] = "wrong"
        (self.directory / "one.json").write_text(json.dumps(descriptor), "utf-8")
        (self.directory / "broken.json").write_text("不是 JSON", "utf-8")
        self.assertEqual((await BridgeClient(self.directory).list_instances())["instances"], [])

    async def test_runtime_error_retains_code_and_chinese_message(self):
        """后端失败不能被 accepted 或空结果掩盖。"""
        await self.start_instance(handler=lambda _: {"ok": False, "error": {"code": "STALE_WEIGHTS", "message": "版本已变化"}})
        with self.assertRaises(BridgeError) as caught:
            await BridgeClient(self.directory).call("edit.apply")
        self.assertEqual((caught.exception.code, str(caught.exception)), ("STALE_WEIGHTS", "版本已变化"))

    async def test_event_wait_drains_pages_before_matching(self):
        """目标在下一页时继续读取，返回最后读到的游标。"""
        pages = []

        def events(request):
            """第一批只有输出事件，第二批才结束。"""
            after = request["params"]["after_cursor"]
            pages.append(after)
            return {"ok": True, "data": {"items": [{"name": "text.updated" if after == 0 else "run.finished", "cursor": after + 1}],
                "next_cursor": after + 1, "latest_cursor": 2, "dropped": False, "instance_id": "one"}}

        await self.start_instance(handler=events)
        value = await BridgeClient(self.directory).wait_events(0, ["run.finished"], 1)
        self.assertEqual(pages, [0, 1])
        self.assertEqual(value["items"][0]["cursor"], 2)
        self.assertFalse(value["timed_out"])

    async def test_event_overflow_is_reported(self):
        """即使目标未出现，发生环缓冲淘汰也必须立即告知调用方。"""
        await self.start_instance(handler=lambda _: {"ok": True, "data": {"items": [], "next_cursor": 500,
            "latest_cursor": 500, "dropped": True, "instance_id": "one"}})
        result = await BridgeClient(self.directory).wait_events(0, ["run.finished"], 1)
        self.assertTrue(result["dropped"])

    async def test_oversized_request_rejected_before_send(self):
        """帧超限在连接层拒绝，避免服务端界面承受无界输入。"""
        await self.start_instance()
        with self.assertRaises(BridgeError) as caught:
            await BridgeClient(self.directory).call("history", {"text": "x" * (1024 * 1024)})
        self.assertEqual(caught.exception.code, "FRAME_TOO_LARGE")


if __name__ == "__main__":
    unittest.main()
