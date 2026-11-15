"""编排 1.0.2 开发回归，仅写入隔离的测试目录，不触碰发布目录。"""
from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))


def create_fixture(output):
    """以真实小模型生成可复查的数值块与 token 记录，不模拟 logits 或内部张量。"""
    import torch
    from tokenizers import Tokenizer
    from tokenizers.models import WordLevel
    from tokenizers.pre_tokenizers import Whitespace
    from transformers import PreTrainedTokenizerFast
    from observatory.model import QwenAdapter
    from observatory.protocol import Emitter, command
    from observatory.storage import atomic_json
    from observatory.worker import Runtime
    from test_project import frames, stop
    from test_runtime import create_local_model

    model_path = output / "测试模型"
    model, original = create_local_model(model_path, layers=2)
    vocabulary = {"[UNK]": 0, "[EOS]": 1, "你好": 2, "用户": 3, "模型": 4}
    vocabulary.update({f"片段{i}": i for i in range(5, 32)})
    backend = Tokenizer(WordLevel(vocabulary, unk_token="[UNK]"))
    backend.pre_tokenizer = Whitespace()
    tokenizer = PreTrainedTokenizerFast(tokenizer_object=backend, unk_token="[UNK]", eos_token="[EOS]", pad_token="[EOS]")
    tokenizer.chat_template = original.chat_template
    tokenizer.save_pretrained(model_path)
    with torch.no_grad():
        # 此测试模型使用可见词条，防止未登记编号解码为空导致界面测试没有输出片段。
        model.lm_head.weight[:2].zero_()
    model.save_pretrained(model_path)
    project = output / "后端项目"
    atomic_json(project / "project.json", {"storage": {"total_mib": 0, "run_mib": 0, "reserve_mib": 256, "auto_clean": False}})
    stream = io.StringIO()
    runtime = Runtime(Emitter("maintenance-fixture", stream))
    try:
        runtime.adapter = QwenAdapter.load(model_path, "cpu", "float32")
        runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}], "mode": "observe",
            "generation": {"max_new_tokens": 4, "temperature": 0, "seed": 42}, "project_root": str(project),
            "trace_root": str(project / "sessions" / ("f" * 32) / "records"), "capture": {"policy": "full", "disk_budget_bytes": 0}}))
        values = [json.loads(line) for line in stream.getvalue().splitlines()]
        fixture = {"model_path": str(model_path), "trace_path": str(runtime.store.root), "project_root": str(project),
            "started": frames(stream, "run.started")[0], "finished": frames(stream, "run.finished")[-1],
            "events": [value for value in values if value["name"] in ("token.sampled", "text.updated", "run.finished")]}
        (output / "后端生成记录.json").write_text(json.dumps(fixture, ensure_ascii=False, indent=2), encoding="utf-8")
    finally:
        stop(runtime)


def run_ui(output, platform, extended):
    """使用独立设置与项目运行真实 Qt 控件，保存检查结果、截图和错误输出。"""
    directory = output / (platform + ("-专项" if extended else "-项目"))
    directory.mkdir(parents=True, exist_ok=True)
    if not extended:
        atomic = directory / "验证模型/config.json"
        atomic.parent.mkdir(parents=True, exist_ok=True)
        atomic.write_text(json.dumps({"num_hidden_layers": 2, "intermediate_size": 40, "hidden_size": 16,
                                      "max_position_embeddings": 8192}), encoding="utf-8")
    env = os.environ | {"QT_QPA_PLATFORM": platform, "QT_PLUGIN_PATH": str(ROOT / ".cache/qt/6.8.3/mingw_64/plugins"),
                        "MODEL_OBSERVATORY_ROOT": str(ROOT), "PYTHONUTF8": "1"}
    env["PATH"] = str(ROOT / ".cache/qt/6.8.3/mingw_64/bin") + ";" + str(ROOT / ".cache/qt/Tools/mingw1310_64/bin") + ";" + env["PATH"]
    flag = "--verify-maintenance" if extended else "--verify-project"
    with (directory / "运行日志.txt").open("w", encoding="utf-8") as log:
        process = subprocess.run([str(ROOT / "work/client-cpp/Xiyuan.exe"), flag, str(directory)],
                                 env=env, stdout=log, stderr=log, timeout=60)
    report_path = directory / ("专项界面验证.json" if extended else "项目验证.json")
    report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.is_file() else {"pass": False, "error": "未生成检查报告"}
    result = {"exit_code": process.returncode, "report": str(report_path), "pass": process.returncode == 0 and report["pass"],
              "checks": len(report.get("checks", {})), "failed": [key for key, value in report.get("checks", {}).items() if value is False]}
    print(platform, flag, json.dumps(result, ensure_ascii=False), flush=True)
    return result


def main():
    """一次测试对应一个新目录，避免旧结果被误当成本次通过证据。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("offscreen", "windows", "both"), default="both")
    parser.add_argument("--skip-backend", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = {}
    if not args.skip_backend:
        with (output / "后端测试日志.txt").open("w", encoding="utf-8") as log:
            result = subprocess.run([sys.executable, "-m", "pytest", "tests", "-q", "--junitxml=" + str(output / "后端测试.xml")],
                                    cwd=ROOT, stdout=log, stderr=log, env=os.environ | {"PYTHONUTF8": "1"}, timeout=180)
        summary["backend"] = {"pass": result.returncode == 0, "exit_code": result.returncode}
        print("backend", result.returncode, flush=True)
    create_fixture(output)
    for platform in ("offscreen", "windows") if args.platform == "both" else (args.platform,):
        for extended in (False, True):
            summary[platform + ("_extended" if extended else "_project")] = run_ui(output, platform, extended)
    (output / "测试汇总.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if all(item["pass"] for item in summary.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
