"""复用批准原型，构建带分块压缩载荷的独立 Windows 安装器。"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work/installer"
PROTOTYPE = ROOT / "installer/prototype.html"
MAGIC = b"XIYUAN_SETUP_V2!"


def prepare_html():
    """保留原型布局与动画，仅替换模拟宿主、目录和安装进度。"""
    html = PROTOTYPE.read_text(encoding="utf-8")
    html = html.replace('aria-label="析元安装器交互原型"', 'aria-label="析元安装器"')
    html = html.replace('<span class="xy-preview-label">交互预览</span>', '<span class="xy-preview-label">1.0.4</span>')
    html = html.replace("安装演示已结束", "安装完成").replace("演示完成", "析元已安装").replace("安装演示进度", "安装进度")
    html = re.sub(r"    function remember\(\).*?\n", "    function remember(){}\n", html)
    html = re.sub(r"    function restore\(snapshot\).*?\n", "    function restore(){}\n", html)
    html = re.sub(r"    function startInstall\(\).*?\n", "    function startInstall(){pendingInstall={action:'install',...state};native(pendingInstall);}\n", html)
    html = re.sub(r"    function confirmOverwrite\(\).*?\n", "    function confirmOverwrite(){const confirmed=replacement;dismissModal();if(!confirmed)return;state.path=confirmed.path;state.step=3;state.progress=0;render();pendingInstall={action:'install',...state,confirm_replace:confirmed.signature};native(pendingInstall);replacement=null;}\n", html)
    html = html.replace('${escapeText(pathError(state.path))}</div>`;', '${escapeText(pathError(state.path))}</div>${existingPath?\'<p class="xy-subtitle">已检测到现有析元，将使用原目录更新。</p>\':\'\'}`;')
    html = html.replace("function openModal(kind){modal=kind;", "function openModal(kind){if(kind==='browse'){native({action:'browse',path:state.path});return;}modal=kind;")
    html = html.replace("取消安装演示？", "取消安装？").replace("退出预览？", "退出安装？")
    html = html.replace("case 'confirm-exit':clearInterval", "case 'confirm-exit':native({action:'close'});return;clearInterval")
    html = html.replace("case 'finish':state.closed=true;", "case 'finish':native({action:'finish'});return;state.closed=true;")
    html = html.replace("restore(window.openai?.widgetState);render(false);", "native({action:'ready'});render(false);")
    html = re.sub(r"    window.addEventListener\('openai:set_globals'.*?\n", "", html)
    bridge = """
    // 宿主只接受本地页面的固定动作，安装进度来自实际落盘字节。
    let pendingInstall=null,existingPath='';
    function native(value){window.chrome.webview.postMessage(value);}
    // 强制关闭只作用于本次重试，不保存为安装偏好；关闭前明确提示未保存内容风险。
    function retryInstall(force){
      if(!pendingInstall)return;
      dismissModal();state.step=3;state.progress=0;render();
      native({...pendingInstall,force_close:force});
    }
    root.addEventListener('click',event=>{
      const action=event.target.closest('button')?.dataset.action;
      if(action==='retry-install')retryInstall(false);
      if(action==='force-install')retryInstall(true);
    });
    window.chrome.webview.addEventListener('message',event=>{
      const value=event.data;
      if(value.type==='init'){state.path=value.path;existingPath=value.existing?value.path:'';const input=root.querySelector('#xy-path');if(input){input.value=state.path;validateInput();}}
      if(value.type==='folder'){state.path=value.path;const input=root.querySelector('#xy-path');if(input){input.value=state.path;validateInput();}}
      if(value.type==='progress'){state.progress=value.progress;updateProgress();const label=root.querySelector('#xy-progress-label');if(label&&value.message)label.textContent=value.message;}
      if(value.type==='complete'){state.progress=100;state.step=4;render();}
      if(value.type==='overwrite'){replacement=value;state.path=value.path;state.step=2;state.progress=0;render().then(()=>openModal('overwrite'));}
      if(value.type==='busy'){state.step=2;render().then(()=>{
        openModal('busy');const dialog=overlay.querySelector('.xy-dialog');dialog.setAttribute('aria-label','处理程序占用');
        const programs=(value.programs||[]).map(program=>escapeText(program.name)+'（进程编号：'+program.pid+'）').join('<br>');
        dialog.innerHTML='<h2>程序文件正在使用</h2><p style="margin-top:12px;overflow-wrap:anywhere">'+escapeText(value.message)+'</p>'+(programs?'<p style="margin-top:12px">'+programs+'</p>':'<p style="margin-top:12px">请手动关闭占用文件的其他程序后重试。</p>')+(value.can_force?'<p style="margin-top:12px">强制关闭可能丢失未保存的内容。</p>':'')+'<div class="xy-dialog-actions"><button type="button" class="xy-button" data-action="dismiss">返回</button><button type="button" class="xy-button" data-action="retry-install">重试</button>'+(value.can_force?'<button type="button" class="xy-button primary" data-action="force-install">强制关闭后继续</button>':'')+'</div>';
        dialog.querySelector('button').focus();
      });}
      if(value.type==='error'){state.step=2;render().then(()=>{openModal('error');overlay.querySelector('.xy-dialog').innerHTML='<h2>安装未完成</h2><p style="margin-top:12px;word-break:break-word">'+escapeText(value.message)+'</p><div class="xy-dialog-actions"><button type="button" class="xy-button primary" data-action="dismiss">返回</button></div>';overlay.querySelector('button').focus();});}
      if(value.type==='closing')native({action:'close'});
    });
    root.addEventListener('pointerdown',event=>{if(event.target.closest('.xy-titlebar')&&!event.target.closest('button'))native({action:'drag'});});
"""
    html = html.replace("    const reduced=", bridge + "\n    const reduced=")
    lucide = (ROOT / ".cache/installer/lucide.min.js").read_text(encoding="utf-8")
    # 页面仅使用内嵌资源，安装时不访问图标 CDN。
    css = "html,body{margin:0;background:#25282d;overflow:hidden}#xy-installer{padding:0!important}#xy-installer .xy-window{border-radius:0;box-shadow:none}"
    document = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>' + css + '</style><script>' + lucide + '</script>' + html + '</html>'
    (WORK / "installer.html").write_text(document, encoding="utf-8")


def compile_host():
    """宿主静态链接系统外运行库，启动时无需先安装 Qt 或 Python。"""
    WORK.mkdir(parents=True, exist_ok=True)
    # 源码下载后可直接准备依赖，不再要求复制开发机器的缓存。
    subprocess.run([sys.executable, str(ROOT/'installer/prepare_dependencies.py')], check=True)
    prepare_html()
    resource = '#include <windows.h>\n1 ICON "' + (ROOT / 'client-cpp/assets/xiyuan.ico').as_posix() + '"\n'
    for number, file in [(101, WORK / "installer.html"), (102, ROOT / ".cache/webview2/WebView2Loader.dll"), (103, ROOT / ".cache/installer/MicrosoftEdgeWebview2Setup.exe")]:
        resource += f'{number} RCDATA "{file.as_posix()}"\n'
    resource += '1 24 "' + (ROOT / 'installer/setup.manifest').as_posix() + '"\n'
    # 安装程序在资源管理器的文件属性中也显示统一产品版本。
    resource += '''
1 VERSIONINFO
FILEVERSION 1,0,4,0
PRODUCTVERSION 1,0,4,0
FILEFLAGSMASK 0x3fL
FILEOS 0x40004L
FILETYPE 1
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "080404b0"
  BEGIN
   VALUE "FileDescription", L"\\x6790\\x5143\\x5b89\\x88c5\\x7a0b\\x5e8f\\0"
   VALUE "FileVersion", L"1.0.4\\0"
   VALUE "ProductName", L"\\x6790\\x5143\\0"
   VALUE "ProductVersion", L"1.0.4\\0"
   VALUE "OriginalFilename", L"XiyuanSetup-1.0.4.exe\\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x0804, 1200
 END
END
'''
    (WORK / "resources.rc").write_text(resource, encoding="utf-8")
    compiler = ROOT / ".cache/qt/Tools/mingw1310_64/bin"
    # 资源编译器会调用同目录预处理器，构建不依赖调用终端的 PATH。
    os.environ['PATH'] = str(compiler) + os.pathsep + os.environ.get('PATH', '')
    subprocess.run([str(compiler / "windres.exe"), "-c", "65001", str(WORK / "resources.rc"), str(WORK / "resources.o")], check=True)
    subprocess.run([str(compiler / "g++.exe"), "-std=c++17", "-O2", "-municode", "-mwindows", "-static", "-DUNICODE", "-D_UNICODE", "-I"+str(ROOT / ".cache/webview2"), "-I"+str(ROOT / ".cache/installer"), str(ROOT / "installer/setup.cpp"), str(WORK / "resources.o"), "-o", str(WORK / "XiyuanSetup-host.exe"), "-lole32", "-loleaut32", "-luuid", "-lshell32", "-lshlwapi", "-lcomctl32", "-lbcrypt", "-lcabinet", "-ladvapi32"], check=True)


def build_payload(source, destination):
    """压缩后均分载荷，生成配套的主程序与数据包，并限制单文件大小。"""
    cabinet = ctypes.WinDLL("cabinet.dll", use_last_error=True)
    cabinet.CreateCompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    cabinet.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    cabinet.CloseCompressor.argtypes = [ctypes.c_void_p]
    handle = ctypes.c_void_p()
    if not cabinet.CreateCompressor(4, None, ctypes.byref(handle)):
        raise ctypes.WinError(ctypes.get_last_error())
    files = sorted(p for p in source.rglob("*") if p.is_file() and p.name not in ("ModelObservatory.exe", "Install-ModelObservatory.ps1"))
    manifest = {"schema":2,"version":"1.0.4","files":[],"total":sum(p.stat().st_size for p in files)}
    destination.parent.mkdir(parents=True, exist_ok=True)
    payload_start = (WORK / "XiyuanSetup-host.exe").stat().st_size
    companion = destination.with_suffix('.data')
    try:
        # 临时流只用于构建；安装时直接跨两文件读取，无需合并大文件。
        with tempfile.TemporaryFile(dir=WORK) as output:
            done = 0
            for path in files:
                item = {"path":path.relative_to(source).as_posix(),"size":path.stat().st_size,"blocks":[]}
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    while raw := stream.read(4*1024**2):
                        digest.update(raw)
                        buffer = ctypes.create_string_buffer(len(raw)+1024*1024)
                        size = ctypes.c_size_t()
                        if not cabinet.Compress(handle, raw, len(raw), buffer, len(buffer), ctypes.byref(size)):
                            raise ctypes.WinError(ctypes.get_last_error())
                        compressed = buffer.raw[:size.value]
                        stored = compressed if len(compressed)<len(raw) else raw
                        item["blocks"].append([payload_start+output.tell(),len(stored),len(raw),int(stored is compressed)])
                        output.write(stored)
                item["sha256"] = digest.hexdigest()
                manifest["files"].append(item)
                done += item["size"]
                if len(manifest["files"]) % 250 == 0:
                    print(f"payload {len(manifest['files'])}/{len(files)} {done*100//manifest['total']}%", flush=True)
            # 分界可落在压缩块中间，清单仍采用连续的逻辑偏移。
            payload_size = output.tell()
            first_size = (payload_size + 1) // 2
            manifest['primary_size'] = payload_start + first_size
            manifest['data'] = {'size':payload_size-first_size, 'sha256':''}
            # 提前检查保守清单大小，避免留下超过 Releases 限制的半成品。
            estimate = len(json.dumps(manifest, ensure_ascii=False).encode()) + 1024
            if max(payload_start+first_size+estimate+64, payload_size-first_size) >= 2*1024**3:
                raise ValueError('压缩后的载荷过大，无法生成两个均小于 2 GiB 的文件')
            shutil.copyfile(WORK / 'XiyuanSetup-host.exe', destination)
            output.seek(0)
            with destination.open('ab') as primary:
                remaining = first_size
                while remaining:
                    chunk = output.read(min(4*1024**2, remaining))
                    primary.write(chunk)
                    remaining -= len(chunk)
                digest = hashlib.sha256()
                with companion.open('wb') as data:
                    while chunk := output.read(4*1024**2):
                        data.write(chunk)
                        digest.update(chunk)
                manifest['data']['sha256'] = digest.hexdigest()
                encoded = json.dumps(manifest, ensure_ascii=False, separators=(',',':')).encode()
                primary.write(encoded)
                primary.write(struct.pack('<QQ32s16s',payload_start,len(encoded),hashlib.sha256(encoded).digest(),MAGIC))
    finally:
        cabinet.CloseCompressor(handle)
    # 校验文件同时列出两部分，便于用户核对下载内容。
    checksums = []
    for path in (destination, companion):
        if path.stat().st_size >= 2*1024**3:
            raise ValueError('安装文件超过 2 GiB 限制')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        checksums.append(f'{digest}  {path.name}\n')
        print(str(path), path.stat().st_size, flush=True)
    destination.with_suffix('.sha256').write_text(''.join(checksums), encoding='utf-8')


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-only",action="store_true")
    parser.add_argument("--source",type=Path,default=ROOT / "dist/ModelObservatoryCpp")
    parser.add_argument("--output",type=Path,default=ROOT / "dist/XiyuanSetup-1.0.4.exe")
    arguments = parser.parse_args()
    compile_host()
    if not arguments.host_only:
        build_payload(arguments.source.resolve(),arguments.output.resolve())
