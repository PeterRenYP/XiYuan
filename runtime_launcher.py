"""C++ 客户端专用运行器入口，不导入 Python 图形界面。"""
import sys


def main():
    """在 Windows 冻结进程中统一 UTF-8 管道，再分流计算与监控。"""
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    if "--monitor" in sys.argv:
        from observatory.telemetry import main as run
    elif "--worker" in sys.argv:
        from observatory.worker import main as run
    elif "--file-worker" in sys.argv:
        from observatory.weightfiles import main as run
    else:
        print("此程序由析元客户端启动，请运行 Xiyuan.exe。", file=sys.stderr)
        return 2
    return run()


if __name__ == "__main__":
    raise SystemExit(main())
