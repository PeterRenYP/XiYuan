"""在隔离目录运行会话修复的原生界面与真实运行器验收。"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    """统一开发版和分发版的环境，保留日志与结构化检查结果。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path, default=ROOT / "work/client-cpp/Xiyuan.exe")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("offscreen", "windows"), default="offscreen")
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--plan", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ | {
        "QT_QPA_PLATFORM": args.platform,
        "MODEL_OBSERVATORY_ROOT": str(ROOT),
        "MODEL_OBSERVATORY_SETTINGS_DIR": str(output / "独立设置"),
        "SESSION_TEST_MODEL": str(ROOT / "artifacts/v1.0.2-release/global-playback-regression/测试模型"),
        "PYTHONUTF8": "1",
    }
    env["PATH"] = str(ROOT / ".cache/qt/6.8.3/mingw_64/bin") + ";" + str(ROOT / ".cache/qt/Tools/mingw1310_64/bin") + ";" + env["PATH"]
    if args.runtime:
        flag, name = "--verify-sessions-runtime", "真实生成切换验收.json"
    elif args.plan:
        flag, name = "--verify-plan-ui", "计划界面验收.json"
    else:
        flag, name = "--verify-sessions", "会话修复验收.json"
    with (output / "运行日志.txt").open("w", encoding="utf-8") as log:
        result = subprocess.run([str(args.target.resolve()), flag, str(output)], cwd=ROOT,
                                env=env, stdout=log, stderr=log, timeout=90)
    report = json.loads((output / name).read_text(encoding="utf-8"))
    failed = [key for key, value in report.get("checks", {}).items() if value is False]
    print(json.dumps({"exit_code": result.returncode, "pass": report.get("pass"),
        "checks": len(report.get("checks", {})), "failed": failed,
        "error": report.get("error"), "details": report.get("details", {}), "report": str(output / name)}, ensure_ascii=False), flush=True)
    raise SystemExit(0 if result.returncode == 0 and report.get("pass") else 1)


if __name__ == "__main__":
    main()
