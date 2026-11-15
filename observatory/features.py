"""工作台新功能：真实词表向量、激活排名、补丁版本和回归验收。"""
from __future__ import annotations
from contextlib import closing, contextmanager

import copy
import datetime
import json
import math
import os
from pathlib import Path
import platform
import re
import sqlite3
import tempfile
import time
import uuid

import torch

from .control import Cancelled
from .protocol import ProtocolError
from .storage import atomic_json
from .traces import TraceStore
from .versions import Versions, vocabulary_hash


def activation_summary(value, positions):
    """逐层保留可合并统计，避免为全层排名保存完整激活矩阵。"""
    rows = value.detach().reshape(-1, value.shape[-1]).float()
    absolute = rows.abs()
    peak, index = absolute.max(dim=0)
    signed = rows.gather(0, index.unsqueeze(0)).squeeze(0)
    normalized = absolute / rows.square().mean(dim=1, keepdim=True).sqrt().clamp_min(1e-12)
    position_tensor = torch.tensor(positions, device=rows.device)
    return torch.stack((peak, signed, absolute.sum(0), (absolute >= .01).sum(0).float(),
        position_tensor[index].float(), normalized.sum(0), rows.max(0).values.clamp_min(0), rows.min(0).values.clamp_max(0)))


class Features:
    """重任务由现有串行运行队列执行，绝不和推理同时改权重。"""

    def __init__(self, runtime):
        """复用运行器与当前适配器，不创建第二套会话状态。"""
        self.runtime = runtime
        self.cancelled = False
        self.analysis_cache = tempfile.TemporaryDirectory(prefix="xiyuan-analysis-")
        self.progress_request = None
        self.progress_action = None
        self.progress_last = None
        self.progress_time = 0

    def progress(self, message, completed=0, total=None, unit="条"):
        """按阶段和真实计数上报，循环内节流，未知总量不伪造百分比。"""
        self.check_cancel()
        if self.progress_action is None:
            return
        now = time.monotonic()
        stage = (message, total, unit)
        if stage == self.progress_last and now - self.progress_time < .2 and completed != total:
            return
        self.progress_last, self.progress_time = stage, now
        self.runtime.emitter.send("feature.progress", {
            "action": self.progress_action, "message": message, "completed": completed,
            "total": total, "unit": unit,
        }, request=self.progress_request)

    @contextmanager
    def analysis_progress(self, action, request):
        """仅关联本次分析请求，结束或异常后清除身份，避免污染下一任务。"""
        self.progress_request, self.progress_action = request, action
        self.progress_last, self.progress_time = None, 0
        try:
            self.progress("正在准备分析")
            yield
            self.progress("正在返回结果")
        finally:
            self.progress_request = self.progress_action = None

    def save_activation(self, path, result):
        """逐条保存完整分析并显示进度；取消或失败只删除临时文件。"""
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
        rows = result.get("rows", [])
        self.progress("正在保存分析结果", 0, len(rows))
        try:
            with temporary.open("w", encoding="utf-8") as handle:
                metadata = {key: value for key, value in result.items() if key != "rows"}
                header = json.dumps(metadata, ensure_ascii=False, allow_nan=False)
                handle.write(header[:-1] + ("," if metadata else "") + '"rows":[')
                for index, row in enumerate(rows):
                    self.check_cancel()
                    handle.write(("," if index else "") + json.dumps(row, ensure_ascii=False, allow_nan=False))
                    self.progress("正在保存分析结果", index + 1, len(rows))
                handle.write("]}")
                self.progress("正在等待分析文件落盘")
                handle.flush()
                os.fsync(handle.fileno())
            self.check_cancel()
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)

    def versions(self):
        """每个模型实例只初始化一次版本管理器。"""
        adapter = self.runtime.adapter
        if adapter is None:
            raise ProtocolError("MODEL_NOT_LOADED", "请先加载模型")
        if not hasattr(adapter, "versions"):
            adapter.versions = Versions(adapter)
        return adapter.versions

    def check_cancel(self):
        """在分块和前向边界响应停止请求。"""
        if self.cancelled or self.runtime.shutdown.is_set():
            raise Cancelled()

    def dispatch(self, payload, request=None):
        """统一分发已获界面授权的功能请求，并校验写入版本。"""
        action = payload.get("action")
        if getattr(self.runtime.adapter, "recovery_required", False) and not str(action).startswith("activation"):
            raise ProtocolError("RECOVERY_REQUIRED", "模型恢复失败，请从已知备份恢复后重新加载")
        if action in ("samples.read", "samples.save", "drafts.read", "drafts.save"):
            return self.project_assets(payload)
        if action in ("reports", "report.read"):
            return self.reports(payload)
        analysis_actions = {"activation": self.activation, "activation.detail": self.activation_detail,
            "activation.query": self.query_activation, "activation.open": self.open_activation}
        if action in analysis_actions:
            with self.analysis_progress(action, request):
                return analysis_actions[action](payload)
        if action == "comparison":
            from .comparison import compare
            return compare(self, payload)
        versions = self.versions()
        if action in ("vector.apply", "vocabulary.apply", "versions.action", "versions.import", "versions.apply", "rules.apply", "rules.slice"):
            if payload.get("expected_weights_revision") != versions.adapter.weights_revision:
                raise ProtocolError("STALE_WEIGHTS", "模型版本已变化，请刷新后重试")
            if payload.get("expected_model_revision", versions.adapter.info["model_revision"]) != versions.adapter.info["model_revision"] or payload.get("expected_vocab_revision", vocabulary_hash(versions.adapter.tokenizer)) != vocabulary_hash(versions.adapter.tokenizer):
                raise ProtocolError("STALE_PREVIEW", "预览来源已变化，请重新预览后保存")
            versions.check()
        if action in ("rules.preview", "rules.apply", "rules.draft", "rules.slice"):
            return self.rules(payload, action == "rules.apply")
        if action == "search":
            return self.search(payload)
        if action in ("vector", "similar"):
            return self.vector(payload, action == "similar")
        if action in ("vector.preview", "vector.apply"):
            return self.edit_vector(payload, action.endswith("apply"))
        if action in ("vocabulary.preview", "vocabulary.apply"):
            return self.edit_vocabulary(payload, action.endswith("apply"))
        if action == "versions":
            return versions.summary()
        if action == "versions.action":
            return versions.action(payload["operation"], payload.get("patch_id"), self.runtime.event)
        if action in ("versions.export", "versions.import", "versions.preview", "versions.apply"):
            return self.patch_file(payload, action != "versions.export", action == "versions.preview")
        if action in ("regression", "export.verify"):
            return self.regression(payload, action == "export.verify")
        raise ProtocolError("UNKNOWN_COMMAND", "未知功能请求")

    def reports(self, payload):
        """仅访问当前项目的已保存报告，便于界面和 MCP 重开相同结果。"""
        root = Path(payload.get("project_root", "")).resolve() / "features"
        if not payload.get("project_root"):
            raise ProtocolError("NO_PROJECT", "请先打开项目")
        if payload["action"] == "reports":
            return {"rows": [{"path":str(p), "name":p.name, "bytes":p.stat().st_size}
                for p in sorted(root.glob("*.json"), key=lambda p:p.stat().st_mtime, reverse=True)
                if p.name.startswith(("activation-", "comparison-", "report-"))]}
        path = Path(payload["path"]).resolve()
        if not path.is_relative_to(root) or not path.is_file() or path.stat().st_size > 64*1024**2:
            raise ProtocolError("INVALID_REPORT", "请选择当前项目内不超过 64 MiB 的报告")
        return json.loads(path.read_text(encoding="utf-8")) | {"path":str(path)}

    def project_assets(self, payload):
        """MCP 与界面共享同一份项目样例和修改草稿文件。"""
        action = payload["action"]
        kind = action.split(".")[0]
        root = Path(payload.get("project_root", "")) / "features"
        if not payload.get("project_root"):
            raise ProtocolError("NO_PROJECT", "请先打开项目")
        path = root / (kind+".json")
        if action.endswith("save"):
            rows = payload.get("items")
            if not isinstance(rows,list) or len(rows) > (100 if kind=="samples" else 128):
                raise ProtocolError("INVALID_PROJECT_DATA", "项目条目数量或格式无效")
            if kind == "drafts":
                for row in rows:
                    if row.get("action") not in ("vector.apply", "vocabulary.apply"):
                        raise ProtocolError("INVALID_DRAFT", "仅支持向量和词表草稿")
                    self.dispatch(row | {"action":row["action"].replace(".apply", ".preview")})
            root.mkdir(parents=True,exist_ok=True)
            atomic_json(path,rows)
        rows = json.loads(path.read_text(encoding="utf-8")) if path.exists() else []
        return {"items":rows,"path":str(path)}

    def search(self, payload):
        """在真实词表中分页查询文本、编号和特殊 Token。"""
        adapter = self.runtime.adapter
        tokenizer = adapter.tokenizer
        query = str(payload.get("query", ""))
        exact = bool(payload.get("exact"))
        special_only = bool(payload.get("special"))
        vocabulary = sorted(tokenizer.get_vocab().items(), key=lambda item: item[1])
        specials = set(tokenizer.all_special_ids)
        rows = []
        for text, index in vocabulary:
            if special_only and index not in specials:
                continue
            if query and not (str(index) == query or (text == query if exact else query.casefold() in text.casefold())):
                continue
            rows.append((text, index))
        offset = max(0, int(payload.get("offset", 0)))
        return {"total": len(rows), "offset": offset, "rows": [
            {"id": index, "text": text, "decoded": tokenizer.decode([index], skip_special_tokens=False),
             "special": index in specials} for text, index in rows[offset:offset+100]],
            "vocabulary_editable": json.loads(tokenizer.backend_tokenizer.to_str())["model"]["type"] == "WordLevel",
            "weights_revision": adapter.weights_revision}

    def associated_files(self, name):
        """返回当前向量关联的真实分词器、配置和权重文件。"""
        root = Path(self.runtime.adapter.info["path"])
        files = [root / n for n in ("tokenizer.json", "tokenizer_config.json", "special_tokens_map.json", "vocab.json", "merges.txt", "tokenizer.model", "config.json") if (root / n).is_file()]
        index = root / "model.safetensors.index.json"
        if index.exists():
            mapping = json.loads(index.read_text(encoding="utf-8"))["weight_map"]
            files.extend([index, root / mapping[name]])
        else:
            files.append(root / "model.safetensors")
        return [str(p) for p in files]

    def embedding(self, payload):
        """通过架构接口解析输入或输出空间，并验证真实矩阵布局。"""
        adapter = self.runtime.adapter
        space = payload.get("space", "input")
        if space not in ("input", "output"):
            raise ProtocolError("INVALID_SPACE", "请选择输入或输出向量")
        module = adapter.model.get_input_embeddings() if space == "input" else adapter.model.get_output_embeddings()
        if module is None or module.weight.ndim != 2 or module.weight.shape[0] != adapter.config.vocab_size:
            raise ProtocolError("UNSUPPORTED_EMBEDDING", "当前架构未提供可编辑的 Token 行向量")
        index = int(payload.get("token_id", -1))
        if index not in adapter.tokenizer.get_vocab().values() or not 0 <= index < module.weight.shape[0]:
            raise ProtocolError("INVALID_TOKEN", "Token 编号不存在或没有对应权重")
        return module.weight, index

    def vector(self, payload, similar=False):
        """向量分片读取，相似度按块计算，不复制整个矩阵到单精度。"""
        weight, index = self.embedding(payload)
        tokenizer = self.runtime.adapter.tokenizer
        versions = self.versions()
        if similar:
            query = torch.tensor(payload["query_vector"], device=weight.device, dtype=torch.float32) if payload.get("query_vector") else weight[index].detach().float()
            if query.ndim != 1 or query.numel() != weight.shape[1] or not torch.isfinite(query).all() or query.norm() == 0:
                raise ProtocolError("INVALID_VECTOR", "查询向量须与当前空间维度一致，且为有限的非零向量")
            query = query / query.norm()
            vocabulary = set(tokenizer.get_vocab().values())
            candidates = []
            with torch.inference_mode():
                for start in range(0, weight.shape[0], 2048):
                    self.check_cancel()
                    block = weight[start:start+2048].float()
                    norms = block.norm(dim=1)
                    scores = (block @ query) / norms.clamp_min(1e-12)
                    values, ids = scores.topk(min(50, scores.numel()))
                    candidates.extend((float(score), start + int(i)) for score, i in zip(values, ids) if start+int(i) in vocabulary and norms[int(i)] > 0)
            candidates = sorted(candidates, reverse=True)[:max(1, min(50, int(payload.get("limit", 20))))]
            return {"rows": [{"id": i, "text": tokenizer.convert_ids_to_tokens(i), "score": score} for score, i in candidates], "space": payload.get("space", "input")}
        start = int(payload.get("start", 0))
        count = int(payload.get("count", 64))
        if not 0 <= start < weight.shape[1] or not 1 <= count <= 512:
            raise ProtocolError("INVALID_RANGE", "切片起点越界或长度超过 512")
        row = weight[index].detach().float()
        input_weight = self.runtime.adapter.model.get_input_embeddings().weight
        output_weight = self.runtime.adapter.model.get_output_embeddings().weight
        return {"token_id": index, "text": tokenizer.convert_ids_to_tokens(index), "decoded": tokenizer.decode([index]),
            "dimension": weight.shape[1], "dtype": {"torch.float32": "单精度", "torch.float16": "半精度", "torch.bfloat16": "脑浮点半精度"}.get(str(weight.dtype), "未适配精度"),
            "shared": input_weight.data_ptr() == output_weight.data_ptr(), "start": start,
            "values": row[start:start+count].tolist(), "norm": float(row.norm()), "min": float(row.min()), "max": float(row.max()),
            "name": versions.canonical(weight), "files": self.associated_files(versions.canonical(weight)),
            "weights_revision": self.runtime.adapter.weights_revision}

    def edit_vector(self, payload, apply):
        """预览和提交共用校验；共享输入输出只生成一个补丁。"""
        weight, index = self.embedding(payload)
        versions = self.versions()
        start, stop = int(payload.get("start", 0)), int(payload.get("stop", 0))
        target = {"name": versions.canonical(weight), "axis": 0, "index": index, "start": start, "stop": stop}
        patch = versions.tensor_patch(target, payload.get("values", []), payload.get("kind", "set"), f"Token {index} 向量")
        self.patch_identity(patch, payload)
        known = next((p for p in versions.state["patches"] if p["id"] == patch["id"]), None)
        if known is not None:
            if any(known.get(k) != patch.get(k) for k in ("type", "target", "values", "kind")):
                raise ProtocolError("PATCH_CONFLICT", "同一草稿编号的修改内容不同，请重新创建草稿")
            return versions.summary() | {"unchanged": True} if apply else {"before": "补丁已存在", "after": "不会重复应用", "count": 0, "overlaps": 1, "patch_id": patch["id"]} | self.preview_identity()
        state = copy.deepcopy(versions.state)
        state["patches"].append(patch)
        new = versions.region(versions.build(state)[target["name"]][1], target)
        old = versions.region(weight, target)
        preview = {"before": old.detach().float().cpu().tolist()[:64], "after": new.detach().float().cpu().tolist()[:64],
            "count": stop-start, "overlaps": sum(p["type"] == "tensor" and p["target"]["name"] == target["name"] and p["enabled"] and
                p["target"]["axis"] == 0 and p["target"]["index"] == index and p["target"]["start"] < stop and p["target"]["stop"] > start for p in versions.state["patches"])}
        return versions.add([patch], self.runtime.event) if apply else preview | self.preview_identity() | {"patch_id": patch["id"]}

    def edit_vocabulary(self, payload, apply):
        """首版只支持普通 WordLevel 条目重命名，禁止结构性改动。"""
        versions = self.versions()
        tokenizer = versions.adapter.tokenizer
        data = json.loads(tokenizer.backend_tokenizer.to_str())
        if data["model"]["type"] != "WordLevel":
            raise ProtocolError("UNSUPPORTED_TOKENIZER", "此分词器仅支持查询，暂不支持词表修改")
        index, text = int(payload.get("token_id", -1)), str(payload.get("text", "")).strip()
        if index in tokenizer.all_special_ids or index in tokenizer.get_added_vocab().values() or not text:
            raise ProtocolError("INVALID_VOCABULARY", "特殊或追加 Token 不可改写，新文本不能为空")
        old = next((text for text, i in tokenizer.get_vocab().items() if i == index), None)
        if old is None or (text in tokenizer.get_vocab() and tokenizer.get_vocab()[text] != index):
            raise ProtocolError("INVALID_VOCABULARY", "编号不存在或新文本已占用")
        import uuid
        patch = {"id": uuid.uuid4().hex, "type": "vocabulary", "title": f"Token {index} 词表", "owner": "vocabulary",
            "enabled": True, "index": index, "before": old, "after": text}
        self.patch_identity(patch, payload)
        known = next((p for p in versions.state["patches"] if p["id"] == patch["id"]), None)
        if known is not None:
            if any(known.get(k) != patch.get(k) for k in ("type", "index", "after")):
                raise ProtocolError("PATCH_CONFLICT", "同一草稿编号的词表内容不同")
            return versions.summary() | {"unchanged": True} if apply else {"before": "补丁已存在", "after": "不会重复应用", "count": 0, "overlaps": 1, "patch_id": patch["id"]} | self.preview_identity()
        state = copy.deepcopy(versions.state)
        state["patches"].append(patch)
        versions.tokenizer_for(state)
        return versions.add([patch], self.runtime.event) if apply else {"before": old, "after": text, "count": 1, "overlaps": 0, "patch_id": patch["id"]} | self.preview_identity()

    @staticmethod
    def patch_identity(patch, payload):
        """预览、保存草稿和提交使用同一编号，避免重复缩放或叠加。"""
        if "patch_id" in payload:
            identity = payload["patch_id"]
            if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{32}", identity):
                raise ProtocolError("INVALID_PATCH", "草稿编号无效")
            patch["id"] = identity

    def preview_identity(self):
        """草稿跨重启仍以内容版本校验，不依赖进程内递增编号。"""
        adapter = self.runtime.adapter
        return {"weights_revision": adapter.weights_revision, "expected_model_revision": adapter.info["model_revision"],
            "expected_vocab_revision": vocabulary_hash(adapter.tokenizer)}

    def rules(self, payload, apply=False):
        """先验证全部草稿；权重成功提交后才一次替换临时配置。"""
        from .editing import validate_rules
        versions = self.versions()
        versions.check()
        draft = payload.get("rules", [])
        if not isinstance(draft, list) or len(draft) > 128:
            raise ProtocolError("INVALID_RULES", "最多添加 128 条规则")
        groups = {"temporary": [], "model": []}
        for rule in draft:
            target = rule.get("target", "temporary")
            if target not in groups or type(rule.get("enabled", True)) is not bool:
                raise ProtocolError("INVALID_RULES", "规则类型或启用状态无效")
            groups[target].append(rule)
        # 停用项同样参与唯一性、数值及范围校验。
        checked = {key: validate_rules(versions.adapter, rows, key) if rows else [] for key, rows in groups.items()}
        active = {key: [r for r in rows if r.get("enabled", True)] for key, rows in checked.items()}
        state = versions.rules_state(active["model"])
        changes = versions.build(state)
        if payload.get("action") == "rules.slice":
            name=payload.get("name")
            if name not in changes:
                raise ProtocolError("INVALID_TARGET", "预览中没有此张量")
            parameter, after=changes[name]
            start,count=int(payload.get("start",0)),int(payload.get("count",64))
            if not 0<=start<after.numel() or not 1<=count<=512:
                raise ProtocolError("INVALID_RANGE", "切片起点越界或数量超过 512")
            return {"name":name,"start":start,"total":after.numel(),"shape":list(after.shape),
                "before":parameter.detach().flatten()[start:start+count].double().cpu().tolist(),
                "after":after.flatten()[start:start+count].double().cpu().tolist()} | self.preview_identity()
        preview = []
        for name, (parameter, after) in changes.items():
            different = parameter.detach() != after
            count = int(different.sum())
            if not count:
                continue
            coordinates = different.nonzero()[:64]
            preview.append({"target": name, "count": count, "total":after.numel(), "shape":list(after.shape), "samples": [
                {"coordinate": c.tolist(), "before": float(parameter.detach()[tuple(c)]), "after": float(after[tuple(c)])}
                for c in coordinates]})
        if not apply:
            return {"targets": preview, "temporary_count": len(active["temporary"]), "model_count": len(active["model"]),
                "rules": draft, "previous_rules": versions.state.get("rules_configuration", []),
                "previous_rules_active": True} | self.preview_identity()
        # commit 的磁盘事务成功后，内存赋值不会失败；提交失败保留原临时规则。
        result = versions.replace_rules(active["model"], self.runtime.event, draft=draft)
        versions.adapter.edit_rules = active["temporary"]
        versions.adapter.activation_patches.clear()
        versions.adapter.reset_cache()
        return result | {"rules": draft, "temporary_rules": active["temporary"]}

    def patch_file(self, payload, importing, preview=False):
        """导入先核对基座和词表，所有目标重新校验后一次性提交。"""
        versions = self.versions()
        versions.check()
        path = Path(payload["path"])
        if not importing:
            if path.exists():
                raise ProtocolError("FILE_EXISTS", "请选择新的补丁文件名")
            atomic_json(path, {"schema": 1, "base_model": versions.state["base_model"], "base_vocab": versions.state["base_vocab"], "patches": versions.state["patches"]})
            return {"path": str(path)}
        if path.stat().st_size > 16 * 1024**2:
            raise ProtocolError("INVALID_PATCH", "补丁文件超过 16 MiB")
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("schema") != 1 or any(data.get(k) != versions.state[k] for k in ("base_model", "base_vocab")):
            raise ProtocolError("INCOMPATIBLE_PATCH", "补丁的基座模型或词表不兼容")
        known = {p["id"]: p for p in versions.state["patches"]}
        patches = []
        for patch in data.get("patches", []):
            if patch["id"] in known:
                if {k:v for k,v in patch.items() if k not in ("enabled", "owner", "source_owner")} != {k:v for k,v in known[patch["id"]].items() if k not in ("enabled", "owner", "source_owner")}:
                    raise ProtocolError("PATCH_CONFLICT", "同编号补丁内容不同")
                continue
            if patch["type"] == "tensor":
                checked = versions.tensor_patch(patch["target"], patch["values"], patch["kind"], patch["title"], patch.get("owner", "import"))
                if checked["shape"] != patch["shape"] or checked["dtype"] != patch["dtype"] or checked["before"] != patch["before"]:
                    raise ProtocolError("INCOMPATIBLE_PATCH", "补丁形状、精度或基座值不匹配")
            elif patch["type"] == "vocabulary":
                if patch["index"] in versions.adapter.tokenizer.all_special_ids or patch["index"] in versions.adapter.tokenizer.get_added_vocab().values():
                    raise ProtocolError("INVALID_PATCH", "不能修改特殊 Token")
            else:
                raise ProtocolError("INVALID_PATCH", "补丁类型未适配")
            # 导入内容拥有独立归属，清空本机规则列表不能撤销导入补丁。
            patch = copy.deepcopy(patch)
            patch["source_owner"] = patch.get("source_owner", patch.get("owner", "import"))
            patch["owner"] = "import"
            patches.append(patch)
        if len(patches) > 128:
            raise ProtocolError("INVALID_PATCH", "单次最多导入 128 个补丁")
        state = copy.deepcopy(versions.state)
        state["patches"].extend(patches)
        versions.build(state)
        versions.tokenizer_for(state)
        if preview:
            # 同行、同列及行列交叉都计入重叠，顺序使用实际补丁重放顺序。
            overlaps = 0
            preceding = list(versions.state["patches"])
            for patch in patches:
                if patch.get("enabled", True) and patch["type"] == "tensor":
                    a = patch["target"]
                    for other in preceding:
                        if not other.get("enabled", True) or other["type"] != "tensor":
                            continue
                        b = other["target"]
                        if a["name"] != b["name"]:
                            continue
                        same_axis = a["axis"] == b["axis"]
                        if (same_axis and a["index"] == b["index"] and max(a["start"], b["start"]) < min(a["stop"], b["stop"])) or (not same_axis and a["start"] <= b["index"] < a["stop"] and b["start"] <= a["index"] < b["stop"]):
                            overlaps += 1
                preceding.append(patch)
            return {"before": f"当前 {len(known)} 个补丁", "after": f"新增 {len(patches)} 个补丁；同位置按文件顺序应用",
                "count": len(patches), "overlaps": overlaps} | self.preview_identity()
        return versions.add(patches, self.runtime.event) if patches else versions.summary() | {"unchanged": True}

    @contextmanager
    def analysis_store(self, payload):
        """独立读取所选回复的记录，不替换当前会话或运行器的活动记录。"""
        self.progress("正在打开观测记录")
        store, owned = self.runtime.store, False
        if payload.get("trace_path"):
            root = Path(payload.get("project_root", "")).resolve() / "sessions"
            path = Path(payload["trace_path"]).resolve()
            if not payload.get("project_root") or not path.is_relative_to(root) or len(path.relative_to(root).parts) != 3 or path.parent.name != "records":
                raise ProtocolError("INVALID_TRACE_PATH", "所选回复的观测记录不属于当前项目")
            if not (path / "manifest.json").is_file():
                raise ProtocolError("NO_TRACE", "所选回复的观测记录已清理或不可用")
            if store is None or store.root != path:
                store, owned = TraceStore(path, readonly=True), True
        try:
            if store is None:
                raise ProtocolError("NO_TRACE", "请先选择观测模式下的回复，或打开来源观测记录")
            if payload.get("run_id") and payload["run_id"] != store.manifest.get("run_id"):
                raise ProtocolError("SOURCE_MISMATCH", "所选回复与观测记录不匹配，未分析其他回复")
            if store.manifest.get("capture_policy") == "none":
                raise ProtocolError("NO_ACTIVATIONS", "该回复使用普通对话模式，没有采集激活数据")
            yield store
        finally:
            if owned:
                store.close()

    def activation_detail(self, payload):
        """数值详情沿用分析来源，读取失败时不退回其他会话。"""
        with self.analysis_store(payload) as store:
            return self.activation_detail_store(store, payload)

    def activation_detail_store(self, store, payload):
        """逐 Token 查询候选通道数值，分页不截断可查询位置总量。"""
        layer, neuron = int(payload["layer"]), int(payload["neuron"])
        page_size = max(1, min(1000, int(payload.get("page_size", 200))))
        requested_page = max(1, int(payload.get("page", 1)))
        self.progress("正在等待采集数据落盘")
        store.flush(cancel=self.check_cancel)
        self.progress("正在扫描数值记录")
        with store.lock:
            records = store.db.execute("SELECT context,refs,forward_id FROM operations WHERE branch=? AND layer=? AND operation='mlp' ORDER BY rowid", (payload.get("branch_id", "base"),layer)).fetchall()
        rows = []
        missing = 0
        positions = set(payload["positions"]) if payload.get("positions") is not None else None
        self.progress("正在读取通道数值", 0, len(records))
        for record_index, record in enumerate(records):
            self.progress("正在读取通道数值", record_index, len(records))
            context, refs = json.loads(record["context"]), json.loads(record["refs"])
            before = store.load(refs.get("baseline"), cancel=self.check_cancel)
            after = store.load(refs.get("output"), cancel=self.check_cancel)
            if before is None or after is None:
                missing += len(context.get("positions", []))
                continue
            if not 0 <= neuron < before.shape[-1]:
                raise ProtocolError("INVALID_TARGET", "候选通道超出记录范围")
            for index, position in enumerate(context["positions"]):
                if index % 256 == 0:
                    self.check_cancel()
                if positions is not None and position not in positions:
                    continue
                rows.append({"position": position, "before": float(before.reshape(-1,before.shape[-1])[index,neuron]),
                    "after": float(after.reshape(-1,after.shape[-1])[index,neuron]), "forward_id": record["forward_id"]})
        self.progress("正在读取通道数值", len(records), len(records))
        total = len(rows)
        pages = max(1, (total + page_size - 1) // page_size)
        page = min(requested_page, pages)
        return {"rows": rows[(page-1)*page_size:page*page_size], "total": total, "page": page,
            "page_size": page_size, "page_count": pages, "missing": missing, "layer": layer, "neuron": neuron,
            **{key: payload[key] for key in ("trace_path", "run_id", "positions", "branch_id", "client_revision") if key in payload}}

    def activation(self, payload):
        """分析所选回复的真实采集数据，不执行推理或改变当前会话。"""
        with self.analysis_store(payload) as store:
            result = self.activation_store(store, payload)
            for key in ("trace_path", "run_id", "positions", "branch_id"):
                if key in payload:
                    result[key] = payload[key]
            try:
                return self.cache_activation(result, payload) if payload.get("paged") else result
            except Cancelled:
                # 新分析未发布时清理它自己的报告，不触及此前成功的分析。
                if result.get("path"):
                    Path(result["path"]).unlink(missing_ok=True)
                raise

    def cache_activation(self, result, payload):
        """缓存完整排名与来源；查询仅访问结果数据库，不再读取张量或运行推理。"""
        identity = uuid.uuid4().hex
        path = Path(self.analysis_cache.name) / (identity + ".sqlite")
        metadata = {key: value for key, value in result.items() if key != "rows"}
        # 无项目的 MCP 分析同样保留完整文件，分页不能使导出只剩当前页。
        if not metadata.get("path"):
            report_path = path.with_suffix(".json")
            self.save_activation(report_path, result)
            metadata["path"] = str(report_path)
        metadata["analysis_id"] = identity
        metadata["analysis_project"] = str(Path(payload.get("project_root", "")).resolve())
        try:
            with closing(sqlite3.connect(path)) as db, db:
                self.progress("正在建立结果索引", 0, len(result.get("rows", [])))
                db.execute("CREATE TABLE metadata(data TEXT NOT NULL)")
                db.execute("INSERT INTO metadata VALUES(?)", (json.dumps(metadata, ensure_ascii=False, allow_nan=False),))
                db.execute("CREATE TABLE ranked(layer INTEGER, neuron INTEGER, score REAL, samples INTEGER, trigger TEXT, data TEXT)")
                for index, row in enumerate(result.get("rows", [])):
                    self.progress("正在建立结果索引", index, len(result["rows"]))
                    db.execute("INSERT INTO ranked VALUES(?,?,?,?,?,?)", (row["layer"], row["neuron"], row["score"], row["samples"],
                        row.get("trigger", ""), json.dumps(row, ensure_ascii=False, allow_nan=False)))
                self.progress("正在建立结果索引", len(result.get("rows", [])), len(result.get("rows", [])))
                self.progress("正在整理查询索引")
                # SQLite 排序时也能响应停止，不必等待全部索引建立完成。
                db.set_progress_handler(lambda: int(self.cancelled or self.runtime.shutdown.is_set()), 1000)
                try:
                    db.execute("CREATE INDEX rank_score ON ranked(score DESC,layer,neuron)")
                    db.execute("CREATE INDEX rank_location ON ranked(layer,neuron)")
                except sqlite3.OperationalError:
                    self.check_cancel()
                    raise
                finally:
                    db.set_progress_handler(None, 0)
            return self.query_activation(dict(payload, analysis_id=identity))
        except Exception:
            # 未完成的快照不发布，取消与失败均清理本次临时数据库及副本。
            path.unlink(missing_ok=True)
            if not result.get("path"):
                path.with_suffix(".json").unlink(missing_ok=True)
            raise

    def open_activation(self, payload):
        """导入完整分析文件为独立快照，分页查询不依赖原始记录仍在运行。"""
        self.progress("正在读取分析文件")
        try:
            result = json.loads(Path(payload["path"]).read_text(encoding="utf-8"))
            if not isinstance(result, dict) or result.get("action", "activation") != "activation" or not isinstance(result.get("rows"), list):
                raise ValueError()
            for index, row in enumerate(result["rows"]):
                self.progress("正在校验分析文件", index, len(result["rows"]))
                if not isinstance(row, dict) or any(type(row.get(key)) is not int or row[key] < 0 for key in ("layer", "neuron", "samples")) or not math.isfinite(float(row["score"])):
                    raise ValueError()
        except (OSError, ValueError, KeyError, TypeError) as exc:
            raise ProtocolError("INVALID_REPORT", "分析文件不可读或格式无效") from exc
        # 复制到本次快照，后续外部修改原文件不会改变当前分析或保存内容。
        result.pop("path", None)
        return self.cache_activation(result, payload)

    def query_activation(self, payload):
        """完整结果先筛选排序后分页；参数绑定与字段白名单保证查询边界。"""
        self.progress("正在查询分析结果")
        identity = str(payload.get("analysis_id", ""))
        if not re.fullmatch(r"[0-9a-f]{32}", identity):
            raise ProtocolError("INVALID_ANALYSIS", "分析结果身份无效，请重新分析")
        path = Path(self.analysis_cache.name) / (identity + ".sqlite")
        if not path.is_file():
            raise ProtocolError("ANALYSIS_EXPIRED", "分析结果已失效，请打开保存的分析或重新分析")
        try:
            page_size = max(1, min(1000, int(payload.get("page_size", 100))))
            page = max(1, int(payload.get("page", 1)))
            clauses, args = [], []
            search = str(payload.get("query", "")).strip().lower()
            if search:
                clauses.append("(instr(CAST(layer AS TEXT),?)>0 OR instr(CAST(neuron AS TEXT),?)>0 OR instr(lower(trigger),?)>0)")
                args.extend([search] * 3)
            for field, comparison, key in (("layer", ">=", "layer_min"), ("layer", "<=", "layer_max"),
                    ("score", ">=", "score_min"), ("samples", ">=", "samples_min")):
                if payload.get(key) not in (None, ""):
                    value = float(payload[key]) if field == "score" else int(payload[key])
                    if not math.isfinite(value):
                        raise ValueError()
                    clauses.append(field + comparison + "?")
                    args.append(value)
            sort = payload.get("sort", "score")
            if sort not in ("layer", "neuron", "score", "samples", "trigger"):
                raise ValueError()
            direction = "ASC" if payload.get("ascending") else "DESC"
        except (ValueError, TypeError, OverflowError) as exc:
            raise ProtocolError("INVALID_ANALYSIS", "分页或筛选条件无效") from exc
        with closing(sqlite3.connect(path)) as db:
            db.set_progress_handler(lambda: int(self.cancelled or self.runtime.shutdown.is_set()), 1000)
            try:
                result = json.loads(db.execute("SELECT data FROM metadata").fetchone()[0])
                if result.pop("analysis_project") != str(Path(payload.get("project_root", "")).resolve()):
                    raise ProtocolError("SOURCE_MISMATCH", "分析结果不属于当前项目")
                where = " WHERE " + " AND ".join(clauses) if clauses else ""
                total = db.execute("SELECT count(*) FROM ranked" + where, args).fetchone()[0]
                pages = max(1, (total + page_size - 1) // page_size)
                page = min(page, pages)
                result["rows"] = [json.loads(row[0]) for row in db.execute(
                    "SELECT data FROM ranked" + where + " ORDER BY " + sort + " " + direction + ",layer ASC,neuron ASC LIMIT ? OFFSET ?",
                    args + [page_size, (page - 1) * page_size])]
                result.update(total=total, unfiltered_total=db.execute("SELECT count(*) FROM ranked").fetchone()[0],
                    page=page, page_size=page_size, page_count=pages, client_revision=payload.get("client_revision"))
                return result
            except sqlite3.OperationalError:
                self.check_cancel()
                raise

    def activation_store(self, store, payload):
        """从真实采集的 MLP 门控乘积统计排名；缺失数据不补零。"""
        result = self.rank_store(store, payload)
        result["provenance"] = {key: store.manifest.get(key) for key in
            ("model", "generation", "messages", "template", "token_ids", "user_positions", "software", "run_id", "capture_policy", "rules_snapshot", "weights_revision", "vocabulary_revision", "environment")}
        result["normalization"] = {"method":"每个 Token 的绝对激活除以该层通道均方根，再对有效 Token 取平均", "reference":"当前层当前 Token 的全部通道"}
        result["range"] = {"start":payload.get("start",0),"stop":payload.get("stop",0),"layer":payload.get("layer",-1)}
        result["run_id"] = store.manifest.get("run_id", "")
        result["branch_id"] = payload.get("branch_id", "base")
        for key in ("trace_path", "run_id", "positions", "branch_id"):
            if key in payload:
                result[key] = payload[key]
        if payload.get("baseline"):
            self.progress("正在分析基线记录")
            baseline = TraceStore(payload["baseline"], readonly=True)
            try:
                if baseline.manifest.get("model", {}).get("model_revision") != store.manifest.get("model", {}).get("model_revision"):
                    raise ProtocolError("INCOMPATIBLE_TRACE", "基线记录的模型版本不同")
                reference = self.rank_store(baseline, dict(payload, all_rows=True, branch_id=payload.get("baseline_branch", "base")))
                lookup = {(r["layer"], r["neuron"]): r for r in reference["rows"]}
                for index, row in enumerate(result["rows"]):
                    self.progress("正在比较基线结果", index, len(result["rows"]))
                    other = lookup.get((row["layer"], row["neuron"]))
                    row["baseline_score"] = other["score"] if other else None
                    row["difference"] = row["score"] - other["score"] if other else None
                    row["baseline_frequency"] = other["frequency"] if other else None
                    row["frequency_difference"] = row["frequency"] - other["frequency"] if other else None
                    row["baseline_samples"] = other["samples"] if other else None
                result["baseline_tokens"] = reference["effective_tokens"]
                result["baseline_provenance"] = {"path":str(baseline.root),"generation":baseline.manifest.get("generation"),
                    "messages":baseline.manifest.get("messages"),"model":baseline.manifest.get("model"),"frequency_threshold":reference["frequency_threshold"]}
            finally:
                baseline.close()
        if payload.get("project_root"):
            path = Path(payload["project_root"]) / "features" / ("activation-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".json")
            path.parent.mkdir(parents=True, exist_ok=True)
            self.save_activation(path, result)
            result["path"] = str(path)
        return result

    def rank_store(self, store, payload):
        """按相同采集点和统计方法合并，各层归一化使用每个 Token 的层内均方根。"""
        self.progress("正在等待采集数据落盘")
        store.flush(cancel=self.check_cancel)
        self.progress("正在扫描观测记录")
        branch = payload.get("branch_id", "base")
        with store.lock:
            records = store.db.execute("SELECT context,refs,layer,forward_id FROM operations WHERE branch=? AND operation='mlp' ORDER BY rowid", (branch,)).fetchall()
        scope = payload.get("scope", "input")
        requested_positions = payload.get("positions")
        if requested_positions is not None and (not isinstance(requested_positions, list) or not requested_positions or len(requested_positions) > 1000000 or any(type(p) is not int or p < 0 for p in requested_positions)):
            raise ProtocolError("INVALID_ANALYSIS", "请选择有效的回复 Token 范围")
        requested_positions = set(requested_positions) if requested_positions is not None else None
        metric = payload.get("metric", "normalized")
        source = payload.get("source", "baseline")
        if metric not in ("peak", "mean", "frequency", "normalized") or source not in ("baseline", "output"):
            raise ProtocolError("INVALID_ANALYSIS", "统计方式或采集点无效")
        input_length = len(store.manifest.get("token_ids", []))
        user_positions = set(store.manifest.get("user_positions", []))
        if scope == "input" and "user_positions" not in store.manifest:
            raise ProtocolError("SCOPE_UNAVAILABLE", "旧记录没有本轮输入范围，请选择全部上下文或指定位置")
        layers, missing = {}, 0
        self.progress("正在读取张量并计算统计", 0, len(records))
        for record_index, record in enumerate(records):
            self.progress("正在读取张量并计算统计", record_index, len(records))
            layer = record["layer"]
            if int(payload.get("layer", -1)) >= 0 and layer != int(payload["layer"]):
                continue
            context, refs = json.loads(record["context"]), json.loads(record["refs"])
            positions = context.get("positions", [])
            selected = [i for i, p in enumerate(positions) if (p in requested_positions if requested_positions is not None else (scope == "all" or scope == "input" and p in user_positions or scope == "reply" and p >= input_length or scope == "range" and int(payload.get("start", 0)) <= p <= int(payload.get("stop", 0))))]
            if not selected:
                continue
            values = store.load(refs.get(source), cancel=self.check_cancel)
            if values is not None:
                rows = values.reshape(-1, values.shape[-1])[selected]
                summary = activation_summary(rows, [positions[i] for i in selected]).cpu()
                self.check_cancel()
            elif len(selected) == len(positions):
                summary = store.load(refs.get("summary_" + source), cancel=self.check_cancel)
            elif scope == "input":
                summary = store.load(refs.get("user_summary_" + source), cancel=self.check_cancel)
            else:
                summary = None
            if summary is None:
                missing += len(selected)
                continue
            summary = summary.float()
            count = len(selected)
            entry = layers.setdefault(layer, {"summary": summary.clone(), "count": 0, "forwards": [record["forward_id"]] * summary.shape[1], "positions": set()})
            if entry["count"]:
                previous = entry["summary"]
                larger = summary[0] > previous[0]
                previous[0:2, larger] = summary[0:2, larger]
                previous[4, larger] = summary[4, larger]
                for index in larger.nonzero().flatten().tolist():
                    entry["forwards"][index] = record["forward_id"]
                previous[[2, 3, 5]] += summary[[2, 3, 5]]
                previous[6] = torch.maximum(previous[6], summary[6])
                previous[7] = torch.minimum(previous[7], summary[7])
            entry["count"] += count
            entry["positions"].update(positions[i] for i in selected)
        self.progress("正在读取张量并计算统计", len(records), len(records))
        rows, effective = [], set()
        total_neurons = sum(entry["summary"].shape[1] for entry in layers.values())
        self.progress("正在汇总神经元排名", 0, total_neurons, "个")
        for layer, entry in layers.items():
            summary, count = entry["summary"], entry["count"]
            effective.update(entry["positions"])
            scores = summary[{"peak": 0, "mean": 2, "frequency": 3, "normalized": 5}[metric]] / (1 if metric == "peak" else count)
            order = scores.argsort(descending=True)
            ranks = torch.empty_like(order)
            ranks[order] = torch.arange(order.numel()) + 1
            for neuron in range(scores.numel()):
                if neuron % 128 == 0:
                    self.progress("正在汇总神经元排名", len(rows), total_neurons, "个")
                rows.append({"layer": layer, "neuron": neuron, "score": float(scores[neuron]), "peak": float(summary[1, neuron]),
                    "mean": float(summary[2, neuron]/count), "frequency": float(summary[3, neuron]/count), "positive": float(summary[6, neuron]),
                    "negative": float(summary[7, neuron]), "position": int(summary[4, neuron]), "forward_id": entry["forwards"][neuron],
                    "samples": count, "layer_rank": int(ranks[neuron])})
        self.progress("正在汇总神经元排名", len(rows), total_neurons, "个")
        self.progress("正在排序分析结果")
        rows.sort(key=lambda row: row["score"], reverse=True)
        # 触发文本仅从记录解码；不能用另一模型的词表解释历史编号。
        token_text = {}
        same_model = self.runtime.adapter is not None and self.runtime.adapter.info.get("model_revision") == store.manifest.get("model", {}).get("model_revision")
        if same_model:
            tokens = store.manifest.get("token_ids", [])
            for i, token in enumerate(tokens):
                if i % 128 == 0:
                    self.progress("正在解码触发 Token", i, len(tokens), "个")
                token_text[i] = self.runtime.adapter.tokenizer.decode([token], skip_special_tokens=False)
        self.progress("正在读取触发记录")
        with store.lock:
            events = store.db.execute("SELECT data FROM events WHERE json_extract(data,'$.name') IN ('token.sampled','run.started') AND json_extract(data,'$.branch_id')=?", (branch,)).fetchall()
        for event_index, event in enumerate(events):
            if event_index % 128 == 0:
                self.progress("正在读取触发记录", event_index, len(events))
            message = json.loads(event["data"])
            item = message["payload"]
            if message["name"] == "run.started":
                for token in item.get("tokens", []):
                    if token["position"] not in token_text:
                        token_text[token["position"]] = token.get("text", "解码未保存")
            else:
                token_text[item["position"]] = item.get("display_text", item.get("text", ""))
        for row_index, row in enumerate(rows):
            if row_index % 256 == 0:
                self.progress("正在匹配触发 Token", row_index, len(rows))
            row["trigger"] = token_text.get(row["position"], "解码未保存")
        return {"rows": rows, "layers": len(layers), "effective_tokens": len(effective), "missing_samples": missing,
            "metric": metric, "source": source, "scope": scope, "frequency_threshold": .01,
            "capture_point": "门控激活与上投影的乘积，下投影之前", "trace": str(store.root),
            "note": "所选内容没有可用激活数据；请确认采集范围。摘要记录无法展开任意片段，最后一个输出 Token 尚未送回模型。" if not rows else "高响应仅作候选；末尾生成 Token 尚未采集"}

    def safe_evaluate(self, adapter, sample):
        """单条失败单独记录，允许其余样例继续，取消仍立即传播。"""
        try:
            result = self.evaluate(adapter, sample)
            result["status"] = "evaluated" if result["passed"] is not None else "not_applicable" if result.get("reason") == "目标须编码为单个 Token" else "unevaluated"
            return result
        except Cancelled:
            raise
        except Exception as exc:
            return {"text": "", "probability": None, "rank": None, "target_ids": [], "passed": False,
                "status": "failed", "reason": "评测失败：" + str(exc)}

    def evaluate(self, adapter, sample):
        """按样例记录的完整生成配置评测，目标指标取首个生成位置。"""
        messages = sample.get("messages") or [{"role": "user", "content": sample.get("input", "")}]
        if not isinstance(messages, list) or not messages or len(messages) > 128:
            raise ProtocolError("INVALID_SAMPLE", "样例输入无效")
        from .worker import generation_options, decode_response
        from .model import sample_logits
        generation = generation_options(adapter.info, {"temperature":0,"seed":42}|sample.get("generation",{}))
        thinking = bool(generation["template_options"].get("enable_thinking", False))
        if thinking and adapter.info.get("capabilities", {}).get("thinking_mode") != "supported":
            raise ProtocolError("UNSUPPORTED_THINKING", "当前模型不支持深度思考")
        text = adapter.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True, **generation["template_options"])
        ids = adapter.tokenizer.encode(text, add_special_tokens=False)
        maximum = int(generation.get("max_new_tokens",sample.get("max_new_tokens",16)))
        generation["max_new_tokens"] = maximum
        if not 1 <= maximum <= 64:
            raise ProtocolError("INVALID_SAMPLE", "样例输出上限须为 1 至 64")
        if len(ids)+maximum > int(adapter.config.max_position_embeddings):
            raise ProtocolError("CONTEXT_OVERFLOW", "样例超出模型上下文容量")
        target = sample.get("target", "")
        target_ids = adapter.tokenizer.encode(target, add_special_tokens=False) if target else []
        if any(t >= adapter.model.get_output_embeddings().weight.shape[0] for t in target_ids):
            raise ProtocolError("INVALID_TARGET", "目标 Token 没有对应输出权重")
        adapter.reset_cache()
        adapter.prompt_length = len(ids)
        inputs = torch.tensor([ids], device=adapter.device)
        generated, probability, rank = [], None, None
        generator = torch.Generator(device=adapter.device).manual_seed(int(generation.get("seed",42)))
        try:
            with torch.inference_mode():
                for step in range(maximum):
                    self.check_cancel()
                    positions = list(range(len(ids))) if step == 0 else [len(ids)+step-1]
                    logits = adapter.forward(inputs, positions, lambda layer, op, fn, values: fn())
                    scores = logits[0,-1].float()
                    if step == 0 and len(target_ids) == 1:
                        probability = float(scores.softmax(-1)[target_ids[0]])
                        rank = int((scores > scores[target_ids[0]]).sum())+1
                    chosen,_,_ = sample_logits(logits,ids+generated,generation,generator,generated)
                    generated.append(chosen)
                    if chosen in adapter.tokenizer.all_special_ids and chosen == adapter.tokenizer.eos_token_id:
                        break
                    inputs = torch.tensor([[chosen]], device=adapter.device)
            text, reasoning, thinking_complete = decode_response(adapter.tokenizer, generated, thinking)
            criterion, expected = sample.get("criterion", "none"), sample.get("expected", "")
            passed = None
            reason = "未设置条件"
            if criterion in ("contains", "exact", "regex"):
                if not expected:
                    raise ProtocolError("INVALID_SAMPLE", "文本通过条件不能为空")
                passed = expected in text if criterion == "contains" else expected == text if criterion == "exact" else bool(re.search(expected,text))
                reason = "符合文本条件" if passed else "未满足文本条件"
            elif criterion in ("probability", "rank"):
                if probability is None:
                    reason = "目标须编码为单个 Token"
                else:
                    threshold = float(sample.get("threshold", .1 if criterion == "probability" else 10))
                    if not math.isfinite(threshold) or threshold < 0:
                        raise ProtocolError("INVALID_SAMPLE", "阈值无效")
                    passed = probability >= threshold if criterion == "probability" else rank <= threshold
                    reason = "符合目标条件" if passed else "未满足目标条件"
            return {"text": text, "probability": probability, "rank": rank, "target_ids": target_ids, "passed": passed, "reason": reason,
                "generation":generation,"token_ids":ids,"template":adapter.tokenizer.chat_template,"generated_ids":generated,
                "reasoning_content":reasoning,"thinking_complete":thinking_complete}
        finally:
            adapter.reset_cache()

    def regression(self, payload, export=False):
        """原版与编辑版同配置运行，任何异常都恢复权重、临时规则和缓存。"""
        versions = self.versions()
        versions.check()
        adapter = versions.adapter
        samples = payload.get("samples", [])
        if not isinstance(samples, list) or not 1 <= len(samples) <= 100:
            raise ProtocolError("INVALID_SAMPLES", "请添加 1 至 100 条验证样例")
        root = Path(payload.get("project_root", ""))
        if not root.is_dir() or not payload.get("project_root"):
            raise ProtocolError("NO_PROJECT", "请先打开项目")
        report = {"schema": 1, "model_revision": adapter.info["model_revision"], "vocabulary": vocabulary_hash(adapter.tokenizer),
            "weights_revision": adapter.weights_revision, "samples": samples, "rows": [], "environment": {"torch": torch.__version__,
            "platform": platform.platform(), "device": str(adapter.device), "dtype": str(next(adapter.model.parameters()).dtype)},
            "generation": {"default_temperature":0,"default_seed":42,"thinking":False}, "temporary_rules": copy.deepcopy(adapter.edit_rules),
            "date": datetime.datetime.now().isoformat(), "patches": versions.summary()["patches"]}
        original_rules = adapter.edit_rules
        original_legacy = adapter.activation_patches
        tokenizer = adapter.tokenizer
        adapter.edit_rules, adapter.activation_patches = [], {}
        try:
            if export:
                from .protocol import command
                self.runtime.export_model(command("model.export", {"destination": payload["destination"]}), emit_result=False)
                self.check_cancel()
                from .model import QwenAdapter
                exported = QwenAdapter.load(payload["destination"], "cpu", "float32")
                same_vocab = vocabulary_hash(exported.tokenizer) == vocabulary_hash(tokenizer)
                current_parameters = dict(adapter.model.named_parameters())
                reloaded_parameters = dict(exported.model.named_parameters())
                exact_weights = current_parameters.keys() == reloaded_parameters.keys()
                for name, parameter in current_parameters.items():
                    self.check_cancel()
                    other = reloaded_parameters.get(name)
                    if other is None or parameter.shape != other.shape or not torch.equal(parameter.detach().cpu(), other.detach().cpu().to(parameter.dtype)):
                        exact_weights = False
                        break
                shared = lambda model: model.get_input_embeddings().weight.data_ptr() == model.get_output_embeddings().weight.data_ptr()
                same_shared = shared(adapter.model) == shared(exported.model)
                report["export"] = {"path": payload["destination"], "vocabulary_equal": same_vocab, "weights_equal": exact_weights, "shared_equal": same_shared,
                    "atol": 1e-5, "rtol": 1e-4}
                try:
                    for i, sample in enumerate(samples):
                        self.runtime.event("feature.progress", {"message": f"验收样例 {i+1}/{len(samples)}"})
                        before, after = self.safe_evaluate(adapter, sample), self.safe_evaluate(exported, sample)
                        p, q = before["probability"], after["probability"]
                        close = p is None and q is None or p is not None and q is not None and math.isclose(p, q, abs_tol=1e-5, rel_tol=1e-4)
                        text_match = bool(before["passed"] and after["passed"]) if sample.get("criterion") in ("exact","contains","regex") else before["text"] == after["text"]
                        match = before["status"] != "failed" and after["status"] != "failed" and text_match and close
                        report["rows"].append({"base": before, "edited": after, "consistent": match})
                    report["export"]["passed"] = same_vocab and exact_weights and same_shared and all(r["consistent"] for r in report["rows"])
                finally:
                    exported.reset_cache()
                    del exported
            else:
                changes = versions.base_tensors()
                saved = {name: parameter.detach().clone() for name, (parameter, _) in changes.items()}
                base_tokenizer = versions.tokenizer_for(versions.state, base=True)
                try:
                    with torch.no_grad():
                        for name, (parameter, base) in changes.items():
                            parameter.copy_(base)
                    adapter.tokenizer = base_tokenizer
                    baseline = []
                    for i, sample in enumerate(samples):
                        self.runtime.event("feature.progress", {"message": f"原版样例 {i+1}/{len(samples)}"})
                        baseline.append(self.safe_evaluate(adapter, sample))
                finally:
                    with torch.no_grad():
                        for name, (parameter, _) in changes.items():
                            parameter.copy_(saved[name])
                    adapter.tokenizer = tokenizer
                adapter.edit_rules, adapter.activation_patches = original_rules, original_legacy
                for i, (sample, before) in enumerate(zip(samples, baseline)):
                    self.runtime.event("feature.progress", {"message": f"编辑版样例 {i+1}/{len(samples)}"})
                    after = self.safe_evaluate(adapter, sample)
                    comparable = before["target_ids"] == after["target_ids"]
                    allowed_drop = sample.get("max_probability_drop")
                    if sample.get("type") == "regular" and allowed_drop is not None:
                        if not comparable or before["probability"] is None or after["probability"] is None:
                            after.update(passed=None, status="unevaluated", reason="退化指标不可比较，目标须为同一单个 Token")
                        else:
                            threshold = float(allowed_drop)
                            if not math.isfinite(threshold) or threshold < 0:
                                after.update(passed=False, status="failed", reason="最大概率降幅须为非负数")
                            else:
                                acceptable = before["probability"]-after["probability"] <= threshold
                                after.update(passed=acceptable if after["passed"] is None else after["passed"] and acceptable,
                                    status="evaluated", reason="退化在允许范围内" if acceptable else "目标概率退化超过允许范围")
                    report["rows"].append({"base": before, "edited": after,
                        "comparable": comparable})
            directory = root / "features"
            directory.mkdir(exist_ok=True)
            atomic_json(directory / "samples.json", samples)
            path = directory / ("report-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".json")
            atomic_json(path, report)
            return report | {"path": str(path)}
        finally:
            adapter.tokenizer, adapter.edit_rules, adapter.activation_patches = tokenizer, original_rules, original_legacy
            adapter.reset_cache()
