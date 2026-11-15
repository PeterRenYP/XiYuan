"""对同一真实模型交替测量独立监控的吞吐、内存与 CPU 开销。"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import psutil
import torch

from observatory.anomaly_sampling import AnomalySampler
from observatory.model import QwenAdapter


def main():
    """保持模型、输入、执行顺序及解码方式一致，仅改变是否启用监控。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--steps", type=int, default=64)
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--output", default=str(ROOT / "artifacts/anomaly-monitor/性能测量.json"))
    args = parser.parse_args()
    environment = os.environ.copy()
    environment["PATH"] = str(ROOT / ".cache/qt/6.8.3/mingw_64/bin") + os.pathsep + str(ROOT / ".cache/qt/Tools/mingw1310_64/bin") + os.pathsep + environment.get("PATH", "")
    output = Path(args.output); output.parent.mkdir(parents=True, exist_ok=True)
    event_path = output.with_suffix(".events.jsonl")
    # 监控结果直写测试日志，测量进程不创建 Python 读取或资源采样线程，避免引入额外锁竞争。
    with event_path.open("wb", buffering=0) as events:
        process = subprocess.Popen([str(ROOT / "work/client-cpp/Xiyuan-Monitor.exe")], stdout=events,
            stderr=subprocess.PIPE, env=environment, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    endpoint = None
    deadline = time.monotonic() + 5
    while endpoint is None and time.monotonic() < deadline:
        with event_path.open("rb") as events:
            line = events.readline()
        if line.endswith(b"\n"):
            endpoint = json.loads(line)["payload"]
        else:
            time.sleep(.02)
    if endpoint is None:
        process.terminate(); raise RuntimeError("独立监控未就绪")
    print("正在加载真实模型", flush=True)
    try:
        adapter = QwenAdapter.load(args.model, "cuda:0", "auto")
        ids = adapter.tokenizer.encode("请逐步解释为什么天空看起来是蓝色的，并比较日出和正午的差别。", add_special_tokens=False)
        prompt = torch.tensor([ids], device=adapter.device)
        results = []
        monitor_process = psutil.Process(process.pid)
        cpu_start = monitor_process.cpu_times()
        measured_start = time.monotonic()
        peak_memory = monitor_process.memory_info().rss

        def run(enabled, index, steps):
            """两种条件均执行相同的前向与贪心选词，不因结束标记提前截断测量。"""
            adapter.reset_cache()
            sampler = AnomalySampler(endpoint if enabled else {}, {"run_id": f"bench-{index}", "branch_id": "base",
                "profile": "benchmark", "layers": adapter.config.num_hidden_layers, "input_tokens": len(ids),
                "max_tokens": steps, "thinking": False})
            selected = []
            value = prompt
            torch.cuda.synchronize()
            begin = time.perf_counter()
            with torch.inference_mode():
                for step in range(steps):
                    positions = list(range(len(ids))) if step == 0 else [len(ids) + step - 1]
                    def execute(layer, operation, function, inputs):
                        """与产品的轻量采样边界一致，不额外做模型前向。"""
                        sampler.progress(step, layer, operation)
                        result = function()
                        sampler.capture(layer, operation, result,
                                        {"generation_step": step}, positions)
                        return result
                    logits = adapter.forward(value, positions, execute)
                    chosen = int(logits[0, -1].argmax())
                    selected.append(chosen)
                    sampler.token(chosen, "answer", adapter.tokenizer.decode([chosen]), True)
                    value = torch.tensor([[chosen]], device=adapter.device)
            torch.cuda.synchronize()
            elapsed = time.perf_counter() - begin
            sampler.finish("completed", "length", True)
            return {"enabled": enabled, "seconds": elapsed, "tokens_per_second": steps / elapsed,
                    "sampled": sampler.sampled, "dropped": sampler.dropped, "interval": sampler.interval,
                    "capture_enqueue_seconds": sampler.overhead, "tokens": selected}

        # 先执行两种模式预热，避免首轮 CUDA 初始化造成错误结论。
        run(False, "warm-off", 16); run(True, "warm-on", 16)
        print("开始交替测量监控关闭与开启", flush=True)
        for pair in range(args.pairs):
            order = (False, True) if pair % 2 == 0 else (True, False)
            for enabled in order:
                result = run(enabled, f"{pair}-{enabled}", args.steps)
                results.append(result)
                print(json.dumps({k: v for k, v in result.items() if k != "tokens"}, ensure_ascii=False), flush=True)
        memory = monitor_process.memory_info()
        peak_memory = max(peak_memory, getattr(memory, "peak_wset", memory.rss))
        cpu_end = monitor_process.cpu_times()
        cpu_seconds = cpu_end.user + cpu_end.system - cpu_start.user - cpu_start.system
        off = statistics.median(r["tokens_per_second"] for r in results if not r["enabled"])
        on = statistics.median(r["tokens_per_second"] for r in results if r["enabled"])
        same = all(results[i]["tokens"] == results[0]["tokens"] for i in range(len(results)))
        summary = {"model": str(Path(args.model)), "device": torch.cuda.get_device_name(), "steps": args.steps,
            "pairs": args.pairs, "without_monitor_tokens_per_second": off, "with_monitor_tokens_per_second": on,
            "throughput_drop_percent": (1 - on / off) * 100, "identical_output_tokens": same,
            "monitor_peak_rss_mib": peak_memory / 1024**2,
            "monitor_one_core_cpu_percent": cpu_seconds / (time.monotonic() - measured_start) * 100,
            "scope": "真实模型前向与逐Token解码；不包含原有界面、观测写盘和模型加载", "runs": results}
        output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({k: v for k, v in summary.items() if k != "runs"}, ensure_ascii=False), flush=True)
    finally:
        process.terminate(); process.wait(timeout=5)


if __name__ == "__main__":
    main()
