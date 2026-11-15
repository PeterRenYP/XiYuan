"""固定前缀、自由生成和条件断点的真实验收。"""
import threading
import time

import pytest
import torch

from test_features import runtime, call
from observatory.control import StepController
from observatory.protocol import command, ProtocolError
from observatory.debugging import validate_points


def test_fixed_prefix_has_real_channel_and_probability_changes(runtime, tmp_path):
    """同前缀下通道置零被数值验证，无干预对照落在明确容差内。"""
    root = tmp_path / "project"
    root.mkdir()
    runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}],
        "generation": {"temperature": 0, "max_new_tokens": 2}, "trace_root": str(root / "records")}))
    baseline = call(runtime, "comparison", mode="fixed", layer=0, neuron=1, operation="mlp", steps=2, project_root=str(root))
    assert all(r["equal_within_tolerance"] for r in baseline["rows"])
    runtime.adapter.edit_rules = [{"layer_index": 0, "neuron_index": 1, "kind": "zero", "value": 0, "scope": "all"}]
    changed = call(runtime, "comparison", mode="fixed", layer=0, neuron=1, operation="mlp", steps=2, target="2", project_root=str(root))
    first = changed["rows"][0]
    assert first["aligned"] and first["maximum_absolute"] > 0
    assert all(r["after"] == 0 for r in first["channel"])
    assert any(r["before"] != 0 for r in first["channel"])
    assert 0 <= first["edited"]["probability"] <= 1 and first["edited"]["rank"] >= 1
    assert all(r["base_prefix"] == r["edited_prefix"] for r in changed["rows"])
    assert runtime.adapter.edit_rules[0]["kind"] == "zero"


def test_monitor_pause_after_value_continue_and_cancel():
    """条件命中停在计算后；继续不会重复命中，取消唤醒等待线程。"""
    event = threading.Event()
    messages = []
    controller = StepController("observe", lambda name, payload: (messages.append(payload), event.set()))
    context = {"layer_index": 0, "operation_id": "mlp", "generation_step": 0}
    controller.configure([dict(context, boundary="after", watch="channel", index=1, comparison="gt", threshold=2)])
    worker = threading.Thread(target=lambda: controller.after(context, torch.tensor([[[0., 3.]]])))
    worker.start()
    try:
        assert event.wait(3)
        assert messages[0]["boundary"] == "after" and messages[0]["observed_value"] == 3
        controller.set_mode("observe")
        worker.join(3)
        assert not worker.is_alive() and not controller.breakpoints
    finally:
        controller.cancel()
        worker.join(3)


def test_breakpoint_validation_and_not_hit(runtime):
    """不支持的采集点拒绝保存；未命中时计算可以结束。"""
    with pytest.raises(ProtocolError):
        validate_points(runtime.adapter, [dict(layer_index=0, operation_id="attention", boundary="after", watch="channel", index=1, threshold=1, comparison="gt")])
    points = validate_points(runtime.adapter, [dict(layer_index=1, operation_id="mlp", generation_step=4)])
    controller = StepController("observe")
    controller.configure(points)
    controller.checkpoint(dict(layer_index=0, operation_id="mlp", generation_step=0))
    assert not controller.waiting and controller.executed == 1
