"""使用分发版真实运行器验证监控通信、重复告警与故障隔离。"""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / "tests")]

import torch
from observatory.protocol import command
from test_runtime import create_local_model


def main():
    """构造专用真实权重模型，不修改用户模型或现有项目。"""
    destination = ROOT / "artifacts/anomaly-monitor/分发验收"
    destination.mkdir(parents=True, exist_ok=True)
    source = destination / "测试模型"
    model, _ = create_local_model(source, layers=2)
    with torch.no_grad():
        model.lm_head.weight.zero_()
    model.save_pretrained(source)
    processes = []
    def launch(filename):
        """仅启动当前验收拥有的隐藏子进程，持续排空协议输出。"""
        log = (destination / (filename + ".log")).open("wb")
        process = subprocess.Popen([str(ROOT / "dist/ModelObservatoryCpp" / filename)] + (["--worker"] if "Runtime" in filename else []),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        processes.append((process, log))
        messages = queue.Queue()
        def read():
            """输出读取不与界面交互，结果必须来自运行器的最终事件。"""
            for line in process.stdout:
                messages.put(json.loads(line))
        threading.Thread(target=read, daemon=True).start()
        return process, messages
    def wait(messages, name, timeout=40):
        """等待指定最终事件，同时保留中途结果用于检查重复次数与Token序列。"""
        seen = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = messages.get(timeout=max(.01, deadline - time.monotonic()))
            seen.append(value)
            if value["name"] == "request.failed":
                raise AssertionError(value)
            if value["name"] == name:
                return seen
        raise AssertionError(name)
    def send(process, name, payload):
        """向真实运行器发送原有版本化协议。"""
        process.stdin.write((json.dumps(command(name, payload), ensure_ascii=False) + "\n").encode("utf-8"))
        process.stdin.flush()
    try:
        monitor, monitor_messages = launch("Xiyuan-Monitor.exe")
        endpoint = wait(monitor_messages, "monitor.ready")[0]["payload"]
        worker, worker_messages = launch("ModelObservatory-Runtime.exe")
        send(worker, "model.load", {"path": str(source), "device": "cpu", "dtype": "float32"})
        wait(worker_messages, "model.ready")
        payload = {"messages": [{"role": "user", "content": "你好"}], "mode": "normal",
            "generation": {"temperature": 0, "max_new_tokens": 48, "seed": 42},
            "trace_root": str(destination / "记录"), "anomaly_monitor": endpoint}
        send(worker, "run.start", payload)
        first = wait(worker_messages, "run.finished")
        detected = wait(monitor_messages, "anomaly.finished")
        repetition = [event for event in detected if event["name"] == "anomaly.detected" and event["payload"]["type"] == "repetition"]
        assert len(repetition) == 1
        assert detected[-1]["payload"]["tensor_samples"] > 0
        monitor.terminate(); monitor.wait(timeout=5)
        send(worker, "run.start", payload)
        second = wait(worker_messages, "run.finished")
        first_tokens = [event["payload"]["id"] for event in first if event["name"] == "token.sampled"]
        second_tokens = [event["payload"]["id"] for event in second if event["name"] == "token.sampled"]
        report = {"打包运行器加载真实模型": True, "普通对话产生内部采样": detected[-1]["payload"]["tensor_samples"] > 0,
            "同次持续重复只触发一次告警": len(repetition) == 1, "监控退出后生成完成": second[-1]["payload"]["outcome"] == "completed",
            "监控退出前后输出Token一致": first_tokens == second_tokens, "生成Token数量": len(second_tokens)}
        assert all(report[key] for key in report if key != "生成Token数量") and len(second_tokens) == 48
        (destination / "验收结果.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False))
    finally:
        for process, log in processes:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
            log.close()


if __name__ == "__main__":
    main()
