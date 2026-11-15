"""1.0.2 的无限配额、逐 Token 证据、原始记录绑定和字节显示回归。"""
import hashlib
import io
import json
from collections import namedtuple

import pytest
import torch

from observatory.model import QwenAdapter
from observatory.protocol import Emitter, ProtocolError, command
from observatory.storage import MIB, ProjectBudget, atomic_json
from observatory.token_analysis import TokenLayout, analyze_token, sample_evidence
from observatory.traces import TraceStore
from observatory.worker import Runtime
from test_project import frames, make_project, stop, wait_for
from test_runtime import create_local_model


@pytest.mark.parametrize("total,run", [(0, 0), (0, 16), (64, 0)])
def test_unlimited_project_and_run_are_independent(tmp_path, monkeypatch, total, run):
    """旧项目的零额度或正数额度均按无限采集处理。"""
    records = make_project(tmp_path, total_mib=total, run_mib=run, auto_clean=False)
    usage = namedtuple("Usage", "total used free")(1024**5, 0, 1024**5)
    monkeypatch.setattr("shutil.disk_usage", lambda path: usage)
    budget = ProjectBudget(tmp_path, records)
    assert budget.limit == 0 and budget.per_run == 0
    assert budget.can_write(1024**4)


@pytest.mark.parametrize("unlimited", [False, True])
def test_tensor_and_index_obey_selected_budget(tmp_path, unlimited):
    """越过旧总量与索引边界后，旧正数额度也继续持久化。"""
    store = TraceStore(tmp_path, budget=0 if unlimited else 16 * MIB)
    try:
        store.bytes_written = 16 * MIB
        store.metadata_bytes = 8 * MIB
        assert store.allow_metadata(4096)
        ref = store.tensor(torch.arange(32, dtype=torch.float32))
        assert ref
        if ref:
            torch.testing.assert_close(store.load(ref), torch.arange(32, dtype=torch.float32))
        else:
            assert store.incomplete and "上限" in store.degraded_reason
    finally:
        store.close()


def test_unlimited_still_protects_disk_and_memory_queue(tmp_path, monkeypatch):
    """磁盘预留仍生效，超过内存队列预算的块也能独占落盘。"""
    records = make_project(tmp_path, total_mib=0, run_mib=0, auto_clean=False)
    budget = ProjectBudget(tmp_path, records)
    store = TraceStore(records / "run", budget=0, project_budget=budget)
    try:
        store.queue_limit = 16
        ref=store.tensor(torch.ones(100))
        torch.testing.assert_close(store.load(ref),torch.ones(100))
        assert not store.incomplete
        store.queue_limit = 64 * MIB
        usage = namedtuple("Usage", "total used free")(1024 * MIB, 800 * MIB, 224 * MIB)
        monkeypatch.setattr("shutil.disk_usage", lambda path: usage)
        assert store.tensor(torch.ones(4)) is None
        assert "空间" in store.degraded_reason
    finally:
        store.close()


def test_selected_token_outside_top_ten_keeps_exact_probability_and_projection():
    """候选表之外的实际输出仍保留概率、排名和完整输出投影和。"""
    hidden = torch.tensor([2., -1., .5])
    head = torch.nn.Linear(3, 32)
    with torch.no_grad():
        head.weight.copy_(torch.arange(96).reshape(32, 3) / 100)
        head.bias.copy_(torch.arange(32) / 100)
        logits = head(hidden).reshape(1, 1, -1)
        probabilities = logits[0, -1].softmax(-1)
        chosen = 4
        result = sample_evidence(logits, logits[0, -1], probabilities, chosen, hidden, head, [chosen])
    assert result["sampling_rank"] > 10
    assert result["probability"] == pytest.approx(float(probabilities[chosen]))
    assert result["raw_probability"] == pytest.approx(float(probabilities[chosen]))
    assert result["projection"]["sum"] == pytest.approx(float(logits[0, -1, chosen]), abs=1e-6)
    assert result["already_generated"] and result["allowed_candidates"] == 32
    json.dumps(result, allow_nan=False)


def test_byte_split_keeps_all_token_identities_and_utf16_offsets():
    """中文字节续片分别可定位，补充平面汉字按 Qt 的 UTF-16 位置计数。"""
    layout = TokenLayout()
    first = layout.update(0, "answer", "\ufffd", "\ufffd")
    assert first[0]["display_piece"] == "字节片段"
    updates = layout.update(1, "answer", "你", "\ufffd")
    assert [row["index"] for row in updates] == [0, 1]
    assert [row["display_piece"] for row in updates] == ["你", "续片"]
    assert all(row["shared_tokens"] == [0, 1] and row["text_end"] == 1 for row in updates)
    next_piece = layout.update(2, "answer", "你𠀀", "𠀀")[0]
    assert (next_piece["text_start"], next_piece["text_end"]) == (1, 3)
    assert layout.update(3, "control", "", "") == []
    thought = layout.update(4, "reasoning", "思考", "思考")[0]
    assert thought["text_start"] == 0 and thought["text_end"] == 2


def test_replay_advances_layout_without_assigning_history_to_new_token():
    """恢复前的重放也推进偏移，之后的新 Token 仅对应新增片段。"""
    layout = TokenLayout()
    layout.update(0, "answer", "第一段", "第一段")
    update = layout.update(1, "answer", "第一段 新增", "新增")[0]
    assert update["display_piece"] == " 新增" and update["text_start"] == 3


def model_digest(adapter):
    """逐参数计算哈希，检查只读分析不会改写任何模型值。"""
    digest = hashlib.sha256()
    for parameter in adapter.model.parameters():
        digest.update(parameter.detach().cpu().contiguous().numpy().tobytes())
    return digest.hexdigest()


@pytest.mark.parametrize("mode", ["normal", "observe"])
def test_real_generation_analysis_does_not_forward_switch_or_mutate(tmp_path, mode):
    """真实小模型生成后，从读取线程分析对应 Token，前向次数和模型权重保持不变。"""
    source = tmp_path / "model"
    create_local_model(source)
    project = tmp_path / "project"
    records = make_project(project, total_mib=0, run_mib=0, auto_clean=False)
    stream = io.StringIO()
    runtime = Runtime(Emitter("token-analysis-test", stream))
    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}], "mode": mode,
            "generation": {"max_new_tokens": 3, "temperature": 0, "seed": 42},
            "project_root": str(project), "trace_root": str(records), "capture": {"policy": "full", "disk_budget_bytes": 0}}))
        sampled = frames(stream, "token.sampled")[0]
        token = sampled["payload"] | {"run_id": sampled["run_id"], "branch_id": sampled["branch_id"]}
        path, run_id, forward_count = runtime.store.root, runtime.run_id, runtime.forward_count
        before = model_digest(runtime.adapter)
        payload = {"token": token, "project_root": str(project), "trace_path": str(path)}
        runtime.handle(command("token.analyze", payload))
        report = wait_for(lambda: frames(stream, "token.analysis"))[0]["payload"]
        assert report["token"]["id"] == token["id"] and report["evidence"] == "原始生成记录"
        assert report["token"]["projection"]["sum"] == pytest.approx(token["raw_logit"], abs=1e-5)
        assert "确定性选择" in report["explanation"] and report["generation"]["temperature"] == 0
        assert bool(report["layers"]) is (mode == "observe")
        assert runtime.forward_count == forward_count and runtime.run_id == run_id and runtime.store.root == path
        assert model_digest(runtime.adapter) == before
        wrong = payload | {"token": token | {"id": token["id"] + 1}}
        with pytest.raises(ProtocolError, match="不匹配"):
            analyze_token(wrong, runtime.store)
        # 切换到另一次真实运行后，旧 token 仍从旧记录读取。
        runtime.run(command("run.start", {"messages": [{"role": "user", "content": "用户"}], "mode": "normal",
            "generation": {"max_new_tokens": 1, "temperature": 0}, "project_root": str(project),
            "trace_root": str(records), "capture": {"disk_budget_bytes": 0}}))
        current_path, current_run = runtime.store.root, runtime.run_id
        old = analyze_token(payload, runtime.store)
        assert old["token"]["run_id"] == run_id and old["token"]["probability"] == token["probability"]
        assert runtime.store.root == current_path and runtime.run_id == current_run
        assert model_digest(runtime.adapter) == before
    finally:
        stop(runtime)


def test_missing_records_explain_saved_data_without_inventing_internals(tmp_path):
    """清理观测记录后仍能解释保存的概率，内部数据保持缺失状态。"""
    report = analyze_token({"project_root": str(tmp_path),
        "trace_path": str(tmp_path / "sessions" / "a" / "records" / "gone"),
        "token": {"probability": .25, "sampling_rank": 2, "raw_rank": 3, "allowed_candidates": 8},
        "generation": {"temperature": .7}})
    assert "25.0000%" in report["explanation"] and "随机抽取" in report["explanation"]
    assert report["layers"] == [] and report["attention"] == [] and "已清理" in report["notes"][0]
    with pytest.raises(ProtocolError, match="不属于"):
        analyze_token({"project_root": str(tmp_path), "trace_path": str(tmp_path.parent)})
