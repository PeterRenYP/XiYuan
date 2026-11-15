"""用小型双文件安装包验收跨文件读取、损坏保护和旧格式兼容。"""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess
import sys


def main():
    """所有失败场景只改测试副本，成功解压后逐个核对原始文件哈希。"""
    setup = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve()
    output = Path(sys.argv[3]).resolve()
    output.mkdir(parents=True, exist_ok=False)
    checks = {}
    with setup.open('rb') as stream:
        stream.seek(-64, 2)
        host, size, digest, magic = struct.unpack('<QQ32s16s', stream.read(64))
        stream.seek(-64-size, 2)
        encoded = stream.read(size)
    manifest = json.loads(encoded)
    assert magic == b'XIYUAN_SETUP_V2!'
    assert hashlib.sha256(encoded).digest() == digest
    boundary = manifest['primary_size']
    assert any(b[0] < boundary < b[0]+b[1] for f in manifest['files'] for b in f['blocks'])
    checks['载荷包含跨文件压缩块'] = True
    assert all(p.stat().st_size < 2*1024**3 for p in (setup, setup.with_suffix('.data')))
    checks['两部分均小于2GiB'] = True
    primary = setup.read_bytes()
    companion = setup.with_suffix('.data').read_bytes()

    def run(label, exe_bytes, data_bytes, success=False, existing=False):
        """恢复配套文件，运行正式解包入口，并检查失败不会留下或改写目标。"""
        # 每个场景使用独立可执行文件，避免系统扫描仍持有上一次运行的文件。
        case_setup = output/'中文 空格目录'/label/'测试安装包.exe'
        case_setup.parent.mkdir(parents=True)
        case_data = case_setup.with_suffix('.data')
        case_setup.write_bytes(exe_bytes)
        case_data.unlink(missing_ok=True)
        if data_bytes is not None:
            case_data.write_bytes(data_bytes)
        target = output/label
        if existing:
            target.mkdir()
            (target/'用户内容.txt').write_text('保留原内容', encoding='utf-8')
        result = subprocess.run([str(case_setup), '--extract-only', str(target), '--test'],
                                timeout=40, creationflags=subprocess.CREATE_NO_WINDOW)
        if success:
            assert result.returncode == 0, label
            for entry in manifest['files']:
                actual = hashlib.sha256((target/entry['path']).read_bytes()).hexdigest()
                assert actual == entry['sha256'], entry['path']
                assert (target/entry['path']).read_bytes() == (source/entry['path']).read_bytes()
            shutil.rmtree(target)
        elif existing:
            assert result.returncode != 0
            assert (target/'用户内容.txt').read_text(encoding='utf-8') == '保留原内容'
            assert len(list(target.iterdir())) == 1
        else:
            assert result.returncode != 0 and not target.exists(), label
        checks[label] = True

    run('中文路径完整解压及逐文件校验', primary, companion, success=True)
    run('缺少数据包', primary, None)
    run('截断数据包', primary, companion[:-1])
    run('混入同大小错误数据包', primary, bytes(len(companion)))
    changed = bytearray(companion)
    changed[len(changed)//2] ^= 255
    run('数据包内容损坏', primary, changed)
    changed_primary = bytearray(primary)
    changed_primary[host] ^= 255
    run('主程序载荷损坏后清理暂存', changed_primary, companion)
    run('失败保留原目标目录', primary, None, existing=True)
    run('正确双包拒绝覆盖既有解压目录', primary, companion, existing=True)
    run('补齐数据包后可重试', primary, companion, success=True)
    # 将两段载荷重建为旧格式，仅验证新宿主仍能读取历史单文件安装包。
    legacy_manifest = dict(manifest)
    legacy_manifest['schema'] = 1
    legacy_manifest.pop('data')
    legacy_manifest.pop('primary_size')
    legacy_encoded = json.dumps(legacy_manifest, ensure_ascii=False, separators=(',', ':')).encode()
    legacy = primary[:boundary]+companion+legacy_encoded+struct.pack(
        '<QQ32s16s', host, len(legacy_encoded), hashlib.sha256(legacy_encoded).digest(), b'XIYUAN_SETUP_V1!')
    run('兼容旧版单文件载荷', legacy, None, success=True)
    report = {'pass': all(checks.values()), 'checks': checks}
    (output/'双文件安装验收.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(checks, ensure_ascii=False))


if __name__ == '__main__':
    main()
