"""Qwen3.5 混合注意力分操作执行器；调用 Transformers 5.3 Apache-2.0 官方算子。"""
import torch
import torch.nn.functional as F
from transformers.masking_utils import create_causal_mask
from transformers.models.qwen3_5.modeling_qwen3_5 import (
    Qwen3_5DynamicCache, apply_rotary_pos_emb, eager_attention_forward)

from .model import QwenAdapter
from .protocol import ProtocolError


class Qwen35Adapter(QwenAdapter):
    """同时维护卷积、增量递归和全注意力缓存，每个操作在执行前可暂停。"""

    def __init__(self, *args, **kwargs):
        """初始化共享编辑能力和混合缓存。"""
        super().__init__(*args, **kwargs)
        self.reset_cache()

    def reset_cache(self):
        """分支重算和模型切换时清空全部三类状态。"""
        self.cache = Qwen3_5DynamicCache(self.config)

    def linear(self, index, layer, hidden, execute):
        """按官方顺序执行门控 DeltaNet，保存真实递归状态。"""
        m = layer.linear_attn
        batch, length, _ = hidden.shape
        recurrent = self.cache.has_previous_state and length == 1
        mixed = execute(index, "linear_qkv", lambda: m.in_proj_qkv(hidden), {"input": hidden}).transpose(1, 2)
        z = execute(index, "linear_z", lambda: m.in_proj_z(hidden), {"input": hidden}).reshape(batch, length, -1, m.head_v_dim)
        b = execute(index, "linear_b", lambda: m.in_proj_b(hidden), {"input": hidden})
        a = execute(index, "linear_a", lambda: m.in_proj_a(hidden), {"input": hidden})

        def convolution():
            """单Token更新原有卷积缓存，预填充建立新的左侧窗口。"""
            if recurrent:
                return m.causal_conv1d_update(mixed, self.cache.conv_states[index],
                    m.conv1d.weight.squeeze(1), m.conv1d.bias, m.activation)
            self.cache.conv_states[index] = F.pad(mixed, (m.conv_kernel_size - length, 0))
            if m.causal_conv1d_fn is not None:
                return m.causal_conv1d_fn(x=mixed, weight=m.conv1d.weight.squeeze(1),
                    bias=m.conv1d.bias, activation=m.activation, seq_idx=None)
            return F.silu(m.conv1d(mixed)[:, :, :length])

        mixed = execute(index, "linear_conv", convolution, {})
        q, k, v = torch.split(mixed.transpose(1, 2), [m.key_dim, m.key_dim, m.value_dim], dim=-1)
        q, k = [x.reshape(batch, length, -1, m.head_k_dim) for x in (q, k)]
        v = v.reshape(batch, length, -1, m.head_v_dim)
        beta = execute(index, "linear_beta", lambda: b.sigmoid(), {})
        decay = execute(index, "linear_decay", lambda: -m.A_log.float().exp() * F.softplus(a.float() + m.dt_bias), {})
        if m.num_v_heads // m.num_k_heads > 1:
            q, k = [x.repeat_interleave(m.num_v_heads // m.num_k_heads, dim=2) for x in (q, k)]

        def delta():
            """官方分块预填充和逐Token递归共用同一状态定义。"""
            fn = m.recurrent_gated_delta_rule if recurrent else m.chunk_gated_delta_rule
            return fn(q, k, v, g=decay, beta=beta,
                initial_state=self.cache.recurrent_states[index] if recurrent else None,
                output_final_state=True, use_qk_l2norm_in_kernel=True)

        core, state = execute(index, "linear_delta", delta, {})
        self.cache.recurrent_states[index] = state
        normalized = execute(index, "linear_norm", lambda: m.norm(core.reshape(-1, m.head_v_dim),
            z.reshape(-1, m.head_v_dim)).reshape(batch, length, -1), {})
        return execute(index, "linear_out", lambda: m.out_proj(normalized), {"input": normalized})

    def full(self, index, layer, hidden, cos, sin, mask, execute):
        """保留查询输出门、头归一化及部分旋转编码的真实顺序。"""
        m = layer.self_attn
        batch, length, _ = hidden.shape
        projected = execute(index, "q", lambda: m.q_proj(hidden), {"input": hidden})
        q, gate = projected.view(batch, length, -1, m.head_dim * 2).chunk(2, dim=-1)
        gate = gate.reshape(batch, length, -1)
        k = execute(index, "k", lambda: m.k_proj(hidden), {"input": hidden})
        v = execute(index, "v", lambda: m.v_proj(hidden), {"input": hidden})
        q = execute(index, "q_norm", lambda: m.q_norm(q).transpose(1, 2), {})
        k = execute(index, "k_norm", lambda: m.k_norm(k.view(batch, length, -1, m.head_dim)).transpose(1, 2), {})
        v = v.view(batch, length, -1, m.head_dim).transpose(1, 2)
        q, k = execute(index, "rope", lambda: apply_rotary_pos_emb(q, k, cos, sin), {})
        k, v = self.cache.update(k, v, index)
        out, weights = execute(index, "attention", lambda: eager_attention_forward(
            m, q, k, v, mask, scaling=m.scaling, dropout=0.), {})
        out = out.reshape(batch, length, -1).contiguous()
        gated = execute(index, "attention_gate", lambda: out * gate.sigmoid(), {})
        return execute(index, "o", lambda: m.o_proj(gated), {"input": gated})

    def forward(self, input_ids, positions, execute):
        """执行纯文本混合网络，所有可编辑激活都来自当前真实前向。"""
        position_ids = torch.tensor([positions], device=self.device)
        hidden = execute(-1, "embed", lambda: self.backbone.embed_tokens(input_ids), {"ids": input_ids})
        cos, sin = self.backbone.rotary_emb(hidden, position_ids)
        mask = create_causal_mask(config=self.config, inputs_embeds=hidden, attention_mask=None,
            cache_position=position_ids[0], past_key_values=self.cache, position_ids=position_ids)
        for index, layer in enumerate(self.backbone.layers):
            residual = hidden
            normalized = execute(index, "norm", lambda: layer.input_layernorm(hidden), {"input": hidden})
            projected = (self.linear(index, layer, normalized, execute) if layer.layer_type == "linear_attention"
                else self.full(index, layer, normalized, cos, sin, mask, execute))
            hidden = execute(index, "attention_residual", lambda: residual + projected, {})
            residual = hidden
            normalized = execute(index, "norm2", lambda: layer.post_attention_layernorm(hidden), {"input": hidden})
            gate = execute(index, "gate", lambda: layer.mlp.gate_proj(normalized), {"input": normalized})
            up = execute(index, "up", lambda: layer.mlp.up_proj(normalized), {"input": normalized})
            activated = execute(index, "silu", lambda: layer.mlp.act_fn(gate), {})

            def product():
                """从基线应用一次激活编辑，分支间不累计缩放。"""
                baseline = activated * up
                from .editing import apply_activations
                edited = apply_activations(self, index, baseline, positions, residual, layer.mlp)
                return edited, baseline

            middle, baseline = execute(index, "mlp", product, {})
            down = execute(index, "down", lambda: layer.mlp.down_proj(middle),
                {"input": middle, "weight_row": layer.mlp.down_proj.weight[0]})
            hidden = execute(index, "residual", lambda: residual + down, {})
        hidden = execute(-1, "final_norm", lambda: self.backbone.norm(hidden), {})
        return execute(-1, "logits", lambda: self.model.lm_head(hidden[:, -1:, :]), {"input": hidden[:, -1:, :]})
