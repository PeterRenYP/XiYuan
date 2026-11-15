"""使用真实本地模型验证神经元采集、权重路径与上下文隔离。"""
import io
import json
import os
from pathlib import Path
import threading

import pytest
import torch
from transformers import Qwen2Config, Qwen2ForCausalLM

from observatory.model import QwenAdapter, inspect_model
from observatory.neurons import sample_indices
from observatory.protocol import Emitter, ProtocolError, command
from observatory.traces import TraceStore
from observatory.worker import Runtime
from test_runtime import create_local_model


@pytest.fixture
def runtime(tmp_path):
    """每项测试使用独立真实权重、运行器和输出流。"""
    source = tmp_path / "模型"
    model, tokenizer = create_local_model(source, layers=3)
    runtime = Runtime(Emitter("neurons-test", io.StringIO()))
    runtime.adapter = QwenAdapter(model, tokenizer, inspect_model(source))
    runtime.run_id = "采集测试"
    yield runtime
    runtime.handle(command("shutdown"))
    runtime.thread.join(5)
    if runtime.store:
        runtime.store.close()


def forward(runtime):
    """同一次真实前向同时保留原始结果，以逐项核对观察值。"""
    captured = {}

    def execute(layer, operation, function, inputs):
        """观察调用不能修改真实计算返回值。"""
        result = function()
        value = result[0] if isinstance(result, tuple) else result
        if layer >= 0 and operation in ("gate", "up", "silu", "mlp"):
            captured[layer, operation] = value.detach().clone()
        runtime.neurons.capture(layer, operation, result,
            dict(forward_id="f0", weights_revision="w0", generation_step=0), [0, 1, 2])
        return result

    runtime.adapter.reset_cache()
    with torch.inference_mode():
        logits = runtime.adapter.forward(torch.tensor([[2, 3, 4]]), [0, 1, 2], execute)
    return logits, captured


def test_actual_values_paths_and_no_inference_changes(runtime):
    """颜色来源、明细和排名等于真实计算，采集不改变输出及权重。"""
    before = {name: value.detach().clone() for name, value in runtime.adapter.model.named_parameters()}
    baseline, _ = forward(runtime)
    assert not runtime.neurons.frames
    runtime.neurons.configure(dict(enabled=True, layer_index=1, neuron_index=17))
    logits, values = forward(runtime)
    torch.testing.assert_close(logits, baseline, rtol=0, atol=0)
    result = runtime.neurons.query(dict(layer_index=1, neuron_index=17))
    selected = result["selected"]
    for field, operation in (("value", "mlp"), ("gate", "gate"), ("silu", "silu"), ("up", "up")):
        assert selected[field] == float(values[1, operation][0, -1, 17])
    row = values[1, "mlp"][0, -1]
    assert selected["rank"] == int((row.abs() > row[17].abs()).sum()) + 1
    assert len(result["frames"]) == 3 and len(result["history"]) == 1
    candidates = result["layers"][2]["indices"]
    actual = runtime.adapter.parameter(2, "up")[candidates].float() @ runtime.adapter.parameter(1, "down")[:, 17].float()
    expected = torch.argsort(actual.abs(), descending=True, stable=True)[:4].tolist()
    assert [p["target_neuron"] for p in result["paths"]] == [candidates[i] for i in expected]
    for path, offset in zip(result["paths"], expected):
        assert path["coefficient"] == float(actual[offset])
    assert not runtime.neurons.query(dict(layer_index=2, neuron_index=0))["paths"]
    for name, value in runtime.adapter.model.named_parameters():
        torch.testing.assert_close(value, before[name], rtol=0, atol=0)


def test_global_paths_do_not_depend_on_selection(runtime):
    """即使检查最后一层，全层每个可视源仍提供真实传播路径。"""
    runtime.neurons.configure(dict(enabled=True, layer_index=2, neuron_index=39))
    _, values = forward(runtime)
    snapshot = runtime.neurons.query(dict(layer_index=2, neuron_index=39))
    assert not snapshot["paths"]
    assert len(snapshot["network_paths"]) == 2
    for entry in snapshot["network_paths"]:
        layer = entry["layer_index"]
        assert len(entry["paths"]) == 40
        for source, target, coefficient in entry["paths"]:
            actual = runtime.adapter.parameter(layer+1, "up").float() @ runtime.adapter.parameter(layer, "down")[:, source].float()
            assert target == int(actual.abs().argmax())
            assert coefficient == pytest.approx(float(actual[target]), abs=1e-8)
        frame = runtime.neurons.frames[layer]
        assert frame["network_paths"] == entry["paths"]
        assert frame["values"] == values[layer, "mlp"][0, -1].tolist()
    first = snapshot["network_paths"]
    runtime.neurons.configure(dict(enabled=True, layer_index=0, neuron_index=17))
    assert runtime.neurons.query(dict(layer_index=0, neuron_index=17))["network_paths"] == first
    runtime.adapter.weights_revision = "w1"
    assert not runtime.neurons.query(dict(historical=True, weights_revision="w0"))["network_paths"]


@pytest.mark.parametrize("layer,neuron", [(-1, 0), (3, 0), (0, -1), (0, 40), (False, 1), (0, True), (1.2, 0), (0, "1")])
def test_target_validation(runtime, layer, neuron):
    """边界、布尔、小数和字符串编号均被拒绝。"""
    with pytest.raises(ProtocolError):
        runtime.neurons.query(dict(layer_index=layer, neuron_index=neuron))


def test_reset_and_identity_isolation(runtime):
    """旧运行、分支、模型与权重的激活不能混进当前详情。"""
    runtime.neurons.configure(dict(enabled=True, layer_index=0, neuron_index=0))
    forward(runtime)
    for identity in ({"run_id": "别的运行"}, {"branch_id": "别的分支"}):
        result = runtime.neurons.query(dict(layer_index=0, neuron_index=0, **identity))
        assert not result["frames"] and result["selected"]["status"] == "not_captured"
    with pytest.raises(ProtocolError):
        runtime.neurons.query(dict(model_revision="别的模型"))
    runtime.adapter.weights_revision = "w1"
    assert not runtime.neurons.query({})["frames"]
    runtime.neurons.reset(disable=True)
    assert not runtime.neurons.frames and not runtime.neurons.vectors and not runtime.neurons.history
    assert not runtime.neurons.enabled


def test_zero_and_nonfinite_are_distinct(runtime):
    """真实零可显示为零，非有限值只能显示缺失。"""
    runtime.neurons.configure(dict(enabled=True))
    row = torch.zeros(1, 1, 40)
    row[0, 0, 1] = float("nan")
    row[0, 0, 2] = float("inf")
    runtime.neurons.capture(0, "mlp", (row, row), dict(forward_id="f0", weights_revision="w0", generation_step=0), [0])
    result = runtime.neurons.query({})
    assert result["selected"]["value"] == 0
    assert result["frames"][0]["values"][:3] == [0, None, None]
    json.dumps(result, allow_nan=False)


def test_normal_generation_and_offline_trace(runtime, tmp_path):
    """普通对话也可观察；离线仅读取确实保存的层与Token。"""
    runtime.neurons.configure(dict(enabled=True, layer_index=0, neuron_index=17))
    payload = dict(messages=[dict(role="user", content="你好")], mode="normal",
        generation=dict(temperature=0, max_new_tokens=2, seed=42), trace_root=str(tmp_path / "记录"))
    runtime.run(command("run.start", payload))
    wire = [json.loads(line) for line in runtime.emitter.stream.getvalue().splitlines()]
    assert any(message["name"] == "neurons.frame" for message in wire)
    assert any(message["name"] == "run.finished" for message in wire)
    # 关闭页面后，同样的生成不会产生额外观察事件，生成Token保持一致。
    first = [m["payload"]["id"] for m in wire if m["name"] == "token.sampled"]
    assert first
    runtime.neurons.configure(dict(enabled=False))
    runtime.emitter.stream.seek(0)
    runtime.emitter.stream.truncate()
    runtime.run(command("run.start", payload))
    wire = [json.loads(line) for line in runtime.emitter.stream.getvalue().splitlines()]
    assert not any(message["name"] == "neurons.frame" for message in wire)
    assert [m["payload"]["id"] for m in wire if m["name"] == "token.sampled"] == first
    payload["mode"] = "observe"
    payload["capture"] = {"policy": "full"}
    runtime.run(command("run.start", payload))
    context, refs = runtime.store.get_operation("base", "f0", 0, "mlp")
    position = context["positions"][-1]
    expected = runtime.store.load(refs["output"])[0, -1, 17].item()
    directory = runtime.store.root
    runtime.store.close()
    runtime.store = TraceStore(directory, readonly=True)
    runtime.adapter = None
    runtime.neurons.reset(disable=True)
    query = dict(layer_index=0, neuron_index=17, historical=True, live=False,
                 forward_id="f0", token_position=position, run_id=runtime.run_id)
    result = runtime.neurons.query(query)
    assert result["selected"]["value"] == expected
    assert not result["paths"]
    assert result["frames"][0]["token_position"] == position
    assert runtime.neurons.query(query | {"token_position": 1000})["selected"]["status"] == "not_captured"
    with pytest.raises(ProtocolError):
        runtime.neurons.query(query | {"model_revision": "不存在的模型"})


def test_read_while_step_paused(runtime, tmp_path):
    """逐步执行等待期间，订阅和查询不进入被阻塞的计算队列。"""
    runtime.neurons.configure(dict(enabled=True))
    paused = threading.Event()
    original = runtime.event

    def event(name, payload=None, context=None):
        """只观察真实暂停通知，不替代执行控制器。"""
        result = original(name, payload, context)
        if name == "step.waiting":
            paused.set()
        return result

    runtime.event = event
    runtime.handle(command("run.start", dict(messages=[dict(role="user", content="你好")], mode="step",
        generation=dict(max_new_tokens=1), trace_root=str(tmp_path / "逐步"))))
    assert paused.wait(10)
    request = command("neurons.query", dict(layer_index=0, neuron_index=0))
    runtime.read_task(request)
    wire = [json.loads(line) for line in runtime.emitter.stream.getvalue().splitlines()]
    assert any(m["request_id"] == request["request_id"] and m["name"] == "neurons.snapshot" for m in wire)
    runtime.handle(command("run.cancel"))
    runtime.tasks.join()


def test_full_search_and_native_fixture(runtime, tmp_path):
    """较宽真实模型证明搜索覆盖非显示通道，同时为原生截图提供真实样本。"""
    torch.manual_seed(27)
    model = Qwen2ForCausalLM(Qwen2Config(vocab_size=32, hidden_size=16, intermediate_size=256,
        num_hidden_layers=24, num_attention_heads=2, num_key_value_heads=1, max_position_embeddings=128)).eval()
    source = tmp_path / "宽模型"
    model.save_pretrained(source)
    runtime.adapter.tokenizer.save_pretrained(source)
    runtime.adapter = QwenAdapter(model, runtime.adapter.tokenizer, inspect_model(source))
    runtime.neurons.configure(dict(enabled=True, layer_index=13, neuron_index=111))
    _, values = forward(runtime)
    assert 112 not in sample_indices(256, 64)
    precise = runtime.neurons.query(dict(layer_index=13, neuron_index=112))
    assert precise["selected"]["value"] == float(values[13, "mlp"][0, -1, 112])
    assert 112 in next(f for f in precise["frames"] if f["layer_index"] == 13)["indices"]
    fixture = runtime.neurons.query(dict(layer_index=13, neuron_index=111))
    assert sum(len(layer["indices"]) for layer in fixture["layers"]) <= 2560
    destination = os.environ.get("NEURON_VERIFICATION_FIXTURE")
    if destination:
        import shutil
        Path(destination).parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, Path(destination).parent / "验收模型", dirs_exist_ok=True)
        Path(destination).write_text(json.dumps(dict(model=runtime.adapter.info | {"weights_revision": "w0"},
            snapshot=fixture), ensure_ascii=False, allow_nan=False), encoding="utf-8")


def test_hybrid_model_observation(runtime):
    """混合注意力模型也读取真实前馈通道，不依赖全注意力结构。"""
    from transformers import Qwen3_5TextConfig, Qwen3_5ForCausalLM
    from observatory.qwen35 import Qwen35Adapter
    config = Qwen3_5TextConfig(vocab_size=32, hidden_size=32, intermediate_size=48,
        num_hidden_layers=2, num_attention_heads=4, num_key_value_heads=2, head_dim=8,
        linear_num_key_heads=2, linear_num_value_heads=4, linear_key_head_dim=8,
        linear_value_head_dim=8, layer_types=["linear_attention", "full_attention"],
        rope_parameters={"rope_type": "default", "rope_theta": 10000., "partial_rotary_factor": .5, "mrope_section": [1, 0, 1]})
    config._attn_implementation = "eager"
    runtime.adapter = Qwen35Adapter(Qwen3_5ForCausalLM(config).eval(),
        info=dict(model_revision="hybrid-test", config=config.to_dict()))
    baseline, _ = forward(runtime)
    runtime.neurons.configure(dict(enabled=True, layer_index=0, neuron_index=47))
    observed, values = forward(runtime)
    torch.testing.assert_close(observed, baseline, rtol=0, atol=0)
    result = runtime.neurons.query(dict(layer_index=0, neuron_index=47))
    assert result["selected"]["value"] == float(values[0, "mlp"][0, -1, 47])
    assert len(result["paths"]) == 4
