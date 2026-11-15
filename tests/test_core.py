"""验证真实数值、执行边界、修改作用域和存储安全。"""
import io
import threading
import time

import pytest
import torch
from transformers import Qwen2Config, Qwen2ForCausalLM
from transformers.cache_utils import DynamicCache

from observatory.control import Cancelled, StepController
from observatory.model import QwenAdapter, sample_logits
from observatory.protocol import ProtocolError
from observatory.traces import TraceStore, safe_path


@pytest.fixture
def adapter():
    """随机小模型只用于可重复数值对照，不充当产品演示数据。"""
    torch.manual_seed(42)
    config = Qwen2Config(vocab_size=128, hidden_size=32, intermediate_size=70,
                        num_hidden_layers=2, num_attention_heads=4, num_key_value_heads=2,
                        max_position_embeddings=128, attention_dropout=0., tie_word_embeddings=True)
    config._attn_implementation = "eager"
    return QwenAdapter(Qwen2ForCausalLM(config).eval())


def test_prefill_and_cached_decode_match_upstream(adapter):
    """预填充和两个缓存解码步逐元素对照固定版本上游实现。"""
    reference_cache = DynamicCache(config=adapter.config)
    results = []
    with torch.inference_mode():
        for ids, positions in (([[2, 8, 9, 11]], [0, 1, 2, 3]), ([[6]], [4]), ([[3]], [5])):
            inputs = torch.tensor(ids)
            reference = adapter.model(inputs, past_key_values=reference_cache, use_cache=True).logits[:, -1:]
            actual = adapter.forward(inputs, positions, lambda layer, op, fn, values: fn())
            results.append(float((reference - actual).abs().max()))
            torch.testing.assert_close(reference, actual, atol=1e-5, rtol=1e-4)
    assert max(results) < 1e-5


def test_step_wait_duplicate_release_and_cancel():
    """未放行不会执行下一段，重复请求不多放行，取消能唤醒等待。"""
    controller = StepController("step")
    executed = []
    def run():
        """在线程内模拟两个真实运算边界。"""
        try:
            controller.checkpoint({})
            executed.append(1)
            controller.checkpoint({})
            executed.append(2)
        except Cancelled:
            pass
    thread = threading.Thread(target=run)
    thread.start()
    deadline = time.monotonic() + 2
    while not controller.waiting and time.monotonic() < deadline:
        time.sleep(.005)
    assert not executed
    boundary, revision = controller.boundary_id, controller.revision
    controller.release("r1", boundary, revision)
    assert controller.release("r1", boundary, revision)["duplicate"]
    deadline = time.monotonic() + 2
    while controller.revision == revision and time.monotonic() < deadline:
        time.sleep(.005)
    assert executed == [1]
    with pytest.raises(ProtocolError):
        controller.release("r2", boundary, revision)
    controller.cancel()
    thread.join(2)
    assert not thread.is_alive() and executed == [1]


def test_activation_and_weight_patch_scope(adapter):
    """重复缩放替换规则；权重修改和恢复能回到原始模型结果。"""
    captured = {}
    def execute(layer, operation, fn, values):
        """捕获指定层乘积，用于核对其他 Token 没被修改。"""
        result = fn()
        if layer == 0 and operation == "mlp":
            captured["m"] = result[0].clone()
        return result
    ids = torch.tensor([[3, 8, 9]])
    with torch.inference_mode():
        baseline = adapter.forward(ids, [0, 1, 2], execute)
        original = captured["m"].clone()
        payload = dict(layer_index=0, neuron_index=69, token_position=1, kind="scale", value=2,
                       expected_weights_revision="w0")
        adapter.patch(payload)
        adapter.patch(payload | {"expected_weights_revision": "w1"})
        adapter.forward(ids, [0, 1, 2], execute)
        assert captured["m"][0, 1, 69] == original[0, 1, 69] * 2
        assert torch.equal(captured["m"][0, 0], original[0, 0])
        actual_weight = adapter.weight_value(0, 69, "gate", 31)
        adapter.patch(payload | dict(kind="gate", dimension=31, value=.5, expected_weights_revision="w2"))
        assert adapter.weight_value(0, 69, "gate", 31) == .5
        adapter.restore(payload | {"expected_weights_revision": "w3"})
        assert adapter.weight_value(0, 69, "gate", 31) == actual_weight
        restored = adapter.forward(ids, [0, 1, 2], execute)
        torch.testing.assert_close(baseline, restored, atol=1e-6, rtol=1e-5)


def test_trace_integrity_and_budget(tmp_path):
    """独立快照不受源张量修改影响，篡改和越界路径被拒绝。"""
    store = TraceStore(tmp_path / "trace", budget=16 * 1024)
    store.save_manifest(schema_version="1.0")
    value = torch.arange(16).reshape(2, 8).float()
    ref = store.tensor(value)
    value.zero_()
    assert store.load(ref).sum() == 120
    large_ref=store.tensor(torch.zeros(4096))
    assert large_ref and not store.incomplete
    assert store.load(large_ref).numel()==4096
    with pytest.raises(ProtocolError):
        safe_path(store.root, "../outside")
    target = store.root / "tensors" / (ref + ".safetensors")
    target.write_bytes(b"bad")
    with pytest.raises(ProtocolError):
        store.load(ref)
    store.close()


def test_sampler_greedy_and_invalid_distribution():
    """零温度产生确定选择，全无效分布明确报错。"""
    generator = torch.Generator().manual_seed(42)
    chosen, _, probabilities = sample_logits(torch.tensor([[[1., 4., 2.]]]), [], {"temperature": 0}, generator)
    assert chosen == 1 and probabilities.tolist() == [0, 1, 0]
    with pytest.raises(ProtocolError):
        sample_logits(torch.full((1, 1, 3), float("-inf")), [], {"temperature": 1}, generator)


def test_monitor_contract_and_missing_values():
    """可用和缺失指标都符合附件契约，缺失显存不会伪装成零。"""
    import json
    from pathlib import Path
    import jsonschema
    from observatory.telemetry import payload
    from observatory.protocol import Emitter
    schema = json.loads((Path(__file__).resolve().parent.parent / "开发附件/protocol-v1.schema.json").read_text(encoding="utf-8"))
    for value in (payload("available", uuid="GPU-test", used=10, total=20, utilization=50), payload("unavailable", "驱动不可用")):
        message = Emitter("test-monitor", io.StringIO()).send("telemetry.sample", value)
        jsonschema.validate(message, schema)
        if value["status"] == "unavailable":
            assert value["metrics"]["memory_used"]["value"] is None
