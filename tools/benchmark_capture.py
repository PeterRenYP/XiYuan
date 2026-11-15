"""用同一微型模型和请求，对照旧分发运行器与新源码运行器的观测耗时。"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from observatory.protocol import command


def measure(program, destination, model):
    """排除进程启动和加载时间，读取完整协议避免管道反压影响结果。"""
    root = Path(__file__).resolve().parents[1]
    destination.mkdir(parents=True, exist_ok=True)
    frames = queue.Queue()
    env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8", HF_HUB_OFFLINE="1")
    with (destination / "运行器日志.txt").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(program, cwd=root, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=log, text=True, encoding="utf-8", creationflags=subprocess.CREATE_NO_WINDOW)

        def read():
            """持续排空事件管道，记录主机接收时间。"""
            for line in process.stdout:
                frames.put((time.perf_counter(), json.loads(line)))

        reader = threading.Thread(target=read, daemon=True)
        reader.start()

        def send(name, payload=None):
            """发送完整协议帧。"""
            process.stdin.write(json.dumps(command(name, payload), ensure_ascii=False) + "\n")
            process.stdin.flush()

        def until(name):
            """等待完成事件并捕获异步错误。"""
            while True:
                timestamp, frame = frames.get(timeout=60)
                assert frame["kind"] != "error", frame
                if frame["name"] == name:
                    return timestamp, frame

        try:
            send("model.load", {"path": str(model), "device": "cpu", "dtype": "float32"})
            until("model.ready")
            results = []
            for index in range(3):
                send("run.start", {"messages": [{"role": "user", "content": "你好"}], "mode": "observe",
                    "generation": {"max_new_tokens": 8, "temperature": 0, "seed": 42,
                        "repetition_penalty": 1., "presence_penalty": 0., "top_p": .8, "top_k": 20},
                    "trace_root": str(destination / "records"), "capture": {"policy": "full", "disk_budget_bytes": 32 * 1024**2}})
                started, _ = until("run.started")
                tokens, operations = [], 0
                while True:
                    timestamp, frame = frames.get(timeout=60)
                    assert frame["kind"] != "error", frame
                    if frame["name"] == "token.sampled":
                        tokens.append(frame["payload"]["id"])
                    if frame["name"] == "operation.completed":
                        operations += 1
                    if frame["name"] == "run.finished":
                        assert frame["payload"]["capture_integrity"] == "complete_for_policy", frame
                        results.append({"seconds": timestamp - started, "tokens": tokens, "operations": operations})
                        break
            return results
        finally:
            send("shutdown")
            process.wait(timeout=15)


def main():
    """只操作隔离模型和记录，不连接用户客户端。"""
    root = Path(__file__).resolve().parents[1]
    directory = root / "artifacts/session-capture-benchmark"
    directory.mkdir(parents=True, exist_ok=True)
    model = root / "artifacts/session-mcp/验证模型"
    old = measure([str(root / "dist/ModelObservatoryCpp/ModelObservatory-Runtime.exe"), "--worker"], directory / "旧版", model)
    new = measure([sys.executable, "-X", "utf8", "runtime_launcher.py", "--worker"], directory / "新版", model)
    result = {"pass": all(a["tokens"] == b["tokens"] and a["operations"] == b["operations"] for a, b in zip(old, new)),
              "old": old, "new": new,
              "old_mean_seconds": sum(item["seconds"] for item in old) / len(old),
              "new_mean_seconds": sum(item["seconds"] for item in new) / len(new)}
    (directory / "观测写入性能.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), "utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    assert result["pass"], result


if __name__ == "__main__":
    main()
