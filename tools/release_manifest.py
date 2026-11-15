"""记录精确依赖、源码和构建结果，并随包保留上游许可文本。"""
from __future__ import annotations

import hashlib
import importlib.metadata as metadata
import json
from pathlib import Path
import platform


def main():
    """为当前分发生成可复查清单；不声称已经完成独立法律审计。"""
    root = Path(__file__).resolve().parent.parent
    output = root / "dist/ModelObservatory"
    notices = output / "third-party-notices"
    notices.mkdir(parents=True, exist_ok=True)
    packages = []
    for distribution in metadata.distributions():
        name = distribution.metadata["Name"]
        entry = {"name": name, "version": distribution.version, "license": distribution.metadata.get("License-Expression") or distribution.metadata.get("License"), "notices": []}
        for item in distribution.files or []:
            if item.name.lower().startswith(("license", "copying", "notice")):
                source = Path(distribution.locate_file(item))
                if source.is_file() and source.stat().st_size < 5 * 1024**2:
                    filename = name + "-" + str(item).replace("/", "_").replace("\\", "_") + ".txt"
                    (notices / filename).write_bytes(source.read_bytes())
                    entry["notices"].append(filename)
        packages.append(entry)
    hashes = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in list((root / "observatory").glob("*.py")) + [root / "main.py", root / "launcher.py", output / "ModelObservatory.exe", output / "ModelObservatory-Runtime.exe"] if path.exists()}
    manifest = {"application": "模型观测台", "version": "1.1.0", "python": platform.python_version(), "platform": platform.platform(), "target": ["Qwen2.5-3B-Instruct", "Qwen3.5-4B（文本）"], "hashes": hashes, "dependencies": packages,
                "validation": "本机验证结果见项目 artifacts 目录；尚未完成干净机器与多显示器测试", "model_weights_included": False}
    (output / "build-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    (output / "使用说明.txt").write_text("模型观测台 1.1\n\n双击 ModelObservatory.exe。点击“选择模型目录”或选择近期模型，再点击加载或切换。支持 Qwen2.5-3B-Instruct 与 Qwen3.5-4B。\n已选目录与已加载模型分别显示；运行时先停止，再切换模型。配置中可设置设备、精度与采样参数。\nQwen3.5 的线性注意力层展示真实递归状态，全注意力层展示 Softmax 权重；本版仅提供文本输入，视觉权重保留在 CPU 并随模型导出。\n正常对话直接生成；观测模式自动运行；逐步模式使用按钮或 F10 放行一次真实运算。\n神经元页可读取实际激活与编辑，修改会建立新分支并重新计算。\n默认记录预算为 2 GiB，达到预算后未保存的张量标为未采集。\n记录、配置和日志保存在当前用户的本地应用数据目录。\nModelObservatory-Runtime.exe 和 _internal 必须与主程序保留在同一目录。\n分发包含 CUDA 12.8 用户态运行库，NVIDIA 显卡仍需兼容驱动；CPU 可加载小模型。\n激活干预不导出为模型参数，参数修改可导出到新目录；原模型不被覆盖。\n第三方许可文本见 third-party-notices；模型使用权遵循模型自身许可。\n", encoding="utf-8")


if __name__ == "__main__":
    main()
