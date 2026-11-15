"""本地 Qwen2.5 稠密模型检查与数值等价的分操作执行器。"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

from .protocol import ProtocolError
from .control import Cancelled
from .traces import safe_path

OPERATIONS = [
    ("norm", "注意力前归一化", "input_layernorm", "Xn = RMSNorm(Hin)"),
    ("q", "查询向量投影", "self_attn.q_proj", "Q = Xn × Wqᵀ + bq"),
    ("k", "键向量投影", "self_attn.k_proj", "K = Xn × Wkᵀ + bk"),
    ("v", "值向量投影", "self_attn.v_proj", "V = Xn × Wvᵀ + bv"),
    ("rope", "旋转位置编码", "self_attn.rotary", "Q′, K′ = RoPE(Q, K, position)"),
    ("attention", "因果注意力", "self_attn", "A = Softmax(Q′K′ᵀ / √d + mask)；Z = A × V"),
    ("o", "注意力输出投影", "self_attn.o_proj", "O = concat(Z) × Woᵀ"),
    ("attention_residual", "注意力残差", "attention_residual", "Hmid = Hin + O"),
    ("norm2", "前馈前归一化", "post_attention_layernorm", "Xf = RMSNorm(Hmid)"),
    ("gate", "门控投影", "mlp.gate_proj", "g = Xf × Wgateᵀ"),
    ("up", "上投影", "mlp.up_proj", "u = Xf × Wupᵀ"),
    ("silu", "门控激活", "mlp.act_fn", "s = SiLU(g)"),
    ("mlp", "门控乘积与激活干预", "mlp.product", "m = SiLU(g) ⊙ u"),
    ("down", "下投影", "mlp.down_proj", "M = m × Wdownᵀ"),
    ("residual", "前馈残差", "residual_stream", "Hout = Hmid + M"),
]
OP_NAMES = {x[0]: x[1] for x in OPERATIONS}
HYBRID_OPERATIONS = [
    ("q_norm", "查询头归一化", "self_attn.q_norm", "Qn = RMSNorm(Q)"),
    ("k_norm", "键头归一化", "self_attn.k_norm", "Kn = RMSNorm(K)"),
    ("attention_gate", "注意力输出门控", "self_attn.gate", "Zg = Z ⊙ Sigmoid(gate)"),
    ("linear_qkv", "线性注意力联合投影", "linear_attn.in_proj_qkv", "QKV = X × Wqkvᵀ"),
    ("linear_z", "线性注意力输出门投影", "linear_attn.in_proj_z", "z = X × Wzᵀ"),
    ("linear_b", "线性注意力更新率投影", "linear_attn.in_proj_b", "b = X × Wbᵀ"),
    ("linear_a", "线性注意力衰减投影", "linear_attn.in_proj_a", "a = X × Waᵀ"),
    ("linear_conv", "因果卷积与激活", "linear_attn.conv1d", "QKV′ = SiLU(CausalConv1d(QKV))"),
    ("linear_beta", "线性注意力更新率", "linear_attn.beta", "β = Sigmoid(b)"),
    ("linear_decay", "线性注意力衰减系数", "linear_attn.decay", "g = −exp(Alog) × Softplus(a + dt_bias)"),
    ("linear_delta", "门控增量递归运算", "linear_attn.delta_rule", "递归状态更新与查询读取；不产生 Softmax 权重矩阵"),
    ("linear_norm", "线性注意力归一化与门控", "linear_attn.norm", "Z = RMSNorm(DeltaOut) ⊙ SiLU(z)"),
    ("linear_out", "线性注意力输出投影", "linear_attn.out_proj", "O = Z × Woutᵀ"),
]
OP_NAMES.update({x[0]: x[1] for x in HYBRID_OPERATIONS})


def operation_catalog(config, layer_index):
    """按实际层类型返回操作目录，线性注意力不冒充全注意力。"""
    if config.get("model_type") != "qwen3_5_text":
        return OPERATIONS
    mlp = OPERATIONS[7:]
    if config["layer_types"][layer_index] == "linear_attention":
        return [OPERATIONS[0]] + HYBRID_OPERATIONS[3:] + mlp
    full = [OPERATIONS[0], ("q", "查询与输出门联合投影", "self_attn.q_proj", "Qraw, gate = split(Xn × Wqᵀ)")] + OPERATIONS[2:4] + HYBRID_OPERATIONS[:2] + OPERATIONS[4:6] + [HYBRID_OPERATIONS[2], OPERATIONS[6]]
    return full + mlp


def inspect_model(directory, hash_weights=False, cancel=None, notify=None):
    """只读取本地 JSON 和 safetensors，不执行模型目录中的代码。"""
    root = Path(directory).resolve()
    from .editing import recover_save
    recover_save(root)
    try:
        raw_config = json.loads((root / "config.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ProtocolError("INVALID_MODEL", "模型配置缺失或损坏") from exc
    family = raw_config.get("model_type")
    if (family, raw_config.get("architectures", [None])[0]) not in (("qwen2", "Qwen2ForCausalLM"), ("qwen3_5", "Qwen3_5ForConditionalGeneration")):
        raise ProtocolError("UNSUPPORTED_ARCHITECTURE", "目前支持 Qwen2/Qwen2.5 和 Qwen3.5 稠密模型的文本计算")
    config = raw_config["text_config"] if family == "qwen3_5" else raw_config
    for key in ("num_hidden_layers", "hidden_size", "intermediate_size", "num_attention_heads", "num_key_value_heads", "vocab_size"):
        if type(config.get(key)) is not int or config[key] <= 0:
            raise ProtocolError("INVALID_CONFIG", f"模型配置字段无效：{key}")
    if config["hidden_size"] % config["num_attention_heads"] or config["num_attention_heads"] % config["num_key_value_heads"]:
        raise ProtocolError("INVALID_CONFIG", "注意力头配置不兼容")
    if config.get("use_sliding_window") or config.get("quantization_config") or (root / "adapter_config.json").exists():
        raise ProtocolError("UNSUPPORTED_BACKEND", "首期不支持滑动窗口、量化权重或 LoRA 模型")
    for filename in ("tokenizer.json", "tokenizer_config.json"):
        if not (root / filename).is_file():
            raise ProtocolError("MISSING_TOKENIZER", f"模型缺少 {filename}")
    index_path = root / "model.safetensors.index.json"
    weight_map = json.loads(index_path.read_text(encoding="utf-8"))["weight_map"] if index_path.exists() else {}
    shards = sorted(set(weight_map.values())) if weight_map else ["model.safetensors"]
    # 哈希校验按实际读取字节报告进度，加载框不使用估算百分比。
    hash_total = sum(safe_path(root, name).stat().st_size for name in shards if safe_path(root, name).is_file())
    hash_done, last_percent = 0, -1
    total, digests, shapes, tensor_bytes = 0, {}, {}, {}
    from safetensors import safe_open
    for name in shards:
        if cancel and cancel.is_set():
            raise Cancelled()
        path = safe_path(root, name)
        if not path.is_file():
            raise ProtocolError("MISSING_WEIGHTS", f"模型权重分块缺失：{name}")
        total += path.stat().st_size
        with safe_open(str(path), framework="pt", device="cpu") as source:
            keys = set(source.keys())
            if any(key not in keys for key, shard in weight_map.items() if shard == name):
                raise ProtocolError("INVALID_WEIGHTS", "权重索引与分块内容不一致")
            for key in keys:
                tensor_slice = source.get_slice(key)
                shapes[key] = tensor_slice.get_shape()
                tensor_bytes[key] = math.prod(shapes[key]) * {"BF16": 2, "F16": 2, "F32": 4, "F64": 8}.get(tensor_slice.get_dtype(), 2)
        if hash_weights:
            digest = hashlib.sha256()
            with path.open("rb") as handle:
                for block in iter(lambda: handle.read(8 * 1024**2), b""):
                    if cancel and cancel.is_set():
                        raise Cancelled()
                    digest.update(block)
                    hash_done += len(block)
                    percent = int(hash_done * 100 / max(1, hash_total))
                    if notify and percent != last_percent:
                        notify("model.progress", {"message": "校验模型文件与权重哈希", "stage": "verify",
                            "completed": hash_done, "total": hash_total})
                        last_percent = percent
            digests[name] = digest.hexdigest()
    h, i, heads = config["hidden_size"], config["intermediate_size"], config["num_attention_heads"]
    model_prefix = "model.language_model." if family == "qwen3_5" else "model."
    expected = {model_prefix + "embed_tokens.weight": [config["vocab_size"], h]}
    for layer in range(config["num_hidden_layers"]):
        prefix = model_prefix + f"layers.{layer}."
        expected.update({prefix + "mlp.gate_proj.weight": [i, h], prefix + "mlp.up_proj.weight": [i, h],
                         prefix + "mlp.down_proj.weight": [h, i]})
        if family == "qwen2":
            expected.update({prefix + "self_attn.q_proj.weight": [h, h],
                             prefix + "self_attn.k_proj.weight": [config["num_key_value_heads"] * (h // heads), h]})
        elif config["layer_types"][layer] == "full_attention":
            expected[prefix + "self_attn.q_proj.weight"] = [heads * config["head_dim"] * 2, h]
        else:
            expected[prefix + "linear_attn.in_proj_qkv.weight"] = [config["linear_num_key_heads"] * config["linear_key_head_dim"] * 2 + config["linear_num_value_heads"] * config["linear_value_head_dim"], h]
    for key, shape in expected.items():
        if shapes.get(key) != shape:
            raise ProtocolError("INVALID_WEIGHTS", f"权重形状不符合配置：{key}")
    revision = hashlib.sha256(json.dumps({"config": raw_config, "hashes": digests}, sort_keys=True).encode()).hexdigest()[:20]
    return {"path": str(root), "name": root.name, "config": config, "raw_config": raw_config, "model_type": family, "weight_bytes": total,
            "gpu_weight_bytes": sum(size for key, size in tensor_bytes.items() if key.startswith(model_prefix) or key == "lm_head.weight"),
            "hashes": digests, "model_revision": revision,
            "capabilities": {key: "supported" for key in ("local_load", "operation_step", "activation_edit", "weight_edit", "attention_weights", "logit_lens", "cache_resume")}}


class QwenAdapter:
    """与固定 Transformers Qwen2 eager 语义保持一致，显式暴露计算边界。"""

    def __init__(self, model, tokenizer=None, info=None):
        self.model = model.eval()
        self.tokenizer = tokenizer
        self.info = info or {}
        self.config = getattr(model.config, "text_config", model.config)
        self.backbone = getattr(model.model, "language_model", model.model)
        self.device = next(self.backbone.parameters()).device
        self.cache = [None] * self.config.num_hidden_layers
        self.activation_patches = {}
        self.edit_rules = []
        self.prompt_length = 0
        self.original_weights = {}
        self.weight_patches = {}
        self.weights_revision = "w0"
        self.revision_number = 0

    @classmethod
    def load(cls, directory, device="cuda:0", dtype="auto", notify=None, cancel=None):
        """本地加载固定架构，禁用远程代码并验证设备资源。"""
        import torch
        from transformers import AutoTokenizer, Qwen2ForCausalLM
        notify = notify or (lambda *args: None)
        notify("model.progress", {"message": "校验模型文件与权重哈希"})
        info = inspect_model(directory, hash_weights=True, cancel=cancel, notify=notify)
        if device.startswith("cuda"):
            if not torch.cuda.is_available():
                raise ProtocolError("CUDA_UNAVAILABLE", "CUDA 不可用，请选择 CPU 或检查驱动")
            try:
                free, _ = torch.cuda.mem_get_info(torch.device(device))
            except Exception as exc:
                raise ProtocolError("INVALID_DEVICE", "指定的 CUDA 设备不可用") from exc
            if free < info["gpu_weight_bytes"] + 768 * 1024**2:
                raise ProtocolError("OUT_OF_MEMORY", "可用显存不足以加载权重并保留计算空间")
        selected_dtype = getattr(torch, dtype) if dtype != "auto" else (torch.bfloat16 if device.startswith("cuda") and torch.cuda.is_bf16_supported() else torch.float32)
        if selected_dtype not in (torch.float32, torch.float16, torch.bfloat16):
            raise ProtocolError("UNSUPPORTED_DTYPE", "不支持该计算精度")
        if device.startswith("cuda") and free < info["gpu_weight_bytes"] * (2 if selected_dtype == torch.float32 else 1) + 768 * 1024**2:
            raise ProtocolError("OUT_OF_MEMORY", "该精度所需显存超过可用容量，请选择 BF16 或 FP16")
        if selected_dtype == torch.bfloat16 and device.startswith("cuda") and not torch.cuda.is_bf16_supported():
            raise ProtocolError("UNSUPPORTED_DTYPE", "当前显卡不支持 BF16")
        notify("model.progress", {"message": "加载本地 Tokenizer 和模型权重"})
        # Windows 上多次切换模型时，异步权重转换会触发 torch_cpu 原生访问异常；串行加载避免并发转换。
        import os
        if os.name == "nt":
            os.environ["HF_DEACTIVATE_ASYNC_LOAD"] = "1"
        tokenizer = AutoTokenizer.from_pretrained(directory, local_files_only=True, trust_remote_code=False)
        template = tokenizer.chat_template or ""
        info["capabilities"]["thinking_mode"] = "supported" if isinstance(template, str) and "enable_thinking" in template and "</think>" in template else "unsupported"
        model_class = Qwen2ForCausalLM
        if info["model_type"] == "qwen3_5":
            from transformers import Qwen3_5ForConditionalGeneration
            model_class = Qwen3_5ForConditionalGeneration
        model = model_class.from_pretrained(directory, local_files_only=True, trust_remote_code=False,
                   dtype=selected_dtype, attn_implementation="eager", low_cpu_mem_usage=True)
        if cancel and cancel.is_set():
            raise Cancelled()
        notify("model.progress", {"message": "将文本模型权重传输到计算设备", "stage": "device"})
        if info["model_type"] == "qwen3_5":
            # 当前产品接收文本，未使用的视觉编码器保留在 CPU，完整权重仍可导出。
            model.model.language_model.to(device)
            model.lm_head.to(device)
        else:
            model.to(device)
        if cancel and cancel.is_set():
            del model
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
            raise Cancelled()
        if info["model_type"] == "qwen3_5":
            from .qwen35 import Qwen35Adapter
            result = Qwen35Adapter(model, tokenizer, info)
        else:
            result = cls(model, tokenizer, info)
        info.update(device=str(result.device), dtype=str(selected_dtype).removeprefix("torch."))
        info["backend"] = "qwen3.5-hybrid-eager" if info["model_type"] == "qwen3_5" else "qwen2-eager"
        notify("model.progress", {"message": "模型初始化完成", "stage": "ready", "completed": 1, "total": 1})
        return result

    def reset_cache(self):
        """每个新运行和重算分支都从空 KV cache 开始。"""
        self.cache = [None] * self.config.num_hidden_layers

    def parameter(self, layer, kind):
        """解析逻辑参数，权重编辑只允许浮点稠密矩阵。"""
        if not 0 <= layer < self.config.num_hidden_layers or kind not in ("gate", "up", "down"):
            raise ProtocolError("INVALID_TARGET", "神经元权重目标无效")
        return getattr(self.backbone.layers[layer].mlp, kind + "_proj").weight

    def weight_value(self, layer, neuron, kind, dimension, original=False):
        """提供实际标量及首次编辑前的原始值。"""
        key = (layer, neuron, kind, dimension)
        weight = self.parameter(layer, kind)
        if not (0 <= neuron < self.config.intermediate_size and 0 <= dimension < self.config.hidden_size):
            raise ProtocolError("INVALID_TARGET", "神经元或隐藏维度越界")
        row, col = (dimension, neuron) if kind == "down" else (neuron, dimension)
        return self.original_weights.get(key, weight[row, col].item()) if original else weight[row, col].item()

    def patch(self, payload):
        """版本化地替换激活规则或单个系数，禁止溢出和累计缩放。"""
        import torch
        if payload.get("expected_weights_revision") != self.weights_revision:
            raise ProtocolError("STALE_WEIGHTS", "权重版本已变化，请刷新后重试")
        layer, neuron = int(payload["layer_index"]), int(payload["neuron_index"])
        if not (0 <= layer < self.config.num_hidden_layers and 0 <= neuron < self.config.intermediate_size):
            raise ProtocolError("INVALID_TARGET", "神经元目标越界")
        kind, value = payload["kind"], float(payload["value"])
        if not math.isfinite(value):
            raise ProtocolError("INVALID_VALUE", "修改值必须是有限数")
        rounded = torch.tensor(value, dtype=next(self.model.parameters()).dtype).item()
        if not math.isfinite(rounded):
            raise ProtocolError("VALUE_OVERFLOW", "修改值超出模型精度范围")
        if kind in ("scale", "zero", "set"):
            position = int(payload["token_position"])
            if position < 0:
                raise ProtocolError("INVALID_TARGET", "Token 位置不能为负数")
            self.activation_patches[(layer, position, neuron)] = (kind, value)
        elif kind in ("gate", "up", "down"):
            dimension = int(payload["dimension"])
            key = layer, neuron, kind, dimension
            original = self.weight_value(*key)
            self.original_weights.setdefault(key, original)
            row, col = (dimension, neuron) if kind == "down" else (neuron, dimension)
            with torch.no_grad():
                self.parameter(layer, kind)[row, col] = value
            self.weight_patches[key] = rounded
        else:
            raise ProtocolError("INVALID_PATCH", "不支持的修改方式")
        self.revision_number += 1
        self.weights_revision = f"w{self.revision_number}"
        self.reset_cache()
        return {"weights_revision": self.weights_revision, "requested_value": value, "rounded_value": rounded,
                "activation_patches": len(self.activation_patches), "weight_patches": len(self.weight_patches)}

    def restore(self, payload):
        """恢复当前层通道的权重和当前 Token 激活，保留其他目标。"""
        import torch
        if payload.get("expected_weights_revision") != self.weights_revision:
            raise ProtocolError("STALE_WEIGHTS", "权重版本已变化")
        layer, neuron, position = (int(payload[k]) for k in ("layer_index", "neuron_index", "token_position"))
        self.activation_patches.pop((layer, position, neuron), None)
        with torch.no_grad():
            for key in list(self.weight_patches):
                if key[:2] == (layer, neuron):
                    _, _, kind, dimension = key
                    row, col = (dimension, neuron) if kind == "down" else (neuron, dimension)
                    self.parameter(layer, kind)[row, col] = self.original_weights[key]
                    del self.weight_patches[key]
        self.revision_number += 1
        self.weights_revision = f"w{self.revision_number}"
        self.reset_cache()
        return {"weights_revision": self.weights_revision}

    def forward(self, input_ids, positions, execute):
        """执行一次真实前向；execute 在计算前负责暂停，在计算后负责采集。"""
        import torch
        from transformers.models.qwen2.modeling_qwen2 import apply_rotary_pos_emb, repeat_kv
        batch, length = input_ids.shape
        c = self.config
        position_ids = torch.tensor([positions], device=self.device)
        hidden = execute(-1, "embed", lambda: self.backbone.embed_tokens(input_ids), {"ids": input_ids})
        cos, sin = self.backbone.rotary_emb(hidden, position_ids)
        for index, layer in enumerate(self.backbone.layers):
            residual = hidden
            normalized = execute(index, "norm", lambda: layer.input_layernorm(hidden), {"input": hidden})
            query = execute(index, "q", lambda: layer.self_attn.q_proj(normalized), {"input": normalized})
            key = execute(index, "k", lambda: layer.self_attn.k_proj(normalized), {"input": normalized})
            value = execute(index, "v", lambda: layer.self_attn.v_proj(normalized), {"input": normalized})
            head_dim = c.hidden_size // c.num_attention_heads
            query = query.view(batch, length, c.num_attention_heads, head_dim).transpose(1, 2)
            key = key.view(batch, length, c.num_key_value_heads, head_dim).transpose(1, 2)
            value = value.view(batch, length, c.num_key_value_heads, head_dim).transpose(1, 2)
            query, key = execute(index, "rope", lambda: apply_rotary_pos_emb(query, key, cos, sin, unsqueeze_dim=1), {})
            old = self.cache[index]
            if old is not None:
                key, value = torch.cat((old[0], key), dim=-2), torch.cat((old[1], value), dim=-2)
            self.cache[index] = key, value
            repeated_k = repeat_kv(key, c.num_attention_heads // c.num_key_value_heads)
            repeated_v = repeat_kv(value, c.num_attention_heads // c.num_key_value_heads)

            def attention():
                """按绝对 query/key 位置构造因果遮罩，支持 1×Tk 缓存解码。"""
                scores = torch.matmul(query, repeated_k.transpose(2, 3)) * head_dim ** -.5
                blocked = torch.arange(key.shape[-2], device=self.device)[None, :] > position_ids[0, :, None]
                scores = scores.masked_fill(blocked[None, None], torch.finfo(scores.dtype).min)
                probabilities = torch.softmax(scores, dim=-1, dtype=torch.float32).to(query.dtype)
                result = torch.matmul(probabilities, repeated_v).transpose(1, 2).contiguous().reshape(batch, length, c.hidden_size)
                return result, probabilities

            attended, weights = execute(index, "attention", attention, {})
            projected = execute(index, "o", lambda: layer.self_attn.o_proj(attended), {"input": attended})
            hidden = execute(index, "attention_residual", lambda: residual + projected, {})
            residual = hidden
            normalized = execute(index, "norm2", lambda: layer.post_attention_layernorm(hidden), {"input": hidden})
            gate = execute(index, "gate", lambda: layer.mlp.gate_proj(normalized), {"input": normalized})
            up = execute(index, "up", lambda: layer.mlp.up_proj(normalized), {"input": normalized})
            activated = execute(index, "silu", lambda: layer.mlp.act_fn(gate), {})

            def product():
                """每次从未干预乘积应用一次规则，重复设置缩放不会累计。"""
                baseline = activated * up
                from .editing import apply_activations
                result = apply_activations(self, index, baseline, positions, residual, layer.mlp)
                return result, baseline

            middle, baseline = execute(index, "mlp", product, {})
            down = execute(index, "down", lambda: layer.mlp.down_proj(middle),
                           {"input": middle, "weight_row": layer.mlp.down_proj.weight[0]})
            hidden = execute(index, "residual", lambda: residual + down, {})
        hidden = execute(-1, "final_norm", lambda: self.backbone.norm(hidden), {})
        return execute(-1, "logits", lambda: self.model.lm_head(hidden[:, -1:, :]), {"input": hidden[:, -1:, :]})


def sample_logits(logits, history, generation, generator, generated=None):
    """分开保留原始分数、处理后分数和全词表实际采样分布。"""
    import torch
    scores = logits[0, -1].float().clone()
    presence = float(generation.get("presence_penalty", 0))
    if not math.isfinite(presence) or not 0 <= presence <= 2:
        raise ProtocolError("INVALID_GENERATION", "存在惩罚必须在零到二之间")
    # 存在惩罚只作用于本轮已生成token，不惩罚用户输入和之前的对话。
    if presence and generated:
        indices = torch.tensor(sorted(set(generated)), device=scores.device)
        scores[indices] -= presence
    penalty = float(generation.get("repetition_penalty", 1.05))
    if penalty <= 0 or not math.isfinite(penalty):
        raise ProtocolError("INVALID_GENERATION", "重复惩罚必须为正数")
    if penalty != 1 and history:
        indices = torch.tensor(sorted(set(history)), device=scores.device)
        selected = scores[indices]
        scores[indices] = torch.where(selected < 0, selected * penalty, selected / penalty)
    temperature = float(generation.get("temperature", .7))
    top_k, top_p = int(generation.get("top_k", 20)), float(generation.get("top_p", .8))
    if temperature < 0 or not math.isfinite(temperature) or top_k < 0 or not 0 < top_p <= 1:
        raise ProtocolError("INVALID_GENERATION", "采样参数范围无效")
    if temperature == 0:
        chosen = int(scores.argmax())
        probabilities = torch.zeros_like(scores)
        probabilities[chosen] = 1
        return chosen, scores, probabilities
    scores /= temperature
    if top_k:
        scores[scores < torch.topk(scores, min(top_k, scores.numel())).values[-1]] = -torch.inf
    if top_p < 1:
        sorted_scores, indices = torch.sort(scores, descending=False)
        remove = sorted_scores.softmax(-1).cumsum(-1) <= (1 - top_p)
        remove[-1] = False
        scores[indices[remove]] = -torch.inf
    if torch.isnan(scores).any() or torch.isposinf(scores).any() or not torch.isfinite(scores).any():
        raise ProtocolError("INVALID_DISTRIBUTION", "采样分数无有效分布")
    probabilities = torch.softmax(scores, -1)
    chosen = int(torch.multinomial(probabilities, 1, generator=generator))
    return chosen, scores, probabilities
