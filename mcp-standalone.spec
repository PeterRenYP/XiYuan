# -*- mode: python ; coding: utf-8 -*-
"""独立 MCP 服务不包含推理库，安装后无需用户另装 Python。"""
from PyInstaller.utils.hooks import collect_submodules, copy_metadata

# 独立服务不使用 SDK 的命令行入口，避免引入可选 CLI 依赖。
hidden=collect_submodules("mcp", filter=lambda name: name != "mcp.cli" and not name.startswith("mcp.cli."))
datas=copy_metadata("mcp")+copy_metadata("jsonschema")+copy_metadata("jsonschema-specifications")
a=Analysis(["run_mcp.py"],pathex=[],binaries=[],datas=datas,hiddenimports=hidden,
    hookspath=[],hooksconfig={},runtime_hooks=[],excludes=["torch","transformers","numpy","PySide6","PyQt6","pytest"],noarchive=False)
pyz=PYZ(a.pure)
exe=EXE(pyz,a.scripts,a.binaries,a.datas,[],name="Xiyuan-MCP",console=True,icon="client-cpp/assets/xiyuan.ico",upx=False)
