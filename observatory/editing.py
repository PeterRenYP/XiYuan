"""多规则神经元干预与模型原目录事务保存。"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import uuid

from .protocol import ProtocolError
from .traces import safe_path


def digest(path):
    """分块计算校验值，避免把完整权重读入内存。"""
    result = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(8 * 1024**2), b""):
            result.update(block)
    return result.hexdigest()


def write_json(path, value):
    """先同步临时文件再原子替换事务元数据。"""
    temporary = path.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def recover_save(root):
    """恢复意外中断的原目录保存，绝不加载半套权重。"""
    root = Path(root).resolve()
    journal = root / "observatory-save-journal.json"
    if not journal.exists():
        return
    transaction = json.loads(journal.read_text(encoding="utf-8"))
    if transaction["state"] == "committing":
        backup = safe_path(root, transaction["backup"])
        for name in transaction["shards"]:
            if name in transaction.get("absent", []):
                continue
            original = safe_path(backup, name)
            if not original.is_file() or digest(original) != transaction["hashes"][name]:
                raise ProtocolError("BACKUP_CORRUPT", "保存中断且备份校验失败，请保留目录并检查备份")
        for name in transaction["shards"]:
            target = safe_path(root, name)
            if name in transaction.get("absent", []):
                target.unlink(missing_ok=True)
                continue
            temporary = target.with_suffix(".restore.tmp")
            shutil.copy2(safe_path(backup, name), temporary)
            os.replace(temporary, target)
    journal.unlink()


def validate_rules(adapter, rules, target):
    """完整校验一批规则后再修改，禁止部分应用和越界。"""
    import torch
    if target not in ("temporary", "model") or not isinstance(rules, list) or not 1 <= len(rules) <= 128:
        raise ProtocolError("INVALID_RULES", "请选择编辑类型，并提供 1 至 128 条规则")
    result, seen = [], set()
    for raw in rules:
        rule = dict(raw)
        layer, neuron = int(rule["layer_index"]), int(rule["neuron_index"])
        if not 0 <= layer < adapter.config.num_hidden_layers or not 0 <= neuron < adapter.config.intermediate_size:
            raise ProtocolError("INVALID_TARGET", "层或神经元通道越界")
        value = float(rule.get("value", 0))
        kind = rule["kind"]
        if not math.isfinite(value) or not math.isfinite(torch.tensor(value, dtype=next(adapter.model.parameters()).dtype).item()):
            raise ProtocolError("INVALID_VALUE", "修改值必须为模型精度可表示的有限数")
        rule.update(layer_index=layer, neuron_index=neuron, value=value)
        if target == "temporary":
            if kind not in ("zero", "scale", "set", "add", "steer"):
                raise ProtocolError("INVALID_RULE", "临时编辑方式无效")
            scope = rule.get("scope", "position")
            if scope not in ("position", "all", "generated"):
                raise ProtocolError("INVALID_SCOPE", "临时编辑作用范围无效")
            rule["scope"] = scope
            rule["token_position"] = int(rule.get("token_position", 0))
            if rule["token_position"] < 0:
                raise ProtocolError("INVALID_TARGET", "Token位置不能为负数")
            # 同一层通道只能出现一条规则，避免范围重叠造成隐含的执行次序。
            identity = layer, neuron
        else:
            projection = rule.get("projection", "down")
            if projection not in ("gate", "up", "down") or kind not in ("zero", "scale", "set"):
                raise ProtocolError("INVALID_RULE", "模型参数编辑方式无效")
            rule["projection"] = projection
            rule["dimension"] = int(rule.get("dimension", 0))
            if kind == "set" and not 0 <= rule["dimension"] < adapter.config.hidden_size:
                raise ProtocolError("INVALID_TARGET", "系数维度越界")
            identity = layer, neuron, projection
        if identity in seen:
            raise ProtocolError("DUPLICATE_RULE", "同一目标存在重复规则，请合并后应用")
        seen.add(identity)
        result.append(rule)
    return result


def apply_activations(adapter, layer, baseline, positions, residual, mlp):
    """从未修改的激活应用规则，保持旧接口兼容和分支基线稳定。"""
    import torch
    rules = [r for r in adapter.edit_rules if r["layer_index"] == layer]
    legacy = [(p, n, k, v) for (l, p, n), (k, v) in adapter.activation_patches.items() if l == layer]
    if not rules and not legacy:
        return baseline
    result = baseline.clone()
    for position, neuron, kind, value in legacy:
        if position in positions:
            row = positions.index(position)
            result[0, row, neuron] = 0 if kind == "zero" else value if kind == "set" else baseline[0, row, neuron] * value
    reference_norm = None
    for rule in rules:
        neuron, value, kind = rule["neuron_index"], rule["value"], rule["kind"]
        rows = [i for i, p in enumerate(positions) if rule["scope"] == "all" or
                (rule["scope"] == "generated" and p >= adapter.prompt_length) or
                (rule["scope"] == "position" and p == rule["token_position"])]
        if not rows:
            continue
        if kind == "steer":
            # 使用当前输入未经干预的层输出范数；预填充排除首个token。
            if reference_norm is None:
                leaving = (residual + mlp.down_proj(baseline)).float().norm(dim=-1)[0]
                candidates = leaving[1:] if leaving.numel() > 1 else leaving
                reference_norm = candidates.median()
            column_norm = mlp.down_proj.weight[:, neuron].float().norm()
            if column_norm == 0:
                raise ProtocolError("ZERO_DIRECTION", "该通道的下投影方向范数为零，无法归一化引导")
            value = value * reference_norm / column_norm
        old = baseline[0, rows, neuron]
        result[0, rows, neuron] = (0 if kind == "zero" else value if kind == "set" else
                                   old * value if kind == "scale" else old + value)
    if not torch.isfinite(result).all():
        raise ProtocolError("VALUE_OVERFLOW", "激活干预产生非有限数，请减小修改值")
    return result


def prepare_weights(adapter, rules):
    """在副本上修改真实参数，所有规则通过后才提交至模型。"""
    import torch
    changes = {}
    prefix = "model.language_model." if adapter.info.get("model_type") == "qwen3_5" else "model."
    for rule in rules:
        layer, projection = rule["layer_index"], rule["projection"]
        key = prefix + f"layers.{layer}.mlp.{projection}_proj.weight"
        if key not in changes:
            parameter = adapter.parameter(layer, projection)
            changes[key] = (parameter, parameter.detach().clone())
        edited = changes[key][1]
        neuron, value, kind = rule["neuron_index"], rule["value"], rule["kind"]
        channel = edited[:, neuron] if projection == "down" else edited[neuron, :]
        if kind == "set":
            channel[rule["dimension"]] = value
        elif kind == "zero":
            channel.zero_()
        else:
            channel.mul_(value)
        if not torch.isfinite(channel).all():
            raise ProtocolError("VALUE_OVERFLOW", "参数编辑溢出，未应用任何规则")
    return changes


def save_current_model(adapter, changes, notify=None, assets=None):
    """只改动受影响分块；备份、校验、事务日志和失败回滚覆盖整个提交。"""
    from safetensors import safe_open
    from safetensors.torch import save_file
    notify = notify or (lambda *args: None)
    root = Path(adapter.info["path"]).resolve()
    recover_save(root)
    index = root / "model.safetensors.index.json"
    mapping = json.loads(index.read_text(encoding="utf-8"))["weight_map"] if index.exists() else {}
    assets = assets or {}
    weight_shards = sorted({mapping.get(key, "model.safetensors") for key in changes})
    shards = weight_shards + list(assets)
    hashes = {name: digest(safe_path(root, name)) for name in shards if safe_path(root, name).exists()}
    absent = [name for name in shards if name not in hashes]
    if any(adapter.info.get("hashes", {}).get(name) != hashes[name] for name in weight_shards):
        raise ProtocolError("MODEL_CHANGED_ON_DISK", "模型文件在加载后已变化，请重新加载后再保存")
    total = sum(safe_path(root, name).stat().st_size for name in shards if name in hashes)
    if shutil.disk_usage(root).free < total * 2 + 256 * 1024**2:
        raise ProtocolError("DISK_FULL", "磁盘空间不足以同时保存备份与临时权重")
    transaction_id = uuid.uuid4().hex
    backup = root / "observatory-backups" / transaction_id
    stage = backup / "staged"
    stage.mkdir(parents=True)
    journal = root / "observatory-save-journal.json"
    transaction = {"state": "committing", "backup": str(backup.relative_to(root)), "shards": shards, "hashes": hashes, "absent": absent}
    memory_before = {key: parameter.detach().cpu().clone() for key, (parameter, _) in changes.items()}
    committed = False
    try:
        for name in shards:
            notify("model.progress", {"message": "备份并生成权重分块：" + name})
            original = safe_path(root, name)
            target = safe_path(backup, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            # 分词器及版本索引与权重使用同一恢复日志，避免留下半个版本。
            if name in assets:
                if original.exists():
                    shutil.copy2(original, target)
                    if digest(target) != hashes[name]:
                        raise ProtocolError("BACKUP_CORRUPT", "配置备份校验失败")
                staged = safe_path(stage, name)
                staged.parent.mkdir(parents=True, exist_ok=True)
                data = assets[name]
                if name == "observatory-versions.json":
                    state = json.loads(data)
                    next_hashes = dict(adapter.info["hashes"])
                    next_hashes.update({part: digest(safe_path(stage, part)) for part in weight_shards})
                    state["model_revision"] = hashlib.sha256(json.dumps({
                        "config": adapter.info["raw_config"], "hashes": next_hashes
                    }, sort_keys=True).encode()).hexdigest()[:20]
                    data = json.dumps(state, ensure_ascii=False, allow_nan=False).encode()
                staged.write_bytes(data)
                with staged.open("rb+") as handle:
                    os.fsync(handle.fileno())
                continue
            shutil.copy2(original, target)
            if digest(target) != hashes[name]:
                raise ProtocolError("BACKUP_CORRUPT", "原模型备份校验失败，未写回模型")
            # 未修改的张量直接保留原始数据和元信息，不重导出视觉或其他参数。
            with safe_open(str(original), framework="pt", device="cpu") as source:
                tensors = {}
                for key in source.keys():
                    old = source.get_tensor(key)
                    if key in changes:
                        import torch
                        parameter, edited = changes[key]
                        # CPU 计算可能提升到单精度；写回仍保留原文件精度，内存同步其舍入结果。
                        stored = edited.detach().to(device="cpu", dtype=old.dtype).contiguous()
                        if not torch.isfinite(stored).all():
                            raise ProtocolError("VALUE_OVERFLOW", "修改值超出模型文件的存储精度，未写回模型")
                        tensors[key] = stored
                        changes[key] = (parameter, stored)
                    else:
                        tensors[key] = old
                staged = safe_path(stage, name)
                staged.parent.mkdir(parents=True, exist_ok=True)
                save_file(tensors, str(staged), metadata=source.metadata())
            del tensors
            with staged.open("rb+") as handle:
                os.fsync(handle.fileno())
        write_json(backup / "backup.json", transaction | {"model": str(root)})
        if any(digest(safe_path(root, name)) != hashes[name] for name in hashes) or any(safe_path(root, name).exists() for name in absent):
            raise ProtocolError("MODEL_CHANGED_ON_DISK", "生成临时权重期间原文件发生变化，未写回模型")
        write_json(journal, transaction)
        for name in shards:
            os.replace(safe_path(stage, name), safe_path(root, name))
        # 新分块写回完成后再更新内存；此前失败不会污染当前模型。
        import torch
        with torch.no_grad():
            for parameter, edited in changes.values():
                parameter.copy_(edited)
        hashes = {name: digest(safe_path(root, name)) for name in weight_shards}
        write_json(journal, transaction | {"state": "committed"})
        committed = True
        journal.unlink()
        adapter.info.setdefault("hashes", {}).update(hashes)
        # 模型版本必须与下次从磁盘加载时一致，否则项目现场会误判权重变化。
        adapter.info["model_revision"] = hashlib.sha256(json.dumps({
            "config": adapter.info["raw_config"], "hashes": adapter.info["hashes"]
        }, sort_keys=True).encode()).hexdigest()[:20]
        return {"path": str(root), "backup_path": str(backup), "saved_shards": shards}
    except Exception:
        if journal.exists():
            try:
                recover_save(root)
            except Exception as recovery_error:
                # 恢复失败后禁止使用可能混合新旧权重的模型继续计算。
                adapter.recovery_required = True
                raise ProtocolError("RECOVERY_REQUIRED", "模型恢复失败，已禁止推理和导出；请从已知备份恢复后重新加载：" + str(recovery_error)) from recovery_error
        if not committed:
            # 文件提交后的元数据写入也可能失败；内存与磁盘必须一起恢复。
            import torch
            with torch.no_grad():
                for key, (parameter, _) in changes.items():
                    parameter.copy_(memory_before[key])
        raise


def restore_backup(adapter, backup_path):
    """以另一次事务恢复备份分块，恢复后要求重新加载模型。"""
    root = Path(adapter.info["path"]).resolve()
    backup = Path(backup_path).resolve()
    if not backup.is_relative_to(root / "observatory-backups"):
        raise ProtocolError("INVALID_BACKUP", "请选择当前模型目录内的备份")
    metadata = json.loads((backup / "backup.json").read_text(encoding="utf-8"))
    from safetensors import safe_open
    changes = {}
    parameters = dict(adapter.model.named_parameters())
    for name in metadata["shards"]:
        path = safe_path(backup, name)
        if digest(path) != metadata["hashes"][name]:
            raise ProtocolError("BACKUP_CORRUPT", "备份分块校验失败")
        with safe_open(str(path), framework="pt", device="cpu") as source:
            for key in source.keys():
                if ".mlp." in key and key in parameters:
                    parameter = parameters[key]
                    changes[key] = (parameter, source.get_tensor(key).to(parameter.device))
    if not changes:
        raise ProtocolError("INVALID_BACKUP", "备份没有可恢复的模型参数")
    return save_current_model(adapter, changes)
