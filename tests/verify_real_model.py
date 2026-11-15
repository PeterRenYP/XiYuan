"""在用户指定的真实模型与 GPU 上生成可复查验证证据。"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import threading
import time

import torch
from transformers.cache_utils import DynamicCache

from observatory.model import QwenAdapter
from observatory.protocol import Emitter, command
from observatory.worker import Runtime


def main():
    """验证真实加载、数值对照、采集、编辑重算、单步与独立监控。"""
    root = Path(__file__).resolve().parent.parent
    output = root / "artifacts"
    output.mkdir(exist_ok=True)
    hybrid = "--qwen35" in sys.argv
    result = {"model": "D:/models/Qwen3.5-4B" if hybrid else "D:/models/models/Qwen__Qwen2.5-3B-Instruct", "tests": {}}
    adapter = QwenAdapter.load(result["model"], "cuda:0", "auto")
    print("MODEL_READY", flush=True)
    result["model_info"] = adapter.info
    tokenizer = adapter.tokenizer
    prompt = tokenizer.apply_chat_template([{"role": "user", "content": "中国的首都是哪里？请简短回答。"}], tokenize=False, add_generation_prompt=True)
    ids = tokenizer(prompt, add_special_tokens=False)["input_ids"]
    if hybrid:
        from transformers.models.qwen3_5.modeling_qwen3_5 import Qwen3_5DynamicCache
        reference_cache = Qwen3_5DynamicCache(adapter.config)
    else:
        reference_cache = DynamicCache(config=adapter.config)
    differences = []
    with torch.inference_mode():
        for step, inputs in enumerate((ids, [100], [200])):
            positions = list(range(len(ids))) if step == 0 else [len(ids) + step - 1]
            tensor = torch.tensor([inputs], device="cuda:0")
            expected = adapter.model(tensor, past_key_values=reference_cache, use_cache=True, logits_to_keep=1).logits
            actual = adapter.forward(tensor, positions, lambda layer, op, fn, values: fn())
            error = (expected.float() - actual.float()).abs()
            differences.append({"step": step, "max_abs": float(error.max()), "mean_abs": float(error.mean())})
            torch.testing.assert_close(expected, actual, atol=.02, rtol=.01)
    adapter.reset_cache()
    del reference_cache, expected, actual, tensor
    torch.cuda.empty_cache()
    result["tests"]["real_model_parity"] = {"pass": True, "dtype": "bfloat16", "steps": differences}
    print("PARITY_PASS", differences, flush=True)
    events = []
    log = (output / ("qwen35-runtime-events.ndjson" if hybrid else "runtime-events.ndjson")).open("w", encoding="utf-8")
    runtime = Runtime(Emitter("integration-runtime", log))
    runtime.adapter = adapter
    original_listener = runtime.emitter.listener
    def listener(message):
        """保留最近事件供等待验证，同时按正式路径可靠落盘。"""
        original_listener(message)
        events.append(message)
    runtime.emitter.listener = listener
    request = command("run.start", {"messages": [{"role": "user", "content": "中国的首都是哪里？请简短回答。"}],
        "mode": "observe", "generation": {"seed": 42, "max_new_tokens": 4, "temperature": 0, "repetition_penalty": 1.05},
        "capture": {"policy": "standard", "disk_budget_bytes": 4 * 1024**3}, "trace_root": str(output / "real-traces")})
    started = time.monotonic()
    runtime.run(request)
    finished = [e for e in events if e["name"] == "run.finished"][-1]["payload"]
    assert finished["outcome"] == "completed" and finished["forward_count"] == 4
    result["tests"]["real_observation"] = {"pass": True, "elapsed_seconds": round(time.monotonic() - started, 2), **finished}
    selection = dict(branch_id="base", forward_id="f0", layer_index=13, token_position=len(ids) - 1, neuron_index=8948, operation="mlp", head=15, dimension=2047, weight_kind="down")
    view = runtime.query(selection)
    assert len(view["neuron"]["rank"]) == 8
    assert view["attention"]["shape"][1] == (32 if hybrid else 16)
    cached = runtime.query(selection | {"forward_id": "f1", "token_position": len(ids)})
    if hybrid:
        assert cached["attention"]["type"] == "linear"
        full = runtime.query(selection | {"layer_index": 15, "forward_id": "f1", "token_position": len(ids)})
        assert full["attention"]["shape"][-2:] == [1, len(ids) + 1]
    else:
        assert cached["attention"]["shape"][-2:] == [1, len(ids) + 1]
    count = runtime.forward_count
    runtime.query(selection)
    assert runtime.forward_count == count
    original_neuron = view["neuron"]["value"]
    _, original_refs = runtime.store.get_operation("base", "f0", -1, "logits")
    original_logits = runtime.store.load(original_refs["output"]).clone()
    runtime.apply_patch(command("patch.apply", dict(layer_index=13, neuron_index=8948, token_position=len(ids) - 1,
                            kind="zero", value=0, expected_weights_revision="w0")))
    branch = runtime.branch_id
    edited = runtime.query(selection | {"branch_id": branch})
    assert edited["neuron"]["value"] == 0
    assert runtime.query(selection)["neuron"]["value"] == original_neuron
    _, refs = runtime.store.get_operation(branch, "f0", -1, "logits")
    edited_logits = runtime.store.load(refs["output"])
    result["tests"]["real_neuron_branch"] = {"pass": True, "branch": branch, "original_activation": original_neuron,
        "edited_activation": edited["neuron"]["value"], "logits_max_abs_change": float((original_logits.float() - edited_logits.float()).abs().max()),
        "old_snapshot_unchanged": True, "cache_rebuilt": True}
    result["tests"]["real_tensor_views"] = {"pass": True, "neuron_count": adapter.config.intermediate_size, "decode_attention_shape": cached["attention"]["shape"], "view_reads_do_not_forward": True}
    print("OBSERVATION_AND_PATCH_PASS", flush=True)
    # 使用真实运算线程检查：等待中主前向次数和完成操作数保持不变。
    step_request = command("run.start", request["payload"] | {"mode": "step"})
    thread = threading.Thread(target=runtime.run, args=(step_request,))
    thread.start()
    deadline = time.monotonic() + 10
    while (not runtime.controller or not runtime.controller.waiting) and time.monotonic() < deadline:
        time.sleep(.01)
    controller = runtime.controller
    assert controller and controller.waiting
    revision = controller.revision
    time.sleep(.4)
    assert controller.executed == 0 and runtime.forward_count == 0
    monitor = subprocess.Popen([sys.executable, str(root / "main.py"), "--monitor"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8")
    try:
        metric = json.loads(monitor.stdout.readline())["payload"]
        assert metric["status"] == "available" and "4070" in metric["name"]
    finally:
        monitor.terminate()
        monitor.wait(5)
    controller.release("release-real-1", controller.boundary_id, revision)
    deadline = time.monotonic() + 10
    while controller.revision == revision and time.monotonic() < deadline:
        time.sleep(.01)
    assert controller.executed == 1 and runtime.forward_count == 0
    controller.cancel()
    thread.join(10)
    assert not thread.is_alive()
    assert [e for e in events if e["name"] == "run.finished"][-1]["payload"]["outcome"] == "cancelled"
    result["tests"]["real_step_and_monitor"] = {"pass": True, "operations_before_release": 0, "operations_after_one_release": 1, "cancel_unblocked": True, "gpu": metric}
    runtime.handle(command("shutdown"))
    runtime.thread.join(10)
    log.close()
    (output / ("4B真实模型验证.json" if hybrid else "真实模型验证.json")).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("ALL_REAL_CHECKS_PASS", flush=True)


if __name__ == "__main__":
    main()
