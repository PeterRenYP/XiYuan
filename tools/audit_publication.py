"""按 Git 忽略规则检查拟公开源码，不初始化项目仓库，也不输出凭据内容。"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SECRET_PATTERNS = {
    'GitHub访问凭据': r'\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{60,})\b',
    '模型服务访问凭据': r'\b(?:sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}|hf_[A-Za-z0-9]{30,})\b',
    '云服务访问凭据': r'\bAKIA[0-9A-Z]{16}\b',
    '私钥内容': r'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----',
    '明文凭据赋值': r'''(?i)\b(?:api[_-]?key|access[_-]?token|client[_-]?secret|password)\b["']?\s*[:=]\s*["'][A-Za-z0-9_./+=-]{16,}["']''',
    '个人主目录': r'(?:[A-Za-z]:[/\\]Users[/\\](?!Public\b|Default\b|\[|<)[^/\\\s\"\']+|/Users/[A-Za-z][^/\s\"\']*)',
}


def public_files():
    """借助临时 Git 元数据读取真实忽略结果，项目目录保持未建仓状态。"""
    (ROOT/'work').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='publication-audit-', dir=ROOT/'work') as temporary:
        metadata = Path(temporary)/'metadata.git'
        subprocess.run(['git', 'init', '--bare', '--quiet', str(metadata)], check=True)
        result = subprocess.run(['git', '--git-dir='+str(metadata), '--work-tree='+str(ROOT),
                                 'ls-files', '--others', '--exclude-standard', '-z'],
                                cwd=ROOT, capture_output=True, check=True)
        return sorted(p.decode('utf-8') for p in result.stdout.split(b'\0') if p)


def main():
    """核对大文件、常见凭据、公开文档链接，以及打包必需文件。"""
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT/'artifacts/publication-check')
    args = parser.parse_args()
    files = public_files()
    findings = []
    sizes = {}
    required = ('README.md', 'LICENSE', 'runtime-cpp.spec', 'mcp-standalone.spec',
                'installer/prepare_dependencies.py', 'client-cpp/assets/xiyuan.ico')
    for name in required:
        if name not in files:
            findings.append({'path': name, 'rule': '构建或发布必需文件被排除'})
    for name in files:
        path = ROOT/name
        sizes[name] = path.stat().st_size
        if sizes[name] >= 100*1024**2:
            findings.append({'path': name, 'rule': '超过GitHub普通文件大小限制'})
        if sizes[name] > 8*1024**2:
            continue
        try:
            content = path.read_text(encoding='utf-8-sig')
        except UnicodeError:
            continue
        for rule, pattern in SECRET_PATTERNS.items():
            for match in re.finditer(pattern, content):
                # 报告仅记录位置和规则，凭据内容始终留在原文件。
                findings.append({'path': name, 'line': content.count('\n', 0, match.start())+1, 'rule': rule})
    for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', (ROOT/'README.md').read_text(encoding='utf-8')):
        if '://' not in target and not target.startswith('#'):
            name = target.split('#')[0]
            if not (ROOT/name).exists() or ((ROOT/name).is_file() and name not in files):
                findings.append({'path': 'README.md', 'target': name, 'rule': '公开文档链接缺失'})
    forbidden = ('.venv/', '.venv-mcp/', '.cache/', 'work/', 'dist/', 'artifacts/', 'data/', '开发附件/', 'previews/')
    for name in files:
        if name.startswith(forbidden):
            findings.append({'path': name, 'rule': '本机目录进入公开范围'})
    report = {'pass': not findings, 'file_count': len(files), 'total_bytes': sum(sizes.values()),
              'findings': findings, 'largest_files': sorted(sizes.items(), key=lambda item: item[1], reverse=True)[:10]}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output/'公开内容检查.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    (output/'公开文件清单.json').write_text(json.dumps(files, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))
    raise SystemExit(0 if report['pass'] else 1)


if __name__ == '__main__':
    main()
