"""启动异常写入日志；自动验证期间不弹出系统错误框。"""
import os
from pathlib import Path
import sys
import traceback


def launch():
    """统一源码和打包入口的异常处理。"""
    try:
        from main import main
        return main()
    except Exception:
        details = traceback.format_exc()
        test_log = os.environ.get("MODEL_OBSERVATORY_TEST_LOG")
        if test_log:
            Path(test_log).write_text(details, encoding="utf-8")
        elif sys.stderr is not None:
            sys.stderr.write(details)
        else:
            log_path = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "ModelObservatory" / "startup-error.log"
            try:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                log_path.write_text(details, encoding="utf-8")
                message = f"模型观测台启动失败，详情已保存到：\n{log_path}"
            except OSError:
                message = "模型观测台启动失败，无法加载依赖。请检查随附运行环境。"
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, message, "模型观测台启动失败", 0x10)
        return 1


if __name__ == "__main__":
    raise SystemExit(launch())
