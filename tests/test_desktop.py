"""原生界面的状态与模型维度映射验证。"""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtWidgets import QApplication

from observatory.desktop import Observatory


@pytest.fixture(scope="module")
def application():
    """复用 Qt 应用，避免测试反复创建全局实例。"""
    return QApplication.instance() or QApplication([])


def test_dimensions_modes_and_fixed_selection(application):
    """通道和头数来自 Qwen2.5 配置，切换模式不清空对话。"""
    window = Observatory(start_processes=False)
    window.configure_dimensions({"config": {"num_hidden_layers": 36, "hidden_size": 2048,
        "intermediate_size": 11008, "num_attention_heads": 16, "num_key_value_heads": 2}})
    assert window.layer_list.count() == 36
    assert window.head_select.count() == 16
    assert window.neuron_id.maximum() == 11007
    assert window.edit_dimension.maximum() == 2047
    window.messages = [{"role": "user", "content": "<b>中文测试</b>"}]
    window.render_chat()
    window.set_mode("normal")
    window.set_mode("step")
    assert window.chat.toPlainText() == "你：<b>中文测试</b>"
    assert len(window.messages) == 1
    window.showMaximized()
    application.processEvents()
    window.toggle_fullscreen()
    assert window.isFullScreen()
    window.exit_fullscreen()
    assert window.isMaximized()
    window.set_mode("observe")
    window.execution = {"layer_index": 10, "operation": "q", "forward_id": "f0", "branch_id": "base", "token_position": 5}
    window.follow_changed(True)
    window.choose_layer(13)
    assert not window.follow.isChecked()
    assert window.selection.layer_index == 13 and window.execution["layer_index"] == 10
    window.close()


def test_empty_snapshot_clears_values_and_token_stage(application):
    """未执行的数据清空旧值，真实Token编号与绝对位置分别显示。"""
    window = Observatory(start_processes=False)
    window.display_snapshot({"status": "available", "matrix": [[1., 2.]], "shape": [1, 2]})
    assert window.matrix.item(0, 0).text() == "+1.0000"
    window.display_snapshot({"status": "not_captured"})
    assert window.matrix.item(0, 0) is None
    window.tokens = [{"position": 5, "id": 151645, "text": "<|im_end|>"}]
    window.selection.token_position = 5
    window.stage_select.setCurrentIndex(window.stage_select.findData("tokens"))
    assert "151645" in window.stage_table.item(0, 1).text()
    assert "T5" in window.stage_table.item(0, 1).text()
    window.close()


def test_hybrid_directory_selection_and_dimensions(application):
    """选目录不改变已加载身份，混合层的操作及通道随模型切换。"""
    window = Observatory(start_processes=False)
    window.loaded = True
    window.select_model_directory("D:/models/Qwen3.5-4B")
    assert window.loaded and window.model_name.text() == "尚未加载模型"
    window.configure_dimensions({"name": "Qwen3.5-4B", "config": {
        "model_type": "qwen3_5_text", "num_hidden_layers": 32, "hidden_size": 2560,
        "intermediate_size": 9216, "num_attention_heads": 16, "num_key_value_heads": 4,
        "linear_num_key_heads": 16, "linear_num_value_heads": 32,
        "layer_types": ["linear_attention"] * 3 + ["full_attention"] + ["linear_attention"] * 28}})
    assert window.head_select.count() == 32
    assert window.neuron_id.maximum() == 9215 and window.edit_dimension.maximum() == 2559
    assert window.operation_select.findData("linear_delta") >= 0
    assert window.operation_select.findData("attention") < 0
    window.choose_layer(3)
    assert window.head_select.count() == 16
    assert window.operation_select.findData("attention_gate") >= 0
    assert window.operation_select.findData("linear_delta") < 0
    window.active = True
    window.select_model_directory("D:/models/models/Qwen__Qwen2.5-3B-Instruct")
    window.set_active(True)
    assert not window.load_button.isEnabled()
    assert window.model_name.text() == "Qwen3.5-4B"
    window.close()
