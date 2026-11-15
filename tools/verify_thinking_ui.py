"""经 MCP 驱动普通分发客户端，验证真实四十亿模型思考和回答流同步到界面。"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def verify(executable, directory, model):
    """仅操作新建验证项目，原模型只加载和推理，不提交权重编辑。"""
    root = Path(__file__).resolve().parents[1]
    directory.mkdir(parents=True, exist_ok=True)
    bridge = directory / "连接信息"
    env = dict(os.environ, MODEL_OBSERVATORY_ROOT=str(root), MODEL_OBSERVATORY_MCP_DIR=str(bridge),
               MODEL_OBSERVATORY_SETTINGS_DIR=str(directory / "界面设置"),
               QT_QPA_PLATFORM="offscreen", PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    checks, metrics = {}, {}
    with (directory / "客户端日志.txt").open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(executable)], cwd=root, env=env, stdout=log, stderr=log,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 20
            while not (bridge / f"{process.pid}.json").exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("验证客户端启动失败")
                await asyncio.sleep(.1)
            parameters = StdioServerParameters(command=str(root / ".venv-mcp/Scripts/python.exe"),
                args=["-X", "utf8", str(root / "run_mcp.py")], cwd=str(root), env=env)
            async with stdio_client(parameters) as (reader, writer):
                async with ClientSession(reader, writer) as session:
                    await session.initialize()

                    async def call(tool_name, **arguments):
                        """公开工具执行错误立即中止，不将 accepted 当作完成。"""
                        result = await session.call_tool("observatory_" + tool_name, arguments)
                        value = result.structuredContent or json.loads(result.content[0].text)
                        assert not result.isError and value["ok"], value
                        return value["data"]

                    async def wait(cursor, name):
                        """按游标等待最终事件，同时捕获异步失败。"""
                        end = time.monotonic() + 180
                        while time.monotonic() < end:
                            page = await call("wait_events", after_cursor=cursor,
                                              names=[name, "request.failed"], timeout_seconds=30)
                            assert not page["dropped"], page
                            for event in page["items"]:
                                assert event["name"] != "request.failed", event
                                if event["name"] == name:
                                    return event
                            cursor = page["next_cursor"]
                        raise TimeoutError(name)

                    instances = (await call("list_instances"))["instances"]
                    assert len(instances) == 1
                    await call("select_instance", instance_id=instances[0]["instance_id"])
                    created = await call("create_project", directory=str(directory / "项目"),
                                         name="思考界面验证", model_path=str(model), device="cuda:0")
                    await wait(created["event_cursor"], "model.ready")
                    await wait(created["event_cursor"], "session.activated")
                    initial = await call("get_state")
                    checks["真实模型默认满输出上限"] = initial["max_tokens"] == initial["max_tokens_limit"] and initial["max_tokens"] > 4096
                    created_session = await call("new_session")
                    await wait(created_session["event_cursor"], "session.activated")
                    await call("set_mode", mode="normal")
                    sent = await call("send_message", text="你好", thinking=True, max_tokens=1024)
                    checks["消息立即渲染到独立对话窗口"] = "你好" in (await call("get_history"))["rendered_text"]
                    state = await call("get_state")
                    checks["普通温度与思考温度独立"] = state["temperature"] == .7 and state["thinking_temperature"] == 1.
                    cursor = sent["event_cursor"]
                    streamed, rendered = 0, 0
                    deadline = time.monotonic() + 300
                    finished = None
                    while time.monotonic() < deadline and finished is None:
                        page = await call("wait_events", after_cursor=cursor,
                                          names=["text.updated", "run.finished", "request.failed"], timeout_seconds=10)
                        assert not page["dropped"], page
                        cursor = page["next_cursor"]
                        for event in page["items"]:
                            assert event["name"] != "request.failed", event
                            if event["name"] == "run.finished":
                                finished = event["payload"]
                            elif event["name"] == "text.updated" and event["payload"].get("reasoning_content"):
                                streamed += 1
                        # 直接读取 QTextBrowser 实际渲染内容，验证运行途中思考可见。
                        if finished is None:
                            history = await call("get_history")
                            if history.get("reasoning_content") and "Thinking Process" in history["rendered_text"]:
                                rendered += 1
                    assert finished, "生成超时"
                    history = await call("get_history")
                    checks["运行中多次同步真实思考文本"] = streamed > 1 and rendered > 1
                    checks["思考自然结束并输出正式回答"] = finished["reason"] == "eos" and finished["thinking_complete"] and bool(history["text"].strip())
                    checks["正式回答实际渲染到界面"] = " ".join(history["text"].split()) in " ".join(history["rendered_text"].split())
                    await call("save_project")
                    metrics = {"thinking_updates": streamed, "rendered_during_run": rendered,
                               "forward_count": finished["forward_count"], "reason": finished["reason"], "text": history["text"]}
        finally:
            # 只结束本脚本创建的客户端及其运行器，不操作其他窗口。
            if process.poll() is None:
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
                process.wait(timeout=15)
            report = {"pass": len(checks) == 6 and all(checks.values()), "checks": checks, "metrics": metrics}
            (directory / "思考界面验证.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), "utf-8")
    assert report["pass"], report
    print(json.dumps(report, ensure_ascii=False, indent=2))


def main():
    """必须显式选择分发程序、只读模型和隔离输出目录。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    args = parser.parse_args()
    asyncio.run(verify(args.executable.resolve(), args.directory.resolve(), args.model.resolve()))


if __name__ == "__main__":
    main()
