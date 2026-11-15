"""从已安装的 Uninstall.exe 入口验证真实卸载，确认框仅操作本次测试进程。"""
from __future__ import annotations
import ctypes
from ctypes import wintypes
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    """确认自有测试窗口，等待后台卸载完成，再清除本次产生的临时卸载器。"""
    root=Path(sys.argv[1]).resolve();uninstaller=root/'Uninstall.exe'
    digest=hashlib.sha256(uninstaller.read_bytes()).hexdigest()
    marker=root/'用户保留.txt';marker.write_text('卸载后保留',encoding='utf-8')
    process=subprocess.Popen([str(uninstaller),'--uninstall','--test'],creationflags=subprocess.CREATE_NO_WINDOW)
    user=ctypes.WinDLL('user32',use_last_error=True)
    callback_type=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
    user.EnumWindows.argtypes=[callback_type,wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
    user.GetClassNameW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
    user.PostMessageW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM]
    confirmed=False

    @callback_type
    def confirm(window,unused):
        """按进程编号和标准对话框类型匹配，不触碰其他窗口或系统审批。"""
        nonlocal confirmed
        pid=wintypes.DWORD();user.GetWindowThreadProcessId(window,ctypes.byref(pid))
        name=ctypes.create_unicode_buffer(80);user.GetClassNameW(window,name,len(name))
        if pid.value==process.pid and name.value=='#32770':
            user.PostMessageW(window,0x111,6,0);confirmed=True
        return True

    try:
        deadline=time.monotonic()+15
        while time.monotonic()<deadline and not confirmed:
            user.EnumWindows(confirm,0);time.sleep(.05)
        assert confirmed,'未找到本次卸载确认窗口'
        assert process.wait(timeout=15)==0
        while time.monotonic()<deadline and ((root/'Xiyuan.exe').exists() or uninstaller.exists()):time.sleep(.05)
        assert not (root/'Xiyuan.exe').exists() and not uninstaller.exists(),'卸载入口未清理程序'
        assert marker.read_text(encoding='utf-8')=='卸载后保留'
        marker.unlink()
        temporary=Path(tempfile.gettempdir())/f'XiyuanUninstall-{process.pid}.exe'
        for _ in range(50):
            try:
                if temporary.exists():
                    assert hashlib.sha256(temporary.read_bytes()).hexdigest()==digest
                    temporary.unlink()
                break
            except PermissionError:time.sleep(.05)
        print('独立卸载入口、后台卸载及用户文件保留通过。')
    finally:
        if process.poll() is None:process.kill();process.wait()


if __name__=='__main__':main()
