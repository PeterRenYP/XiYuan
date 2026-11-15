"""项目配额、会话隔离和真实模型暂停现场恢复回归。"""
import io
import json
import time
from collections import namedtuple

import pytest
import torch

from observatory.model import QwenAdapter
from observatory.protocol import Emitter, ProtocolError, command
from observatory.storage import MIB, ProjectBudget, atomic_json, directory_bytes
from observatory.traces import TraceStore
from observatory.worker import Runtime
from test_runtime import create_local_model


def make_project(root, **storage):
    """建立独立测试项目，容量保持很小且不写入用户模型。"""
    atomic_json(root / "project.json", {"storage": {
        "total_mib": 64, "run_mib": 16, "reserve_mib": 256,
        "auto_clean": True, **storage}})
    return root / "sessions" / ("a" * 32) / "records"


def test_quota_preserves_pinned_sessions_and_unknown_files(tmp_path):
    """超过旧项目额度不清理缓存，也不影响会话、保留现场或其他文件。"""
    records = make_project(tmp_path)
    for name, pinned, managed, status in (("cache", False, "observatory-project", "completed"),
            ("saved", True, "observatory-project", "completed"),
            ("foreign", False, "other", "completed"),
            ("active", False, "observatory-project", "running")):
        target = records / name
        atomic_json(target / "manifest.json", dict(pinned=pinned, managed_by=managed, status=status))
        with (target / "block").open("wb") as handle:
            handle.truncate(13 * MIB)
    atomic_json(records.parent / "state.json", {"messages": ["保留上下文"]})
    budget = ProjectBudget(tmp_path, records)
    assert (records / "cache").exists()
    assert all((records / name).is_dir() for name in ("saved", "foreign", "active"))
    assert json.loads((records.parent / "state.json").read_text("utf-8"))["messages"]
    assert budget.used == directory_bytes(tmp_path)
    # 第二轮仍统计第一轮的文件，不会重获一份总容量。
    with (records / "saved" / "block2").open("wb") as handle:
        handle.truncate(18 * MIB)
    restored=ProjectBudget(tmp_path, records)
    assert restored.limit==0 and restored.per_run==0
    assert (records / "cache").exists()


def test_project_path_escape_and_low_disk_stop_capture(tmp_path, monkeypatch):
    """拒绝项目外路径；磁盘保留空间不足时停止采集而不抛出生成异常。"""
    records = make_project(tmp_path)
    with pytest.raises(ProtocolError):
        ProjectBudget(tmp_path, tmp_path.parent / "outside")
    budget = ProjectBudget(tmp_path, records)
    store = TraceStore(records / "run", project_budget=budget, budget=16 * MIB)
    usage = namedtuple("Usage", "total used free")(1024 * MIB, 770 * MIB, 254 * MIB)
    monkeypatch.setattr("shutil.disk_usage", lambda path: usage)
    assert store.tensor(torch.ones(32)) is None
    assert store.incomplete and "空间" in store.degraded_reason
    assert not list((store.root / "tensors").iterdir())
    store.close()


def wait_for(predicate, timeout=15):
    """有界等待后台真实运算，失败时让测试报告具体断言。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.005)
    raise AssertionError("等待真实计算边界超时")


def test_low_space_during_generation_keeps_tokens_and_reports_reason(tmp_path, monkeypatch):
    """采集途中空间不足，仍正常输出Token并发送明确的降级原因。"""
    source = tmp_path / "model"
    create_local_model(source)
    root = tmp_path / "project"
    records = make_project(root)
    stream = io.StringIO()
    runtime = Runtime(Emitter("low-disk-test", stream))
    original = TraceStore.tensor
    count = 0

    def capture(store, value):
        """在第三次真实张量采集时模拟空闲空间降到保护线以下。"""
        nonlocal count
        count += 1
        if count == 3:
            usage = namedtuple("Usage", "total used free")(1024 * MIB, 770 * MIB, 254 * MIB)
            monkeypatch.setattr("shutil.disk_usage", lambda path: usage)
        return original(store, value)

    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        monkeypatch.setattr(TraceStore, "tensor", capture)
        runtime.run(command("run.start", dict(messages=[dict(role="user", content="你好")],
            mode="observe", generation=dict(max_new_tokens=2, temperature=0, seed=42),
            project_root=str(root), trace_root=str(records))))
        assert len(frames(stream, "token.sampled")) == 2
        assert frames(stream, "run.finished")[-1]["payload"]["outcome"] == "completed"
        assert "空间" in frames(stream, "capture.degraded")[0]["payload"]["message"]
        assert directory_bytes(root) < 64 * MIB
    finally:
        stop(runtime)


def frames(stream, name):
    """从真实协议中读取指定事件。"""
    return [value for line in stream.getvalue().splitlines()
            if (value := json.loads(line))["name"] == name]


def stop(runtime):
    """无论断言是否成功，都释放后台线程和数据库。"""
    runtime.handle(command("shutdown"))
    runtime.thread.join(10)
    assert not runtime.thread.is_alive()


def test_checkpoint_rebuild_matches_uninterrupted_and_isolates_rules(tmp_path):
    """已采样后保存并重建，回到同一边界；后续随机采样与不中断一致。"""
    source = tmp_path / "model"
    create_local_model(source, layers=2)
    root = tmp_path / "project"
    records = make_project(root)
    stream = io.StringIO()
    runtime = Runtime(Emitter("checkpoint-test", stream))
    rules = [dict(layer_index=0, neuron_index=3, kind="zero", scope="all", value=0)]
    request = dict(messages=[dict(role="user", content="你好")], mode="observe",
        generation=dict(max_new_tokens=4, temperature=.7, seed=42),
        project_root=str(root), trace_root=str(records), capture=dict(policy="standard", layer=1, disk_budget_bytes=16 * MIB))
    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        runtime.activate_session(command("session.activate", dict(temporary_rules=rules, weights_revision="w3")))
        runtime.run(command("run.start", request))
        baseline = [frame["payload"]["id"] for frame in frames(stream, "token.sampled")]
        assert len(baseline) >= 2
        runtime.handle(command("run.start", request | {"mode": "step"}))
        wait_for(lambda: runtime.controller and runtime.controller.waiting and frames(runtime.emitter.stream, "step.waiting") and frames(runtime.emitter.stream, "step.waiting")[-1]["payload"]["state_revision"] == runtime.controller.revision)
        # 先执行完整预填充和首次采样，再执行下一轮一个操作。
        while len(frames(stream, "token.sampled")) == len(baseline):
            controller = runtime.controller
            revision = controller.revision
            controller.release(str(revision), controller.boundary_id, revision)
            wait_for(lambda: controller.waiting and controller.revision > revision and frames(runtime.emitter.stream, "step.waiting")[-1]["payload"]["state_revision"] == controller.revision)
        controller = runtime.controller
        revision = controller.revision
        controller.release(str(revision), controller.boundary_id, revision)
        wait_for(lambda: controller.waiting and controller.revision > revision and frames(runtime.emitter.stream, "step.waiting")[-1]["payload"]["state_revision"] == controller.revision)
        executed = controller.executed
        boundary = frames(stream, "step.waiting")[-1]["payload"]
        checkpoint = records.parent / "checkpoint.json"
        runtime.handle(command("run.checkpoint", dict(project_root=str(root), path=str(checkpoint))))
        assert checkpoint.is_file() and checkpoint.stat().st_size < 10000
        assert json.loads(checkpoint.read_text("utf-8"))["executed"] == executed
        runtime.handle(command("run.cancel"))
        runtime.tasks.join()
        stop(runtime)

        # 模拟真正重启进程：新模型实例、新运行器、新缓存。
        restored_stream = io.StringIO()
        runtime = Runtime(Emitter("restored-test", restored_stream))
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        runtime.handle(command("run.restore", dict(path=str(checkpoint), temporary_rules=rules)))
        wait_for(lambda: runtime.controller and runtime.controller.waiting and frames(runtime.emitter.stream, "step.waiting") and frames(runtime.emitter.stream, "step.waiting")[-1]["payload"]["state_revision"] == runtime.controller.revision)
        restored = frames(restored_stream, "step.waiting")[-1]["payload"]
        assert restored["executed"] == executed
        assert (restored["operation_id"], restored["forward_id"], restored["layer_index"]) == (
            boundary["operation_id"], boundary["forward_id"], boundary["layer_index"])
        assert runtime.adapter.edit_rules[0]["neuron_index"] == 3
        assert runtime.adapter.revision_number == 3
        # 重建的旧观察仍可读取，而不是只有进度文字。
        _, refs = runtime.store.get_operation("base", "f0", 0, "mlp")
        assert torch.count_nonzero(runtime.store.load(refs["output"])[..., 3]) == 0
        controller = runtime.controller
        revision = controller.revision
        controller.release("one", controller.boundary_id, revision)
        wait_for(lambda: controller.waiting and controller.revision > revision and frames(runtime.emitter.stream, "step.waiting")[-1]["payload"]["state_revision"] == controller.revision)
        assert controller.executed == executed + 1
        controller.set_mode("observe")
        runtime.tasks.join()
        continuation = [frame["payload"]["id"] for frame in frames(restored_stream, "token.sampled")]
        assert continuation == baseline[1:]
        assert frames(restored_stream, "run.finished")[-1]["payload"]["text"] == frames(stream, "run.finished")[0]["payload"]["text"]
        runtime.activate_session(command("session.activate", {}))
        assert runtime.adapter.edit_rules and runtime.last_request is None
        runtime.activate_session(command("session.activate", {"temporary_rules": []}))
        assert runtime.adapter.edit_rules == []
        assert not any(json.loads(line)["kind"] == "error" for line in restored_stream.getvalue().splitlines())
    finally:
        if runtime.thread.is_alive():
            stop(runtime)
