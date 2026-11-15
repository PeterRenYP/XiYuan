"""对照真实四十亿模型的官方缓存推理，并验证问候能结束思考并输出回答。"""
import argparse
import io
import json
from pathlib import Path
import sqlite3
import sys

import torch
from transformers.models.qwen3_5.modeling_qwen3_5 import Qwen3_5DynamicCache

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from observatory.model import QwenAdapter
from observatory.protocol import Emitter, command
from observatory.worker import Runtime


def main():
    """只读原模型和问题记录；报告及少量新运行索引写入隔离验证目录。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    adapter = QwenAdapter.load(args.model, "cuda:0", "auto")
    original = json.loads((args.trace / "manifest.json").read_text("utf-8"))
    with sqlite3.connect((args.trace / "index.sqlite").resolve().as_uri() + "?mode=ro", uri=True) as connection:
        samples = [event["payload"]["id"] for row in connection.execute("SELECT data FROM events ORDER BY seq")
                   if (event := json.loads(row[0]))["name"] == "token.sampled"]
    checks, metrics = {}, {}
    reference_cache = Qwen3_5DynamicCache(adapter.config)
    ids = original["token_ids"]
    differences, same_argmax, finite = [], 0, True
    # 使用同一个已加载模型、两份独立缓存、同一token序列，排除采样随机性和权重副本差异。
    with torch.inference_mode():
        for step in range(min(len(samples), 512)):
            input_ids = torch.tensor([ids if step == 0 else [samples[step - 1]]], device=adapter.device)
            positions = list(range(len(ids))) if step == 0 else [len(ids) + step - 1]
            expected = adapter.model(input_ids=input_ids, past_key_values=reference_cache,
                                     use_cache=True, logits_to_keep=1).logits
            actual = adapter.forward(input_ids, positions, lambda layer, operation, function, inputs: function())
            difference = (actual.float() - expected.float()).abs().max().item()
            differences.append(difference)
            same_argmax += int(actual.argmax(-1).item() == expected.argmax(-1).item())
            finite &= bool(torch.isfinite(actual).all() and torch.isfinite(expected).all())
            if step % 64 == 0:
                print(f"官方路径对照：{step + 1} 步，当前最大差异 {max(differences):.6f}", flush=True)
    metrics["reference_comparison"] = {"steps": len(differences), "max_logit_difference": max(differences),
        "argmax_agreement": same_argmax / len(differences), "dtype": str(next(adapter.backbone.parameters()).dtype)}
    checks["真实模型五百一十二步与官方推理一致"] = finite and len(differences) == 512 and max(differences) <= .125 and same_argmax / len(differences) >= .99
    del reference_cache, expected, actual
    adapter.reset_cache()
    torch.cuda.empty_cache()
    stream = io.StringIO()
    runtime = Runtime(Emitter("thinking-verification", stream))
    runtime.adapter = adapter
    cases = {"首次问候": [{"role": "user", "content": "你好"}],
             "中断后再次问候": [{"role": "user", "content": "你好"},
                 {"role": "assistant", "content": "", "reasoning_content": "Thinking Process:\n\n1", "thinking_complete": False},
                 {"role": "user", "content": "你好"}]}
    try:
        for name, messages in cases.items():
            stream.seek(0)
            stream.truncate()
            runtime.run(command("run.start", {"messages": messages, "mode": "normal",
                "trace_root": str(args.output / "records"), "generation": {"max_new_tokens": 1024,
                "seed": 42, "template_options": {"enable_thinking": True}}}))
            finished = [json.loads(line)["payload"] for line in stream.getvalue().splitlines()
                        if json.loads(line)["name"] == "run.finished"][-1]
            checks[name + "结束思考并给出正式回答"] = bool(finished["thinking_complete"] and finished["text"].strip() and finished["reason"] == "eos")
            metrics[name] = {key: finished[key] for key in ("reason", "forward_count", "text", "reasoning_content", "thinking_complete")}
            metrics[name]["effective_generation"] = runtime.store.manifest["generation"]
            if name == "中断后再次问候":
                checks["中断历史没有空助手或历史思考"] = all(message["role"] != "assistant" for message in runtime.store.manifest["messages"])
            print(name, finished["reason"], finished["forward_count"], finished["text"], flush=True)
    finally:
        runtime.handle(command("shutdown"))
        runtime.thread.join(10)
    result = {"pass": all(checks.values()), "checks": checks, "metrics": metrics}
    (args.output / "思考真实模型验证.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), "utf-8")
    assert result["pass"], result


if __name__ == "__main__":
    main()
