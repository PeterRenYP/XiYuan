"""断点与监视的能力校验，不允许保存永远无法触发的采集点。"""
import math

from .model import operation_catalog
from .protocol import ProtocolError


def validate_points(adapter, points):
    """所有条件先完成结构、坐标和模型适配校验，再整体替换。"""
    if adapter is None:
        raise ProtocolError("MODEL_NOT_LOADED", "请先加载模型")
    if not isinstance(points, list) or len(points) > 64:
        raise ProtocolError("INVALID_BREAKPOINT", "最多设置 64 个断点或监视")
    result = []
    for raw in points:
        point = dict(raw)
        layer, operation = point.get("layer_index"), point.get("operation_id")
        step = point.get("generation_step")
        if layer is not None and (type(layer) is not int or not -1 <= layer < adapter.config.num_hidden_layers):
            raise ProtocolError("INVALID_BREAKPOINT", "断点层索引越界")
        if step is not None and (type(step) is not int or step < 0):
            raise ProtocolError("INVALID_BREAKPOINT", "生成步不能为负数")
        if operation is not None:
            allowed = {o[0] for i in range(adapter.config.num_hidden_layers) if layer is None or layer == i for o in operation_catalog(adapter.info["config"], i)}
            allowed |= {"sample", "embed", "final_norm", "logits"} if layer in (None, -1) else set()
            if operation not in allowed:
                raise ProtocolError("UNSUPPORTED_BREAKPOINT", "当前层不支持该运算")
        boundary = point.get("boundary", "before")
        if boundary not in ("before", "after"):
            raise ProtocolError("INVALID_BREAKPOINT", "暂停边界无效")
        if boundary == "after":
            watch = point.get("watch")
            if watch not in ("channel", "token") or operation != ("mlp" if watch == "channel" else "sample"):
                raise ProtocolError("UNSUPPORTED_WATCH", "通道监视仅支持门控乘积之后；Token 监视仅支持采样概率")
            if (watch == "channel" and (layer is None or layer < 0)) or (watch == "token" and layer != -1):
                raise ProtocolError("INVALID_BREAKPOINT", "请指定通道所在层；Token 概率使用输出层")
            limit = adapter.config.intermediate_size if watch == "channel" else adapter.model.get_output_embeddings().weight.shape[0]
            index = point.get("index")
            threshold = point.get("threshold")
            if type(index) is not int or not 0 <= index < limit or not isinstance(threshold, (float,int)) or not math.isfinite(threshold):
                raise ProtocolError("INVALID_WATCH", "监视编号越界或阈值无效")
            if point.get("comparison") not in ("gt", "ge", "lt", "le"):
                raise ProtocolError("INVALID_WATCH", "比较条件无效")
        elif layer is None and step is None and operation is None:
            raise ProtocolError("INVALID_BREAKPOINT", "请至少指定层、算子或生成步")
        result.append(point)
    return result
