"""安装载荷损坏与卸载保留用户文件的独立验收。"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys


def main():
    """仅卸载测试目录中带有效安装清单的本轮安装。"""
    setup=Path(sys.argv[1]).resolve();installed=Path(sys.argv[2]).resolve();output=Path(sys.argv[3]).resolve();output.mkdir(parents=True,exist_ok=False)
    checks={}
    with setup.open('rb') as stream:
        stream.seek(-64,2);start,size,digest,magic=struct.unpack('<QQ32s16s',stream.read(64));stream.seek(-64-size,2);manifest=json.loads(stream.read(size))
    checks['安装载荷包含文件哈希']=all(len(f['sha256'])==64 for f in manifest['files'])
    for file in manifest['files']:
        path=installed/file['path']
        with path.open('rb') as stream:actual=hashlib.file_digest(stream,'sha256').hexdigest()
        assert actual==file['sha256'],path
    checks['真实安装文件逐个哈希一致']=True
    broken=output/'损坏安装包.exe';shutil.copy2(setup,broken)
    companion=setup.with_suffix('.data');broken_data=broken.with_suffix('.data')
    if companion.exists():
        # 数据包保持原样，只用硬链接配对损坏的主程序，避免复制大文件。
        os.link(companion,broken_data)
    with broken.open('r+b') as stream:
        offset=next(b[0] for f in manifest['files'] for b in f['blocks']);stream.seek(offset);value=stream.read(1);stream.seek(offset);stream.write(bytes([value[0]^255]))
    target=output/'损坏解压'
    try:
        result=subprocess.run([str(broken),'--extract-only',str(target),'--test'],timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
    finally:
        # 损坏副本只供本次验证使用，避免保留数 GiB 的重复安装包。
        broken.unlink(missing_ok=True)
        broken_data.unlink(missing_ok=True)
    assert result.returncode!=0 and not target.exists();checks['损坏载荷拒绝并清理']=True
    # 清单之外新增的用户文件在卸载后仍须保留。
    user=installed/'用户模型'/'保留.txt';user.parent.mkdir(exist_ok=True);user.write_text('用户数据',encoding='utf-8')
    result=subprocess.run([str(setup),'--remove-root',str(installed),'--test'],timeout=60,creationflags=subprocess.CREATE_NO_WINDOW)
    assert result.returncode==0 and user.read_text(encoding='utf-8')=='用户数据' and not (installed/'Xiyuan.exe').exists()
    checks['卸载保留用户新增文件']=True
    if os.environ.get('XIYUAN_CLEAN_INSTALL_TEST')=='1':
        # 保留行为验证后，仅清理由本脚本创建的标记文件和空目录。
        user.unlink();user.parent.rmdir();installed.rmdir()
        checks['清理本次验收目录']=True
    (output/'安装器存储验收.json').write_text(json.dumps({'pass':all(checks.values()),'checks':checks},ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(checks,ensure_ascii=False))


if __name__=='__main__':main()
