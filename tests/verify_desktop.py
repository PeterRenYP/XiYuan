"""通过真实 QProcess 和 Qt 事件循环验证桌面交互并保存截图。"""
from __future__ import annotations

import json
from pathlib import Path
import sys
import time

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication

from observatory.desktop import Observatory


def main():
    """加载真实 3B 模型、生成、定位激活，再检查真实单步等待。"""
    app = QApplication([])
    app.setOrganizationName("ModelObservatory")
    app.setApplicationName("模型观测台")
    root = Path(__file__).resolve().parent.parent / "artifacts"
    root.mkdir(exist_ok=True)
    window = Observatory()
    window.show()
    result = {"tests": {}, "errors": []}
    hybrid = "--qwen35" in sys.argv
    if hybrid:
        window.select_model_directory("D:/models/Qwen3.5-4B")
    phase = {"value": "loading"}
    start = time.monotonic()

    def save(name):
        """截图使用实际渲染完成后的原生窗口。"""
        window.grab().save(str(root / (("4B" if hybrid else "") + name)))

    def received(message):
        """按真实完成事件安排验证，不依赖固定的模型执行耗时。"""
        name = message["name"]
        if name not in ("operation.completed", "allocator.sample", "view.snapshot"):
            print("DESKTOP_EVENT", name, flush=True)
        if message["kind"] == "error":
            result["errors"].append(message["payload"])
            finish(1)
        elif name == "model.ready" and phase["value"] == "loading":
            if hybrid:
                assert window.layer_list.count() == 32 and window.neuron_id.maximum() == 9215
                assert window.head_select.count() == 32
            phase["value"] = "observing"
            window.maximum.setValue(2)
            window.temperature.setValue(0)
            window.prompt.setText("中国的首都是哪里？简短回答。")
            QTimer.singleShot(100, window.send)
        elif name == "run.finished" and phase["value"] == "observing":
            assert message["payload"]["outcome"] == "completed"
            result["tests"]["real_chat"] = message["payload"]
            phase["value"] = "viewing"
            window.follow.setChecked(False)
            window.tabs.setCurrentIndex(3)
            window.choose_layer(13)
            window.neuron_id.setValue(8948)
            window.token_select.setValue(len(window.tokens) - 1)
            window.selection.forward_id = "f0"
            window.select(operation="mlp")
        elif name == "view.snapshot" and phase["value"] == "viewing" and message["payload"].get("neuron") and message["payload"]["selection"]["neuron_index"] == 8948:
            phase["value"] = "screenshots"
            QTimer.singleShot(300, screenshots)
        elif name == "step.waiting" and phase["value"] == "stepping":
            result["tests"]["waiting_boundary"] = message["payload"]
            phase["value"] = "cancelling"
            save("真实逐步等待.png")
            QTimer.singleShot(400, window.cancel)
        elif name == "run.finished" and phase["value"] == "cancelling":
            assert message["payload"]["outcome"] == "cancelled"
            assert message["payload"]["forward_count"] == 0
            result["tests"]["step_cancel"] = True
            if hybrid:
                phase["value"] = "switch3b"
                window.select_model_directory("D:/models/models/Qwen__Qwen2.5-3B-Instruct")
                QTimer.singleShot(100, window.load_model)
            else:
                finish(0)
        elif name == "model.ready" and phase["value"] == "switch3b":
            assert window.layer_list.count() == 36 and window.neuron_id.maximum() == 11007
            assert not window.snapshot.get("neuron") and window.trace_path is None
            phase["value"] = "switch4b"
            window.select_model_directory("D:/models/Qwen3.5-4B")
            QTimer.singleShot(100, window.load_model)
        elif name == "model.ready" and phase["value"] == "switch4b":
            assert window.layer_list.count() == 32 and window.neuron_id.maximum() == 9215
            result["tests"]["model_switch"] = {"pass": True, "sequence": ["4B", "3B", "4B"], "history_cleared": True}
            finish(0)

    def screenshots():
        """真实激活分布和编辑页，以及最小尺寸都能访问聊天与 GPU。"""
        save("真实神经元观测.png")
        window.neuron_views.setCurrentIndex(3)
        save("真实神经元编辑.png")
        window.resize(1240, 760)
        app.processEvents()
        save("最小窗口1240.png")
        window.resize(1540, 960)
        window.set_mode("normal")
        app.processEvents()
        save("真实正常对话.png")
        window.set_mode("step")
        phase["value"] = "stepping"
        window.prompt.setText("继续。")
        window.send()
        result["tests"]["dimensions"] = {"layers": window.layer_list.count(), "heads": window.head_select.count(), "neuron_maximum": window.neuron_id.maximum()}

    def finish(code):
        """写入独立验证结果后走正常关闭路径。"""
        result["elapsed_seconds"] = round(time.monotonic() - start, 2)
        result["pass"] = code == 0
        (root / ("4B桌面实机验证.json" if hybrid else "桌面实机验证.json")).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        window.close()
        app.exit(code)

    window.worker.message.connect(received)
    QTimer.singleShot(600, window.load_model)
    QTimer.singleShot(5000, lambda: print("DESKTOP_STATE", phase, window.status.text(), window.run_state.text(), window.worker.process.bytesToWrite(), flush=True))
    QTimer.singleShot(240000, lambda: finish(2))
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
