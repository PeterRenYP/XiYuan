"""构建析元完整分发版，内置 Python、推理依赖与独立 MCP。"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]


def run(arguments,env=None):
    """每个构建步骤失败立即停止，避免发布缺少运行库的目录。"""
    subprocess.run([str(v) for v in arguments],cwd=ROOT,env=env,check=True)


def main():
    """可跳过未改动运行器的重新分析，最终目录仍检查内置 Python。"""
    parser=argparse.ArgumentParser();parser.add_argument("--skip-runtime",action="store_true");parser.add_argument("--setup",action="store_true");args=parser.parse_args()
    qt=ROOT/".cache/qt/6.8.3/mingw_64";compiler=ROOT/".cache/qt/Tools/mingw1310_64/bin"
    env=os.environ.copy();env["PATH"]=str(compiler)+os.pathsep+str(qt/"bin")+os.pathsep+env["PATH"]
    python=ROOT/".venv/Scripts/python.exe";cmake=ROOT/".venv/Scripts/cmake.exe";output=ROOT/"dist/ModelObservatoryCpp"
    # 首次克隆后也能配置工程，完整分发同时构建独立监控进程。
    run([cmake,"-S",ROOT/"client-cpp","-B",ROOT/"work/client-cpp","-G","Ninja","-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_PREFIX_PATH="+str(qt),"-DCMAKE_CXX_COMPILER="+str(compiler/"g++.exe"),
         "-DCMAKE_MAKE_PROGRAM="+str(ROOT/".venv/Scripts/ninja.exe")],env)
    run([cmake,"--build",ROOT/"work/client-cpp","-j","4"],env)
    if not args.skip_runtime:
        runtime_env=os.environ.copy();runtime_env.pop("PYTHONPATH",None)
        runtime_env["PATH"]=str(python.parent)+os.pathsep+sys.base_prefix+os.pathsep+os.path.join(os.environ.get("SystemRoot",r"C:\Windows"),"System32")
        run([python,"-m","PyInstaller","--noconfirm","--clean","--workpath","work/runtime-cpp","--distpath","dist","runtime-cpp.spec"],runtime_env)
        mcp_python=ROOT/".venv-mcp/Scripts/python.exe"
        if not mcp_python.is_file():raise RuntimeError("请先运行 setup-mcp.ps1 -Build，准备独立 MCP 打包环境")
        run([mcp_python,"-m","PyInstaller","--noconfirm","--clean","--workpath","work/mcp-standalone","--distpath","dist/mcp-standalone","mcp-standalone.spec"],runtime_env)
    for name in ("ModelObservatory-Runtime.exe","_internal/python312.dll","_internal/torch/lib/torch_cpu.dll"):
        if not (output/name).is_file():raise RuntimeError("分发包缺少内置运行依赖："+name)
    shutil.copy2(ROOT/"work/client-cpp/Xiyuan.exe",output/"Xiyuan.exe")
    shutil.copy2(ROOT/"work/client-cpp/Xiyuan-Monitor.exe",output/"Xiyuan-Monitor.exe")
    shutil.copy2(ROOT/"dist/mcp-standalone/Xiyuan-MCP.exe",output/"Xiyuan-MCP.exe")
    run([qt/"bin/windeployqt.exe","--release","--compiler-runtime","--no-translations","--no-opengl-sw","--no-system-d3d-compiler",output/"Xiyuan.exe"],env)
    shutil.copy2(qt/"plugins/platforms/qoffscreen.dll",output/"platforms/qoffscreen.dll")
    run([python,ROOT/"tools/native_manifest.py"],env)
    if args.setup:run([python,ROOT/"installer/build.py"],env)
    print(output/"Xiyuan.exe",flush=True)


if __name__=="__main__":main()
