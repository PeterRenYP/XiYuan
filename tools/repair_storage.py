"""迁移旧系统盘记录，并清理本工程产生的已验证测试副本。"""
from pathlib import Path
import hashlib
import json
import os
import shutil


def digest(path):
    """分块校验文件，避免将大权重一次读入内存。"""
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 * 1024**2), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    """只处理明确列出的旧记录和自有测试目录，不遍历删除用户模型。"""
    root = Path(__file__).resolve().parent.parent
    source = Path(os.environ["LOCALAPPDATA"]) / "ModelObservatory" / "模型观测台" / "traces"
    destination = Path(root.anchor) / "ModelObservatoryData" / "旧版运行记录"
    report = {"migrated_bytes": 0, "migrated_to": str(destination), "removed": []}
    if source.exists():
        # 先复制并逐文件校验，全部通过后才删除旧目录。
        destination.mkdir(parents=True, exist_ok=True)
        files = [p for p in source.rglob("*") if p.is_file()]
        for i, path in enumerate(files):
            target = destination / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or target.stat().st_size != path.stat().st_size or digest(target) != digest(path):
                shutil.copy2(path, target)
            if digest(target) != digest(path):
                raise RuntimeError(f"迁移校验失败：{path}")
            report["migrated_bytes"] += path.stat().st_size
            if i % 5000 == 0:
                print(f"旧记录校验：{i}/{len(files)}", flush=True)
        expected = (Path(os.environ["LOCALAPPDATA"]) / "ModelObservatory" / "模型观测台" / "traces").resolve()
        if source.resolve() != expected or source.is_symlink():
            raise RuntimeError("旧记录目录校验失败")
        shutil.rmtree(source)
    artifacts = root / "artifacts"
    names = ["cpp-models", "真实导出模型-191124", "真实导出模型-191646",
             "真实导出模型-184828", "真实导出模型-191905", "packaged-traces", "real-traces"]
    for name in names:
        target = (artifacts / name).resolve()
        if target.parent != artifacts.resolve() or target.is_symlink():
            raise RuntimeError("测试目录校验失败")
        if target.exists():
            # 保留验证结果与文字元信息；大权重、备份和张量已完成验收。
            evidence = root / "artifacts" / "保留的测试结果" / name
            for path in target.rglob("*.json"):
                if path.stat().st_size < 1024**2 and path.name not in ("tokenizer.json",):
                    saved = evidence / path.relative_to(target)
                    saved.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, saved)
            shutil.rmtree(target)
            report["removed"].append(str(target))
    for relative in ("dist/ModelObservatory", ".cache/pip", ".cache/wheels"):
        target = (root / relative).resolve()
        if not target.is_relative_to(root) or target.is_symlink():
            raise RuntimeError("缓存目录校验失败")
        if target.exists():
            shutil.rmtree(target)
            report["removed"].append(str(target))
    (artifacts / "存储整理结果.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
