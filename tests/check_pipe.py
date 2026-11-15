"""检查运行器真实管道请求与响应，避免仅验证进程内接口。"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading

from observatory.protocol import command


def main():
    """经 stdin/stdout 发送检查模型命令并等待关联响应。"""
    root = Path(__file__).resolve().parent.parent
    environment = os.environ | {"PYTHONUTF8": "1", "MODEL_OBSERVATORY_DIAGNOSTIC": "1"}
    process = subprocess.Popen([sys.executable, "-u", str(root / "main.py"), "--worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", env=environment)
    messages = queue.Queue()
    def read():
        """在读取线程等待，主线程保留超时控制。"""
        for line in process.stdout:
            messages.put(json.loads(line))
    threading.Thread(target=read, daemon=True).start()
    try:
        print(messages.get(timeout=10)["name"], flush=True)
        process.stdin.write(json.dumps(command("model.inspect", {"path": "D:/models/models/Qwen__Qwen2.5-3B-Instruct"})) + "\n")
        process.stdin.flush()
        value = messages.get(timeout=15)
        print(value["name"], value["payload"].get("name"), flush=True)
    finally:
        process.terminate()
        process.wait(5)
        print(process.stderr.read(), flush=True)


if __name__ == "__main__":
    main()
