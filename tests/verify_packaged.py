"""对最终 EXE 的真实推理、参数导出重载和 UI 启动进行验证。"""
from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

import torch

from observatory.model import inspect_model
from observatory.protocol import command
from observatory.traces import TraceStore


def main():
    """通过最终运行器管道完成真实 3B 权重编辑与重载对照。"""
    root = Path(__file__).resolve().parent.parent
    output = root / "artifacts"
    result = {"tests": {}}
    hybrid = "--qwen35" in sys.argv
    package = root / "dist/ModelObservatory"
    result["runtime_sha256"] = hashlib.sha256((package / "ModelObservatory-Runtime.exe").read_bytes()).hexdigest()
    result["build_version"] = json.loads((package / "build-manifest.json").read_text(encoding="utf-8"))["version"]
    messages, logs = queue.Queue(), (output / "packaged-runtime.log").open("w", encoding="utf-8")
    environment = os.environ | {"PYTHONUTF8": "1", "MODEL_OBSERVATORY_TEST_LOG": str(output / "packaged-error.log")}
    process = subprocess.Popen([str(package / "ModelObservatory-Runtime.exe"), "--worker"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=logs, text=True, encoding="utf-8", env=environment)
    def read():
        """把完整协议帧送入等待队列，保持 stdout 不阻塞。"""
        for line in process.stdout:
            try:
                messages.put(json.loads(line))
            except ValueError:
                print("INVALID_FRAME", line[:200], flush=True)
    threading.Thread(target=read, daemon=True).start()
    def send(name, payload=None):
        """发送版本化请求。"""
        process.stdin.write(json.dumps(command(name, payload), ensure_ascii=False) + "\n")
        process.stdin.flush()
    def wait(name, timeout=120):
        """等待具体事件，任何运行器错误立即失败。"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                value = messages.get(timeout=min(5, max(.01, deadline - time.monotonic())))
            except queue.Empty:
                if process.poll() is not None:
                    raise RuntimeError("打包运行器提前退出，代码 " + str(process.returncode))
                continue
            if value["kind"] == "error":
                raise RuntimeError(value["payload"])
            if value["name"] == name:
                print("PACKAGED", name, flush=True)
                return value
        raise TimeoutError(name)
    def run():
        """执行固定短输入，捕获真实 logits 供重载对照。"""
        send("run.start", {"messages": [{"role": "user", "content": "只回答一个词：中国的首都是哪里？"}],
            "mode": "observe", "generation": {"max_new_tokens": 1, "seed": 42, "temperature": 0, "template_options": {"enable_thinking": False}},
            "capture": {"policy": "standard", "disk_budget_bytes": 4 * 1024**3}, "trace_root": str(output / "packaged-traces")})
        value = wait("run.finished")
        assert value["payload"]["outcome"] == "completed"
        return value
    try:
        wait("runtime.hello", 45)
        source = "D:/models/Qwen3.5-4B" if hybrid else "D:/models/models/Qwen__Qwen2.5-3B-Instruct"
        send("model.load", {"path": source, "device": "cuda:0", "dtype": "auto"})
        ready = wait("model.ready")["payload"]
        result["tests"]["packaged_load"] = {"pass": True, "model": ready["name"], "dtype": ready["dtype"]}
        previous_hybrid = output / "4B打包实机验证.json"
        if not hybrid and previous_hybrid.exists():
            previous = json.loads(previous_hybrid.read_text(encoding="utf-8"))["tests"]["packaged_neuron_weight_edit"]
            send("trace.open", {"path": previous["trace"]})
            wait("trace.opened")
            send("view.query", {"branch_id": previous["patch"]["branch_id"], "forward_id": "f0",
                "layer_index": 13, "token_position": 0, "neuron_index": 9215, "head": 31,
                "dimension": 2559, "operation": "linear_delta", "lens": True})
            historical = wait("view.snapshot")["payload"]
            assert historical["attention"]["type"] == "linear"
            assert "weight" not in historical and "lens" not in historical
            result["tests"]["cross_model_offline_trace"] = {"pass": True, "loaded_model": ready["name"], "trace_model": "Qwen3.5-4B"}
        initial = run()
        if hybrid:
            selection = {"branch_id": "base", "forward_id": "f0", "layer_index": 13,
                "token_position": 0, "neuron_index": 9215, "head": 31,
                "dimension": 2559, "weight_kind": "down", "operation": "linear_delta", "lens": True}
            send("view.query", selection)
            view = wait("view.snapshot")["payload"]
            assert view["attention"]["type"] == "linear" and view["attention"]["shape"] == [1, 32, 128, 128]
            assert view["neuron"] and view["lens"]["candidates"]
            send("tensor.slice", {"tensor_ref": view["refs"]["recurrent_state"], "ranges": [
                {"start": start, "stop": stop, "step": 1} for start, stop in ((0, 1), (31, 32), (0, 4), (0, 4))]})
            sliced = wait("tensor.slice.result")["payload"]
            assert sliced["shape"] == [1, 1, 4, 4]
            send("scalar.query", selection | {"operation": "linear_qkv", "row": 0, "column": 0})
            scalar = wait("scalar.result")["payload"]
            assert scalar["terms"] == 2560
            result["tests"]["hybrid_views"] = {"pass": True, "state_shape": view["attention"]["shape"], "slice_shape": sliced["shape"], "scalar": scalar}
        send("patch.apply", {"layer_index": 13, "neuron_index": ready["config"]["intermediate_size"] - 1, "dimension": ready["config"]["hidden_size"] - 1, "token_position": 0,
                             "kind": "gate", "value": .125, "expected_weights_revision": "w0"})
        patch = wait("patch.applied")["payload"]
        edited = wait("run.finished")
        assert patch["rounded_value"] == .125
        trace = TraceStore(edited["payload"]["path"], readonly=True)
        _, refs = trace.get_operation(patch["branch_id"], "f0", -1, "logits")
        expected = trace.load(refs["output"]).clone()
        trace.close()
        destination = output / ("真实导出模型-" + time.strftime("%H%M%S"))
        send("model.export", {"destination": str(destination)})
        wait("model.exported", 180)
        send("model.unload")
        wait("model.unloaded")
        send("model.load", {"path": str(destination), "device": "cuda:0", "dtype": "auto"})
        wait("model.ready", 120)
        reloaded = run()
        trace = TraceStore(reloaded["payload"]["path"], readonly=True)
        _, refs = trace.get_operation("base", "f0", -1, "logits")
        actual = trace.load(refs["output"])
        torch.testing.assert_close(expected, actual, atol=0, rtol=0)
        trace.close()
        assert inspect_model(source, hash_weights=True)["hashes"] == ready["hashes"]
        result["tests"]["real_export_reload"] = {"pass": True, "destination": str(destination), "logits_max_abs_error": float((expected.float() - actual.float()).abs().max()), "source_hashes_unchanged": True}
        result["tests"]["packaged_neuron_weight_edit"] = {"pass": True, "patch": patch, "trace": edited["payload"]["path"]}
        if hybrid:
            switches = []
            for path in ("D:/models/models/Qwen__Qwen2.5-3B-Instruct", source):
                send("model.load", {"path": path, "device": "cuda:0", "dtype": "auto"})
                info = wait("model.ready")["payload"]
                switches.append({"name": info["name"], "layers": info["config"]["num_hidden_layers"], "outcome": run()["payload"]["outcome"]})
            result["tests"]["same_process_model_switch"] = {"pass": True, "switches": switches}
            send("run.start", {"messages": [{"role": "user", "content": "你好"}], "mode": "step",
                "generation": {"max_new_tokens": 1, "temperature": 0},
                "capture": {"policy": "standard", "disk_budget_bytes": 512 * 1024**2},
                "trace_root": str(output / "packaged-traces")})
            boundary = wait("step.waiting")["payload"]
            for _ in range(20):
                if boundary["operation_id"] == "linear_delta":
                    break
                send("step.release", {"expected_boundary_id": boundary["boundary_id"], "expected_state_revision": boundary["state_revision"]})
                boundary = wait("step.waiting")["payload"]
            assert boundary["operation_id"] == "linear_delta"
            send("step.release", {"expected_boundary_id": boundary["boundary_id"], "expected_state_revision": boundary["state_revision"]})
            next_boundary = wait("step.waiting")["payload"]
            assert next_boundary["operation_id"] == "linear_norm" and next_boundary["executed"] == boundary["executed"] + 1
            send("run.cancel")
            assert wait("run.finished")["payload"]["outcome"] == "cancelled"
            result["tests"]["hybrid_step"] = {"pass": True, "before": boundary, "after": next_boundary}
        send("shutdown")
        process.wait(15)
        screenshot = output / "最终EXE界面.png"
        desktop = subprocess.run([str(package / "ModelObservatory.exe"), "--screenshot", str(screenshot)], env=environment, timeout=30)
        assert desktop.returncode == 0 and screenshot.exists()
        result["tests"]["packaged_desktop"] = {"pass": True, "screenshot": str(screenshot), "exit_code": desktop.returncode}
        result["pass"] = True
    except Exception as exc:
        result["pass"] = False
        result["error"] = str(exc)
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(10)
        logs.close()
        (output / ("4B打包实机验证.json" if hybrid else "打包实机验证.json")).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PACKAGED_ALL_PASS", flush=True)


if __name__ == "__main__":
    main()
