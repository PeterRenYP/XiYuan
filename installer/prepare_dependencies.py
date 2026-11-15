"""下载安装器构建依赖，固定 SDK 与前端库版本，支持从空缓存准备。"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import time
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WEBVIEW_VERSION = '1.0.2903.40'
WEBVIEW_SHA256 = 'ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74'


def download(url: str, destination: Path, expected: str | None = None):
    """复用有效缓存，先下载到临时文件，校验成功后再替换目标。"""
    if destination.is_file():
        if expected is None or hashlib.sha256(destination.read_bytes()).hexdigest() == expected:
            return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.download')
    try:
        print('下载构建依赖：' + destination.name, flush=True)
        request = urllib.request.Request(url, headers={'User-Agent': 'Xiyuan-build/1.0.4'})
        # 下载站点偶尔重置连接时重试，每次都覆盖临时文件，避免拼接残缺内容。
        for attempt in range(3):
            try:
                with urllib.request.urlopen(request, timeout=30) as response, temporary.open('wb') as output:
                    shutil.copyfileobj(response, output)
                break
            except (OSError, urllib.error.URLError) as error:
                if attempt == 2:
                    raise RuntimeError('构建依赖下载失败：' + destination.name + '，请检查网络后重试。') from error
                time.sleep(attempt+1)
        if expected and hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
            raise RuntimeError('构建依赖校验失败：' + destination.name)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def prepare_dependencies(cache: Path):
    """只提取编译所需的 x64 SDK 文件；运行库引导程序从微软官方地址获取。"""
    installer = cache/'installer'
    sdk = cache/'webview2'
    package = installer/'microsoft.web.webview2.nupkg'
    download(f'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/{WEBVIEW_VERSION}', package, WEBVIEW_SHA256)
    sdk.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(package) as archive:
        for member in ('build/native/include/WebView2.h', 'build/native/x64/WebView2Loader.dll'):
            (sdk/Path(member).name).write_bytes(archive.read(member))
    # JSON 头文件也从 NuGet 固定版本获取，减少对原始代码下载站点的依赖。
    json_header = installer/'json.hpp'
    json_sha256 = '9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6'
    if not json_header.is_file() or hashlib.sha256(json_header.read_bytes()).hexdigest() != json_sha256:
        json_package = installer/'nlohmann.json.nupkg'
        download('https://www.nuget.org/api/v2/package/nlohmann.json/3.11.3', json_package,
                 '70db5f6adce6852393468c7cfb540a9232a57f3aa12757f11e29f0767f9eba68')
        with zipfile.ZipFile(json_package) as archive:
            header = archive.read('build/native/include/nlohmann/json.hpp')
        if hashlib.sha256(header).hexdigest() != json_sha256:
            raise RuntimeError('JSON 头文件校验失败')
        json_header.write_bytes(header)
    download('https://unpkg.com/lucide@0.468.0/dist/umd/lucide.min.js',
             installer/'lucide.min.js', '3411692820cb8d47543f69496aa25fd603a358f4498046f41c508a5a3342210e')
    # 微软的 Evergreen 引导程序持续更新，SDK、JSON 与图标库使用固定版本。
    download('https://go.microsoft.com/fwlink/p/?LinkId=2124703', installer/'MicrosoftEdgeWebview2Setup.exe')
    print('安装器构建依赖已就绪。', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cache-root', type=Path, default=ROOT/'.cache')
    args = parser.parse_args()
    prepare_dependencies(args.cache_root.resolve())
