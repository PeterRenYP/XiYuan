"""解压最终安装包，逐文件校验并验证安装包内客户端的会话功能。"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    """流式计算文件哈希，避免将大型安装包一次读入内存。"""
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    """只向新的隔离目录解压，不运行系统安装或覆盖现有客户端。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--setup", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    setup = args.setup.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    extracted = output / "解压验证"
    checks = {}
    with setup.open("rb") as stream:
        stream.seek(-64, 2)
        start, size, expected, magic = struct.unpack("<QQ32s16s", stream.read(64))
        stream.seek(-64 - size, 2)
        encoded = stream.read(size)
    assert magic == b"XIYUAN_SETUP_V1!" and hashlib.sha256(encoded).digest() == expected
    manifest = json.loads(encoded)
    assert manifest["version"] == "1.0.2" and start > 0
    checks["安装包版本与清单摘要正确"] = True
    subprocess.run([str(setup), "--extract-only", str(extracted), "--test"],
                   check=True, timeout=240, creationflags=subprocess.CREATE_NO_WINDOW)
    print("安装包解压完成，开始逐文件哈希校验", flush=True)
    for file in manifest["files"]:
        path = (extracted / file["path"]).resolve()
        assert path.is_relative_to(extracted) and path.stat().st_size == file["size"]
        assert digest(path) == file["sha256"], str(path)
    checks["解压文件逐个哈希一致"] = True
    checks["安装包客户端与最终构建一致"] = digest(extracted / "Xiyuan.exe") == digest(ROOT / "dist/ModelObservatoryCpp/Xiyuan.exe")
    build = json.loads((extracted / "build-manifest.json").read_text(encoding="utf-8"))
    checks["内置本轮验收证据全部通过"] = len(build["validation"]) == 5 and all(report["pass"] for report in build["validation"].values())
    for arguments in ([], ["--runtime"]):
        name = "安装包真实生成" if arguments else "安装包界面"
        subprocess.run([sys.executable, str(ROOT / "tools/run_session_checks.py"),
                        "--target", str(extracted / "Xiyuan.exe"), "--output", str(output / name), *arguments], check=True)
        checks[name] = True
    report = {"pass": all(checks.values()), "checks": checks, "version": "1.0.2",
              "files": len(manifest["files"]), "bytes": setup.stat().st_size,
              "sha256": digest(setup), "setup": str(setup)}
    (output / "安装包验收.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False), flush=True)
    assert report["pass"]


if __name__ == "__main__":
    main()
