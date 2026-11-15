"""混合注意力的数值、缓存和编辑回归验证。"""
import torch
from transformers import Qwen3_5TextConfig, Qwen3_5ForCausalLM
from transformers.models.qwen3_5.modeling_qwen3_5 import Qwen3_5DynamicCache
from observatory.qwen35 import Qwen35Adapter


def test_hybrid_prefill_decode_and_edit():
    """两类层对照官方预填充与两步解码，编辑后重建所有缓存。"""
    torch.manual_seed(12)
    c = Qwen3_5TextConfig(vocab_size=64, hidden_size=32, intermediate_size=48,
        num_hidden_layers=2, num_attention_heads=4, num_key_value_heads=2, head_dim=8,
        linear_num_key_heads=2, linear_num_value_heads=4, linear_key_head_dim=8,
        linear_value_head_dim=8, layer_types=["linear_attention", "full_attention"],
        rope_parameters={"rope_type": "default", "rope_theta": 10000., "partial_rotary_factor": .5, "mrope_section": [1, 0, 1]})
    c._attn_implementation = "eager"
    adapter = Qwen35Adapter(Qwen3_5ForCausalLM(c).eval())
    cache = Qwen3_5DynamicCache(c)
    operations = []

    def execute(layer, op, fn, inputs):
        """记录真实调用，确认线性和全注意力均经过观测边界。"""
        operations.append((layer, op))
        return fn()

    with torch.inference_mode():
        for ids, positions in (([[2, 8, 9]], [0, 1, 2]), ([[6]], [3]), ([[7]], [4])):
            x = torch.tensor(ids)
            expected = adapter.model(x, past_key_values=cache, use_cache=True, logits_to_keep=1).logits
            actual = adapter.forward(x, positions, execute)
            torch.testing.assert_close(actual, expected, atol=1e-6, rtol=1e-5)
        assert (0, "linear_delta") in operations and (1, "attention_gate") in operations
        adapter.patch(dict(layer_index=0, neuron_index=47, token_position=2,
            kind="set", value=5., expected_weights_revision="w0"))
        captured = {}

        def edited(layer, op, fn, inputs):
            """保存干预结果，验证基线和修改后激活分离。"""
            value = fn()
            if layer == 0 and op == "mlp":
                captured[op] = value
            return value

        adapter.forward(torch.tensor([[2, 8, 9]]), [0, 1, 2], edited)
        assert captured["mlp"][0][0, 2, 47] == 5.
        assert captured["mlp"][1][0, 2, 47] != 5.
        assert adapter.cache.conv_states[0] is not None
