"""真实进程通信、采样边界与生成故障隔离回归。"""
import io
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import threading
import time

import pytest
import torch

from observatory.anomaly_sampling import AnomalySampler, baseline_identity
from observatory.model import QwenAdapter, inspect_model
from observatory.protocol import Emitter, command
from observatory.worker import Runtime
from test_runtime import create_local_model


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def monitor():
    """启动实际 C++ 检测进程，通过本地网络接收生成侧真实数据。"""
    executable = ROOT / "work/client-cpp/Xiyuan-Monitor.exe"
    if not executable.exists():
        pytest.skip("需要先构建原生异常监控进程")
    environment = os.environ.copy()
    environment["PATH"] = str(ROOT / ".cache/qt/6.8.3/mingw_64/bin") + os.pathsep + str(ROOT / ".cache/qt/Tools/mingw1310_64/bin") + os.pathsep + environment.get("PATH", "")
    process = subprocess.Popen([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=environment, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    messages = queue.Queue()
    def read():
        """单独读取输出，测试线程不在无输出时阻塞。"""
        for line in process.stdout:
            messages.put(json.loads(line))
    threading.Thread(target=read, daemon=True).start()
    ready = messages.get(timeout=5)
    assert ready["name"] == "monitor.ready"
    try:
        yield process, ready["payload"], messages
    finally:
        process.terminate()
        process.wait(timeout=5)


def wait_message(messages, name, timeout=5):
    """等待实际检测结果，accepted 或连接成功不能代替异常结果。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = messages.get(timeout=max(.01, deadline - time.monotonic()))
        if message["name"] == name:
            return message
    raise AssertionError(name)


def test_real_sampler_to_native_detector_and_ignore(monitor, tmp_path):
    """连续重复只报告一次，忽略后的新一轮异常不再向用户重复提示。"""
    process, endpoint, messages = monitor
    sampler = AnomalySampler(endpoint, {"run_id": "r1", "branch_id": "base", "profile": "test",
        "layers": 2, "max_tokens": 10000, "project_root": str(tmp_path)})
    wait_message(messages, "anomaly.started")
    for step in range(40):
        sampler.progress(step, 0, "sample")
        sampler.token(7, "answer", "重复", True)
        time.sleep(.003)
    result = wait_message(messages, "anomaly.detected")
    assert result["payload"]["type"] == "repetition"
    assert process.poll() is None
    sampler.finish("completed", "length", True)
    result = wait_message(messages, "anomaly.finished")
    assert result["run_id"] == "r1"


def test_real_model_output_unchanged_with_monitor(monitor, tmp_path):
    """真实小模型接入监控前后输出Token一致，正常对话模式也产生内部采样。"""
    _, endpoint, messages = monitor
    source = tmp_path / "模型"
    model, tokenizer = create_local_model(source, layers=2)
    runtime = Runtime(Emitter("test", io.StringIO()))
    runtime.adapter = QwenAdapter(model, tokenizer, inspect_model(source))
    def run(endpoint, name):
        """相同输入与种子执行独立运行，关闭全量观测以验证轻量采样独立性。"""
        runtime.emitter.stream = io.StringIO()
        runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}], "mode": "normal",
            "generation": {"temperature": 0, "max_new_tokens": 32, "seed": 42},
            "trace_root": str(tmp_path / name), "anomaly_monitor": endpoint}))
        events = [json.loads(line) for line in runtime.emitter.stream.getvalue().splitlines()]
        return [event["payload"]["id"] for event in events if event["name"] == "token.sampled"]
    try:
        before = run({}, "无监控")
        after = run(endpoint, "有监控")
        assert before == after and before
        result = wait_message(messages, "anomaly.finished")
        assert result["payload"]["tensor_samples"] > 0
        assert runtime.anomaly.sampled > 0
    finally:
        runtime.handle(command("shutdown"))
        runtime.thread.join(5)
        if runtime.store:
            runtime.store.close()


def test_unavailable_monitor_never_blocks_or_changes_tensors(monkeypatch):
    """连接失败立即降级，原始张量数值与返回对象均保持不变。"""
    class UnavailableSocket:
        """模拟独立监控进程已退出，非阻塞连接立即失败。"""
        def setblocking(self, value):
            """测试替身不创建系统句柄。"""
        def connect_ex(self, address):
            """模拟连接失败。"""
            raise OSError("监控不可用")
        def close(self):
            """失败后允许安全关闭。"""
    monkeypatch.setattr(socket, "socket", lambda *args: UnavailableSocket())
    sampler = AnomalySampler({"port": 1, "secret": "test"}, {"run_id": "r", "branch_id": "base", "layers": 1})
    deadline = time.monotonic() + 1
    while sampler.enabled and time.monotonic() < deadline:
        time.sleep(.005)
    assert not sampler.enabled
    value = torch.randn(1, 2, 128)
    original = value.clone()
    sampler.progress(0, 0, "mlp")
    sampler.capture(0, "mlp", value, {"generation_step": 0}, [0, 1])
    torch.testing.assert_close(value, original, rtol=0, atol=0)


def test_queue_is_bounded_under_backpressure(monkeypatch):
    """网络不可写期间，生产者继续运行，缓存不会随Token数量增长。"""
    class BusySocket:
        """模拟永久拥堵的本地连接。"""
        def setblocking(self, value):
            """保持非阻塞语义。"""
        def connect_ex(self, address):
            """连接成功后所有发送都不可写。"""
            return 0
        def send(self, data):
            """不可写时立即返回控制权。"""
            raise BlockingIOError()
        def close(self):
            """结束时不等待写出。"""
    monkeypatch.setattr(socket, "socket", lambda *args: BusySocket())
    sampler = AnomalySampler({"port": 1, "secret": "test"}, {"run_id": "r", "branch_id": "base", "layers": 1})
    started = time.monotonic()
    for step in range(10000):
        sampler.progress(step, 0, "sample")
        sampler.token(step, "answer", "字" * 4096, True)
    elapsed = time.monotonic() - started
    assert len(sampler.wire) <= 65536 and sampler.dropped > 0
    assert elapsed < 2
    sampler.finish("completed", "length", True)


def test_profile_separates_weights_rules_and_sampling(tmp_path):
    """模型参数、采样或干预改变后不得继续使用旧基线。"""
    model, tokenizer = create_local_model(tmp_path / "模型")
    adapter = QwenAdapter(model, tokenizer, inspect_model(tmp_path / "模型"))
    base = baseline_identity(adapter, {"temperature": .7, "seed": 1}, [], "normal", "none")
    assert base == baseline_identity(adapter, {"temperature": .7, "seed": 2}, [], "normal", "none")
    assert base != baseline_identity(adapter, {"temperature": 1.}, [], "normal", "none")
    adapter.weights_revision = "w1"
    assert base != baseline_identity(adapter, {"temperature": .7}, [], "normal", "none")


@pytest.mark.skipif(not torch.cuda.is_available(), reason="需要可用 CUDA 设备")
def test_cuda_snapshot_is_async_and_bounded(monitor):
    """验证真实 CUDA 异步拷贝的生命周期与固定快照上限。"""
    _, endpoint, messages = monitor
    sampler = AnomalySampler(endpoint, {"run_id": "gpu", "branch_id": "base", "profile": "gpu", "layers": 1})
    wait_message(messages, "anomaly.started")
    value = torch.randn(1, 1, 4096, device="cuda")
    with torch.inference_mode():
        for step in range(0, 80, 8):
            sampler.progress(step, 0, "mlp")
            sampler.capture(0, "mlp", value, {"generation_step": step}, [step])
            assert len(sampler.pending) <= 4
    sampler.finish("completed", "length", True)
    wait_message(messages, "anomaly.finished")
