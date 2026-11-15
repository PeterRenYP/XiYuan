# -*- mode: python ; coding: utf-8 -*-
"""原生 UI 与控制台运行器共享一份可携带依赖目录。"""
from PyInstaller.utils.hooks import collect_data_files, copy_metadata

datas = collect_data_files("transformers")
for distribution in ("torch", "transformers", "tokenizers", "safetensors", "huggingface-hub", "nvidia-ml-py", "regex", "requests", "tqdm", "numpy", "packaging", "PyYAML", "filelock"):
    datas += copy_metadata(distribution)

a = Analysis(["launcher.py"], pathex=[], binaries=[], datas=datas,
    hiddenimports=["pynvml", "transformers.models.qwen2.modeling_qwen2", "transformers.models.qwen2.configuration_qwen2",
                   "observatory.qwen35", "transformers.models.qwen3_5.modeling_qwen3_5", "transformers.models.qwen3_5.configuration_qwen3_5",
                   "transformers.models.qwen2.tokenization_qwen2_fast", "tokenizers", "safetensors.torch"],
    hookspath=[], hooksconfig={}, runtime_hooks=[],
    excludes=["tensorflow", "jax", "flax", "matplotlib", "pandas", "scipy", "IPython", "pytest", "torchvision", "torchaudio", "cv2", "sklearn", "triton"],
    noarchive=False)
pyz = PYZ(a.pure)
# 图形入口无控制台，运行器入口提供可靠 stdin/stdout 管道。
desktop = EXE(pyz, a.scripts, [], exclude_binaries=True, name="ModelObservatory", console=False, disable_windowed_traceback=True)
runtime = EXE(pyz, a.scripts, [], exclude_binaries=True, name="ModelObservatory-Runtime", console=True)
collection = COLLECT(desktop, runtime, a.binaries, a.datas, strip=False, upx=False, name="ModelObservatory")
