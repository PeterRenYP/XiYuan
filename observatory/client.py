"""Qt 异步进程客户端，负责 UTF-8 分帧、日志和进程生命周期。"""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import subprocess

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, Signal

from .protocol import MAX_LINE, command


class ProcessClient(QObject):
    """推理和监控各用一个拥有明确归属的 QProcess。"""
    message = Signal(dict)
    failed = Signal(str)
    stopped = Signal(int)

    def __init__(self, mode, data_root, parent=None):
        super().__init__(parent)
        self.mode, self.data_root = mode, Path(data_root)
        self.buffer = bytearray()
        self.process = QProcess(self)
        self.process.readyReadStandardOutput.connect(self.read_output)
        self.process.readyReadStandardError.connect(self.read_log)
        self.process.errorOccurred.connect(lambda err: self.failed.emit("子进程异常：" + self.process.errorString()))
        self.process.finished.connect(lambda code, status: self.stopped.emit(code))

    def start(self):
        """冻结版启动同一 EXE 的工作模式，源码版启动当前解释器。"""
        self.data_root.mkdir(parents=True, exist_ok=True)
        environment = QProcessEnvironment.systemEnvironment()
        environment.insert("PYTHONUTF8", "1")
        environment.insert("PYTHONIOENCODING", "utf-8")
        environment.insert("HF_HUB_OFFLINE", "1")
        environment.insert("TOKENIZERS_PARALLELISM", "false")
        self.process.setProcessEnvironment(environment)
        if getattr(sys, "frozen", False):
            # 控制台运行器提供可靠管道，UI 启动器保持无控制台窗口。
            runtime = Path(sys.executable).with_name("ModelObservatory-Runtime.exe")
            self.process.setProgram(str(runtime) if runtime.exists() else sys.executable)
            self.process.setArguments(["--" + self.mode])
        else:
            self.process.setProgram(sys.executable)
            self.process.setArguments(["-u", str(Path(__file__).resolve().parent.parent / "main.py"), "--" + self.mode])
        self.process.start()

    def send(self, name, payload=None):
        """异步发送一条命令，返回请求编号。"""
        msg = command(name, payload)
        self.process.write((json.dumps(msg, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8"))
        return msg["request_id"]

    def read_output(self):
        """按字节累积完整帧，避免中文被分段读取破坏。"""
        self.buffer.extend(bytes(self.process.readAllStandardOutput()))
        if len(self.buffer) > MAX_LINE and b"\n" not in self.buffer:
            self.failed.emit("子进程通信帧超过上限")
            self.process.kill()
            return
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            if not line.strip():
                continue
            try:
                if len(line) > MAX_LINE:
                    raise ValueError("通信帧超过上限")
                message = json.loads(line.decode("utf-8"))
                if message.get("protocol_version") != "1.0":
                    raise ValueError("协议版本不兼容")
                self.message.emit(message)
            except (ValueError, UnicodeError) as exc:
                self.failed.emit("子进程通信失败：" + str(exc))

    def read_log(self):
        """第三方库日志写入用户目录，不污染协议通道。"""
        data = bytes(self.process.readAllStandardError())
        with (self.data_root / (self.mode + ".log")).open("ab") as handle:
            handle.write(data)

    def stop(self):
        """先取消并协作退出，超时后只终止本客户端拥有的进程。"""
        if self.process.state() == QProcess.NotRunning:
            return
        if self.mode == "worker":
            self.send("shutdown")
            if self.process.waitForFinished(2000):
                return
        self.process.terminate()
        if not self.process.waitForFinished(1000):
            if sys.platform == "win32" and self.process.processId():
                # Windows 虚拟环境启动器可能有自有子进程；仅结束本 QProcess 的进程树。
                subprocess.run(["taskkill", "/PID", str(self.process.processId()), "/T", "/F"],
                               capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW, timeout=5)
            self.process.kill()
            self.process.waitForFinished(1000)
