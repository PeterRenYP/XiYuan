"""稀疏补丁按基座重放；索引、分词器和真实权重共同事务提交。"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import uuid

import torch

from .editing import save_current_model
from .protocol import ProtocolError


def vocabulary_hash(tokenizer):
    """把完整编码规则纳入版本身份，避免相同词表但不同规则的误导入。"""
    return hashlib.sha256(tokenizer.backend_tokenizer.to_str().encode()).hexdigest()


class Versions:
    """只存受影响区间的基座值，不复制整份模型到内存。"""

    def __init__(self, adapter):
        """从当前模型目录恢复版本；外部改动必须明确阻止旧补丁重放。"""
        self.adapter = adapter
        self.path = Path(adapter.info["path"]) / "observatory-versions.json"
        self.state = json.loads(self.path.read_text(encoding="utf-8")) if self.path.exists() else {
            "schema": 1, "base_model": adapter.info["model_revision"],
            "base_vocab": vocabulary_hash(adapter.tokenizer), "model_revision": adapter.info["model_revision"],
            "vocab_revision": vocabulary_hash(adapter.tokenizer), "patches": [], "undo": [], "redo": []}

    def check(self):
        """在修改前检查磁盘、模型内存和词表三种身份。"""
        if getattr(self.adapter, "recovery_required", False):
            raise ProtocolError("RECOVERY_REQUIRED", "模型恢复失败，请恢复已知备份并重新加载")
        if self.adapter.weight_patches:
            raise ProtocolError("VERSION_CONFLICT", "存在未纳入版本的旧参数修改，请先恢复旧修改或导出模型副本")
        if self.state["model_revision"] != self.adapter.info["model_revision"] or self.state["vocab_revision"] != vocabulary_hash(self.adapter.tokenizer):
            raise ProtocolError("VERSION_CONFLICT", "模型已由其他入口修改，旧版本不能重放；请重新建立基座")
        if self.path.exists() and json.loads(self.path.read_text(encoding="utf-8")) != self.state:
            raise ProtocolError("VERSION_CONFLICT", "版本文件已变化，请重新加载模型")

    def parameter(self, name):
        """解析真实参数名称，兼容输入与输出共享存储。"""
        parameters = dict(self.adapter.model.named_parameters(remove_duplicate=False))
        if name not in parameters:
            raise ProtocolError("INVALID_TARGET", "补丁目标不存在：" + name)
        return parameters[name]

    def canonical(self, parameter):
        """共享权重统一使用磁盘中存在的名称，防止重复编辑。"""
        root = Path(self.adapter.info["path"])
        index = root / "model.safetensors.index.json"
        if index.exists():
            keys = json.loads(index.read_text(encoding="utf-8"))["weight_map"]
        else:
            from safetensors import safe_open
            with safe_open(str(root / "model.safetensors"), framework="pt") as file:
                keys = set(file.keys())
        for name, candidate in self.adapter.model.named_parameters(remove_duplicate=False):
            if candidate.data_ptr() == parameter.data_ptr() and name in keys:
                return name
        raise ProtocolError("INVALID_TARGET", "该参数没有可保存的权重映射")

    def storage_dtype(self, name):
        """使用磁盘存储精度校验与预览，CPU 提升精度不会改变模型文件类型。"""
        from safetensors import safe_open
        root=Path(self.adapter.info["path"])
        index=root/"model.safetensors.index.json"
        mapping=json.loads(index.read_text(encoding="utf-8"))["weight_map"] if index.exists() else {}
        with safe_open(str(root/mapping.get(name,"model.safetensors")),framework="pt") as source:
            dtype=source.get_slice(name).get_dtype()
        value={"F32":torch.float32,"F16":torch.float16,"BF16":torch.bfloat16}.get(dtype)
        if value is None:raise ProtocolError("UNSUPPORTED_DTYPE", "该权重精度尚不支持修改")
        return value

    @staticmethod
    def region(tensor, target):
        """只允许有界的矩阵行或列区间，导入补丁也走相同校验。"""
        axis, index, start, stop = (target.get(k) for k in ("axis", "index", "start", "stop"))
        if any(type(v) is not int for v in (axis, index, start, stop)) or tensor.ndim != 2 or axis not in (0, 1):
            raise ProtocolError("INVALID_TARGET", "补丁坐标无效")
        if not 0 <= index < tensor.shape[axis] or not 0 <= start < stop <= tensor.shape[1-axis] or stop-start > 262144:
            raise ProtocolError("INVALID_TARGET", "补丁区间越界或过大")
        return tensor[index, start:stop] if axis == 0 else tensor[start:stop, index]

    def base_tensors(self):
        """逆序恢复全部受影响区间；停用中间补丁也能保留其他补丁的效果。"""
        changes = {}
        for patch in reversed(self.state["patches"]):
            if patch["type"] != "tensor":
                continue
            target = patch["target"]
            name = target["name"]
            if name not in changes:
                parameter = self.parameter(name)
                changes[name] = (parameter, parameter.detach().clone())
            region = self.region(changes[name][1], target)
            region.copy_(torch.tensor(patch["before"], device=region.device, dtype=region.dtype))
        return changes

    def tokenizer_for(self, state, base=False):
        """恢复原始条目再按顺序应用，特殊 Token 和未适配的分词器不可改写。"""
        from tokenizers import Tokenizer
        from transformers import PreTrainedTokenizerFast
        data = json.loads(self.adapter.tokenizer.backend_tokenizer.to_str())
        patches = [p for p in state["patches"] if p["type"] == "vocabulary"]
        if not patches:
            return self.adapter.tokenizer
        if data["model"]["type"] != "WordLevel":
            raise ProtocolError("UNSUPPORTED_TOKENIZER", "此分词器暂不支持词表编辑")
        vocab = data["model"]["vocab"]
        for patch in reversed(self.state["patches"]):
            if patch["type"] == "vocabulary" and patch["after"] in vocab:
                index = vocab.pop(patch["after"])
                vocab[patch["before"]] = index
        for patch in patches:
            if not base and patch["enabled"]:
                old = next((text for text, index in vocab.items() if index == patch["index"]), None)
                if old is None or (patch["after"] in vocab and vocab[patch["after"]] != patch["index"]):
                    raise ProtocolError("INVALID_VOCABULARY", "词表编号或文本重复")
                del vocab[old]
                vocab[patch["after"]] = patch["index"]
        tokenizer = PreTrainedTokenizerFast(tokenizer_object=Tokenizer.from_str(json.dumps(data)),
            **copy.deepcopy(self.adapter.tokenizer.init_kwargs))
        tokenizer.chat_template = self.adapter.tokenizer.chat_template
        for key in ("unk_token", "eos_token", "bos_token", "pad_token", "additional_special_tokens"):
            value = getattr(self.adapter.tokenizer, key, None)
            if value is not None:
                setattr(tokenizer, key, value)
        for patch in patches:
            if not base and patch["enabled"] and tokenizer.encode(patch["after"], add_special_tokens=False) != [patch["index"]]:
                raise ProtocolError("INVALID_VOCABULARY", "新文本无法稳定编码为原编号，请使用单个词表条目")
        return tokenizer

    def build(self, state):
        """从基座重建结果，缩放和叠加都不会因重复提交而累计。"""
        changes = self.base_tensors()
        for patch in state["patches"]:
            if patch["type"] != "tensor":
                continue
            target = patch["target"]
            name = target["name"]
            if name not in changes:
                parameter = self.parameter(name)
                changes[name] = (parameter, parameter.detach().clone())
            region = self.region(changes[name][1], target)
            if patch["enabled"]:
                values = torch.tensor(patch["values"], device=region.device, dtype=region.dtype)
                kind = patch["kind"]
                if values.numel() not in (1, region.numel()) or kind not in ("set", "scale", "add", "zero"):
                    raise ProtocolError("INVALID_PATCH", "补丁数值数量或修改方式无效")
                region.copy_(0 if kind == "zero" else values if kind == "set" else region * values if kind == "scale" else region + values)
                if not torch.isfinite(region).all():
                    raise ProtocolError("VALUE_OVERFLOW", "补丁数值超出目标精度")
        for name,(parameter,edited) in list(changes.items()):
            stored=edited.to(self.storage_dtype(name))
            if not torch.isfinite(stored).all():
                raise ProtocolError("VALUE_OVERFLOW", "修改值超出模型存储精度")
            changes[name]=(parameter,stored.to(parameter.dtype))
        return changes

    def commit(self, state, notify=None):
        """版本索引随同参数提交，失败不改变版本或内存分词器。"""
        self.check()
        changes = self.build(state)
        tokenizer = self.tokenizer_for(state)
        assets = {}
        if any(p["type"] == "vocabulary" for p in state["patches"]):
            with tempfile.TemporaryDirectory() as directory:
                tokenizer.save_pretrained(directory)
                assets = {p.name: p.read_bytes() for p in Path(directory).iterdir() if p.is_file()}
        # 提前按磁盘原精度舍入，保证版本校验身份与提交结果一致。
        from safetensors import safe_open
        root = Path(self.adapter.info["path"])
        index = root / "model.safetensors.index.json"
        mapping = json.loads(index.read_text(encoding="utf-8"))["weight_map"] if index.exists() else {}
        for name, (parameter, edited) in changes.items():
            with safe_open(str(root / mapping.get(name, "model.safetensors")), framework="pt") as file:
                dtype = file.get_slice(name).get_dtype()
            storage = {"F32": torch.float32, "F16": torch.float16, "BF16": torch.bfloat16}.get(dtype)
            if storage is None or not torch.isfinite(edited.to(storage)).all():
                raise ProtocolError("VALUE_OVERFLOW", "修改值超出模型存储精度")
        # 权重提交会生成新的内容版本；索引的预期版本由暂存分片算出。
        state["vocab_revision"] = vocabulary_hash(tokenizer)
        assets["observatory-versions.json"] = json.dumps(state, ensure_ascii=False, allow_nan=False).encode()
        result = save_current_model(self.adapter, changes, notify, assets)
        # 分片哈希在暂存阶段计算并写入索引，保证崩溃恢复后可直接打开。
        self.state = json.loads(self.path.read_text(encoding="utf-8"))
        self.adapter.tokenizer = tokenizer
        self.adapter.revision_number += 1
        self.adapter.weights_revision = f"w{self.adapter.revision_number}"
        self.adapter.reset_cache()
        return result | self.summary()

    def tensor_patch(self, target, values, kind="set", title="向量修改", owner="vectors"):
        """校验新目标并保存对应基座值，用于预览、导入和真正提交。"""
        parameter = self.parameter(target["name"])
        if self.canonical(parameter) != target["name"]:
            raise ProtocolError("INVALID_TARGET", "共享权重需要使用统一目标")
        base = self.base_tensors().get(target["name"], (parameter, parameter))[1]
        region = self.region(base, target)
        values = torch.tensor(values, dtype=torch.float64).flatten()
        if values.numel() not in (1, region.numel()) or not torch.isfinite(values).all():
            raise ProtocolError("INVALID_VALUES", "请填写有限数值，数量须为一个或与区间长度一致")
        return {"id": uuid.uuid4().hex, "type": "tensor", "title": title, "owner": owner, "enabled": True,
            "target": target, "shape": list(parameter.shape), "dtype": str(self.storage_dtype(target["name"])), "kind": kind,
            "values": values.tolist(), "before": region.detach().double().cpu().tolist()}

    def add(self, patches, notify=None):
        """新增一组补丁并保留撤销栈；新编辑会清除重做记录。"""
        state = copy.deepcopy(self.state)
        state["undo"].append(self.snapshot())
        state["undo"] = state["undo"][-128:]
        state["redo"] = []
        state["patches"].extend(patches)
        if len(state["patches"]) > 512:
            raise ProtocolError("HISTORY_FULL", "当前基座最多保存 512 个补丁，请导出版本后使用新的模型副本继续")
        return self.commit(state, notify)

    def rules_state(self, rules):
        """完整替换规则列表所属补丁，保留独立修改，并复用相同规则避免累计。"""
        from .editing import validate_rules
        state = copy.deepcopy(self.state)
        selected = validate_rules(self.adapter, rules, "model") if rules else []
        for patch in state["patches"]:
            if patch.get("owner") == "rules":
                patch["enabled"] = False
        for rule in selected:
            parameter = self.adapter.parameter(rule["layer_index"], rule["projection"])
            axis = 1 if rule["projection"] == "down" else 0
            start = rule["dimension"] if rule["kind"] == "set" else 0
            stop = start + 1 if rule["kind"] == "set" else parameter.shape[1-axis]
            target = {"name": self.canonical(parameter), "axis": axis, "index": rule["neuron_index"], "start": start, "stop": stop}
            patch = self.tensor_patch(target, [rule["value"]], rule["kind"],
                f"第 {rule['layer_index']} 层 · 通道 {rule['neuron_index']}", "rules")
            # 内容编号不含临时版本；停用再启用同一规则仍从原基座重放。
            content = {"target": target, "kind": rule["kind"], "value": rule["value"], "base": state["base_model"]}
            patch["id"] = hashlib.sha256(json.dumps(content, sort_keys=True).encode()).hexdigest()[:32]
            if any(p["id"] == patch["id"] and p.get("owner") != "rules" for p in state["patches"]):
                patch["id"] = "local-" + patch["id"]
            known = next((p for p in state["patches"] if p["id"] == patch["id"]), None)
            if known:
                known["enabled"] = True
            else:
                state["patches"].append(patch)
        if len(state["patches"]) > 512:
            raise ProtocolError("HISTORY_FULL", "补丁历史已满，请导出模型副本建立新基座")
        return state

    def snapshot(self):
        """同一个版本同时记录权重启停和完整规则，撤销不会留下混合配置。"""
        return {"flags":[p["enabled"] for p in self.state["patches"]],
            "rules":copy.deepcopy(self.state.get("rules_configuration", []))}

    def replace_rules(self, rules, notify=None, draft=None):
        """一次提交列表的完整目标，删除和清空仅撤销此列表拥有的补丁。"""
        self.check()
        state = self.rules_state(rules)
        if draft is not None:
            state["rules_configuration"] = copy.deepcopy(draft)
        if state == self.state:
            return self.summary() | {"unchanged": True}
        state["undo"].append(self.snapshot())
        state["undo"] = state["undo"][-128:]
        state["redo"] = []
        return self.commit(state, notify)

    def action(self, action, patch_id=None, notify=None):
        """撤销、重做和逐条启停均从基座重建，不做逆向数学运算。"""
        state = copy.deepcopy(self.state)
        current = self.snapshot()
        if action in ("undo", "redo"):
            stack = state[action]
            if not stack:
                raise ProtocolError("NO_HISTORY", "没有可撤销或重做的修改")
            previous = stack.pop()
            # 兼容旧版本只保存启停数组的历史。
            flags = previous.get("flags", []) if isinstance(previous, dict) else previous
            if isinstance(previous, dict):
                state["rules_configuration"] = previous.get("rules", [])
            state["redo" if action == "undo" else "undo"].append(current)
            for i, patch in enumerate(state["patches"]):
                patch["enabled"] = flags[i] if i < len(flags) else False
        elif action == "toggle":
            patch = next((p for p in state["patches"] if p["id"] == patch_id), None)
            if patch is None:
                raise ProtocolError("INVALID_PATCH", "请选择一个补丁")
            state["undo"].append(current)
            state["redo"] = []
            patch["enabled"] = not patch["enabled"]
            # 版本页停用规则补丁时，同步规则列表，防止界面与真实权重相反。
            if patch.get("owner") == "rules":
                for rule in state.get("rules_configuration", []):
                    if rule.get("target") != "model":
                        continue
                    parameter = self.adapter.parameter(rule["layer_index"], rule["projection"])
                    axis = 1 if rule["projection"] == "down" else 0
                    start = rule["dimension"] if rule["kind"] == "set" else 0
                    target = {"name": self.canonical(parameter), "axis": axis, "index": rule["neuron_index"],
                        "start": start, "stop": start+1 if rule["kind"] == "set" else parameter.shape[1-axis]}
                    if target == patch["target"] and rule["kind"] == patch["kind"] and [rule["value"]] == patch["values"]:
                        rule["enabled"] = patch["enabled"]
        else:
            raise ProtocolError("INVALID_ACTION", "不支持的版本操作")
        result = self.commit(state, notify)
        if "rules_configuration" in state:
            self.adapter.edit_rules = [r for r in state["rules_configuration"] if r.get("enabled",True) and r.get("target", "temporary") == "temporary"]
            self.adapter.activation_patches.clear()
            self.adapter.reset_cache()
        return result

    def summary(self):
        """界面只获取摘要，完整补丁通过受限文件导入导出。"""
        return {"weights_revision": self.adapter.weights_revision, "model_revision": self.adapter.info["model_revision"],
            **({"rules":copy.deepcopy(self.state["rules_configuration"]), "temporary_rules":[r for r in self.state["rules_configuration"] if r.get("enabled",True) and r.get("target","temporary")=="temporary"]} if "rules_configuration" in self.state else {}),
            "patches": [{k: v for k, v in p.items() if k not in ("values", "before")} for p in self.state["patches"]],
            "can_undo": bool(self.state["undo"]), "can_redo": bool(self.state["redo"]),
            "compatible": not self.adapter.weight_patches and self.state["model_revision"] == self.adapter.info["model_revision"] and self.state["vocab_revision"] == vocabulary_hash(self.adapter.tokenizer)}
