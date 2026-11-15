"""独立推理进程：命令读取不被计算边界等待阻塞。"""
from __future__ import annotations

import copy
import json
import math
import os
from pathlib import Path
import queue
import shutil
import sys
import threading
import time
import traceback
import uuid

from .control import Cancelled, StepController
from .model import OP_NAMES, QwenAdapter, inspect_model, sample_logits
from .protocol import Emitter, MAX_LINE, ProtocolError, parse_command
from .traces import TraceStore
from .storage import ProjectBudget, atomic_json


def conversation_messages(messages):
    """推理历史只包含可见正式回复；中断的纯思考保留在界面，不伪造空助手回合。"""
    return [{"role": message["role"], "content": message["content"]}
            for message in messages
            if message["role"] != "assistant" or message["content"].strip()]


def generation_options(info, generation):
    """按模型和思考模式补齐官方推荐采样参数，显式传入的设置优先。"""
    result = copy.deepcopy(generation)
    options = result.setdefault("template_options", {})
    options.setdefault("enable_thinking", False)
    thinking = bool(options["enable_thinking"])
    hybrid = info.get("model_type") == "qwen3_5"
    defaults = {"temperature": 1.0 if hybrid and thinking else .7,
                "top_p": .95 if hybrid and thinking else .8, "top_k": 20,
                "presence_penalty": 1.5 if hybrid else 0.0, "repetition_penalty": 1.0}
    for key, value in defaults.items():
        result.setdefault(key, value)
    return result


def decode_response(tokenizer, generated, thinking_enabled, markers=None):
    """在移除特殊Token前按思考边界分流，避免思考文本混入正式回答。"""
    if markers is None:
        vocabulary = tokenizer.get_vocab()
        markers = vocabulary.get("<think>"), vocabulary.get("</think>")
    start, end = markers
    decode = lambda ids: tokenizer.decode(ids, skip_special_tokens=True, clean_up_tokenization_spaces=False)
    start_index = generated.index(start) + 1 if start is not None and start in generated else 0
    has_start = start_index > 0
    if thinking_enabled or has_start:
        if end is not None and end in generated:
            boundary = generated.index(end)
            return decode(generated[boundary + 1:]), decode(generated[start_index:boundary]), True
        return "", decode(generated[start_index:]), False
    return decode(generated), "", True


def finite(value):
    """JSON 中用空值表示非有限标量，原始数值仍保存在二进制块。"""
    value = float(value)
    return value if math.isfinite(value) else None


def table(value, rows=6, cols=8):
    """只返回有界二维切片，绝不把大张量装进协议帧。"""
    if value is None:
        return None
    if value.ndim == 0:
        value = value.reshape(1, 1)
    elif value.ndim == 1:
        value = value.unsqueeze(0)
    else:
        value = value.reshape(-1, value.shape[-1])
    return [[finite(x) for x in row] for row in value[:rows, :cols].float().tolist()]


class Runtime:
    """一个模型实例和一个串行执行线程；只读切片可以独立响应。"""

    def __init__(self, emitter=None):
        self.emitter = emitter or Emitter("runtime-" + uuid.uuid4().hex[:8])
        self.tasks = queue.Queue(maxsize=64)
        self.adapter = None
        self.store = None
        self.controller = None
        self.busy = False
        self.reserved = 0
        self.capture_policy = "standard"
        self.shutdown = threading.Event()
        self.lock = threading.RLock()
        self.read_lock = threading.RLock()
        self.last_request = None
        self.run_id = None
        self.branch_id = "base"
        self.forward_count = 0
        self.degraded_notified = False
        self.loading_cancel = threading.Event()
        self.loading = False
        self.selected_layer = 0
        from .features import Features
        self.features = Features(self)
        from .neurons import Neurons
        self.neurons = Neurons(self)
        self.anomaly = None
        self.thread = threading.Thread(target=self.work, name="模型串行执行", daemon=True)
        self.thread.start()
        self.emitter.listener = self.record_event

    def record_event(self, message):
        """运行事件可靠落盘；读取响应不重复写进历史记录。"""
        if self.store and not self.store.readonly and message["kind"] == "event" and message["name"].startswith(("run.", "operation.", "token.", "text.", "step.", "branch.", "capture.", "allocator.")):
            self.store.event(message)

    def event(self, name, payload=None, context=None):
        """附加当前运行和分支身份。"""
        return self.emitter.send(name, payload, context=context, run_id=self.run_id, branch_id=self.branch_id)

    def result(self, request, name, payload):
        """关联请求响应，界面可忽略已经过期的查询。"""
        return self.emitter.send(name, payload, request, "result", run_id=self.run_id, branch_id=self.branch_id)

    def handle(self, request):
        """读取线程只执行轻量控制，重任务交给串行执行线程。"""
        name, payload = request["name"], request["payload"]
        if name == "hello":
            self.result(request, "hello.ack", {"version": "1.0.4", "adapter": "qwen2-qwen3.5-eager-5.3.0", "max_frame_bytes": MAX_LINE})
        elif name == "run.cancel":
            # 旧会话的停止请求不能取消新的运行；未指定编号的旧接口保持兼容。
            if payload.get("run_id") and payload["run_id"] != self.run_id:
                raise ProtocolError("STALE_RUN", "运行已变化，未停止其他任务")
            self.features.cancelled = True
            if self.loading:
                self.loading_cancel.set()
            if self.controller:
                self.controller.cancel()
            self.result(request, "cancel.accepted", {"accepted": True})
        elif name == "capture.select":
            self.selected_layer = int(payload.get("layer", 0))
        elif name == "neurons.configure":
            self.result(request, "neurons.configured", self.neurons.configure(payload))
        elif name == "neurons.query":
            threading.Thread(target=self.read_task, args=(request,), daemon=True).start()
        elif name == "run.checkpoint":
            if not self.controller or not self.last_request:
                raise ProtocolError("NOT_RUNNING", "当前没有可保存的运行现场")
            self.controller.save_at_boundary(lambda executed: self.save_checkpoint(request, executed))
        elif name == "step.release":
            if not self.controller:
                raise ProtocolError("NOT_RUNNING", "尚未启动计算")
            result = self.controller.release(request["request_id"], payload["expected_boundary_id"], payload["expected_state_revision"])
            self.result(request, "step.accepted", result)
        elif name == "run.change_mode":
            if self.controller:
                if payload["mode"] != "normal" and self.capture_policy == "none":
                    self.capture_policy = "standard"
                result = self.controller.set_mode(payload["mode"])
                self.event("run.mode_changed", result)
            self.result(request, "mode.accepted", {"mode": payload["mode"]})
        elif name == "debug.configure":
            # 暂停期间也允许更新监视条件，无需进入被暂停的计算队列。
            from .debugging import validate_points
            points = validate_points(self.adapter, payload.get("points", []))
            if self.controller:
                self.controller.configure(points)
            self.result(request, "debug.configured", {"points": points})
        elif name == "shutdown":
            self.shutdown.set()
            self.loading_cancel.set()
            if self.controller:
                self.controller.cancel()
            self.tasks.put(None)
        elif name in ("token.analyze", "feature.request", "model.load", "model.inspect", "model.unload", "run.start", "run.restore", "session.activate", "trace.close", "patch.apply", "patch.restore", "edit.apply", "edit.clear", "model.restore_backup", "model.export", "trace.open", "trace.export", "view.query", "tensor.slice", "scalar.query"):
            with self.lock:
                if self.busy and name in ("feature.request", "model.load", "model.unload", "run.start", "run.restore", "session.activate", "trace.close", "edit.apply", "edit.clear", "model.restore_backup", "model.export", "trace.open", "trace.export"):
                    raise ProtocolError("BUSY", "当前任务尚未结束，请先停止或等待完成")
                if name in ("token.analyze", "view.query", "tensor.slice", "scalar.query"):
                    # 读取不进入计算队列，暂停时查看数据仍能响应。
                    threading.Thread(target=self.read_task, args=(request,), daemon=True).start()
                    return
                if name in ("patch.apply", "patch.restore") and self.busy:
                    self.result(request, "patch.accepted", {"state": "accepted", "effect": "after_current_run"})
                if name == "model.load":
                    self.loading_cancel.clear()
                    self.loading = True
                if name == "feature.request":
                    self.features.cancelled = False
                self.tasks.put_nowait(request)
                if name != "model.inspect":
                    self.reserved += 1
                    self.busy = True
        else:
            raise ProtocolError("UNKNOWN_COMMAND", "未知运行命令")

    def read_task(self, request):
        """后台读取历史快照，不触发模型前向或改变执行位置。"""
        try:
            with self.read_lock:
                if request["name"] == "token.analyze":
                    from .token_analysis import analyze_token
                    self.result(request, "token.analysis", analyze_token(request["payload"], self.store))
                elif request["name"] == "neurons.query":
                    self.result(request, "neurons.snapshot", self.neurons.query(request["payload"]))
                elif request["name"] == "view.query":
                    self.result(request, "view.snapshot", self.query(request["payload"]))
                elif request["name"] == "scalar.query":
                    self.result(request, "scalar.result", self.scalar(request["payload"]))
                else:
                    self.result(request, "tensor.slice.result", self.slice(request["payload"]))
        except Exception as exc:
            self.emitter.error(exc, request)

    def work(self):
        """串行化加载、推理和修改，避免两个任务混用参数或 KV cache。"""
        while True:
            request = self.tasks.get()
            if request is None:
                break
            try:
                name, payload = request["name"], request["payload"]
                if self.shutdown.is_set():
                    continue
                if name == "model.inspect":
                    self.result(request, "model.inspected", inspect_model(payload["path"]))
                elif name == "feature.request":
                    with self.read_lock:
                        result = self.features.dispatch(payload, request=request)
                    self.result(request, "feature.result", result | {"action": payload.get("action")})
                elif name == "model.load":
                    self.event("model.loading", {"path": payload["path"]})
                    try:
                        inspect_model(payload["path"], cancel=self.loading_cancel)
                        with self.read_lock:
                            self.unload()
                        self.event("model.unloaded")
                        self.adapter = QwenAdapter.load(payload["path"], payload.get("device", "cuda:0"), payload.get("dtype", "auto"), self.event, self.loading_cancel)
                        from .model import operation_catalog
                        self.event("model.ready", self.adapter.info | {"weights_revision": self.adapter.weights_revision,
                            "operation_catalog": [operation_catalog(self.adapter.info["config"], i) for i in range(self.adapter.config.num_hidden_layers)]})
                    except Cancelled:
                        self.event("model.cancelled", {"message": "模型加载已取消"})
                    finally:
                        self.loading = False
                elif name == "model.unload":
                    with self.read_lock:
                        self.unload()
                    self.event("model.unloaded")
                elif name == "run.start":
                    self.run(request)
                elif name == "run.restore":
                    self.restore_checkpoint(request)
                elif name == "session.activate":
                    self.activate_session(request)
                elif name == "trace.close":
                    with self.read_lock:
                        self.neurons.reset()
                        if self.store:
                            self.store.close()
                            self.store = None
                        self.last_request = None
                    self.result(request, "trace.closed", {})
                elif name in ("patch.apply", "patch.restore"):
                    self.apply_patch(request)
                elif name in ("edit.apply", "edit.clear", "model.restore_backup"):
                    self.edit(request)
                elif name == "model.export":
                    self.export_model(request)
                elif name == "trace.open":
                    with self.read_lock:
                        self.neurons.reset()
                        if self.store:
                            self.store.close()
                        self.store = TraceStore(payload["path"], readonly=True)
                    self.run_id = self.store.manifest.get("run_id")
                    from .model import operation_catalog
                    config = self.store.manifest.get("model", {}).get("config", {})
                    self.result(request, "trace.opened", {"manifest": self.store.manifest, "catalog": self.store.catalog(), "path": str(self.store.root),
                        "operation_catalog": [operation_catalog(config, i) for i in range(config.get("num_hidden_layers", 0))]})
                elif name == "trace.export":
                    if not self.store:
                        raise ProtocolError("NO_TRACE", "尚无可导出的记录")
                    destination = Path(payload["destination"]).resolve()
                    if destination.exists() or destination.is_relative_to(self.store.root):
                        raise ProtocolError("INVALID_DESTINATION", "请选择尚不存在且位于记录外的目录")
                    self.store.flush()
                    with self.store.lock:
                        if not self.store.readonly:
                            self.store.db.execute("PRAGMA wal_checkpoint(FULL)")
                        shutil.copytree(self.store.root, destination)
                    self.result(request, "trace.exported", {"path": str(destination)})
            except Exception as exc:
                if isinstance(exc, Cancelled) and request["name"] == "feature.request":
                    self.emitter.error(ProtocolError("CANCELLED", "任务已取消，保留原有分析结果"), request)
                else:
                    traceback.print_exc(file=sys.stderr)
                    self.emitter.error(exc, request)
            finally:
                with self.lock:
                    if request["name"] != "model.inspect":
                        self.reserved = max(0, self.reserved - 1)
                    self.busy = self.reserved > 0
                self.tasks.task_done()
        if self.store:
            self.store.close()

    def unload(self):
        """释放模型和缓存，不关闭已保存的 Trace。"""
        self.neurons.reset(disable=True)
        if self.adapter:
            self.adapter.reset_cache()
            self.adapter = None
            import gc
            import torch
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
        self.last_request = None

    def activate_session(self, request):
        """会话只切换上下文；仅显式项目规则快照可以替换当前干预。"""
        from .editing import validate_rules
        if not self.adapter:
            raise ProtocolError("MODEL_NOT_LOADED", "请先加载项目模型")
        value = request["payload"]
        expected = value.get("model_revision")
        if expected and expected != self.adapter.info.get("model_revision"):
            raise ProtocolError("MODEL_CHANGED", "模型文件已变化，不能自动恢复旧的神经元现场，请重新选择模型或建立新会话")
        if "temporary_rules" in value:
            rules = value["temporary_rules"]
            self.adapter.edit_rules = validate_rules(self.adapter, rules, "temporary") if rules else []
        self.adapter.activation_patches.clear()
        self.neurons.reset()
        self.adapter.weights_revision = value.get("weights_revision", self.adapter.weights_revision)
        # 继续递增当前会话版本，不能在恢复后重复使用旧版本号。
        self.adapter.revision_number = int(self.adapter.weights_revision.removeprefix("w"))
        self.adapter.reset_cache()
        with self.read_lock:
            if self.store:
                self.store.close()
                self.store = None
            path = value.get("trace_path")
            if path and Path(path, "manifest.json").is_file():
                self.store = TraceStore(path, readonly=True)
            self.run_id = self.store.manifest.get("run_id") if self.store else None
            self.branch_id = "base"
        self.last_request = None
        self.result(request, "session.activated", {"temporary_rules": self.adapter.edit_rules,
            "weights_revision": self.adapter.weights_revision, "trace_available": self.store is not None})

    def save_checkpoint(self, request, executed):
        """保存确定性重放配方；不复制模型权重或巨型 KV 缓存。"""
        try:
            value = request["payload"]
            project = Path(value["project_root"]).resolve()
            path = Path(value["path"]).resolve()
            if not path.is_relative_to(project / "sessions") or not (project / "project.json").is_file():
                raise ProtocolError("INVALID_CHECKPOINT", "恢复点必须保存在当前项目的会话目录中")
            data = {"schema_version": 1, "executed": executed, "request": self.last_request,
                "sampling_version": 2,
                "model_revision": self.adapter.info.get("model_revision"),
                "device": str(self.adapter.device), "dtype": self.adapter.info.get("dtype"),
                "temporary_rules": self.adapter.edit_rules, "weights_revision": self.adapter.weights_revision,
                "branch_id": self.branch_id}
            atomic_json(path, data)
            if self.store and not self.store.readonly:
                self.store.save_manifest(pinned=True)
            self.result(request, "checkpoint.saved", {"path": str(path), "executed": executed})
        except Exception as exc:
            self.emitter.error(exc, request)

    def restore_checkpoint(self, request):
        """同模型、设备和种子下重建缓存，精确回到保存的运算边界后等待继续。"""
        path = Path(request["payload"]["path"])
        if path.stat().st_size > 2 * 1024**2:
            raise ProtocolError("INVALID_CHECKPOINT", "恢复点文件过大")
        value = json.loads(path.read_text(encoding="utf-8"))
        if not self.adapter or value["model_revision"] != self.adapter.info.get("model_revision"):
            raise ProtocolError("MODEL_CHANGED", "模型与保存现场不一致，无法恢复计算")
        if value["device"] != str(self.adapter.device) or value["dtype"] != self.adapter.info.get("dtype"):
            raise ProtocolError("DEVICE_CHANGED", "恢复计算需要使用保存时的设备和计算精度")
        # 恢复旧缓存不能暗中撤回项目后来应用或清除的神经元规则。
        current_rules = request["payload"].get("temporary_rules", self.adapter.edit_rules)
        if current_rules:
            from .editing import validate_rules
            current_rules = validate_rules(self.adapter, current_rules, "temporary")
        if current_rules != value.get("temporary_rules", []):
            raise ProtocolError("EDIT_RULES_CHANGED", "项目临时规则已变化，无法恢复旧计算现场。请使用当前规则发送新的输入")
        self.activate_session({**request, "payload": value})
        replay = copy.deepcopy(value["request"])
        if value.get("sampling_version", 1) < 2:
            # 旧恢复点按当时的采样和原始历史重放，不能悄悄改变保存边界之前的token。
            old_generation = replay["payload"].setdefault("generation", {})
            for key, default in {"presence_penalty": 0.0, "repetition_penalty": 1.05,
                                 "top_p": .8, "top_k": 20, "temperature": .7}.items():
                old_generation.setdefault(key, default)
            replay["payload"]["resume_legacy_history"] = True
        replay["request_id"] = request["request_id"]
        replay["payload"]["resume_executed"] = value["executed"]
        replay["payload"]["project_root"] = str(path.resolve().parent.parent.parent)
        replay["payload"]["trace_root"] = str(path.resolve().parent / "records")
        replay["payload"]["resume_branch"] = value["branch_id"]
        replay["payload"]["mode"] = "step"
        replay["payload"]["anomaly_monitor"] = request["payload"].get("anomaly_monitor", {})
        self.event("checkpoint.rebuilding", {"executed": value["executed"]})
        self.run(replay)

    def run(self, request, fork=False):
        """多轮消息经过真实模板、真实分词和缓存生成循环。"""
        import torch
        if self.adapter is None:
            raise ProtocolError("MODEL_NOT_LOADED", "请先加载本地模型")
        if getattr(self.adapter, "recovery_required", False):
            raise ProtocolError("RECOVERY_REQUIRED", "模型恢复失败，请从已知备份恢复后重新加载")
        payload = copy.deepcopy(request["payload"])
        adapter, tokenizer = self.adapter, self.adapter.tokenizer
        messages = payload.get("messages", [])
        if not messages or any(m.get("role") not in ("system", "user", "assistant") or not isinstance(m.get("content"), str) for m in messages):
            raise ProtocolError("INVALID_MESSAGES", "对话消息格式无效")
        if not payload.get("resume_legacy_history"):
            messages = conversation_messages(messages)
        generation = generation_options(adapter.info, payload.get("generation", {}))
        thinking_enabled = bool(generation.get("template_options", {}).get("enable_thinking", False))
        vocabulary = tokenizer.get_vocab()
        thinking_markers = vocabulary.get("<think>"), vocabulary.get("</think>")
        if thinking_enabled and adapter.info.get("capabilities", {}).get("thinking_mode") != "supported":
            raise ProtocolError("THINKING_UNSUPPORTED", "当前模型不支持原生深度思考模式")
        context_limit = int(adapter.config.max_position_embeddings)
        requested_maximum = int(generation.get("max_new_tokens", context_limit - 1))
        seed = int(generation.get("seed", 42))
        if not 1 <= requested_maximum < context_limit or not 0 <= seed < 2**63:
            raise ProtocolError("INVALID_GENERATION", "输出上限或随机种子无效")
        stop_strings = generation.get("stop", [])
        if not isinstance(stop_strings, list) or any(not isinstance(s, str) or not s for s in stop_strings):
            raise ProtocolError("INVALID_GENERATION", "停止字符串无效")
        template = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True, **generation.get("template_options", {}))
        encoded = tokenizer(template, add_special_tokens=False, return_offsets_mapping=True)
        ids = encoded["input_ids"]
        adapter.prompt_length = len(ids)
        if len(ids) >= context_limit:
            raise ProtocolError("CONTEXT_TOO_LONG", "输入与历史已占满模型上下文，请清理历史或新建会话")
        maximum = min(requested_maximum, context_limit - len(ids))
        generation["max_new_tokens"] = maximum
        generation["requested_max_new_tokens"] = generation.get("requested_max_new_tokens", requested_maximum)
        mode = payload.get("mode", "observe")
        if payload.get("breakpoints") and mode == "normal":
            mode = "observe"
        if mode not in ("normal", "observe", "step"):
            raise ProtocolError("INVALID_MODE", "运行模式无效")
        capture = payload.get("capture", {})
        policy = capture.get("policy", "standard") if mode != "normal" else "none"
        self.capture_policy = policy
        # 所有入口统一不限采集量，兼容恢复点和 MCP 传来的旧额度字段。
        budget = 0
        if policy not in ("none", "standard", "full"):
            raise ProtocolError("INVALID_CAPTURE", "采集策略无效")
        if not fork:
            # 等待正在读取旧记录的视图完成，再替换数据库连接。
            with self.read_lock:
                if self.store:
                    self.store.close()
                    self.store = None
                self.run_id = uuid.uuid4().hex
                self.branch_id = payload.get("resume_branch", "base")
                root = Path(payload.get("trace_root", str(Path.cwd() / "data/records")))
                project_budget = ProjectBudget(payload["project_root"], root, budget) if payload.get("project_root") else None
                self.store = TraceStore(root / self.run_id, budget=budget, project_budget=project_budget)
            self.last_request = copy.deepcopy(request)
            # 保存实际生效的采样参数，使以后版本仍能准确重建本轮计算。
            self.last_request["payload"]["generation"] = copy.deepcopy(generation)
        self.forward_count = 0
        self.controller = StepController(mode, lambda name, value: self.event(name, value))
        self.neurons.reset()
        from .debugging import validate_points
        self.controller.configure(validate_points(adapter, payload.get("breakpoints", [])))
        self.controller.replay_remaining = int(payload.get("resume_executed", 0))
        self.selected_layer = int(capture.get("layer", 0))
        # 根据真实模板偏移排除历史与角色标记，只统计本轮用户正文。
        current_user = next((m["content"] for m in reversed(messages) if m["role"] == "user"), "")
        user_start = template.rfind(current_user) if current_user else -1
        user_positions = [i for i, (start, stop) in enumerate(encoded.get("offset_mapping", []))
                          if user_start >= 0 and stop > user_start and start < user_start + len(current_user)]
        edited_layers = {rule["layer_index"] for rule in adapter.edit_rules}
        self.degraded_notified = False
        self.store.save_manifest(schema_version="1.0", run_id=self.run_id, model=copy.deepcopy(adapter.info),
            generation=generation, messages=messages, template=template, token_ids=ids,
            rules_snapshot=copy.deepcopy(adapter.edit_rules), weights_revision=adapter.weights_revision,
            vocabulary_revision=__import__("observatory.versions",fromlist=["vocabulary_hash"]).vocabulary_hash(tokenizer),
            environment={"device":str(adapter.device),"dtype":str(next(adapter.model.parameters()).dtype),"torch":torch.__version__},
            offsets=encoded.get("offset_mapping"), user_positions=user_positions, special_token_ids=tokenizer.all_special_ids,
            capture_policy=policy, disk_budget_bytes=budget, backend=adapter.info.get("backend", "qwen2-eager"), status="running",
            managed_by="observatory-project" if payload.get("project_root") else "legacy", pinned=False,
            software={"torch": torch.__version__, "transformers": __import__("transformers").__version__, "application": "1.0.4"})
        self.event("run.started", {"path": str(self.store.root), "branch_id": self.branch_id, "weights_revision": adapter.weights_revision,
                                  "requested_max_new_tokens": requested_maximum, "max_new_tokens": maximum,
                                  "context_limit": context_limit, "input_tokens": len(ids), "mode": mode,
                                  "template": template, "generation": generation,
                                  "tokens": [{"position": p, "id": t, "text": tokenizer.convert_ids_to_tokens(t), "special": t in tokenizer.all_special_ids} for p, t in enumerate(ids)]})
        # 监控配置来自客户端当次连接，不复用恢复点里可能已经失效的端口。
        from .anomaly_sampling import AnomalySampler, baseline_identity
        self.anomaly = AnomalySampler(payload.get("anomaly_monitor", {}), {
            "run_id": self.run_id, "branch_id": self.branch_id,
            "profile": baseline_identity(adapter, generation, adapter.edit_rules, mode, policy),
            "project_root": payload.get("project_root", ""), "layers": adapter.config.num_hidden_layers,
            "input_tokens": len(ids), "max_tokens": maximum, "thinking": thinking_enabled,
            "step_mode": mode == "step"}, self.controller)
        adapter.reset_cache()
        generated, history = [], list(ids)
        from .token_analysis import TokenLayout, sample_evidence
        token_layout = TokenLayout()
        input_ids = torch.tensor([ids], device=adapter.device)
        generator = torch.Generator(device=adapter.device).manual_seed(seed)
        outcome, reason, text = "completed", "length", ""
        reasoning_content, thinking_complete = "", not thinking_enabled
        last_allocator = 0
        try:
            with torch.inference_mode():
                for step in range(maximum):
                    if self.controller.cancelled:
                        raise Cancelled()
                    positions = list(range(len(ids))) if step == 0 else [len(ids) + step - 1]
                    forward_id = f"f{step}"
                    prediction_hidden = None
                    if not self.controller.replay_remaining:
                        self.event("forward.started", {"forward_id": forward_id, "positions": positions, "generation_step": step})

                    def execute(layer, operation, function, inputs):
                        """暂停在真正调用运算之前；完成 GPU 运算和快照后再发布完成。"""
                        nonlocal last_allocator, prediction_hidden
                        context = {"model_id": adapter.info.get("name", "qwen2"),
                            "model_revision": adapter.info.get("model_revision", "test"),
                            "weights_revision": adapter.weights_revision, "generation_step": step,
                            "forward_id": forward_id, "layer_index": layer, "operation_id": operation}
                        replaying = self.controller.replay_remaining > 0
                        self.anomaly.progress(step, layer, operation)
                        self.controller.checkpoint({**context, "layer_index": layer,
                                                   "operation_name": OP_NAMES.get(operation, {"embed": "查词向量", "final_norm": "最终归一化", "logits": "词表输出"}.get(operation, operation))})
                        started_at = time.perf_counter()
                        result = function()
                        if not replaying:
                            self.neurons.capture(layer, operation, result, context, positions)
                            # 监控故障只能停用自身，不能中断或修改原始模型计算。
                            try:
                                self.anomaly.capture(layer, operation, result, context, positions)
                            except Exception:
                                self.anomaly.capture_disabled = True
                        if operation == "logits" and inputs.get("input") is not None:
                            # 只保留预测位置的一行，避免引用整段预填充张量。
                            prediction_hidden = inputs["input"][0, -1].detach().clone()
                        if self.controller.mode != "normal" and self.capture_policy != "none":
                            values = dict(inputs)
                            if isinstance(result, tuple):
                                values["output"] = result[0]
                                values[{"attention": "weights", "rope": "key", "mlp": "baseline", "linear_delta": "recurrent_state"}.get(operation, "auxiliary")] = result[1]
                            else:
                                values["output"] = result
                            # 标准模式保存当前检查层及所有层输出；完整模式才采集全部运算输入输出。
                            watched = any(p.get("layer_index") == layer for p in self.controller.breakpoints)
                            wanted = not payload.get("project_root") or policy == "full" or layer == self.selected_layer or layer in edited_layers or watched or layer == -1 or operation == "residual"
                            captured = values if wanted else {}
                            # 所有层只额外保存有界统计；完整数值仍遵守原采集策略和磁盘预算。
                            if operation == "mlp" and isinstance(result, tuple):
                                from .features import activation_summary
                                captured = dict(captured)
                                user_rows = [i for i, p in enumerate(positions) if p in user_positions]
                                for source, value in (("output", result[0]), ("baseline", result[1])):
                                    captured["summary_" + source] = activation_summary(value, positions)
                                    if user_rows:
                                        captured["user_summary_" + source] = activation_summary(value.reshape(-1, value.shape[-1])[user_rows], [positions[i] for i in user_rows])
                            refs = self.store.operation(context | {"branch_id": self.branch_id, "positions": positions}, captured)
                            if self.store.incomplete and not self.degraded_notified:
                                self.degraded_notified = True
                                self.event("capture.degraded", {"code": "CAPTURE_STORAGE_UNAVAILABLE", "message": self.store.degraded_reason})
                            primary = result[0] if isinstance(result, tuple) else result
                            preview_value = result[1] if isinstance(result, tuple) and operation in ("attention", "linear_delta") else primary
                            if not replaying:
                                self.event("operation.completed", {"tensor_refs": [r for r in refs.values() if r], "origin": "captured", "device_completed": True,
                                       "preview": table(preview_value, 2, 12), "shape": list(primary.shape), "preview_shape": list(preview_value.shape),
                                       "compute_and_capture_ms": (time.perf_counter() - started_at) * 1000,
                                       "operation": operation, "positions": positions, "available": bool(refs.get("output"))}, context | {"layer_index": layer if layer >= 0 else None})
                        if time.monotonic() - last_allocator > .5 and adapter.device.type == "cuda":
                            self.event("allocator.sample", {"allocated": torch.cuda.memory_allocated(adapter.device), "reserved": torch.cuda.memory_reserved(adapter.device),
                                      "peak": torch.cuda.max_memory_allocated(adapter.device)})
                            last_allocator = time.monotonic()
                        if operation == "mlp":
                            self.controller.after(context, result[0] if isinstance(result, tuple) else result)
                        return result

                    logits = adapter.forward(input_ids, positions, execute)
                    self.forward_count += 1
                    replaying_sample = self.controller.replay_remaining > 0
                    self.controller.checkpoint({"operation_id": "sample", "operation_name": "选择下一个 Token", "forward_id": forward_id, "layer_index": -1, "generation_step": step})
                    chosen, scores, probabilities = sample_logits(logits, history, generation, generator, generated)
                    evidence = sample_evidence(logits, scores, probabilities, chosen, prediction_hidden, adapter.model.lm_head, generated)
                    top = probabilities.topk(10)
                    candidates = [{"id": int(t), "text": tokenizer.decode([int(t)]), "probability": finite(p), "raw_logit": finite(logits[0, -1, int(t)])} for p, t in zip(top.values.tolist(), top.indices.tolist())]
                    if self.controller.mode != "normal" and self.capture_policy != "none":
                        context = {"branch_id": self.branch_id, "forward_id": forward_id, "layer_index": -1, "operation_id": "sample", "positions": [positions[-1]],
                                   "generation_step": step, "weights_revision": adapter.weights_revision, "chosen_token_id": chosen,
                                   "candidates": candidates}
                        self.store.operation(context, {"scores": scores, "probabilities": probabilities})
                    self.controller.after({"operation_id": "sample", "operation_name": "目标 Token 概率", "forward_id": forward_id,
                        "layer_index": -1, "generation_step": step}, probabilities)
                    generated.append(chosen)
                    history.append(chosen)
                    decoded, reasoning, complete = decode_response(tokenizer, generated, thinking_enabled, thinking_markers)
                    # 字节级 Token 可能暂时无法组成完整字符，等后续 Token 到达再显示。
                    if not decoded.endswith("\ufffd"):
                        text = decoded
                    if not reasoning.endswith("\ufffd"):
                        reasoning_content = reasoning
                    thinking_complete = complete
                    matched = next((s for s in stop_strings if s in decoded), None)
                    if matched:
                        text = decoded.split(matched, 1)[0]
                    # 重放期间也推进显示偏移，恢复后的首个新 token 不得占用整段历史文本。
                    channel = "control" if chosen in tokenizer.all_special_ids else (
                        "reasoning" if (thinking_enabled or thinking_markers[0] in generated) and not thinking_complete else "answer")
                    piece = tokenizer.decode([chosen], skip_special_tokens=True, clean_up_tokenization_spaces=False)
                    layout_updates = token_layout.update(step, channel, reasoning if channel == "reasoning" else text if matched else decoded, piece)
                    if not replaying_sample:
                        try:
                            self.anomaly.token(chosen, channel, piece, thinking_complete)
                        except Exception:
                            self.anomaly.capture_disabled = True
                            self.anomaly._disable()
                        self.event("token.sampled", {"id": chosen, "position": len(ids) + step, "forward_id": forward_id,
                                               "generation_step": step, "channel": channel, "display_piece": piece,
                                               "display_updates": layout_updates, "capture_layer": self.selected_layer,
                                               "model_revision": adapter.info.get("model_revision"), "weights_revision": adapter.weights_revision,
                                               **evidence,
                                               "display_text": tokenizer.decode([chosen], clean_up_tokenization_spaces=False), "special": chosen in tokenizer.all_special_ids,
                                               "text": tokenizer.convert_ids_to_tokens(chosen), "candidates": candidates})
                        self.event("text.updated", {"text": text, "reasoning_content": reasoning_content,
                            "thinking_enabled": thinking_enabled, "thinking_complete": thinking_complete,
                            "generation_step": step, "token_id": chosen})
                    eos = adapter.model.generation_config.eos_token_id
                    stop_ids = set(eos if isinstance(eos, list) else [eos]) | {tokenizer.eos_token_id}
                    if chosen in stop_ids:
                        reason = "eos"
                        break
                    if matched:
                        reason = "stop"
                        break
                    input_ids = torch.tensor([[chosen]], device=adapter.device)
        except Cancelled:
            outcome, reason = "cancelled", "cancelled"
        except Exception:
            outcome, reason = "failed", "error"
            raise
        finally:
            if self.anomaly:
                try:
                    self.anomaly.finish(outcome, reason, thinking_complete)
                except Exception:
                    self.anomaly._disable()
            if self.controller and self.controller.save_callback:
                callback, self.controller.save_callback = self.controller.save_callback, None
                callback(self.controller.executed)
            self.controller = None
            try:
                self.store.flush()
            except Exception as exc:
                outcome, reason = "failed", "storage_error"
                self.emitter.error(exc, request)
            if self.store.incomplete and not self.degraded_notified:
                self.degraded_notified = True
                self.event("capture.degraded", {"code": "CAPTURE_STORAGE_UNAVAILABLE", "message": self.store.degraded_reason})
            integrity = "not_recorded" if self.capture_policy == "none" else "incomplete" if self.store.incomplete or outcome == "failed" else "complete_for_policy"
            branches = self.store.manifest.get("branches", {})
            branches[self.branch_id] = {"weights_revision": adapter.weights_revision, "outcome": outcome, "text": text,
                "reasoning_content": reasoning_content, "thinking_complete": thinking_complete,
                "forward_count": self.forward_count, "seed": seed}
            self.store.save_manifest(status=outcome, branches=branches, capture_policy=self.capture_policy, capture_integrity=integrity, bytes_written=self.store.bytes_written)
            self.event("run.finished", {"outcome": outcome, "reason": reason, "forward_count": self.forward_count, "text": text,
                       "reasoning_content": reasoning_content, "thinking_enabled": thinking_enabled, "thinking_complete": thinking_complete,
                       "max_new_tokens": maximum, "requested_max_new_tokens": requested_maximum,
                       "capture_integrity": integrity, "last_reliable_seq": self.emitter.seq, "path": str(self.store.root)})
            adapter.reset_cache()

    def apply_patch(self, request):
        """在原运行结束后串行应用修改，再以同输入和 seed 建立新分支。"""
        if not self.adapter or not self.last_request or not self.store or self.store.readonly:
            raise ProtocolError("NO_EDITABLE_RUN", "请先完成一次真实模型运行")
        if self.adapter.info.get("model_revision") != self.store.manifest.get("model", {}).get("model_revision"):
            raise ProtocolError("MODEL_MISMATCH", "当前模型与记录来源不同，请先使用当前模型完成一次运行")
        with self.read_lock:
            if request["name"] == "patch.restore":
                result = self.adapter.restore(request["payload"])
            else:
                result = self.adapter.patch(request["payload"])
        parent = self.branch_id
        self.branch_id = "branch-" + uuid.uuid4().hex[:8]
        patch_log = self.store.manifest.get("patches", [])
        patch_log.append({"branch_id": self.branch_id, "parent_branch_id": parent, "command": request["name"], "target": request["payload"], "result": result})
        self.store.save_manifest(patches=patch_log)
        self.result(request, "patch.applied", result | {"branch_id": self.branch_id, "parent_branch_id": parent, "state": "applied"})
        self.event("branch.created", {"branch_id": self.branch_id, "parent_branch_id": parent, "prefix_policy": "same_prompt_same_seed_fresh_cache"})
        replay = copy.deepcopy(self.last_request)
        replay["payload"]["mode"] = "observe"
        replay["payload"].setdefault("capture", {})["policy"] = "standard"
        replay["payload"]["capture"]["layer"] = self.selected_layer
        self.run(replay, fork=True)

    def edit(self, request):
        """临时规则只影响后续输入；真实模型写回仍保留参数比较分支。"""
        from .editing import validate_rules, prepare_weights, save_current_model, restore_backup
        adapter = self.adapter
        if not adapter:
            raise ProtocolError("MODEL_NOT_LOADED", "请先加载模型")
        payload = request["payload"]
        source_revision = adapter.info.get("model_revision")
        if payload.get("expected_weights_revision") != adapter.weights_revision:
            raise ProtocolError("STALE_WEIGHTS", "模型版本已变化，请刷新后重试")
        with self.read_lock:
            result = {}
            if request["name"] == "edit.clear":
                adapter.edit_rules = []
                adapter.activation_patches.clear()
            elif request["name"] == "model.restore_backup":
                result = restore_backup(adapter, payload["backup_path"])
            else:
                target = payload["target"]
                rules = validate_rules(adapter, payload.get("rules"), target)
                if target == "temporary":
                    adapter.edit_rules = rules
                else:
                    # 原有权重编辑入口也纳入同一版本历史，按真实通道区间记录。
                    versions = self.features.versions()
                    versions.check()
                    patches = []
                    for rule in rules:
                        parameter = adapter.parameter(rule["layer_index"], rule["projection"])
                        axis = 1 if rule["projection"] == "down" else 0
                        start = rule["dimension"] if rule["kind"] == "set" else 0
                        stop = start + 1 if rule["kind"] == "set" else parameter.shape[1-axis]
                        target = {"name": versions.canonical(parameter), "axis": axis, "index": rule["neuron_index"], "start": start, "stop": stop}
                        patches.append(versions.tensor_patch(target, [rule["value"]], rule["kind"],
                            f"第 {rule['layer_index']} 层 · 通道 {rule['neuron_index']}", "rules"))
                    result = versions.replace_rules(rules, self.event)
                    # 已保存的参数成为新基线，旧单系数恢复接口不能撤销磁盘状态。
                    adapter.original_weights.clear()
                    adapter.weight_patches.clear()
                result.update(target=target, rules=rules)
            adapter.revision_number += 1
            adapter.weights_revision = f"w{adapter.revision_number}"
            adapter.reset_cache()
            result.update(weights_revision=adapter.weights_revision, temporary_rules=adapter.edit_rules,
                          model_revision=adapter.info.get("model_revision"))
        self.result(request, "edit.applied", result)
        if request["name"] == "edit.clear" or payload.get("target") == "temporary":
            return
        # 没有运行历史也能配置规则；跨模型和离线历史不自动重算。
        if self.last_request and self.store and not self.store.readonly and self.store.manifest.get("model", {}).get("model_revision") == source_revision:
            parent = self.branch_id
            self.branch_id = "branch-" + uuid.uuid4().hex[:8]
            logs = self.store.manifest.get("patches", [])
            logs.append({"branch_id": self.branch_id, "parent_branch_id": parent, "command": request["name"], "target": payload, "result": result})
            self.store.save_manifest(patches=logs)
            self.event("branch.created", {"branch_id": self.branch_id, "parent_branch_id": parent})
            replay = copy.deepcopy(self.last_request)
            replay["payload"]["mode"] = "observe"
            replay["payload"].setdefault("capture", {})["policy"] = "standard"
            replay["payload"]["capture"]["layer"] = self.selected_layer
            self.run(replay, fork=True)

    def export_model(self, request, emit_result=True):
        """导出到新目录，原模型目录和原始权重始终保留。"""
        if not self.adapter:
            raise ProtocolError("MODEL_NOT_LOADED", "请先加载模型")
        if getattr(self.adapter, "recovery_required", False):
            raise ProtocolError("RECOVERY_REQUIRED", "模型恢复失败，不能导出；请从已知备份恢复后重新加载")
        destination = Path(request["payload"]["destination"]).resolve()
        source = Path(self.adapter.info["path"]).resolve()
        if destination == source or destination.is_relative_to(source) or destination.exists():
            raise ProtocolError("INVALID_DESTINATION", "请选择模型目录以外、尚不存在的新目录")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(destination.parent).free < self.adapter.info["weight_bytes"] + 256 * 1024**2:
            raise ProtocolError("DISK_FULL", "导出磁盘空间不足")
        self.event("model.progress", {"message": "导出修改后的真实模型权重"})
        self.adapter.model.save_pretrained(str(destination), safe_serialization=True, max_shard_size="4GB")
        self.adapter.tokenizer.save_pretrained(str(destination))
        # 完整保留视觉处理器配置，文本工作台不修改这些模型资产。
        for filename in ("preprocessor_config.json", "processor_config.json", "video_preprocessor_config.json"):
            if (source / filename).is_file():
                shutil.copy2(source / filename, destination / filename)
        # 激活干预属于运行策略，不伪装成已经写入模型参数。
        metadata = {"source": str(source), "source_revision": self.adapter.info["model_revision"], "weights_revision": self.adapter.weights_revision,
                    "parameter_patches": [{"target": list(k), "value": v} for k, v in self.adapter.weight_patches.items()],
                    "activation_patches_not_exported": len(self.adapter.activation_patches) + len(self.adapter.edit_rules)}
        (destination / "observatory-patches.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
        if (source / "LICENSE").exists():
            shutil.copy2(source / "LICENSE", destination / "LICENSE")
        result = {"path": str(destination), "weights_revision": self.adapter.weights_revision}
        if emit_result:
            self.result(request, "model.exported", result)
        return result

    def slice(self, payload):
        """验证所有轴和元素预算后提供任意张量切片。"""
        if not self.store:
            raise ProtocolError("NO_TRACE", "尚无计算记录")
        value = self.store.load(payload["tensor_ref"])
        if value is None:
            raise ProtocolError("TENSOR_NOT_CAPTURED", "张量未采集或不可用")
        ranges = payload.get("ranges", [])
        if len(ranges) != value.ndim:
            raise ProtocolError("INVALID_SLICE", "切片轴数不匹配")
        maximum = min(int(payload.get("max_elements", 4096)), 65536)
        if maximum < 1:
            raise ProtocolError("INVALID_SLICE", "切片元素上限无效")
        slices, count = [], 1
        for axis, extent in zip(ranges, value.shape):
            start, stop, step = (int(axis[k]) for k in ("start", "stop", "step"))
            if step < 1 or not 0 <= start <= stop <= extent:
                raise ProtocolError("INVALID_SLICE", "切片范围越界")
            count *= len(range(start, stop, step))
            slices.append(slice(start, stop, step))
        if count > maximum:
            raise ProtocolError("SLICE_TOO_LARGE", "切片超过元素预算")
        result = value[tuple(slices)].float()
        return {"shape": list(result.shape), "values": [finite(x) for x in result.flatten().tolist()], "origin": "captured"}

    def scalar(self, payload):
        """用真实输入和参数解释投影标量，明确区分 FP32 重算与原内核值。"""
        import torch
        if not self.store or not self.adapter:
            raise ProtocolError("SOURCE_UNAVAILABLE", "完整内积需要加载原始模型参数")
        layer, operation = int(payload["layer_index"]), payload["operation"]
        context, refs = self.store.get_operation(payload["branch_id"], payload["forward_id"], layer, operation)
        if not context or context.get("weights_revision") != self.adapter.weights_revision or self.store.manifest.get("model", {}).get("model_revision") != self.adapter.info.get("model_revision"):
            raise ProtocolError("SOURCE_UNAVAILABLE", "该历史版本的完整参数未保存，仍可读取捕获标量")
        inputs, output = self.store.load(refs.get("input")), self.store.load(refs.get("output"))
        if inputs is None or output is None:
            raise ProtocolError("TENSOR_NOT_CAPTURED", "该运算没有可用的投影输入与输出")
        row, column = int(payload["row"]), int(payload["column"])
        inputs = inputs.reshape(-1, inputs.shape[-1])
        output = output.reshape(-1, output.shape[-1])
        if not 0 <= row < inputs.shape[0] or not 0 <= column < output.shape[1]:
            raise ProtocolError("INVALID_SELECTION", "标量位置越界")
        module = self.adapter.backbone.layers[layer]
        if operation in ("q", "k", "v", "o"):
            projection = getattr(module.self_attn, operation + "_proj")
        elif operation in ("gate", "up", "down"):
            projection = getattr(module.mlp, operation + "_proj")
        elif operation in ("linear_qkv", "linear_z", "linear_b", "linear_a", "linear_out"):
            projection = getattr(module.linear_attn, "out_proj" if operation == "linear_out" else "in_proj_" + operation.removeprefix("linear_"))
        else:
            raise ProtocolError("NO_SCALAR_PROJECTION", "该运算不是线性投影，可直接读取其捕获值")
        weight = projection.weight[column].detach().cpu().float()
        x = inputs[row].float()
        bias = float(projection.bias[column]) if projection.bias is not None else 0.
        products = x * weight
        recomputed = float(products.sum()) + bias
        captured = float(output[row, column])
        return {"origin": "derived", "captured": finite(captured), "recomputed_fp32": finite(recomputed),
                "absolute_error": finite(abs(recomputed - captured)), "terms": x.numel(), "bias": bias,
                "first_terms": [{"input": finite(a), "weight": finite(b), "product": finite(c)} for a, b, c in zip(x[:8], weight[:8], products[:8])]}

    def query(self, payload):
        """把同一历史事件转换为检查器、注意力和神经元视图。"""
        import torch
        if not self.store:
            return {"selection": payload, "status": "not_captured"}
        store = self.store
        branch, forward_id = payload.get("branch_id", "base"), payload.get("forward_id", "f0")
        layer, position, neuron = (int(payload.get(k, default)) for k, default in (("layer_index", 0), ("token_position", 0), ("neuron_index", 0)))
        operation, head = payload.get("operation", "norm"), int(payload.get("head", 0))
        config = store.manifest.get("model", {}).get("config", {})
        if not config:
            # 新记录切换到清单初始化之间，界面可能发来上一帧查询；此时尚未采集。
            return {"selection": payload, "status": "not_captured"}
        model_matches = self.adapter is not None and self.adapter.info.get("model_revision") == store.manifest.get("model", {}).get("model_revision")
        if not (0 <= layer < config.get("num_hidden_layers", 0) and 0 <= neuron < config.get("intermediate_size", 0)):
            raise ProtocolError("INVALID_SELECTION", "所选层或神经元越界")
        operation_layer = -1 if operation in ("embed", "final_norm", "logits") else layer
        context, refs = store.get_operation(branch, forward_id, operation_layer, operation)
        result = {"selection": payload, "status": "available" if context else "not_captured", "context": context, "refs": refs, "origin": "captured"}
        with store.lock:
            result["tensors"] = {name: dict(row) | {"shape": json.loads(row["shape"])} for name, ref in refs.items()
                if ref and (row := store.db.execute("SELECT ref,shape,dtype,nbytes FROM tensors WHERE ref=?", (ref,)).fetchone())}
        if context:
            output = store.load(refs.get("output"))
            input_value = store.load(refs.get("input"))
            start = 0
            positions = context.get("positions", [])
            if output is not None and output.ndim == 3 and output.shape[1] == len(positions) and position in positions:
                start = max(0, positions.index(position) - 2)
                matrix_value = output[:, start:start + 6]
                input_value = input_value[:, start:start + 6] if input_value is not None and input_value.ndim == 3 else input_value
            else:
                matrix_value = output
            result.update(matrix=table(matrix_value), input_matrix=table(input_value), matrix_row_start=start,
                          row_positions=positions[start:start + 6], shape=list(output.shape) if output is not None else None)
        context_mlp, refs_mlp = store.get_operation(branch, forward_id, layer, "mlp")
        if context_mlp and position in context_mlp.get("positions", []):
            row = context_mlp["positions"].index(position)
            middle = store.load(refs_mlp.get("output"))
            base = store.load(refs_mlp.get("baseline"))
            if middle is not None:
                values = middle[0, row].float()
                order = torch.argsort(values.abs(), descending=True, stable=True)[:8].tolist()
                start = neuron // 32 * 32
                details = {"origin": "captured", "start": start, "grid": [finite(x) for x in values[start:start + 32].tolist()],
                           "rank": [{"index": j, "value": finite(values[j])} for j in order],
                           "value": finite(values[neuron]), "baseline": finite(base[0, row, neuron]) if base is not None else None,
                           "maximum": finite(values.abs().max()), "positive": int((values > 0).sum()), "negative": int((values < 0).sum()),
                           "zero": int((values == 0).sum()), "nan": int(torch.isnan(values).sum()), "inf": int(torch.isinf(values).sum())}
                for key, op in (("gate", "gate"), ("up", "up"), ("silu", "silu")):
                    _, related = store.get_operation(branch, forward_id, layer, op)
                    tensor = store.load(related.get("output"))
                    details[key] = finite(tensor[0, row, neuron]) if tensor is not None else None
                _, down_refs = store.get_operation(branch, forward_id, layer, "down")
                weight_row = store.load(down_refs.get("weight_row"))
                details["contribution"] = finite(weight_row[neuron].float() * values[neuron]) if weight_row is not None else None
                result["neuron"] = details
        context_a, refs_a = store.get_operation(branch, forward_id, layer, "attention")
        if config.get("layer_types", [])[layer:layer + 1] == ["linear_attention"]:
            context_l, refs_l = store.get_operation(branch, forward_id, layer, "linear_delta")
            state = store.load(refs_l.get("recurrent_state"))
            if state is not None and 0 <= head < state.shape[1]:
                result["attention"] = {"type": "linear", "matrix": table(state[0, head], 8, 12),
                    "shape": list(state.shape), "value_head": head,
                    "key_head": head // (config["linear_num_value_heads"] // config["linear_num_key_heads"]),
                    "query_positions": list(range(min(8, state.shape[-2])))}
        if context_a:
            weights = store.load(refs_a.get("weights"))
            if weights is not None and 0 <= head < weights.shape[1]:
                result["attention"] = {"matrix": table(weights[0, head], 8, 12), "shape": list(weights.shape),
                                       "query_positions": context_a.get("positions", [])[:8], "key_positions": list(range(min(12, weights.shape[-1]))),
                                       "kv_head": head // (config["num_attention_heads"] // config["num_key_value_heads"])}
        if model_matches:
            dimension, kind = int(payload.get("dimension", 0)), payload.get("weight_kind", "down")
            if kind not in ("gate", "up", "down"):
                kind = "down"
            result["weight"] = {"value": self.adapter.weight_value(layer, neuron, kind, dimension),
                                "original": self.adapter.weight_value(layer, neuron, kind, dimension, original=True),
                                "weights_revision": self.adapter.weights_revision}
        sample_context, sample_refs = store.get_operation(branch, forward_id, -1, "sample")
        if sample_context:
            result["chosen_token_id"] = sample_context.get("chosen_token_id")
        probabilities = store.load(sample_refs.get("probabilities"))
        _, logit_refs = store.get_operation(branch, forward_id, -1, "logits")
        logits = store.load(logit_refs.get("output"))
        if probabilities is not None:
            top = probabilities.topk(10)
            tokenizer = self.adapter.tokenizer if model_matches else None
            result["probabilities"] = [{"id": int(t), "text": tokenizer.decode([int(t)]) if tokenizer else "Token " + str(int(t)), "probability": finite(p),
                                        "raw_logit": finite(logits[0, -1, int(t)]) if logits is not None else None} for p, t in zip(top.values, top.indices)]
            if not tokenizer and sample_context and sample_context.get("candidates"):
                result["probabilities"] = sample_context["candidates"]
        if payload.get("lens") and model_matches and context and context.get("weights_revision") == self.adapter.weights_revision:
            # 观察性投影独立计数，不修改主 forward_count 或 KV cache。
            _, residual_refs = store.get_operation(branch, forward_id, layer, "residual")
            hidden = store.load(residual_refs.get("output"))
            if hidden is not None:
                with torch.inference_mode():
                    positions = context.get("positions", [])
                    row = positions.index(position) if position in positions else len(positions) - 1
                    h = hidden[:, row:row + 1, :].to(self.adapter.device)
                    distribution = self.adapter.model.lm_head(self.adapter.backbone.norm(h))[0, -1].float().softmax(-1)
                    top = distribution.topk(10)
                    result["lens"] = {"origin": "derived", "additional_projection_count": 1,
                                      "candidates": [{"id": int(t), "text": self.adapter.tokenizer.decode([int(t)]), "probability": finite(p)} for p, t in zip(top.values.tolist(), top.indices.tolist())]}
        if payload.get("overview"):
            # 按真实层输出显示同一绝对token的隐藏通道；未完成层保留空值。
            start = min(neuron, max(0, config["hidden_size"] - 32))
            overview = []
            for index in range(config["num_hidden_layers"]):
                layer_context, layer_refs = store.get_operation(branch, forward_id, index, "residual")
                hidden = store.load(layer_refs.get("output"))
                if layer_context and hidden is not None and position in layer_context.get("positions", []):
                    row = layer_context["positions"].index(position)
                    overview.append([finite(x) for x in hidden[0, row, start:start + 32].float().tolist()])
                else:
                    overview.append([None] * min(32, config["hidden_size"]))
            result["layer_overview"] = overview
            result["overview_start"] = start
        return result


def main():
    """启动命令读取循环；第三方库日志留在标准错误。"""
    # 原生数值库崩溃也记录 Python 调用位置，客户端仍通过进程退出信号弹窗。
    import faulthandler
    faulthandler.enable(file=sys.stderr, all_threads=True)
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TOKENIZERS_PARALLELISM"] = "false"
    # Windows 数值库的 DLL 初始化必须在进程主线程完成，避免首次在线程内加载时挂起。
    import numpy
    import torch
    from transformers import AutoTokenizer, Qwen2ForCausalLM, Qwen3_5ForConditionalGeneration
    if os.environ.get("MODEL_OBSERVATORY_DIAGNOSTIC"):
        import faulthandler
        faulthandler.dump_traceback_later(5, file=sys.stderr)
    runtime = Runtime()
    runtime.emitter.send("runtime.hello", {"version": "1.0.4", "pid": os.getpid()})
    while not runtime.shutdown.is_set():
        line = sys.stdin.buffer.readline(MAX_LINE + 1)
        if not line:
            runtime.handle({"name": "shutdown", "payload": {}, "request_id": "eof"})
            break
        request = None
        try:
            request = parse_command(line.decode("utf-8"))
            runtime.handle(request)
        except Exception as exc:
            runtime.emitter.error(exc, request)
    runtime.thread.join(timeout=10)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
