"""在硬链接隔离副本上验证两个真实模型的原目录保存和重载。"""
from __future__ import annotations

import gc
import json
import os
from pathlib import Path
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import torch
from observatory.editing import prepare_weights, save_current_model, validate_rules
from observatory.model import QwenAdapter, inspect_model


def clone_file(source, target):
    """权重只建立硬链接；写回使用原子替换，不会写穿用户原模型。"""
    if str(source).endswith(".safetensors"):
        os.link(source, target)
    else:
        shutil.copy2(source, target)
    return target


def main():
    """逐个加载实际模型，保存后对比重载数值并校验原文件未变。"""
    root = Path(__file__).resolve().parent.parent
    output = root / "artifacts/cpp-models"
    output.mkdir(parents=True, exist_ok=True)
    result = {}
    for name, source in (("4B", Path("D:/models/Qwen3.5-4B")), ("3B", Path("D:/models/models/Qwen__Qwen2.5-3B-Instruct"))):
        started = time.monotonic()
        destination = output / (name + "-" + str(time.time_ns()))
        original = inspect_model(source, hash_weights=True)
        shutil.copytree(source, destination, copy_function=clone_file,
                        ignore=shutil.ignore_patterns("observatory-backups", "observatory-save-journal.json"))
        print(name, "加载隔离副本", flush=True)
        adapter = QwenAdapter.load(destination, "cuda:0", "bfloat16")
        ids = torch.tensor([[2, 8, 9]], device=adapter.device)
        rules = validate_rules(adapter, [dict(layer_index=13, neuron_index=10, projection="down", kind="zero", value=0)], "model")
        print(name, "写回隔离副本", flush=True)
        saved = save_current_model(adapter, prepare_weights(adapter, rules),
            lambda event, payload: print(name, payload["message"], flush=True))
        with torch.inference_mode():
            expected = adapter.forward(ids, [0, 1, 2], lambda layer, op, fn, values: fn()).cpu()
        del adapter
        gc.collect()
        torch.cuda.empty_cache()
        print(name, "重新加载并对照", flush=True)
        reloaded = QwenAdapter.load(destination, "cuda:0", "bfloat16")
        assert torch.count_nonzero(reloaded.parameter(13, "down")[:, 10]).item() == 0
        with torch.inference_mode():
            actual = reloaded.forward(ids, [0, 1, 2], lambda layer, op, fn, values: fn()).cpu()
        error = (expected.float() - actual.float()).abs().max().item()
        assert error == 0
        del reloaded, ids, actual, expected
        gc.collect()
        torch.cuda.empty_cache()
        assert inspect_model(source, hash_weights=True)["hashes"] == original["hashes"]
        result[name] = dict(pass_test=True, reload_max_abs_error=error, original_hashes_unchanged=True,
            saved=saved, seconds=round(time.monotonic() - started, 2))
        (output / "真实模型原目录保存验证.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(name, "通过", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
