"""相同前缀与自由生成的真实对照，记录可重开的逐步数值证据。"""
from __future__ import annotations

import copy
import datetime
import platform
from pathlib import Path

import torch

from .model import sample_logits, operation_catalog
from .protocol import ProtocolError
from .storage import atomic_json
from .versions import vocabulary_hash


def compare(features, payload):
    """两边使用相同权重与生成配置，只改变临时规则；退出时恢复缓存和规则。"""
    runtime = features.runtime
    adapter = runtime.adapter
    if adapter is None:
        raise ProtocolError("MODEL_NOT_LOADED", "请先加载模型")
    if not runtime.last_request:
        raise ProtocolError("NO_INPUT", "请先完成一轮对话，再用本轮输入重跑")
    request = copy.deepcopy(runtime.last_request["payload"])
    from .worker import generation_options, conversation_messages
    generation = generation_options(adapter.info, request.get("generation", {}))
    messages = conversation_messages(request["messages"])
    template = adapter.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True, **generation.get("template_options", {}))
    ids = adapter.tokenizer.encode(template, add_special_tokens=False)
    steps = int(payload.get("steps", 4))
    if not 1 <= steps <= 64 or len(ids) + steps > adapter.config.max_position_embeddings:
        raise ProtocolError("INVALID_STEPS", "对照步数须为 1 至 64，且不能超出模型上下文")
    mode = payload.get("mode", "fixed")
    if mode not in ("fixed", "free"):
        raise ProtocolError("INVALID_MODE", "请选择固定前缀或自由生成对照")
    layer, neuron = int(payload.get("layer", 0)), int(payload.get("neuron", 0))
    operation = payload.get("operation", "mlp")
    if not 0 <= layer < adapter.config.num_hidden_layers or not 0 <= neuron < adapter.config.intermediate_size:
        raise ProtocolError("INVALID_TARGET", "对照层或通道越界")
    allowed = operation_catalog(adapter.info["config"], layer)
    if operation not in [o[0] for o in allowed]:
        raise ProtocolError("UNSUPPORTED_OPERATION", "当前层不支持这个采集点")
    target = payload.get("target", "").strip()
    target_ids = [int(target)] if target.isdecimal() else adapter.tokenizer.encode(target, add_special_tokens=False) if target else []
    vocabulary_size = adapter.model.get_output_embeddings().weight.shape[0]
    if len(target_ids) > 1 or any(not 0 <= t < vocabulary_size for t in target_ids):
        raise ProtocolError("INVALID_TARGET", "目标须为单个 Token 文本或有效编号")
    original = copy.deepcopy(adapter.edit_rules)
    legacy = copy.deepcopy(adapter.activation_patches)
    seed = int(generation.get("seed", 42))
    report = {"schema": 1, "mode": mode, "model": copy.deepcopy(adapter.info), "weights_revision": adapter.weights_revision,
        "vocabulary": vocabulary_hash(adapter.tokenizer), "messages": messages, "template": template, "token_ids": ids,
        "rules": original, "legacy_rules": legacy, "generation": generation, "seed": seed, "prefix_source": "原始分支生成序列",
        "capture_point": operation, "layer": layer, "neuron": neuron,
        "environment": {"torch": torch.__version__, "transformers": __import__("transformers").__version__,
            "platform": platform.platform(), "device": str(adapter.device), "dtype": str(next(adapter.model.parameters()).dtype),
            "deterministic_algorithms": torch.are_deterministic_algorithms_enabled()},
        "atol": float(payload.get("atol", 1e-5)), "rtol": float(payload.get("rtol", 1e-4)), "rows": []}
    generated = [[], []]
    generators = [torch.Generator(device=adapter.device).manual_seed(seed) for _ in range(2)]
    divergence = None
    adapter.prompt_length = len(ids)

    def forward(prefix, enabled):
        """每步从完整前缀重算，避免两边缓存混用或前缀来源不明。"""
        features.check_cancel()
        adapter.reset_cache()
        adapter.edit_rules = original if enabled else []
        adapter.activation_patches = legacy if enabled else {}
        captured = None
        pre_intervention = None

        def execute(index, name, function, inputs):
            """只保留选定采集点；完整差值统计在当前层上计算。"""
            nonlocal captured, pre_intervention
            features.check_cancel()
            result = function()
            if index == layer and name == operation:
                value = result[0] if isinstance(result, tuple) else result
                captured = value.detach().float().cpu()
                if name == "mlp" and isinstance(result, tuple):
                    pre_intervention = result[1].detach().float().cpu()
            return result

        with torch.inference_mode():
            logits = adapter.forward(torch.tensor([prefix], device=adapter.device), list(range(len(prefix))), execute)
        return logits, captured, pre_intervention

    try:
        for step in range(steps):
            runtime.event("feature.progress", {"message": f"对照第 {step+1}/{steps} 步"})
            prefixes = [ids + generated[0], ids + (generated[0] if mode == "fixed" else generated[1])]
            base_logits, before, _ = forward(prefixes[0], False)
            chosen_base, _, _ = sample_logits(base_logits, prefixes[0], generation, generators[0], generated[0])
            edited_logits, after, pre = forward(prefixes[1], True)
            chosen_edited, _, _ = sample_logits(edited_logits, prefixes[1], generation, generators[1], generated[1])
            target_id = target_ids[0] if target_ids else chosen_base

            def metric(logits):
                """目标概率取原始 logits 的完整词表 softmax，排名也使用相同采集点。"""
                scores = logits[0, -1].float()
                return {"probability": float(scores.softmax(-1)[target_id]), "rank": int((scores > scores[target_id]).sum()) + 1}

            aligned = prefixes[0] == prefixes[1]
            row = {"step": step, "position": len(prefixes[0])-1, "target_id": target_id,
                "target_text": adapter.tokenizer.decode([target_id]), "base": metric(base_logits), "edited": metric(edited_logits),
                "base_token": chosen_base, "edited_token": chosen_edited, "aligned": aligned,
                "base_prefix": prefixes[0], "edited_prefix": prefixes[1]}
            if before is None or after is None:
                row.update(status="missing", reason="该采集点未执行")
            elif not aligned:
                row.update(status="different_context", reason="前缀已不同，后续差异不能解释为相同输入下的干预效果")
            elif before.shape != after.shape:
                row.update(status="shape_mismatch", reason="采集张量形状不匹配")
            else:
                delta = after - before
                row.update(status="available", shape=list(before.shape), maximum_absolute=float(delta.abs().max()),
                    mean_absolute=float(delta.abs().mean()), equal_within_tolerance=torch.allclose(before, after, atol=report["atol"], rtol=report["rtol"]))
                # 有界切片支持核查原始值；摘要明确记录切片总量和完整形状。
                start = int(payload.get("slice_start", 0))
                if start < 0 or start >= before.numel():
                    raise ProtocolError("INVALID_SLICE", "对照切片起点越界")
                stop = min(before.numel(), start+4096)
                row["slice"] = {"start": start, "stop": stop, "total": before.numel(),
                    "before": before.flatten()[start:stop].tolist(), "after": after.flatten()[start:stop].tolist(), "difference": delta.flatten()[start:stop].tolist()}
                if operation == "mlp":
                    row["channel"] = [{"position": p, "before": float(before[0,p,neuron]), "after": float(after[0,p,neuron]),
                        "edited_pre_intervention": float(pre[0,p,neuron]) if pre is not None else None} for p in range(len(prefixes[0]))]
            report["rows"].append(row)
            if divergence is None and chosen_base != chosen_edited:
                divergence = step
            generated[0].append(chosen_base)
            generated[1].append(chosen_edited)
            eos = adapter.tokenizer.eos_token_id
            if chosen_base == eos and (mode == "fixed" or chosen_edited == eos):
                break
        report.update(first_divergence=divergence, base_tokens=generated[0], edited_tokens=generated[1],
            base_text=adapter.tokenizer.decode(generated[0], skip_special_tokens=True), edited_text=adapter.tokenizer.decode(generated[1], skip_special_tokens=True))
        root = Path(payload.get("project_root", ""))
        if not payload.get("project_root") or not root.is_dir():
            raise ProtocolError("NO_PROJECT", "请先打开项目")
        path = root / "features" / ("comparison-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".json")
        path.parent.mkdir(exist_ok=True)
        atomic_json(path, report)
        return report | {"path": str(path)}
    finally:
        adapter.edit_rules = original
        adapter.activation_patches = legacy
        adapter.reset_cache()
