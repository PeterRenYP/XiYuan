"""神经元页面的有界实时采集、精确查询与跨层投影耦合。"""
from __future__ import annotations

from collections import deque
import math
import threading
import time

from .protocol import ProtocolError


def scalar(value):
    """非有限数值按缺失显示，不能把异常激活伪装为零。"""
    number = float(value)
    return number if math.isfinite(number) else None


def sample_indices(count, samples):
    """均匀采样实际通道编号，始终保留首尾编号。"""
    samples = min(count, samples)
    return [i * (count - 1) // max(1, samples - 1) for i in range(samples)]


class Neurons:
    """只在页面订阅期间采集，缓存每层最近一行 CPU 数值而非整段张量。"""

    def __init__(self, runtime):
        self.runtime = runtime
        self.lock = threading.RLock()
        self.enabled = False
        self.layer = self.neuron = 0
        self.subscription = 0
        self.frames, self.vectors, self.stages, self.last_capture = {}, {}, {}, {}
        self.history = deque(maxlen=128)
        self.path_cache = {}
        self.network_cache = {}

    def reset(self, disable=False):
        """模型、运行或会话变化后丢弃旧身份的数据，避免跨上下文混色。"""
        with self.lock:
            self.frames.clear()
            self.vectors.clear()
            self.stages.clear()
            self.last_capture.clear()
            self.history.clear()
            self.path_cache.clear()
            if disable:
                self.enabled = False
                self.network_cache.clear()
            self.subscription += 1

    def model(self, payload=None):
        """历史视图以记录清单为准，在线视图使用实际加载的模型。"""
        runtime = self.runtime
        recorded = runtime.store.manifest.get("model", {}) if runtime.store else {}
        expected = (payload or {}).get("model_revision")
        if expected and recorded.get("model_revision") == expected and ((payload or {}).get("historical") or runtime.adapter is None or runtime.adapter.info.get("model_revision") != expected):
            return recorded
        if runtime.adapter is not None:
            if expected and runtime.adapter.info.get("model_revision") != expected:
                raise ProtocolError("MODEL_CHANGED", "模型已变化，请重新打开神经元页面")
            return runtime.adapter.info
        if expected and recorded.get("model_revision") != expected:
            raise ProtocolError("MODEL_CHANGED", "模型已变化，请重新打开神经元页面")
        return recorded

    def metadata(self, model=None):
        """从实际权重形状读取各层数量，离线时使用来源记录配置。"""
        model = model or self.model()
        adapter = self.runtime.adapter
        if adapter and adapter.info.get("model_revision") != model.get("model_revision"):
            adapter = None
        config = model.get("config", {})
        layers = int(config.get("num_hidden_layers", 0))
        samples = max(8, min(64, 2560 // max(1, layers)))
        result = []
        for layer in range(layers):
            count = int(adapter.parameter(layer, "up").shape[0]) if adapter else int(config.get("intermediate_size", 0))
            result.append({"layer_index": layer, "count": count, "indices": sample_indices(count, samples)})
        return result

    def validate(self, payload):
        """拒绝布尔、小数、负数及超出实际模型维度的编号。"""
        layer, neuron = payload.get("layer_index", 0), payload.get("neuron_index", 0)
        if type(layer) is not int or type(neuron) is not int:
            raise ProtocolError("INVALID_TARGET", "层号与神经元编号必须是整数")
        model = self.model(payload)
        config = model.get("config", {})
        if not 0 <= layer < int(config.get("num_hidden_layers", 0)):
            raise ProtocolError("INVALID_TARGET", "层号超出当前模型范围")
        adapter = self.runtime.adapter
        count = int(adapter.parameter(layer, "up").shape[0]) if adapter and adapter.info.get("model_revision") == model.get("model_revision") else int(config.get("intermediate_size", 0))
        if not 0 <= neuron < count:
            raise ProtocolError("INVALID_TARGET", "神经元编号超出当前层范围")
        return layer, neuron

    def configure(self, payload):
        """轻量订阅命令可以在生成与逐步暂停期间即时生效。"""
        enabled = bool(payload.get("enabled", False))
        layer, neuron = self.validate(payload) if enabled else (self.layer, self.neuron)
        with self.lock:
            if (layer, neuron) != (self.layer, self.neuron):
                self.history.clear()
                self.stages.clear()
            if not enabled:
                # 关闭页面或暂停时释放门控标量所引用的 GPU 存储。
                self.stages.clear()
            self.enabled, self.layer, self.neuron = enabled, layer, neuron
            self.subscription += 1
            return {"enabled": enabled, "subscription": self.subscription}

    def capture(self, layer, operation, result, context, positions):
        """在已有前向完成点采样，不新增前向，也不改变返回张量。"""
        if not self.enabled or layer < 0 or operation not in ("gate", "up", "silu", "mlp"):
            return
        import torch
        with self.lock:
            subscription, selected_layer, neuron = self.subscription, self.layer, self.neuron
            identity = (self.runtime.run_id, self.runtime.branch_id, context["forward_id"], layer,
                        context["weights_revision"], selected_layer, neuron)
            if operation != "mlp":
                if layer == selected_layer:
                    value = result[0] if isinstance(result, tuple) else result
                    self.stages.setdefault(identity, {})[operation] = value.reshape(-1, value.shape[-1])[-1, neuron].detach()
                return
            now = time.monotonic()
            if now - self.last_capture.get(layer, -math.inf) < .10:
                self.stages.pop(identity, None)
                return
            self.last_capture[layer] = now
            stage = self.stages.pop(identity, {})
        value = result[0] if isinstance(result, tuple) else result
        # 每层仅传输最后一个输入位置的一行；前填充不会复制整段上下文。
        row = value.reshape(-1, value.shape[-1])[-1].detach().float()
        stage_names = [key for key in ("gate", "silu", "up") if key in stage]
        combined = torch.cat([row] + [stage[key].float().reshape(1) for key in stage_names]).cpu()
        vector = combined[:row.numel()].clone()
        config = self.runtime.adapter.info["config"]
        samples = max(8, min(64, 2560 // max(1, int(config["num_hidden_layers"]))))
        indices = sample_indices(vector.numel(), samples)
        if layer == selected_layer and neuron not in indices:
            indices.append(neuron)
        finite_values = vector[torch.isfinite(vector)]
        frame = {"layer_index": layer, "indices": indices, "values": [scalar(vector[i]) for i in indices],
                 "maximum": scalar(finite_values.abs().max()) if finite_values.numel() else None,
                 "forward_id": context["forward_id"], "token_position": positions[-1],
                 "generation_step": context["generation_step"], "weights_revision": context["weights_revision"],
                 "model_revision": self.runtime.adapter.info.get("model_revision"),
                 "subscription": subscription, "origin": "captured"}
        # 全局传播按实际层独立提供，不依赖当前检查的是哪个神经元。
        frame["network_paths"] = self.network_paths(layer, self.metadata())
        details = {key: scalar(combined[row.numel()+i]) for i, key in enumerate(stage_names)}
        if layer == selected_layer:
            details.update(neuron_index=neuron, value=scalar(vector[neuron]))
            if details["value"] is not None:
                details["rank"] = int((vector.abs() > abs(details["value"])).sum())+1
            frame["selected"] = details
        with self.lock:
            if subscription != self.subscription or not self.enabled:
                return
            self.frames[layer], self.vectors[layer] = frame, vector
            if layer == selected_layer:
                self.history.append({"position": positions[-1], "value": scalar(vector[neuron]),
                                     "forward_id": context["forward_id"], "neuron_index": neuron})
        self.runtime.event("neurons.frame", frame, context)

    def paths(self, layer, neuron, metadata):
        """只计算到下一层可见候选的真实线性耦合，不宣称为完整因果连接。"""
        adapter = self.runtime.adapter
        if adapter is None or layer+1 >= len(metadata):
            return []
        import torch
        key = (adapter.info.get("model_revision"), adapter.weights_revision, layer, neuron)
        with self.lock:
            if key in self.path_cache:
                return self.path_cache[key]
        indices = metadata[layer+1]["indices"]
        with torch.inference_mode():
            down = adapter.parameter(layer, "down")[:, neuron].detach().float()
            up = adapter.parameter(layer+1, "up")[indices].detach().float()
            coefficients = torch.mv(up, down).cpu()
        result = []
        for offset in torch.argsort(coefficients.abs(), descending=True, stable=True).tolist():
            coefficient = scalar(coefficients[offset])
            if coefficient is None or coefficient == 0:
                continue
            result.append({"source_layer": layer, "source_neuron": neuron, "target_layer": layer+1,
                           "target_neuron": indices[offset], "coefficient": coefficient})
            if len(result) == 4:
                break
        with self.lock:
            if len(self.path_cache) >= 16:
                self.path_cache.pop(next(iter(self.path_cache)))
            self.path_cache[key] = result
        return result

    def network_paths(self, layer, metadata):
        """批量计算每个显示节点到下一层的最强真实投影耦合，按权重版本缓存。"""
        adapter = self.runtime.adapter
        if adapter is None or layer+1 >= len(metadata):
            return []
        import torch
        identity = (adapter.info.get("model_revision"), adapter.weights_revision)
        with self.lock:
            cached = self.network_cache.get(identity, {})
            if layer in cached:
                return cached[layer]
        sources, targets = metadata[layer]["indices"], metadata[layer+1]["indices"]
        if not sources or not targets:
            return []
        # 只取可视通道的行列，不复制完整模型矩阵或构造全量稠密连接。
        with torch.inference_mode():
            down = adapter.parameter(layer, "down")[:, sources].detach().float()
            up = adapter.parameter(layer+1, "up")[targets].detach().float()
            coefficients = (up @ down).cpu()
            finite = torch.isfinite(coefficients)
            best = coefficients.abs().masked_fill(~finite, -1).argmax(dim=0).tolist()
        result = []
        for offset, target in enumerate(best):
            coefficient = scalar(coefficients[target, offset])
            if coefficient is not None and coefficient != 0:
                result.append([sources[offset], targets[target], coefficient])
        with self.lock:
            # 仅保留当前模型版本；新一轮生成复用相同拓扑，不重复读权重。
            if identity not in self.network_cache:
                self.network_cache = {identity: {}}
            self.network_cache[identity][layer] = result
        return result

    def query(self, payload):
        """返回同一来源的精确数值；缺失、历史和实时数据不会相互拼接。"""
        import torch
        layer, neuron = self.validate(payload)
        runtime, model = self.runtime, self.model(payload)
        metadata = self.metadata(model)
        live = bool(payload.get("live", True))
        branch = payload.get("branch_id", runtime.branch_id)
        matching_run = "run_id" not in payload or (payload.get("run_id") or "") == (runtime.run_id or "")
        with self.lock:
            compatible = live and matching_run and branch == runtime.branch_id
            frame = dict(self.frames.get(layer, {})) if compatible else {}
            if frame and (frame.get("model_revision") != model.get("model_revision") or runtime.adapter and frame.get("weights_revision") != runtime.adapter.weights_revision):
                frame = {}
            vector = self.vectors.get(layer) if frame else None
            frames = [dict(item) for item in self.frames.values() if item.get("model_revision") == model.get("model_revision") and (runtime.adapter is None or item.get("weights_revision") == runtime.adapter.weights_revision)] if compatible else []
            history = list(self.history) if frame and layer == self.layer and neuron == self.neuron else []
        selected = {"layer_index": layer, "neuron_index": neuron, "status": "not_captured"}
        detail = {}
        if vector is not None:
            detail = frame.get("selected", {})
            if detail.get("neuron_index") != neuron:
                detail = {}
        elif runtime.store and matching_run:
            store = runtime.store
            recorded = store.manifest.get("model", {})
            # 当前加载的其他模型不能为历史记录提供激活或权重解释。
            if recorded.get("model_revision") == model.get("model_revision"):
                forward = payload.get("forward_id", "f0")
                context, refs = store.get_operation(branch, forward, layer, "mlp")
                positions = context.get("positions", []) if context else []
                position = int(payload.get("token_position", positions[-1] if positions else 0))
                value = store.load(refs.get("output")) if refs else None
                if value is not None and position in positions:
                    row_index = positions.index(position)
                    vector = value.reshape(-1, value.shape[-1])[row_index].float().cpu()
                    frame = {"layer_index": layer, "forward_id": forward, "token_position": position,
                             "weights_revision": context.get("weights_revision"), "model_revision": recorded.get("model_revision"),
                             "indices": metadata[layer]["indices"], "origin": "captured"}
                    frame["values"] = [scalar(vector[i]) for i in frame["indices"]]
                    finite_values = vector[torch.isfinite(vector)]
                    frame["maximum"] = scalar(finite_values.abs().max()) if finite_values.numel() else None
                    frames = [frame]
                    for name in ("gate", "silu", "up"):
                        _, stage_refs = store.get_operation(branch, forward, layer, name)
                        stage = store.load(stage_refs.get("output")) if stage_refs else None
                        if stage is not None:
                            detail[name] = scalar(stage.reshape(-1, stage.shape[-1])[row_index, neuron])
        if vector is not None:
            number = scalar(vector[neuron])
            selected.update({key: frame.get(key) for key in ("forward_id", "token_position", "weights_revision")})
            selected.update(status="available", value=number, gate=detail.get("gate"), silu=detail.get("silu"), up=detail.get("up"))
            if number is not None:
                selected["rank"] = int((vector.abs() > abs(number)).sum())+1
            if neuron not in frame["indices"]:
                frame["indices"] = frame["indices"]+[neuron]
                frame["values"] = frame["values"]+[number]
            frames = [frame if item["layer_index"] == layer else item for item in frames]
        # 离线记录即使没有激活块，也不能套用另一权重版本的连接系数。
        expected_weights = payload.get("weights_revision")
        if expected_weights is None and payload.get("historical") and runtime.store:
            expected_weights = runtime.store.manifest.get("weights_revision")
        paths_allowed = runtime.adapter is not None and runtime.adapter.info.get("model_revision") == model.get("model_revision") and (expected_weights is None or expected_weights == runtime.adapter.weights_revision) and (not frame or frame.get("weights_revision") == runtime.adapter.weights_revision)
        paths = self.paths(layer, neuron, metadata) if paths_allowed else []
        network = [{"layer_index": item["layer_index"], "paths": self.network_paths(item["layer_index"], metadata)}
                   for item in metadata[:-1]] if paths_allowed else []
        weights = frame.get("weights_revision") if frame else expected_weights
        if weights is None:
            weights = runtime.adapter.weights_revision if runtime.adapter else (runtime.store.manifest.get("weights_revision") if runtime.store else None)
        return {"model_revision": model.get("model_revision"), "weights_revision": weights,
                "layers": metadata, "frames": frames, "selected": selected, "history": history, "paths": paths,
                "network_paths": network,
                "path_kind": "linear_projection", "path_note": "相邻层下投影与上投影的线性耦合；未包含归一化、注意力和门控的间接作用",
                "run_id": runtime.run_id, "branch_id": branch}
