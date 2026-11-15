"""析元入口；桌面交给 C++，工作进程直接分流。"""
from __future__ import annotations

import os
from pathlib import Path
import sys


def main():
    """启动原生桌面、真实推理工作进程或独立显卡监控。"""
    # 冻结后的 Windows EXE 不读取 PYTHONUTF8，必须显式统一管道编码。
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    if "--worker" in sys.argv:
        from observatory.worker import main as worker_main
        return worker_main()
    if "--file-worker" in sys.argv:
        from observatory.weightfiles import main as file_main
        return file_main()
    if "--monitor" in sys.argv:
        from observatory.telemetry import main as monitor_main
        return monitor_main()
    # 默认桌面入口已迁移为 C++；Python 只承担独立运行器与兼容启动。
    import subprocess
    root = Path(__file__).resolve().parent
    packaged = root / "dist/ModelObservatoryCpp/Xiyuan.exe"
    development = root / "work/client-cpp/Xiyuan.exe"
    executable = packaged if packaged.exists() else development
    if not executable.exists():
        print("C++ 客户端尚未构建，请运行 build.ps1。", file=sys.stderr)
        return 2
    environment = os.environ.copy()
    if executable == development:
        qt = root / ".cache/qt/6.8.3/mingw_64"
        compiler = root / ".cache/qt/Tools/mingw1310_64/bin"
        environment["PATH"] = str(qt / "bin") + os.pathsep + str(compiler) + os.pathsep + environment.get("PATH", "")
        environment["QT_PLUGIN_PATH"] = str(qt / "plugins")
    return subprocess.call([str(executable), *sys.argv[1:]], env=environment)


if __name__ == "__main__":
    raise SystemExit(main())
