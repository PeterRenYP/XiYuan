"""验证项目共享干预、无重算应用和真实思考Token分流。"""
import io
import json

import pytest
import torch

from observatory.model import QwenAdapter, sample_logits
from observatory.protocol import Emitter, ProtocolError, command
from observatory.worker import Runtime, decode_response, conversation_messages, generation_options
from test_runtime import create_local_model


def events(stream, name):
    """从协议输出读取指定事件，避免以界面状态代替实际运行断言。"""
    return [frame for line in stream.getvalue().splitlines()
            if (frame := json.loads(line))["name"] == name]


def stop(runtime):
    """关闭线程和记录，测试不遗留后台模型进程。"""
    runtime.handle(command("shutdown"))
    runtime.thread.join(5)


def test_thinking_sampling_uses_model_defaults_and_keeps_overrides():
    """思考和普通模式采用各自推荐值，显式零温度等自定义设置仍然有效。"""
    info = {"model_type": "qwen3_5"}
    thinking = generation_options(info, {"template_options": {"enable_thinking": True}})
    assert (thinking["temperature"], thinking["top_p"], thinking["presence_penalty"]) == (1., .95, 1.5)
    normal = generation_options(info, {})
    assert (normal["temperature"], normal["top_p"], normal["presence_penalty"]) == (.7, .8, 1.5)
    custom = generation_options(info, {"temperature": 0, "presence_penalty": 0,
        "template_options": {"enable_thinking": True}})
    assert custom["temperature"] == 0 and custom["presence_penalty"] == 0
    assert generation_options({"model_type": "qwen2"}, {})["presence_penalty"] == 0


def test_output_limit_above_4096_and_remaining_context_are_respected(tmp_path, monkeypatch):
    """实际运行接受更高上限，按完整历史分词后约束剩余容量，并保留原请求额度。"""
    source = tmp_path / "model"
    create_local_model(source)
    stream = io.StringIO()
    runtime = Runtime(Emitter("output-limit", stream))
    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        runtime.adapter.config.max_position_embeddings = 8192

        def eos(logits, history, generation, generator, generated=None):
            """前向仍执行真实计算，仅固定立即正常结束，避免测试生成数千无关Token。"""
            scores = logits[0, -1].clone()
            return 1, scores, torch.softmax(scores, -1)

        monkeypatch.setattr("observatory.worker.sample_logits", eos)
        payload = {"messages": [{"role": "user", "content": "你好"}], "mode": "normal",
                   "trace_root": str(tmp_path / "records"), "generation": {"max_new_tokens": 5000}}
        runtime.run(command("run.start", payload))
        assert events(stream, "run.started")[-1]["payload"]["max_new_tokens"] == 5000
        assert events(stream, "run.finished")[-1]["payload"]["reason"] == "eos"
        runtime.adapter.config.max_position_embeddings = 128
        payload["generation"]["max_new_tokens"] = 127
        runtime.run(command("run.start", payload))
        start = events(stream, "run.started")[-1]["payload"]
        assert start["max_new_tokens"] == 128 - start["input_tokens"]
        assert start["requested_max_new_tokens"] == 127
    finally:
        stop(runtime)


def test_presence_penalty_counts_generated_tokens_once_and_preserves_prompt():
    """存在惩罚与频率惩罚不同：重复出现只减一次，输入中的Token不受影响。"""
    logits = torch.tensor([[[2., 3., 2.5, 1.]]])
    chosen, scores, _ = sample_logits(logits, [0, 0, 1, 1],
        {"temperature": 0, "repetition_penalty": 1., "presence_penalty": 1.5},
        torch.Generator().manual_seed(42), [1, 1])
    assert chosen == 2
    torch.testing.assert_close(scores, torch.tensor([2., 1.5, 2.5, 1.]))
    torch.testing.assert_close(logits, torch.tensor([[[2., 3., 2.5, 1.]]]))
    with pytest.raises(ProtocolError, match="存在惩罚"):
        sample_logits(logits, [], {"presence_penalty": float("nan")}, torch.Generator())


def test_incomplete_thought_is_not_sent_as_empty_assistant(tmp_path, monkeypatch):
    """界面保留未完成思考，实际模板排除空助手和历史思考文字。"""
    history = [{"role": "user", "content": "你好"},
        {"role": "assistant", "content": "", "reasoning_content": "未完成思考", "thinking_complete": False},
        {"role": "user", "content": "第二条输入"},
        {"role": "assistant", "content": "正式回答", "reasoning_content": "历史思考"},
        {"role": "user", "content": "第三条输入"}]
    clean = conversation_messages(history)
    assert len(clean) == 4 and clean[0]["content"] == "你好"
    assert all(set(message) == {"role", "content"} for message in clean)
    assert clean[2] == {"role": "assistant", "content": "正式回答"}
    assert len(history) == 5 and history[1]["reasoning_content"] == "未完成思考"


def test_project_edits_apply_without_replay_and_survive_sessions(tmp_path):
    """应用后旧输出不变，下一输入真实采用规则，会话清理不撤销干预。"""
    source = tmp_path / "model"
    create_local_model(source)
    stream = io.StringIO()
    runtime = Runtime(Emitter("project-edits", stream))
    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        request = command("run.start", {"messages": [{"role": "user", "content": "你好"}],
            "mode": "observe", "generation": {"max_new_tokens": 1, "temperature": 0},
            "trace_root": str(tmp_path / "records")})
        runtime.run(request)
        starts = len(events(stream, "run.started"))
        text = events(stream, "run.finished")[-1]["payload"]["text"]
        runtime.edit(command("edit.apply", {"target": "temporary", "expected_weights_revision": "w0",
            "rules": [{"layer_index": 0, "neuron_index": 3, "kind": "zero", "scope": "all", "value": 0}]}))
        assert len(events(stream, "run.started")) == starts
        assert events(stream, "run.finished")[-1]["payload"]["text"] == text
        assert not events(stream, "branch.created")
        runtime.activate_session(command("session.activate", {}))
        assert runtime.adapter.edit_rules and runtime.adapter.weights_revision == "w1"
        runtime.run(request)
        _, refs = runtime.store.get_operation("base", "f0", 0, "mlp")
        assert torch.count_nonzero(runtime.store.load(refs["output"])[..., 3]) == 0
        starts = len(events(stream, "run.started"))
        runtime.edit(command("edit.clear", {"expected_weights_revision": "w1"}))
        assert len(events(stream, "run.started")) == starts
        runtime.activate_session(command("session.activate", {}))
        assert runtime.adapter.edit_rules == []
        # 不同项目显式传入自己的规则，不继承上一项目状态。
        runtime.activate_session(command("session.activate", {"temporary_rules": [], "weights_revision": "w0"}))
        assert runtime.adapter.weights_revision == "w0"
    finally:
        stop(runtime)


def test_thinking_stream_and_final_response_are_separate(tmp_path, monkeypatch):
    """真实模型运算配合固定采样序列，验证模板开关、思考流和最终回答。"""
    source = tmp_path / "model"
    create_local_model(source)
    stream = io.StringIO()
    runtime = Runtime(Emitter("thinking", stream))
    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32")
        tokenizer = runtime.adapter.tokenizer
        tokenizer.add_special_tokens({"additional_special_tokens": ["<think>", "</think>"]})
        tokenizer.chat_template = "{% for m in messages %}{{ m.content }} {% endfor %}{% if enable_thinking %}<think>{% else %}<think></think>{% endif %}"
        runtime.adapter.info["capabilities"]["thinking_mode"] = "supported"
        end = tokenizer.convert_tokens_to_ids("</think>")
        sequence = iter([2, end, 3, 1])

        def sample(logits, history, generation, generator, generated=None):
            """只控制采样，前向、缓存、模板和协议仍执行真实路径。"""
            chosen = next(sequence)
            scores = logits[0, -1].clone()
            return chosen, scores, torch.softmax(scores, -1)

        monkeypatch.setattr("observatory.worker.sample_logits", sample)
        runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}],
            "mode": "normal", "trace_root": str(tmp_path / "records"),
            "generation": {"max_new_tokens": 4, "template_options": {"enable_thinking": True}}}))
        updates = events(stream, "text.updated")
        assert updates[0]["payload"]["text"] == ""
        assert updates[0]["payload"]["reasoning_content"] == "你好"
        assert updates[0]["payload"]["thinking_complete"] is False
        final = events(stream, "run.finished")[-1]["payload"]
        assert final["reasoning_content"] == "你好" and final["text"] == "用户"
        assert final["thinking_complete"] is True
        assert decode_response(tokenizer, [2], True) == ("", "你好", False)
        assert decode_response(tokenizer, [3], False) == ("用户", "", True)
        assert decode_response(tokenizer, [end, 3], True) == ("用户", "", True)
        runtime.adapter.info["capabilities"]["thinking_mode"] = "unsupported"
        with pytest.raises(ProtocolError, match="不支持原生"):
            runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}],
                "generation": {"template_options": {"enable_thinking": True}}}))
    finally:
        stop(runtime)


def test_loading_reports_measured_progress_and_checkpoint_rejects_changed_rules(tmp_path):
    """哈希阶段提供实际字节进度，旧恢复点不撤销当前项目规则。"""
    source = tmp_path / "model"
    create_local_model(source)
    progress = []
    runtime = Runtime(Emitter("checkpoint-edits", io.StringIO()))
    try:
        runtime.adapter = QwenAdapter.load(source, "cpu", "float32", lambda name, data: progress.append(data))
        measured = [data for data in progress if data.get("stage") == "verify"]
        assert measured and measured[-1]["completed"] == measured[-1]["total"] > 0
        assert any(data.get("stage") == "device" for data in progress)
        checkpoint = tmp_path / "checkpoint.json"
        checkpoint.write_text(json.dumps({"model_revision": runtime.adapter.info["model_revision"],
            "device": "cpu", "dtype": "float32", "temporary_rules": [{"kind": "zero"}]}), "utf-8")
        with pytest.raises(ProtocolError, match="规则已变化"):
            runtime.restore_checkpoint(command("run.restore", {"path": str(checkpoint), "temporary_rules": []}))
        assert runtime.adapter.edit_rules == []
    finally:
        stop(runtime)
