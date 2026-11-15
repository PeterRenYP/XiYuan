"""在真实 Qwen3.5 上只读验证搜索、相似向量、全层统计及推理对照。"""
import io
import json
from pathlib import Path
import sys
import torch

from observatory.model import QwenAdapter, inspect_model
from observatory.protocol import Emitter, command
from observatory.worker import Runtime


def main():
    """模型只读加载，测试产物全部写入指定独立项目。"""
    model_path, output = Path(sys.argv[1]), Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    project = output / "项目"
    project.mkdir(exist_ok=True)
    (project / "project.json").write_text(json.dumps({"storage": {"total_mib": 512, "run_mib": 128}}), encoding="utf-8")
    runtime = Runtime(Emitter("read-only-real", io.StringIO()))
    report = {}
    try:
        runtime.adapter = QwenAdapter.load(model_path, "cuda:0", "bfloat16")
        adapter = runtime.adapter
        original_hashes = adapter.info["hashes"].copy()
        token = adapter.tokenizer.encode("天空", add_special_tokens=False)[0]
        search = runtime.features.dispatch({"action": "search", "query": str(token), "exact": True})
        vector = runtime.features.dispatch({"action": "vector", "token_id": token})
        similar = runtime.features.dispatch({"action": "similar", "token_id": token, "limit": 5})
        runtime.run(command("run.start", {"messages": [{"role": "user", "content": "天空为什么是蓝色的？"}], "mode": "observe",
            "generation": {"max_new_tokens": 3, "temperature": 0}, "project_root": str(project.resolve()),
            "trace_root": str((project / "records").resolve()), "capture": {"policy": "standard", "layer": 0, "disk_budget_bytes": 128 * 1024**2}}))
        ranking = runtime.features.dispatch({"action": "activation", "scope": "input", "metric": "normalized"})
        # 从实际排名选中通道，在相同前缀下核对干预前后，而非只比较最终文本。
        candidate=runtime.features.dispatch({"action":"activation","scope":"input","metric":"normalized","layer":0})["rows"][0]
        options={"action":"comparison","project_root":str(project.resolve()),"layer":candidate["layer"],"neuron":candidate["neuron"],"operation":"mlp","steps":2,"target":str(token)}
        unchanged=runtime.features.dispatch(options|{"mode":"fixed"})
        adapter.edit_rules=[{"layer_index":candidate["layer"],"neuron_index":candidate["neuron"],"kind":"zero","value":0.,"scope":"all","token_position":0}]
        fixed=runtime.features.dispatch(options|{"mode":"fixed"})
        free=runtime.features.dispatch(options|{"mode":"free"})
        if free["first_divergence"] is None:
            # 增强同一候选以产生可见分歧，验证分歧后的上下文标记。
            adapter.edit_rules[0].update(kind="scale",value=1000.)
            free=runtime.features.dispatch(options|{"mode":"free","steps":3})
        channels=[v for row in fixed["rows"] for v in row.get("channel",[])]
        zeroed=bool(channels) and all(v["after"]==0 for v in channels) and any(abs(v["edited_pre_intervention"] or 0)>0 for v in channels)
        detail=runtime.features.dispatch({"action":"activation.detail","layer":candidate["layer"],"neuron":candidate["neuron"]})
        adapter.edit_rules=[]
        regression = runtime.features.dispatch({"action": "regression", "project_root": str(project.resolve()),
            "samples": [{"input": "你好", "target": "好", "max_new_tokens": 2}]})
        report = {"model": str(model_path), "layers": adapter.config.num_hidden_layers, "ranking_layers": ranking["layers"],
            "effective_tokens": ranking["effective_tokens"], "token_search": search["rows"][0]["id"] == token,
            "vector_dimension": vector["dimension"], "similar_self": similar["rows"][0]["id"] == token,
            "no_edit_baseline_equal": regression["rows"][0]["base"] == regression["rows"][0]["edited"],
            "source_hashes_unchanged": inspect_model(model_path, hash_weights=True)["hashes"] == original_hashes,
            "ranking": ranking["rows"][:5],"fixed_prefix_channel_zeroed":zeroed,
            "no_intervention_within_tolerance":all(r.get("equal_within_tolerance") for r in unchanged["rows"]),
            "fixed_target_metrics":all("probability" in r["base"] and "rank" in r["edited"] for r in fixed["rows"]),
            "raw_candidate_available":bool(detail["rows"]),"free_first_divergence":free["first_divergence"],
            "free_context_marking":all(r["status"]=="different_context" for r in free["rows"] if not r["aligned"]),
            "comparison_reports":[unchanged["path"],fixed["path"],free["path"]]}
        report["pass"] = all(report[k] for k in ("token_search","similar_self","no_edit_baseline_equal","source_hashes_unchanged","fixed_prefix_channel_zeroed","no_intervention_within_tolerance","fixed_target_metrics","raw_candidate_available","free_context_marking")) and report["ranking_layers"] == report["layers"]
        print(json.dumps({k: v for k, v in report.items() if k != "ranking"}, ensure_ascii=False))
    finally:
        (output / "真实模型验证.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        runtime.handle(command("shutdown"))
        runtime.thread.join(10)
    if not report.get("pass"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
