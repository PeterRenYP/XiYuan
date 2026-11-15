"""保存真实采样证据，并按具体回复与生成步解释输出。"""
from __future__ import annotations

import json
import math
from pathlib import Path

from .protocol import ProtocolError
from .traces import TraceStore


def sample_evidence(logits, scores, probabilities, chosen, hidden, head, generated):
    """只保存当前生成步的有界统计，不保存整份词表或重新执行模型。"""
    import torch
    raw = logits[0, -1].float()
    # 标量合并传回 CPU，避免逐个字段触发显卡同步。
    values = torch.stack((probabilities[chosen], (probabilities > probabilities[chosen]).sum().float() + 1,
                          (raw > raw[chosen]).sum().float() + 1, raw[chosen],
                          (raw[chosen] - raw.logsumexp(-1)).exp(), scores[chosen],
                          (probabilities > 0).sum().float(),
                          -(probabilities * probabilities.clamp_min(1e-30).log()).sum())).cpu().tolist()
    result = dict(zip(("probability", "sampling_rank", "raw_rank", "raw_logit", "raw_probability",
                       "adjusted_score", "allowed_candidates", "entropy"), values))
    for key in ("sampling_rank", "raw_rank", "allowed_candidates"):
        result[key] = int(result[key])
    result["already_generated"] = chosen in generated
    if hidden is not None and hasattr(head, "weight"):
        # 这里是该次前向的最终隐藏向量，贡献可核对到实际输出投影分数。
        products = (hidden.float() * head.weight[chosen].float()).cpu()
        positive = torch.topk(products, min(8, products.numel())).indices.tolist()
        negative = torch.topk(-products, min(8, products.numel())).indices.tolist()
        bias = float(head.bias[chosen]) if getattr(head, "bias", None) is not None else 0.0
        result["projection"] = {
            "positive": [{"index": i, "value": float(products[i])} for i in positive if products[i] > 0],
            "negative": [{"index": i, "value": float(products[i])} for i in negative if products[i] < 0],
            "sum": float(products.sum()) + bias,
            "bias": bias,
        }
    # 非有限值不进入协议或项目 JSON。
    return _finite_values(result)


def _finite_values(value):
    """递归清理嵌套贡献表中的非有限值，避免破坏整个协议帧。"""
    if isinstance(value, dict):
        return {key: _finite_values(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_finite_values(item) for item in value]
    return None if isinstance(value, float) and not math.isfinite(value) else value


class TokenLayout:
    """用实际生成编号维持显示片段，跨字节字符保留全部来源 Token。"""

    def __init__(self):
        self.pending = {"answer": [], "reasoning": []}
        self.displayed = {"answer": "", "reasoning": ""}

    def update(self, index, channel, decoded, piece):
        """完整字符出现后更新先前字节片段，续片仍可单独右键分析。"""
        if channel == "control":
            return []
        pending = self.pending[channel]
        pending.append(index)
        if decoded.endswith("\ufffd"):
            return [{"index": index, "display_piece": "字节片段", "joined_text": "等待后续字节"}]
        previous = self.displayed[channel]
        text = decoded[len(previous):] if decoded.startswith(previous) else piece
        self.displayed[channel] = decoded
        # Qt 使用 UTF-16 字符位置，补充平面字符不能直接使用 Python 的字符计数。
        start = len(previous.encode("utf-16-le")) // 2
        end = start + len(text.encode("utf-16-le")) // 2
        updates = [{"index": token_index, "display_piece": text if i == 0 else "续片",
                    "joined_text": text, "shared_tokens": list(pending), "hidden": not text,
                    "text_start": start, "text_end": end} for i, token_index in enumerate(pending)]
        pending.clear()
        return updates


def analyze_token(payload, current_store=None):
    """读取指定记录，绝不切换会话、修改权重或让模型事后编造原因。"""
    token = dict(payload.get("token", {}))
    report = {"token": token, "generation": payload.get("generation", {}), "layers": [], "attention": [],
              "notes": [], "context": payload.get("prompt_text", ""), "evidence": "会话保存的生成数据"}
    path_text = payload.get("trace_path", "")
    project_text = payload.get("project_root", "")
    store, owned = None, False
    if path_text and project_text:
        root, path = Path(project_text).resolve(), Path(path_text).resolve()
        if not path.is_relative_to(root / "sessions") or len(path.relative_to(root / "sessions").parts) != 3 or path.parent.name != "records":
            raise ProtocolError("INVALID_TRACE_PATH", "所选 Token 的记录路径不属于原项目")
        if (path / "manifest.json").is_file():
            store = current_store if current_store and current_store.root == path else TraceStore(path, readonly=True)
            owned = store is not current_store
    try:
        if store:
            if store.manifest.get("run_id") != token.get("run_id"):
                raise ProtocolError("TOKEN_SOURCE_MISMATCH", "Token 与生成记录不匹配，未分析其他回复")
            store.flush()
            with store.lock:
                row = store.db.execute(
                    "SELECT data FROM events WHERE json_extract(data,'$.name')='token.sampled' "
                    "AND json_extract(data,'$.branch_id')=? AND json_extract(data,'$.payload.position')=? LIMIT 1",
                    (token.get("branch_id", "base"), token.get("position"))).fetchone()
            if row:
                recorded = json.loads(row[0])["payload"]
                if recorded.get("id") != token.get("id") or recorded.get("forward_id") != token.get("forward_id"):
                    raise ProtocolError("TOKEN_SOURCE_MISMATCH", "Token 编号或生成步与记录不匹配")
                # 显示片段可能由后续字节补全；数值取原记录，显示对应关系保留会话快照。
                display = {key: token[key] for key in ("display_piece", "joined_text", "shared_tokens") if key in token}
                report["token"] = token = token | recorded | display
                report["evidence"] = "原始生成记录"
            else:
                report["notes"].append("原始事件未保存，使用会话保留的采样数据。")
            # 同一记录可能包含不同分支，优先使用该次回复保存的生效配置。
            if not report["generation"]:
                report["generation"] = store.manifest.get("generation", {})
            prompts = store.manifest.get("messages", [])
            report["context"] = next((m["content"] for m in reversed(prompts) if m.get("role") == "user"), report["context"])
            _internal_evidence(store, token, report)
        else:
            report["notes"].append("该次观测记录不存在或已清理；仅分析会话中保存的生成数据。")
        _explain(report)
        return _finite_values(report)
    finally:
        if owned:
            store.close()


def _internal_evidence(store, token, report):
    """只查看该 Token 的预测前向，不误用把它再次送入模型后的激活。"""
    import torch
    config = store.manifest.get("model", {}).get("config", {})
    count = int(config.get("num_hidden_layers", 0))
    branch, forward = token.get("branch_id", "base"), token.get("forward_id", "f0")
    position = int(token.get("position", 0)) - 1
    layers = sorted({max(0, min(count - 1, int(token.get("capture_layer", 0)))), count - 1}) if count else []
    for layer in layers:
        context, refs = store.get_operation(branch, forward, layer, "residual")
        hidden = store.load(refs.get("output"))
        if context and hidden is not None and position in context.get("positions", []):
            row = hidden[0, context["positions"].index(position)].float()
            top = torch.topk(row.abs(), min(5, row.numel())).indices.tolist()
            report["layers"].append({"layer": layer, "rms": float(row.square().mean().sqrt()),
                                     "channels": [{"index": i, "value": float(row[i])} for i in top]})
        context, refs = store.get_operation(branch, forward, layer, "attention")
        weights = store.load(refs.get("weights"))
        if context and weights is not None and weights.ndim == 4 and position in context.get("positions", []):
            values = weights[0, :, context["positions"].index(position), :].float().mean(0)
            top = torch.topk(values, min(5, values.numel())).indices.tolist()
            offsets = store.manifest.get("offsets", [])
            template = store.manifest.get("template", "")
            report["attention"].append({"layer": layer, "positions": [
                {"position": i, "weight": float(values[i]),
                 "text": template[offsets[i][0]:offsets[i][1]] if i < len(offsets) else "生成历史位置 " + str(i)} for i in top]})
    if not report["layers"]:
        report["notes"].append("预测该 Token 的层激活未采集。")
    if not report["attention"]:
        report["notes"].append("预测该 Token 的注意力关联未采集；递归注意力层没有可直接等同的注意力概率矩阵。")


def _explain(report):
    """把真实概率与采样机制组织为中文说明，区分输出贡献和上下文关联。"""
    token, generation = report["token"], report["generation"]
    probability, rank = token.get("probability"), token.get("sampling_rank")
    lines = []
    if probability is None:
        lines.append("这条历史回复没有保存所选 Token 的完整采样概率，无法还原其选择过程。")
    elif generation.get("temperature") == 0:
        lines.append(f"本次采用确定性选择。所选 Token 在调整后的分数中排名第 {rank}，模型选择最高分输出。")
    else:
        lines.append(f"所选 Token 在实际采样分布中排名第 {rank}，被抽中的概率为 {probability:.4%}。")
        lines.append("本次按照采样分布随机抽取，因此输出不必是概率最高的候选。" if rank and rank > 1 else "本次抽中了采样分布中概率最高的候选。")
    if token.get("raw_rank") is not None:
        lines.append(f"采样处理前的词表分数排名为第 {token['raw_rank']}；经过温度、惩罚和候选筛选后，保留 {token.get('allowed_candidates', '未保存')} 个非零概率候选。")
    if token.get("already_generated"):
        lines.append("这个 Token 在本轮此前已经出现，存在惩罚与重复惩罚的实际配置见采样设置。")
    projection = token.get("projection", {})
    if projection:
        lines.append("输出通道表列出最终隐藏向量与所选词表权重逐项相乘的贡献：正值提高该 Token 的输出分数，负值降低分数。")
    else:
        report["notes"].append("输出通道贡献未保存。")
    if report["attention"]:
        lines.append("上下文关联表展示预测前向中注意力较高的位置；它表示关联强度，不能单独解释为因果贡献。")
    report["explanation"] = "\n".join(lines)
