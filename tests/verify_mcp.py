"""经真实 MCP 标准管道驱动原生客户端与 CPU 模型，验证完整控制链路。"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def verify(executable: Path, directory: Path, model: Path) -> dict:
    """测试仅操作指定目录中的项目与复制模型，不写入用户的实际项目。"""
    root = Path(__file__).resolve().parents[1]
    directory.mkdir(parents=True, exist_ok=True)
    bridge_directory = directory / "连接信息"
    env = os.environ.copy()
    env.update(MODEL_OBSERVATORY_ROOT=str(root), MODEL_OBSERVATORY_MCP_DIR=str(bridge_directory),
               MODEL_OBSERVATORY_SETTINGS_DIR=str(directory / "界面设置"),
               QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORM_PLUGIN_PATH=str(executable.parent / "platforms" if (executable.parent / "platforms").is_dir() else root / ".cache/qt/6.8.3/mingw_64/plugins/platforms"),
               PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    checks = {}
    completed = False
    original_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in model.glob("*.safetensors")}
    log = (directory / "客户端日志.txt").open("w", encoding="utf-8")
    process = subprocess.Popen([str(executable)], cwd=root, env=env, stdout=log, stderr=log,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    called = set()
    try:
        deadline = time.monotonic() + 15
        while not list(bridge_directory.glob("*.json")):
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("测试客户端启动失败，请检查客户端日志")
            await asyncio.sleep(.1)
        # 直接校验原生桥的认证与帧解析；令牌内容不进入验证报告。
        descriptor = json.loads((bridge_directory / (str(process.pid) + ".json")).read_text("utf-8"))
        for payload, expected in ((b'{"token":"wrong","params":{}}\n', "UNAUTHORIZED"), (b'{broken}\n', "INVALID_JSON")):
            raw_reader, raw_writer = await asyncio.open_connection("127.0.0.1", descriptor["port"])
            raw_writer.write(payload)
            await raw_writer.drain()
            response = json.loads(await raw_reader.readline())
            assert not response["ok"] and response["error"]["code"] == expected, response
            raw_writer.close()
            await raw_writer.wait_closed()
        checks["原生桥认证与坏帧拒绝"] = True
        parameters = StdioServerParameters(command=str(root / ".venv-mcp/Scripts/python.exe"),
            args=["-X", "utf8", str(root / "run_mcp.py")], cwd=str(root), env=env)
        async with stdio_client(parameters) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                await session.initialize()
                listed = await session.list_tools()
                checks["标准协议握手与工具目录"] = len(listed.tools) == 32

                async def call(tool_name, **arguments):
                    """所有业务操作都走公开 MCP 工具，执行错误立即中止测试。"""
                    called.add(tool_name)
                    response = await session.call_tool("observatory_" + tool_name, arguments)
                    value = response.structuredContent or json.loads(response.content[0].text)
                    if response.isError or not value["ok"]:
                        raise AssertionError((tool_name, value))
                    return value["data"]

                async def error(name, code, **arguments):
                    """校验失败仍是标准 MCP 执行错误，并保留机器可读错误码。"""
                    called.add(name)
                    response = await session.call_tool("observatory_" + name, arguments)
                    value = response.structuredContent or json.loads(response.content[0].text)
                    assert response.isError and value["error"]["code"] == code, value

                async def wait(after, name, seconds=30):
                    """等待最终事件并同时监听失败，避免把提交成功误认为执行完成。"""
                    page = await call("wait_events", after_cursor=after, names=[name, "request.failed"], timeout_seconds=seconds)
                    assert not page["timed_out"] and not page["dropped"], page
                    failures = [event for event in page["items"] if event["name"] == "request.failed"]
                    assert not failures, failures
                    return next(event for event in page["items"] if event["name"] == name)

                instances = await call("list_instances")
                assert len(instances["instances"]) == 1
                await call("select_instance", instance_id=instances["instances"][0]["instance_id"])
                state = await call("get_state")
                await error("send_message", "INVALID_PARAMS", text="你好", unknown=True)
                checks["未知参数被拒绝"] = True

                result = await call("create_project", directory=str(directory / "项目"), name="MCP 联调项目",
                                    model_path=str(model), device="cpu")
                await wait(result["event_cursor"], "model.ready")
                await wait(result["event_cursor"], "session.activated")
                empty = await call("get_state")
                checks["新项目零会话且默认满输出上限"] = empty["session_count"] == 0 and empty["max_tokens"] == empty["max_tokens_limit"]
                result = await call("new_session")
                await wait(result["event_cursor"], "session.activated")
                info = await call("model_info")
                checks["加载真实模型与算子目录"] = info["config"]["num_hidden_layers"] == 2 and len(info["operation_catalog"]) == 2
                await call("get_telemetry")
                await error("send_message", "INVALID_PARAMS", text="你好", thinking=True)
                checks["不支持的思考模式被拒绝"] = True

                await call("set_mode", mode="observe")
                result = await call("send_message", text="你好", max_tokens=2, temperature=0)
                immediate = await call("get_history", offset=0, limit=20)
                checks["MCP发送后界面立即显示用户消息"] = "你好" in immediate["rendered_text"]
                checks["MCP发送后独立对话窗口可见"] = (await call("get_state"))["conversation_visible"]
                finished = await wait(result["event_cursor"], "run.finished")
                assert finished["payload"]["outcome"] == "completed", finished
                history = await call("get_history", offset=0, limit=20)
                checks["对话与界面历史同步"] = history["items"][0]["content"] == "你好" and len(history["items"]) >= 2
                checks["MCP单次输出上限不覆盖界面默认"] = (await call("get_state"))["max_tokens"] == empty["max_tokens"]
                checks["模型回复实际渲染到界面"] = bool(history["text"]) and " ".join(history["text"].split()) in " ".join(history["rendered_text"].split())
                token_page = await call("get_tokens", offset=0, limit=100)
                checks["输入与生成Token完整可读"] = any(item.get("generated") for item in token_page["items"])
                selected = await call("select_view", layer_index=1, operation="gate", neuron_index=3, token_position=0, head=0)
                assert selected["layer_index"] == 1
                checks["观测位置与界面同步"] = (await call("get_state"))["selection"] == selected
                await call("select_view", layer_index=0, operation="gate", neuron_index=3, token_position=0, head=0)
                state = await call("get_state")
                snapshot = await call("query_view", branch_id="base", forward_id="f0", layer_index=0,
                                      operation="gate", neuron_index=3, token_position=0, head=0)
                assert snapshot["status"] == "available", snapshot
                ref = snapshot["refs"]["output"]
                shape = snapshot["tensors"]["output"]["shape"]
                sliced = await call("tensor_slice", tensor_ref=ref, ranges=[{"start": 0, "stop": min(n, 2), "step": 1} for n in shape])
                scalar = await call("query_scalar", branch_id="base", forward_id="f0", layer_index=0,
                                    operation="gate", row=0, column=0)
                checks["真实快照张量与标量内积"] = bool(sliced["values"]) and scalar["terms"] == 16

                await error("apply_rules", "STALE_WEIGHTS", expected_weights_revision="w999", target="temporary",
                            expected_model_revision=state["model_revision"],
                            rules=[{"layer_index": 0, "neuron_index": 3, "kind": "zero", "scope": "all"}])
                checks["过期编辑版本被拒绝"] = True
                await error("apply_rules", "STALE_MODEL", expected_weights_revision=state["weights_revision"],
                            expected_model_revision="other-model", target="temporary",
                            rules=[{"layer_index": 0, "neuron_index": 3, "kind": "zero", "scope": "all"}])
                checks["切换模型后旧编辑请求被拒绝"] = True
                result = await call("apply_rules", expected_weights_revision=state["weights_revision"], target="temporary",
                                    expected_model_revision=state["model_revision"],
                                    rules=[{"layer_index": 0, "neuron_index": 3, "kind": "zero", "scope": "all"}])
                await wait(result["event_cursor"], "edit.applied")
                rules = await call("get_rules")
                assert len(rules["rules"]) == 1
                checks["MCP应用规则后界面编辑表同步"] = (await call("get_state"))["edit_rule_count"] == 1
                # 无效批量规则必须全部失败，并保留之前已经应用的规则。
                bad = await call("apply_rules", expected_weights_revision=rules["weights_revision"], target="temporary",
                                 expected_model_revision=state["model_revision"],
                                 rules=[{"layer_index": 999, "neuron_index": 3, "kind": "zero"}])
                page = await call("wait_events", after_cursor=bad["event_cursor"], names=["request.failed"], timeout_seconds=10)
                assert page["items"][0]["payload"]["code"] == "INVALID_TARGET", page
                checks["后端编辑失败完整回传且保留旧规则"] = len((await call("get_rules"))["rules"]) == 1 and not (await call("get_state"))["editing"]
                result = await call("send_message", text="你好", max_tokens=1, temperature=0)
                await wait(result["event_cursor"], "run.finished")
                neuron = await call("query_view", branch_id="base", forward_id="f0", layer_index=0,
                                    operation="mlp", neuron_index=3, token_position=0)
                checks["神经元临时规则实际生效"] = neuron["neuron"]["value"] == 0
                first_session = (await call("list_sessions"))["current"]
                result = await call("new_session")
                await wait(result["event_cursor"], "session.activated")
                checks["新建会话保留项目规则"] = len((await call("get_rules"))["rules"]) == 1
                result = await call("switch_session", session_id=first_session)
                await wait(result["event_cursor"], "session.activated")

                state = await call("get_state")
                result = await call("clear_rules", expected_weights_revision=state["weights_revision"], expected_model_revision=state["model_revision"])
                await wait(result["event_cursor"], "edit.applied")
                assert not (await call("get_rules"))["rules"]
                checks["MCP清除规则后界面编辑表同步"] = (await call("get_state"))["edit_rule_count"] == 0
                await call("save_project")
                checks["会话切换清除规则与保存"] = True

                state = await call("get_state")
                result = await call("apply_rules", expected_weights_revision=state["weights_revision"], target="model",
                    expected_model_revision=state["model_revision"],
                    rules=[{"layer_index": 0, "neuron_index": 3, "kind": "scale", "value": .5, "projection": "down"}])
                edited = await wait(result["event_cursor"], "edit.applied")
                # 权重修改可能触发已有对话重算，必须等待串行运行结束再执行下一步。
                await asyncio.sleep(.3)
                state = await call("get_state")
                if state["active"]:
                    await wait(edited["cursor"], "run.finished")
                backup = edited["payload"]["backup_path"]
                result = await call("export_model", destination=str(directory / "导出模型"))
                await wait(result["event_cursor"], "model.exported")
                state = await call("get_state")
                result = await call("restore_backup", expected_weights_revision=state["weights_revision"], expected_model_revision=state["model_revision"], backup_path=backup)
                restored = await wait(result["event_cursor"], "edit.applied")
                await asyncio.sleep(.3)
                state = await call("get_state")
                if state["active"]:
                    await wait(restored["cursor"], "run.finished")
                checks["权重备份写入导出与恢复"] = (directory / "导出模型/config.json").exists()
                checks["恢复后权重哈希与原文件一致"] = original_hashes == {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in model.glob("*.safetensors")}

                await error("set_mode", "FEATURE_DISABLED", mode="step")
                await error("step", "FEATURE_DISABLED", boundary_id="disabled", state_revision=0)
                await error("resume_run", "FEATURE_DISABLED")
                checks["调试相关公开入口明确暂时下线"] = True
                await call("set_mode", mode="observe")
                result = await call("send_message", text="你好", max_tokens=256, temperature=0)
                await wait(result["event_cursor"], "run.started")
                saved = await call("save_project")
                checkpoint = await wait(saved["event_cursor"], "checkpoint.saved")
                checks["运行中安全保存"] = Path(checkpoint["payload"]["path"]).exists()
                # 模式选择仅影响下一条输入，当前运行保留发送时的观测策略。
                await call("set_mode", mode="normal")
                state = await call("get_state")
                checks["运行中切换仅影响下一条消息"] = state["mode"] == "normal" and state["running_mode"] == "observe"
                if state["active"]:
                    result = await call("cancel")
                    await wait(result["event_cursor"], "run.finished")
                checks["生成状态可停止"] = not (await call("get_state"))["active"]
                result = await call("open_project", directory=str(directory / "项目"))
                await wait(result["event_cursor"], "model.ready")
                await wait(result["event_cursor"], "session.activated")
                checks["重开项目恢复会话与用户输出设置"] = (await call("get_state"))["max_tokens"] == empty["max_tokens"]

                result = await call("export_trace", destination=str(directory / "导出记录"))
                await wait(result["event_cursor"], "trace.exported")
                result = await call("open_trace", path=str(directory / "导出记录"))
                await wait(result["event_cursor"], "trace.opened")
                checks["观测记录导出与离线打开"] = (await call("get_state"))["offline"]
                offline_view = await call("query_view", branch_id="base", forward_id="f0", layer_index=0,
                                          operation="embed", neuron_index=0, token_position=0)
                checks["离线打开后仍可读取真实观测值"] = offline_view["status"] == "available"
                await call("get_events", after_cursor=0, limit=2)
                result = await call("load_model", path=str(model), device="cpu")
                await wait(result["event_cursor"], "model.ready")
                await wait(result["event_cursor"], "session.activated")
                checks["离线后重新加载模型"] = (await call("get_state"))["loaded"]
                checks["全部公开工具实际调用"] = len(called) == 32
                completed = True
    finally:
        # 只清理本测试启动的进程树，实际客户端和另一会话的运行器不会受到影响。
        if process.poll() is None:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
            else:
                process.terminate()
            process.wait(timeout=15)
        log.close()
        report = {"pass": completed and bool(checks) and all(checks.values()) and len(called) == 32,
                  "checks": checks, "called_tools": sorted(called)}
        (directory / "MCP验证.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), "utf-8")
    assert report["pass"], report
    return report


def main():
    """显式指定测试程序、隔离目录和测试模型。"""
    parser = argparse.ArgumentParser(description="模型观测台 MCP 完整联调")
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    args = parser.parse_args()
    report = asyncio.run(verify(args.executable.resolve(), args.directory.resolve(), args.model.resolve()))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
