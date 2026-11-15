# -*- mode: python ; coding: utf-8 -*-
"""C++ 客户端的独立计算运行器；不收集 Python 图形界面或 Qt 库。"""
from PyInstaller.utils.hooks import collect_data_files, copy_metadata

datas = collect_data_files("transformers")
for distribution in ("torch", "transformers", "tokenizers", "safetensors", "huggingface-hub", "nvidia-ml-py", "regex", "requests", "tqdm", "numpy", "packaging", "PyYAML", "filelock"):
    datas += copy_metadata(distribution)
a = Analysis(["runtime_launcher.py"], pathex=[], binaries=[], datas=datas,
    hiddenimports=["pynvml", "observatory.editing", "observatory.qwen35", "observatory.features", "observatory.versions", "observatory.neurons", "observatory.anomaly_sampling",
        "transformers.models.qwen2.modeling_qwen2", "transformers.models.qwen2.configuration_qwen2",
        "transformers.models.qwen3_5.modeling_qwen3_5", "transformers.models.qwen3_5.configuration_qwen3_5",
        "transformers.models.qwen2.tokenization_qwen2_fast", "tokenizers", "safetensors.torch"],
    hookspath=[], hooksconfig={}, runtime_hooks=[],
    excludes=["PySide6", "PyQt6", "PyQt5", "tensorflow", "jax", "flax", "matplotlib", "pandas", "scipy", "IPython", "pytest", "torchvision", "torchaudio", "cv2", "sklearn", "triton"], noarchive=False)
pyz = PYZ(a.pure)
runtime = EXE(pyz, a.scripts, [], exclude_binaries=True, name="ModelObservatory-Runtime", console=True)
collection = COLLECT(runtime, a.binaries, a.datas, strip=False, upx=False, name="ModelObservatoryCpp")
