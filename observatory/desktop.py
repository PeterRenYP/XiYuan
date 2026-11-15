"""原生模型观测工作台；显示层不执行模型计算。"""
from __future__ import annotations

import ctypes
from dataclasses import asdict, dataclass
from pathlib import Path
import sys
import time

from PySide6.QtCore import QSettings, QStandardPaths, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QKeySequence, QShortcut
from PySide6.QtWidgets import (QAbstractItemView, QButtonGroup, QCheckBox, QComboBox, QDialog,
    QDoubleSpinBox, QFileDialog, QFormLayout, QHBoxLayout, QLineEdit, QListWidget, QScrollArea,
    QSpinBox, QSplitter, QTabWidget, QTableWidget, QTableWidgetItem, QTextBrowser, QHeaderView, QWidget)

from .client import ProcessClient
from .model import OPERATIONS, HYBRID_OPERATIONS, OP_NAMES, operation_catalog
from .widgets import STYLE, LayerScene, TitleBar, box, button, label, panel

# 首次启动保持模型目录为空，历史记录仅来自用户自己的选择。
DEFAULT_MODEL = ""
MODES = {"normal": "正常对话", "observe": "观测模式", "step": "逐步模式"}
STATES = {"completed": "已完成", "cancelled": "已取消", "failed": "执行失败"}
ALL_OPS = [("embed", "词向量查表", "embed_tokens", "X = E[token_id]")] + OPERATIONS + HYBRID_OPERATIONS + [
    ("final_norm", "最终归一化", "norm", "Y = RMSNorm(Hlast)"), ("logits", "词表输出", "lm_head", "z = Y × Wvocabᵀ")]


@dataclass
class Selection:
    """查看坐标独立于真实执行坐标，所有视图共用这一身份。"""
    branch_id: str = "base"
    forward_id: str = "f0"
    layer_index: int = 0
    token_position: int = 0
    neuron_index: int = 0
    operation: str = "norm"
    head: int = 0
    dimension: int = 0
    weight_kind: str = "down"
    lens: bool = False


def number(value, precision=5):
    """缺失值使用中文显示，绝不当作零。"""
    return "未采集" if value is None else f"{value:+.{precision}f}"


class Observatory(QWidget):
    """异步原生工作台，独立运行器与监控进程保持界面响应。"""

    def __init__(self, mode="observe", start_processes=True):
        super().__init__()
        self.setWindowTitle("析元 · 本地模型")
        self.setWindowFlags(Qt.Window | Qt.FramelessWindowHint)
        self.resize(1540, 960)
        self.setMinimumSize(1240, 760)
        self.setStyleSheet(STYLE)
        self.settings = QSettings("ModelObservatory", "Desktop")
        self.data_root = Path(QStandardPaths.writableLocation(QStandardPaths.AppLocalDataLocation))
        self.data_root.mkdir(parents=True, exist_ok=True)
        self.model_path = self.settings.value("model_path", DEFAULT_MODEL)
        self.recent_paths = self.settings.value("recent_models", [])
        if isinstance(self.recent_paths, str):
            self.recent_paths = [self.recent_paths]
        self.loading_model = False
        self.mode, self.loaded, self.active = mode, False, False
        self.view_only, self.closing, self.was_maximized = False, False, False
        self.selection, self.execution = Selection(), None
        self.info, self.messages, self.tokens = {}, [], []
        self.snapshot, self.boundary = {}, None
        self.weights_revision, self.trace_path, self.run_id = "w0", None, None
        self.query_id, self.query_dirty, self.last_metric = None, False, 0
        self.forwards = []
        self.template = ""
        self.build()
        self.set_mode(mode)
        self.worker = ProcessClient("worker", self.data_root, self)
        self.monitor = ProcessClient("monitor", self.data_root, self)
        self.worker.message.connect(self.on_message)
        self.worker.failed.connect(self.show_error)
        self.worker.stopped.connect(self.worker_stopped)
        self.monitor.message.connect(self.on_metric)
        self.monitor.failed.connect(lambda text: self.gpu_state.setText("监控不可用：" + text))
        self.timer = QTimer(self)
        self.timer.setInterval(180)
        self.timer.timeout.connect(self.refresh_tick)
        self.timer.start()
        for key, action in (("F11", self.toggle_fullscreen), ("Escape", self.exit_fullscreen), ("F10", self.release),
                            ("Ctrl+O", self.configure_model), ("Ctrl+Return", self.send)):
            QShortcut(QKeySequence(key), self, action)
        self.set_active(False)
        if start_processes:
            self.worker.start()
            self.monitor.start()

    def build(self):
        """构建三列工作台，中间对话窗口在所有模式保持可见。"""
        root = box(self, margins=0, spacing=6)
        self.titlebar = TitleBar(self)
        root.addWidget(self.titlebar)
        toolbar = QWidget()
        row = box(toolbar, True, 8)
        self.mode_group, self.mode_buttons = QButtonGroup(self), {}
        for key, text in MODES.items():
            control = button(text, lambda checked=False, k=key: self.set_mode(k))
            control.setCheckable(True)
            self.mode_group.addButton(control)
            self.mode_buttons[key] = control
            row.addWidget(control)
        self.run_state = label("未加载模型", "accent")
        row.addWidget(self.run_state, 1)
        self.follow = QCheckBox("跟随实时位置")
        self.follow.setChecked(True)
        self.follow.toggled.connect(self.follow_changed)
        row.addWidget(self.follow)
        self.next_button = button("等待计算边界", self.release, True)
        self.next_button.setEnabled(False)
        row.addWidget(self.next_button)
        row.addWidget(button("配置", self.configure_model))
        root.addWidget(toolbar)
        self.main_splitter = QSplitter(Qt.Horizontal)
        sidebar, left = panel("模型管理")
        sidebar.setMinimumWidth(180)
        sidebar.setMaximumWidth(270)
        self.model_name = label("尚未加载模型", "heading", 15)
        self.model_facts = label("配置与权重待校验", "muted")
        self.path_label = label(self.model_path, "muted")
        self.load_button = button("加载本地模型", self.load_model, True)
        self.recent_models = QComboBox()
        self.recent_models.setToolTip("选择目录后点击加载或切换，才会更换运行模型")
        self.refresh_recent()
        self.recent_models.activated.connect(lambda index: self.select_model_directory(self.recent_models.itemData(index)))
        for widget in (self.model_name, button("选择模型目录", self.browse_model), self.recent_models, self.path_label, self.model_facts, self.load_button,
                       button("卸载模型", lambda: self.worker.send("model.unload"))):
            left.addWidget(widget)
        self.layer_list = QListWidget()
        self.layer_list.currentRowChanged.connect(self.choose_layer)
        left.addWidget(self.layer_list, 1)
        self.trace_label = label("尚无运行记录", "muted")
        left.addWidget(self.trace_label)
        for text, callback in (("打开离线记录", self.open_trace), ("导出运行记录", self.export_trace), ("导出修改后的模型", self.export_model)):
            left.addWidget(button(text, callback))
        self.main_splitter.addWidget(sidebar)
        center = QWidget()
        middle = box(center, margins=2)
        self.position_label = label("执行位置：尚未运行", "accent", 14)
        middle.addWidget(self.position_label)
        self.navigation = QWidget()
        nav = box(self.navigation, True, 0)
        self.branch_select, self.forward_select = QComboBox(), QComboBox()
        self.branch_select.addItem("原始分支", "base")
        self.branch_select.currentIndexChanged.connect(self.history_changed)
        self.forward_select.currentIndexChanged.connect(self.history_changed)
        self.token_select = QSpinBox()
        self.token_select.setPrefix("位置 T")
        self.token_select.valueChanged.connect(lambda value: self.select(token_position=value))
        self.token_select.setToolTip("包含聊天模板和特殊Token的绝对位置，不是Token编号")
        self.stage_select = QComboBox()
        for title, key in (("输入文本", "input"), ("分词结果", "tokens"), ("查词向量", "embed"), ("层内计算", "layer"),
                           ("词表输出", "logits"), ("Token选择", "sample"), ("解码文字", "decode")):
            self.stage_select.addItem(title, key)
        self.stage_select.setCurrentIndex(3)
        self.stage_select.currentIndexChanged.connect(self.stage_changed)
        nav.addWidget(self.stage_select)
        for widget in (self.branch_select, self.forward_select, self.token_select):
            nav.addWidget(widget, 1)
        middle.addWidget(self.navigation)
        self.content_splitter, self.debug_splitter = QSplitter(Qt.Vertical), QSplitter(Qt.Horizontal)
        self.debug_splitter.addWidget(self.scrolling(self.build_inspector()))
        self.debug_splitter.addWidget(self.scrolling(self.build_scene()))
        self.debug_splitter.setSizes([530, 330])
        self.content_splitter.addWidget(self.debug_splitter)
        chat_panel, chat_box = panel("对话窗口")
        self.chat = QTextBrowser()
        self.chat.setMinimumHeight(65)
        self.chat.setPlainText("请选择并加载本地模型。观测模式自动运行，逐步模式在真实计算边界等待。")
        chat_box.addWidget(self.chat)
        self.content_splitter.addWidget(chat_panel)
        self.content_splitter.setSizes([540, 145])
        middle.addWidget(self.content_splitter, 1)
        composer, compose = panel()
        input_row = QHBoxLayout()
        self.prompt = QLineEdit()
        self.prompt.setPlaceholderText("输入消息，Ctrl+Enter 发送")
        self.prompt.returnPressed.connect(self.send)
        self.send_button, self.stop_button = button("发送并观测", self.send, True), button("停止生成", self.cancel)
        input_row.addWidget(self.prompt, 1)
        input_row.addWidget(self.send_button)
        input_row.addWidget(self.stop_button)
        compose.addLayout(input_row)
        options = QHBoxLayout()
        self.temperature, self.maximum, self.seed = QDoubleSpinBox(), QSpinBox(), QSpinBox()
        self.temperature.setRange(0, 2)
        self.temperature.setSingleStep(.1)
        self.temperature.setValue(.7)
        self.maximum.setRange(1, 4096)
        self.maximum.setValue(64)
        self.seed.setRange(0, 2147483647)
        self.seed.setValue(42)
        for title, control in (("温度", self.temperature), ("输出上限", self.maximum), ("随机种子", self.seed)):
            options.addWidget(label(title))
            options.addWidget(control)
        options.addWidget(button("新建对话", self.new_conversation))
        compose.addLayout(options)
        middle.addWidget(composer)
        self.main_splitter.addWidget(center)
        gpu, right = panel("GPU 监控")
        gpu.setMinimumWidth(210)
        gpu.setMaximumWidth(290)
        self.gpu_name, self.gpu_uuid = label("等待 NVML", "heading", 14), label("", "muted")
        self.gpu_memory, self.gpu_usage = label("未采集", "accent", 23), label("未采集", "accent", 23)
        self.gpu_state = label("等待监控进程", "muted")
        self.allocator = label("运行器分配器：未加载模型", "muted")
        for widget in (self.gpu_name, self.gpu_uuid, label("整卡显存 / NVML"), self.gpu_memory, label("整卡利用率 / NVML"),
                       self.gpu_usage, self.gpu_state, label("推理框架显存 / PyTorch", "heading"), self.allocator,
                       label("整卡显存包含其他程序。已分配包含在保留显存中，两者不相加。", "muted")):
            right.addWidget(widget)
        right.addStretch()
        self.event_count = label("主前向次数：0\n读取视图不会重新推理", "muted")
        right.addWidget(self.event_count)
        self.main_splitter.addWidget(gpu)
        for splitter in (self.main_splitter, self.debug_splitter, self.content_splitter):
            splitter.setHandleWidth(6)
            for i in range(splitter.count()):
                splitter.setCollapsible(i, False)
        self.main_splitter.setSizes([195, 1090, 240])
        root.addWidget(self.main_splitter, 1)
        self.status = label("未加载模型 / 不显示演示数值", "muted")
        self.status.setMinimumHeight(26)
        root.addWidget(self.status)

    def scrolling(self, content):
        """短窗口保留原生滚动条，保证操作可达。"""
        result = QScrollArea()
        result.setWidgetResizable(True)
        result.setWidget(content)
        return result

    def make_table(self, rows=6, cols=8):
        """创建有界切片表格，不向控件灌入完整模型张量。"""
        result = QTableWidget(rows, cols)
        result.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        result.verticalHeader().setDefaultSectionSize(30)
        result.setEditTriggers(QAbstractItemView.NoEditTriggers)
        result.setMinimumHeight(160)
        result.setFont(QFont("Consolas", 10))
        return result

    def build_inspector(self):
        """默认数值页，不添加教学页。"""
        widget, layout = panel("计算检查器")
        widget.setMinimumWidth(370)
        self.operation_select = QComboBox()
        for key, title, module, formula in ALL_OPS:
            self.operation_select.addItem(title, key)
        self.operation_select.currentIndexChanged.connect(lambda i: self.select(operation=self.operation_select.itemData(i)))
        layout.addWidget(self.operation_select)
        self.tabs = QTabWidget()
        numeric = QWidget()
        numbers = box(numeric, margins=0)
        self.formula, self.shape_label, self.identity = label("选择运算", "heading", 16), label("未执行或未采集", "muted"), label("", "accent")
        self.matrix = self.make_table()
        self.matrix.cellClicked.connect(self.scalar_selected)
        self.scalar_label = label("点击元素查看真实标量", "muted")
        for item in (self.formula, self.shape_label, self.identity, self.matrix, self.scalar_label):
            numbers.addWidget(item)
        self.stage_table = self.make_table(1, 2)
        self.stage_table.setHorizontalHeaderLabels(["字段", "实际数据"])
        self.stage_table.hide()
        numbers.addWidget(self.stage_table)
        self.tabs.addTab(numeric, "运算数值")
        tensor = QWidget()
        tensors = box(tensor, margins=0)
        self.tensor_select = QComboBox()
        self.tensor_select.currentIndexChanged.connect(self.tensor_changed)
        self.tensor_matrix = self.make_table()
        self.slice_ranges = QLineEdit()
        self.slice_ranges.setPlaceholderText("各轴范围，例如 0:1,0:4,0:8")
        self.tensor_note = label("safetensors 快照；未采集的值不会补造。", "muted")
        for item in (self.tensor_select, self.tensor_matrix, self.slice_ranges, button("读取指定切片", self.read_slice), self.tensor_note):
            tensors.addWidget(item)
        self.tabs.addTab(tensor, "张量切片")
        attention = QWidget()
        attentions = box(attention, margins=0)
        self.head_select = QComboBox()
        self.head_select.currentIndexChanged.connect(lambda i: self.select(head=max(0, i)))
        self.attention_table, self.attention_note = self.make_table(8, 12), label("等待真实注意力", "muted")
        for item in (self.head_select, self.attention_table, self.attention_note):
            attentions.addWidget(item)
        self.tabs.addTab(attention, "注意力")
        self.tabs.addTab(self.build_neurons(), "神经元观测")
        self.tabs.currentChanged.connect(self.inspector_changed)
        layout.addWidget(self.tabs)
        return widget

    def build_neurons(self):
        """动态 MLP 通道定位、实际门控数值和版本化编辑入口。"""
        widget = QWidget()
        layout = box(widget, margins=0)
        controls = QHBoxLayout()
        controls.addWidget(label("神经元编号"))
        self.neuron_id = QSpinBox()
        self.neuron_id.setPrefix("N")
        self.neuron_id.valueChanged.connect(lambda v: self.select(neuron_index=v))
        controls.addWidget(self.neuron_id)
        controls.addWidget(button("定位最强激活", self.strongest))
        layout.addLayout(controls)
        self.neuron_context = label("未采集", "accent")
        layout.addWidget(self.neuron_context)
        self.neuron_views = QTabWidget()
        distribution = QWidget()
        dist = box(distribution, margins=0)
        self.neuron_grid, self.neuron_top = self.make_table(4, 8), self.make_table(8, 3)
        self.neuron_grid.verticalHeader().setDefaultSectionSize(43)
        self.neuron_grid.setMinimumHeight(200)
        self.neuron_grid.currentCellChanged.connect(self.neuron_cell)
        self.neuron_top.setHorizontalHeaderLabels(["排名", "通道", "激活"])
        self.neuron_top.currentCellChanged.connect(self.neuron_rank)
        self.neuron_summary = label("等待真实门控后激活", "muted")
        dist.addWidget(self.neuron_grid)
        dist.addWidget(self.neuron_summary)
        dist.addWidget(label("正值为蓝色，负值为橙色。高激活不等于对最终答案影响大。", "muted"))
        self.neuron_views.addTab(distribution, "局部分布")
        self.neuron_views.addTab(self.neuron_top, "绝对值前八")
        calculation = QWidget()
        calc = box(calculation, margins=0)
        self.neuron_calculation, self.neuron_contribution = label("未执行", "heading", 15), label("未执行", "muted")
        calc.addWidget(self.neuron_calculation)
        calc.addWidget(self.neuron_contribution)
        calc.addStretch()
        self.neuron_views.addTab(calculation, "单元计算")
        editor = QWidget()
        edit = box(editor, margins=0)
        self.edit_kind = QComboBox()
        for title, key in (("激活：缩放", "scale"), ("激活：置零", "zero"), ("激活：指定值", "set"),
                           ("权重：门控投影", "gate"), ("权重：上投影", "up"), ("权重：下投影", "down")):
            self.edit_kind.addItem(title, key)
        self.edit_kind.currentIndexChanged.connect(self.edit_changed)
        self.edit_dimension, self.edit_value = QSpinBox(), QDoubleSpinBox()
        self.edit_dimension.valueChanged.connect(lambda v: self.select(dimension=v))
        self.edit_value.setDecimals(7)
        self.edit_value.setRange(-1e8, 1e8)
        self.edit_value.setValue(1)
        self.edit_value.setSingleStep(.1)
        form = QFormLayout()
        for title, field in (("修改方式", self.edit_kind), ("隐藏维度 d", self.edit_dimension), ("修改值或缩放系数", self.edit_value)):
            form.addRow(title, field)
        edit.addLayout(form)
        self.edit_comparison = label("等待实际原值与当前值", "muted")
        edit.addWidget(self.edit_comparison)
        self.apply_button = button("应用并建立重算分支", self.apply_edit, True)
        self.restore_button = button("恢复当前神经元并重算", self.restore_edit)
        edit.addWidget(self.apply_button)
        edit.addWidget(self.restore_button)
        edit.addWidget(label("运行中提交的修改在本次结束后应用。重算使用相同输入与种子、空缓存。激活只作用于指定层、位置与通道。参数导出不包含激活干预。", "muted"))
        edit.addStretch()
        self.neuron_views.addTab(editor, "调整编辑")
        self.neuron_views.currentChanged.connect(lambda i: self.follow.setChecked(False) if i == 3 else None)
        layout.addWidget(self.neuron_views)
        return widget

    def build_scene(self):
        """结构投影和Token分布共用所选前向，不触发主推理。"""
        widget, layout = panel("三维结构与Token分布")
        widget.setMinimumWidth(245)
        self.scene = LayerScene()
        self.scene.selected.connect(self.choose_layer)
        layout.addWidget(self.scene, 1)
        self.lens = QCheckBox("中间层观察性投影")
        self.lens.toggled.connect(lambda v: self.select(lens=v))
        layout.addWidget(self.lens)
        self.probability_table = self.make_table(10, 3)
        self.probability_table.setHorizontalHeaderLabels(["Token", "采样概率", "原始分数"])
        self.probability_note = label("等待真实分布；前十项不等于全分布。", "muted")
        layout.addWidget(self.probability_table)
        layout.addWidget(self.probability_note)
        return widget

    def set_mode(self, mode):
        """只更改执行策略与布局，保留聊天和历史记录。"""
        self.mode = mode
        self.mode_buttons[mode].setChecked(True)
        self.debug_splitter.setVisible(mode != "normal")
        self.navigation.setVisible(mode != "normal")
        self.follow.setVisible(mode == "observe")
        self.next_button.setVisible(mode == "step")
        self.send_button.setText({"normal": "发送", "observe": "发送并观测", "step": "发送并等待放行"}[mode])
        if self.active and hasattr(self, "worker"):
            self.worker.send("run.change_mode", {"mode": mode})

    def configure_model(self):
        """配置只保存路径与选项，真实就绪状态由运行器返回。"""
        dialog = QDialog(self)
        dialog.setWindowTitle("模型与运行配置")
        dialog.setMinimumWidth(570)
        layout, form = box(dialog), QFormLayout()
        path = QLineEdit(self.model_path)
        def browse():
            """选取目录，不触发模型加载。"""
            selected = QFileDialog.getExistingDirectory(dialog, "选择本地模型目录", path.text())
            if selected:
                path.setText(selected)
        layout.addWidget(button("浏览模型目录", browse))
        device, dtype = QComboBox(), QComboBox()
        for title, key in (("显卡 0（CUDA）", "cuda:0"), ("处理器（CPU）", "cpu")):
            device.addItem(title, key)
        for title, key in (("自动", "auto"), ("BF16", "bfloat16"), ("FP16", "float16"), ("FP32", "float32")):
            dtype.addItem(title, key)
        device.setCurrentIndex(max(0, device.findData(self.settings.value("device", "cuda:0"))))
        dtype.setCurrentIndex(max(0, dtype.findData(self.settings.value("dtype", "auto"))))
        top_k, top_p, penalty = QSpinBox(), QDoubleSpinBox(), QDoubleSpinBox()
        top_k.setRange(0, 151936)
        top_p.setRange(.01, 1)
        penalty.setRange(.1, 3)
        top_k.setValue(int(self.settings.value("top_k", 20)))
        top_p.setValue(float(self.settings.value("top_p", .8)))
        penalty.setValue(float(self.settings.value("repetition_penalty", 1.05)))
        stop = QLineEdit(self.settings.value("stop", ""))
        for title, field in (("模型目录", path), ("执行设备", device), ("计算精度", dtype), ("候选数量上限（0 不限）", top_k),
                             ("累计概率阈值", top_p), ("重复惩罚", penalty), ("停止字符串（用 | 分隔）", stop)):
            form.addRow(title, field)
        layout.addLayout(form)
        def save():
            """保存用户配置；更换模型后需要显式重新加载。"""
            self.select_model_directory(path.text().strip())
            for key, value in (("model_path", self.model_path), ("device", device.currentData()), ("dtype", dtype.currentData()),
                               ("top_k", top_k.value()), ("top_p", top_p.value()), ("repetition_penalty", penalty.value()),
                               ("stop", stop.text())):
                self.settings.setValue(key, value)
            dialog.accept()
            self.status.setText("配置已保存；模型、设备或精度变化在重新加载后生效。")
        layout.addWidget(button("保存配置", save, True))
        dialog.exec()

    def load_model(self):
        """显式启动本地加载，未收到就绪事件前禁止发送消息。"""
        if self.active:
            return
        self.view_only = False
        self.load_button.setEnabled(False)
        self.run_state.setText("校验并加载模型中")
        self.worker.send("model.load", {"path": self.model_path, "device": self.settings.value("device", "cuda:0"), "dtype": self.settings.value("dtype", "auto")})

    def refresh_recent(self):
        """显示最近目录，路径和实际已加载身份分开保存。"""
        self.recent_models.clear()
        for path in dict.fromkeys([self.model_path] + self.recent_paths):
            self.recent_models.addItem(Path(path).name, path)

    def select_model_directory(self, path):
        """仅选择下一次加载目录，当前运行模型不受影响。"""
        if not path:
            return
        self.model_path = str(path)
        self.recent_paths = list(dict.fromkeys([self.model_path] + self.recent_paths))[:10]
        self.settings.setValue("model_path", self.model_path)
        self.settings.setValue("recent_models", self.recent_paths)
        self.path_label.setText("已选目录：" + self.model_path)
        self.load_button.setText("切换到所选模型" if self.loaded else "加载所选模型")
        self.refresh_recent()

    def browse_model(self):
        """使用原生目录选择器自由选择本地模型。"""
        self.select_model_directory(QFileDialog.getExistingDirectory(self, "选择本地模型目录", self.model_path))

    def send(self):
        """提交完整多轮角色消息，一次只运行一个生成任务。"""
        text = self.prompt.text().strip()
        if not self.loaded or self.view_only:
            self.show_error("请先加载本地模型")
            return
        if self.active or not text:
            return
        self.messages += [{"role": "user", "content": text}, {"role": "assistant", "content": "", "state": "running"}]
        self.render_chat()
        payload = {"messages": [{"role": m["role"], "content": m["content"]} for m in self.messages[:-1]], "mode": self.mode,
            "generation": {"seed": self.seed.value(), "max_new_tokens": self.maximum.value(), "temperature": self.temperature.value(),
                "top_k": int(self.settings.value("top_k", 20)), "top_p": float(self.settings.value("top_p", .8)),
                "repetition_penalty": float(self.settings.value("repetition_penalty", 1.05)), "stop": [s for s in self.settings.value("stop", "").split("|") if s], "template_options": {"enable_thinking": False}},
            "capture": {"policy": "none" if self.mode == "normal" else "standard", "disk_budget_bytes": 0},
            "trace_root": str(self.data_root / "traces")}
        self.execution, self.boundary, self.query_id = None, None, None
        self.forwards, self.snapshot = [], {}
        self.forward_select.blockSignals(True)
        self.forward_select.clear()
        self.forward_select.blockSignals(False)
        self.branch_select.blockSignals(True)
        self.branch_select.clear()
        self.branch_select.addItem("原始分支", "base")
        self.branch_select.blockSignals(False)
        self.selection.branch_id = "base"
        self.display_snapshot({"status": "pending"})
        self.worker.send("run.start", payload)
        self.prompt.clear()
        self.set_active(True)

    def set_active(self, active):
        """集中维护按钮状态，查看和监控始终独立于计算。"""
        self.active = active
        self.send_button.setEnabled(self.loaded and not active and not self.view_only)
        self.stop_button.setEnabled(active)
        self.load_button.setEnabled(not active and not self.loading_model)
        editable = self.loaded and not self.view_only and self.trace_path is not None
        self.apply_button.setEnabled(editable)
        self.restore_button.setEnabled(editable)

    def cancel(self):
        """请求取消，保留已经产生的部分回复。"""
        if self.active:
            self.worker.send("run.cancel")
            self.run_state.setText("取消中")

    def release(self):
        """同一边界只提交一次放行；观测模式不响应 F10。"""
        if self.mode == "step" and self.boundary and self.active:
            self.worker.send("step.release", {"expected_boundary_id": self.boundary["boundary_id"],
                             "expected_state_revision": self.boundary["state_revision"], "granularity": "operation"})
            self.boundary = None
            self.next_button.setEnabled(False)

    def render_chat(self):
        """纯文本显示模型输出，防止用户文本被解释成富文本。"""
        chunks = []
        for message in self.messages:
            title = {"user": "你", "assistant": "模型", "system": "系统"}[message["role"]]
            state = message.get("state")
            if state in ("cancelled", "failed"):
                title += "（部分回复，" + STATES[state] + "）"
            chunks.append(title + "：" + message["content"])
        self.chat.setPlainText("\n\n".join(chunks))
        self.chat.verticalScrollBar().setValue(self.chat.verticalScrollBar().maximum())

    def new_conversation(self):
        """空闲时新建对话，已保存的运行记录保留。"""
        if not self.active:
            self.messages = []
            self.chat.setPlainText("新对话已准备，可以输入消息。")

    def on_message(self, message):
        """执行位置只由真实事件推进；查看请求按请求编号丢弃过期响应。"""
        name, value = message["name"], message["payload"]
        if message["kind"] == "error":
            self.loading_model = False
            self.show_error(value["message"])
            self.query_id = None
            if value["code"] not in ("STALE_BOUNDARY", "NOT_WAITING", "STALE_WEIGHTS", "INVALID_SLICE", "TENSOR_NOT_CAPTURED"):
                self.set_active(False)
            self.load_button.setEnabled(True)
        elif name == "runtime.hello":
            self.worker.send("hello")
            self.status.setText("运行器已连接，请加载模型")
        elif name == "model.progress":
            self.run_state.setText(value["message"])
        elif name == "model.loading":
            self.loading_model = True
            self.set_active(True)
        elif name == "model.cancelled":
            self.loading_model = False
            self.set_active(False)
            self.run_state.setText("模型加载已取消")
        elif name == "model.ready":
            self.loading_model = False
            self.info, self.loaded, self.weights_revision = value, True, value["weights_revision"]
            self.selection, self.execution, self.boundary = Selection(), None, None
            self.query_id, self.run_id, self.tokens, self.forwards = None, None, [], []
            self.forward_select.clear()
            self.branch_select.clear()
            self.branch_select.addItem("原始分支", "base")
            self.trace_label.setText("尚无运行记录")
            self.load_button.setText("切换到所选模型")
            self.trace_path, self.snapshot = None, {}
            self.configure_dimensions(value)
            self.neuron_id.setValue(0)
            self.edit_dimension.setValue(0)
            self.token_select.setRange(0, 0)
            self.display_snapshot({"status": "not_captured"})
            self.allocator.setText("处理器运行 / 显卡分配器不可用" if value.get("device", "").startswith("cpu") else "模型已加载 / 等待分配器采样")
            self.run_state.setText("模型已就绪")
            self.set_active(False)
        elif name == "model.unloaded":
            self.loaded = False
            self.run_state.setText("模型已卸载")
            self.allocator.setText("分配器指标不可用 / 模型已卸载")
            self.set_active(self.loading_model)
        elif name == "run.started":
            self.run_id, self.trace_path = message["run_id"], value["path"]
            self.weights_revision, self.tokens = value["weights_revision"], value["tokens"]
            self.template = value.get("template", "")
            self.token_select.setRange(0, max(0, len(self.tokens) - 1))
            self.trace_label.setText("记录 " + self.run_id[:12] + "\n版本 " + self.weights_revision)
            self.selection.branch_id = value["branch_id"]
            self.run_state.setText("执行中")
            self.set_active(True)
        elif name == "forward.started":
            entry = (message["branch_id"], value["forward_id"])
            if entry not in self.forwards:
                self.forwards.append(entry)
                self.forward_select.blockSignals(True)
                self.forward_select.addItem(f"第 {value['generation_step'] + 1} 次前向", entry)
                self.forward_select.blockSignals(False)
            if self.follow.isChecked():
                self.selection.forward_id = value["forward_id"]
                self.selection.token_position = value["positions"][-1]
        elif name == "operation.completed":
            context = message["context"]
            op = value["operation"]
            self.execution = dict(branch_id=message["branch_id"], forward_id=context["forward_id"], operation=op, token_position=value["positions"][-1])
            layer_index = context["layer_index"] if context["layer_index"] is not None else -1
            if layer_index >= 0:
                self.execution["layer_index"] = layer_index
            title = OP_NAMES.get(op, {"embed": "词向量查表", "final_norm": "最终归一化", "logits": "词表输出"}.get(op, op))
            self.position_label.setText("执行位置：" + (f"第 {layer_index + 1} 层 / " if layer_index >= 0 else "") + title)
            if self.follow.isChecked():
                self.follow_changed(True)
        elif name == "step.waiting":
            self.boundary = value
            self.next_button.setEnabled(True)
            self.next_button.setText("放行：" + value["operation_name"])
            self.run_state.setText("计算边界等待")
            self.status.setText("下一运算尚未执行：" + value["operation_name"])
        elif name == "run.mode_changed":
            self.boundary = None
            self.next_button.setEnabled(False)
            self.run_state.setText("已切换为" + MODES[value["mode"]])
        elif name == "text.updated":
            if self.messages and self.messages[-1]["role"] == "assistant":
                self.messages[-1]["content"] = value["text"]
                self.render_chat()
        elif name == "token.sampled":
            self.token_select.setMaximum(value["position"])
            if self.follow.isChecked():
                self.show_probabilities(value["candidates"])
        elif name == "run.finished":
            self.boundary = None
            self.next_button.setEnabled(False)
            self.set_active(False)
            self.run_state.setText(STATES[value["outcome"]])
            if self.messages and self.messages[-1]["role"] == "assistant":
                self.messages[-1].update(content=value["text"], state=value["outcome"])
                self.render_chat()
            self.event_count.setText(f"主前向次数：{value['forward_count']}\n读取视图不会重新推理")
            self.status.setText(STATES[value["outcome"]] + " / " + ("部分数据未采集" if value["capture_integrity"] == "incomplete" else "记录已保存") + " / " + value["path"])
            self.query_dirty = True
        elif name == "allocator.sample":
            self.allocator.setText("已分配：%.2f GiB\n保留：%.2f GiB\n峰值：%.2f GiB" % tuple(value[k] / 1024**3 for k in ("allocated", "reserved", "peak")))
        elif name == "capture.degraded":
            self.status.setText(value["message"])
        elif name == "view.snapshot":
            if message["request_id"] == self.query_id:
                self.query_id = None
                if value.get("selection") == asdict(self.selection):
                    self.display_snapshot(value)
        elif name == "tensor.slice.result":
            cols = value["shape"][-1] if value["shape"] else 1
            values = value["values"]
            self.fill_table(self.tensor_matrix, [values[i:i + cols] for i in range(0, len(values), cols)])
            self.tensor_note.setText("真实采集 / 切片形状 " + str(value["shape"]))
        elif name == "scalar.result":
            self.scalar_label.setText("捕获值：" + number(value["captured"], 8) + "\n派生 FP32 内积重算：" + number(value["recomputed_fp32"], 8) +
                                      f"\n{value['terms']} 项加权求和，偏置 {value['bias']:+.6f}；绝对误差 {value['absolute_error']:.8g}")
        elif name == "patch.accepted":
            self.status.setText("修改已受理，本次运行结束后应用并重算")
        elif name == "patch.applied":
            self.weights_revision = value["weights_revision"]
            self.status.setText("修改已应用 / 版本 " + self.weights_revision)
        elif name == "branch.created":
            self.branch_select.blockSignals(True)
            self.branch_select.addItem("重算分支 " + value["branch_id"][-8:], value["branch_id"])
            self.branch_select.setCurrentIndex(self.branch_select.count() - 1)
            self.branch_select.blockSignals(False)
            self.follow.setChecked(True)
            self.set_mode("observe")
        elif name == "trace.opened":
            self.display_trace(value)
        elif name in ("trace.exported", "model.exported"):
            self.status.setText("已导出：" + value["path"])

    def configure_dimensions(self, info):
        """所有层、头、通道和隐藏维度来自实际 config。"""
        c = info["config"]
        self.display_config = c
        self.model_name.setText(info.get("name", "Qwen2.5"))
        self.model_facts.setText(f"{c['num_hidden_layers']} 层 / 隐藏维度 {c['hidden_size']}\n{c['num_attention_heads']} 查询头 / {c['num_key_value_heads']} KV 头\nMLP 通道 {c['intermediate_size']}\n" + {"bfloat16": "BF16", "float16": "FP16", "float32": "FP32"}.get(info.get("dtype"), "离线记录"))
        self.layer_list.blockSignals(True)
        self.layer_list.clear()
        self.layer_list.addItems([f"第 {i + 1:02d} 层" + (" · 线性" if kind == "linear_attention" else " · 全注意力" if kind else "")
            for i, kind in enumerate(c.get("layer_types", [None] * c["num_hidden_layers"]))])
        self.layer_list.setCurrentRow(0)
        self.layer_list.blockSignals(False)
        self.head_select.blockSignals(True)
        self.head_select.clear()
        self.head_select.addItems([f"查询头 {i} / KV 头 {i // (c['num_attention_heads'] // c['num_key_value_heads'])}" for i in range(c["num_attention_heads"])])
        self.head_select.blockSignals(False)
        self.neuron_id.setRange(0, c["intermediate_size"] - 1)
        self.edit_dimension.setRange(0, c["hidden_size"] - 1)
        self.scene.count = c["num_hidden_layers"]
        self.scene.update()
        self.refresh_layer_controls(0)

    def refresh_layer_controls(self, index):
        """切层时按真实架构更新操作与头范围，避免沿用另一模型的维度。"""
        c = getattr(self, "display_config", None)
        if not c or not 0 <= index < c["num_hidden_layers"]:
            return
        self.operation_select.blockSignals(True)
        self.operation_select.clear()
        for key, title, module, formula in [ALL_OPS[0]] + operation_catalog(c, index) + ALL_OPS[-2:]:
            self.operation_select.addItem(title, key)
        if self.operation_select.findData(self.selection.operation) < 0:
            self.selection.operation = "norm"
        self.operation_select.setCurrentIndex(self.operation_select.findData(self.selection.operation))
        self.operation_select.blockSignals(False)
        self.head_select.blockSignals(True)
        self.head_select.clear()
        linear = c.get("layer_types", [])[index:index + 1] == ["linear_attention"]
        count = c["linear_num_value_heads"] if linear else c["num_attention_heads"]
        groups = count // (c["linear_num_key_heads"] if linear else c["num_key_value_heads"])
        self.head_select.addItems([f"值头 {i} / 查询键头 {i // groups}" if linear else f"查询头 {i} / KV 头 {i // groups}" for i in range(count)])
        self.selection.head = min(self.selection.head, count - 1)
        self.head_select.setCurrentIndex(self.selection.head)
        self.head_select.blockSignals(False)

    def display_trace(self, value):
        """没有模型也能读取已保存的历史数值。"""
        self.view_only, self.trace_path = True, value["path"]
        manifest = value["manifest"]
        self.configure_dimensions(manifest["model"])
        self.messages = manifest.get("messages", []) + [{"role": "assistant", "content": manifest.get("branches", {}).get("base", {}).get("text", "")}]
        self.render_chat()
        self.forward_select.blockSignals(True)
        self.branch_select.blockSignals(True)
        self.forward_select.clear()
        self.branch_select.clear()
        self.forwards, branches = [], set()
        for item in value["catalog"]:
            branch, forward_id = item["branch_id"], item["forward_id"]
            if branch not in branches:
                self.branch_select.addItem("原始分支" if branch == "base" else "重算分支 " + branch[-8:], branch)
                branches.add(branch)
            self.forwards.append((branch, forward_id))
            self.forward_select.addItem("前向 " + forward_id[1:], (branch, forward_id))
        self.forward_select.blockSignals(False)
        self.branch_select.blockSignals(False)
        self.token_select.setRange(0, max(0, len(manifest.get("token_ids", [])) - 1))
        self.selection = Selection()
        self.template = manifest.get("template", "")
        self.tokens = [{"position": i, "id": t} for i, t in enumerate(manifest.get("token_ids", []))]
        self.query_dirty = True
        self.follow.setChecked(False)
        self.set_active(False)
        self.run_state.setText("离线记录查看")
        self.status.setText("已打开离线记录：" + self.trace_path)

    def on_metric(self, message):
        """独立监控，缺失或过期的读数不会显示为零。"""
        value = message["payload"]
        self.last_metric = time.monotonic()
        if value.get("status") != "available":
            self.gpu_state.setText("指标不可用：" + value.get("reason", "未知原因"))
            self.gpu_memory.setText("不可用")
            self.gpu_usage.setText("不可用")
        elif value.get("index", 0) == 0:
            self.gpu_name.setText(value["name"])
            self.gpu_uuid.setText(value["uuid"])
            self.gpu_memory.setText(f"{value['used'] / 1024**3:.2f} / {value['total'] / 1024**3:.2f} GiB")
            self.gpu_usage.setText(f"{value['utilization']}%")
            self.gpu_state.setText("真实整卡指标 / 每秒更新")

    def refresh_tick(self):
        """合并视图刷新，不将 UI 处理速度作为计算放行条件。"""
        if self.last_metric and time.monotonic() - self.last_metric > 5:
            self.gpu_state.setText("指标已过期，等待监控恢复")
            self.gpu_memory.setText("已过期")
            self.gpu_usage.setText("已过期")
        if self.query_dirty and not self.query_id and self.trace_path:
            self.query_dirty = False
            self.query_id = self.worker.send("view.query", asdict(self.selection))

    def select(self, **changes):
        """改变共享查看坐标，绝不触发前向或单步放行。"""
        for key, value in changes.items():
            setattr(self.selection, key, value)
        if changes and hasattr(self, "follow"):
            self.follow.setChecked(False)
        if hasattr(self, "scene"):
            self.scene.layer = self.selection.layer_index
            self.scene.update()
        self.query_dirty = True

    def stage_changed(self):
        """阶段导航直接展示真实输入、分词、采样与解码字段。"""
        self.follow.setChecked(False)
        stage = self.stage_select.currentData()
        if stage in ("embed", "logits"):
            self.select(operation=stage)
            self.operation_select.blockSignals(True)
            self.operation_select.setCurrentIndex(self.operation_select.findData(stage))
            self.operation_select.blockSignals(False)
        self.show_stage()

    def show_stage(self):
        """没有对应事件的数据明确缺失，不沿用其他阶段的矩阵。"""
        stage = self.stage_select.currentData()
        numeric = stage in ("embed", "layer", "logits")
        self.matrix.setVisible(numeric)
        self.stage_table.setVisible(not numeric)
        self.operation_select.setVisible(numeric)
        if numeric:
            return
        if stage == "input":
            rows = [["实际聊天模板", self.template or "尚未分词"], ["用户输入", next((m["content"] for m in reversed(self.messages) if m["role"] == "user"), "尚无输入")]]
        elif stage == "tokens":
            rows = [[f"位置 T{entry['position']}", f"Token编号 {entry['id']} / " + entry.get("text", "显示文本未保存")] for entry in self.tokens[:64]] or [["分词结果", "尚未分词"]]
            selected = next((e for e in self.tokens if e["position"] == self.selection.token_position), None)
            if selected:
                rows.insert(0, ["当前绝对位置", f"T{selected['position']} / Token编号 {selected['id']}"])
        elif stage == "sample":
            chosen = self.snapshot.get("chosen_token_id")
            rows = [["选中Token编号", str(chosen) if chosen is not None else "尚未采样"], ["分布", "右侧为真实全词表分布的前十项"]]
        else:
            rows = [["实际解码文本", self.messages[-1]["content"] if self.messages and self.messages[-1]["role"] == "assistant" else "尚未生成"]]
        self.fill_table(self.stage_table, rows)
        self.stage_table.setHorizontalHeaderLabels(["字段", "实际数据"])
        self.stage_table.resizeRowsToContents()

    def choose_layer(self, index):
        """用户选层会固定查看，执行继续。"""
        if index >= 0:
            self.refresh_layer_controls(index)
            self.follow.setChecked(False)
            self.select(layer_index=index)
            self.layer_list.blockSignals(True)
            self.layer_list.setCurrentRow(index)
            self.layer_list.blockSignals(False)

    def follow_changed(self, checked):
        """跟随只同步查看位置，不控制模型执行。"""
        if not checked or not self.execution:
            return
        if self.neuron_views.currentIndex() == 3:
            self.neuron_views.setCurrentIndex(0)
        for key, value in self.execution.items():
            setattr(self.selection, key, value)
        self.refresh_layer_controls(self.selection.layer_index)
        stage = "embed" if self.selection.operation == "embed" else "logits" if self.selection.operation in ("final_norm", "logits") else "layer"
        self.stage_select.blockSignals(True)
        self.stage_select.setCurrentIndex(self.stage_select.findData(stage))
        self.stage_select.blockSignals(False)
        self.show_stage()
        for control, method, value in ((self.layer_list, "setCurrentRow", self.selection.layer_index),
                                       (self.token_select, "setValue", self.selection.token_position),
                                       (self.operation_select, "setCurrentIndex", self.operation_select.findData(self.selection.operation))):
            control.blockSignals(True)
            getattr(control, method)(value)
            control.blockSignals(False)
        self.select()

    def history_changed(self, *_):
        """读取历史分支或前向，执行计数不变。"""
        entry, branch = self.forward_select.currentData(), self.branch_select.currentData()
        if entry and branch:
            self.follow.setChecked(False)
            if self.sender() == self.forward_select:
                branch = entry[0]
                self.branch_select.blockSignals(True)
                self.branch_select.setCurrentIndex(self.branch_select.findData(branch))
                self.branch_select.blockSignals(False)
            elif entry[0] != branch:
                entry = next((x for x in self.forwards if x[0] == branch), entry)
                self.forward_select.blockSignals(True)
                self.forward_select.setCurrentIndex(self.forward_select.findData(entry))
                self.forward_select.blockSignals(False)
            self.select(branch_id=branch, forward_id=entry[1])

    def inspector_changed(self, index):
        """神经元页读取所选层 MLP，后台观测继续。"""
        if index == 3:
            self.follow.setChecked(False)
            self.select(operation="mlp")
            self.operation_select.blockSignals(True)
            self.operation_select.setCurrentIndex(self.operation_select.findData("mlp"))
            self.operation_select.blockSignals(False)
            self.stage_select.blockSignals(True)
            self.stage_select.setCurrentIndex(self.stage_select.findData("layer"))
            self.stage_select.blockSignals(False)
            self.show_stage()

    def edit_changed(self):
        """内部英文标识仅用于通信，用户选项始终为中文。"""
        kind = self.edit_kind.currentData()
        self.edit_dimension.setEnabled(kind in ("gate", "up", "down"))
        self.edit_value.setEnabled(kind != "zero")
        self.select(weight_kind=kind if kind in ("gate", "up", "down") else "down")

    def fill_table(self, widget, values):
        """清空旧数值，避免缺失数据沿用其他事件内容。"""
        widget.blockSignals(True)
        widget.clearContents()
        if values:
            widget.setRowCount(min(64, len(values)))
            widget.setColumnCount(min(32, max(len(row) for row in values)))
            for row, items in enumerate(values[:64]):
                for col, value in enumerate(items[:32]):
                    item = QTableWidgetItem(value if isinstance(value, str) else number(value, 4))
                    item.setTextAlignment(Qt.AlignCenter)
                    widget.setItem(row, col, item)
        widget.blockSignals(False)

    def display_snapshot(self, value):
        """同一次查询同时更新数值、注意力、神经元与候选概率。"""
        self.snapshot = value
        s, context = self.selection, value.get("context") or {}
        c = getattr(self, "display_config", {})
        catalog = operation_catalog(c, s.layer_index) if 0 <= s.layer_index < c.get("num_hidden_layers", 0) else []
        op = next((r for r in catalog + ALL_OPS if r[0] == s.operation), ALL_OPS[1])
        self.formula.setText(op[1] + "\n" + op[3])
        self.shape_label.setText(("真实采集" if value.get("status") == "available" else "未执行或未采集") + (" / " + str(value["shape"]) if value.get("shape") else ""))
        branch_text = "原始分支" if s.branch_id == "base" else "重算分支 " + s.branch_id[-8:]
        self.identity.setText(f"{branch_text} / 前向 {s.forward_id[1:]} / L{s.layer_index + 1} T{s.token_position}\n版本 {context.get('weights_revision', '未采集')}")
        self.fill_table(self.matrix, value.get("matrix"))
        if value.get("row_positions") and self.matrix.rowCount() == len(value["row_positions"]):
            self.matrix.setVerticalHeaderLabels([f"T{p}" for p in value["row_positions"]])
        previous = self.tensor_select.currentData()
        self.tensor_select.blockSignals(True)
        self.tensor_select.clear()
        names = {"input": "输入", "output": "输出", "weights": "注意力权重", "recurrent_state": "线性注意力递归状态", "key": "旋转后的键", "baseline": "干预前激活", "weight_row": "下投影第零行", "ids": "Token编号"}
        for key, ref in value.get("refs", {}).items():
            self.tensor_select.addItem(names.get(key, "附加张量") + (" / 已保存" if ref else " / 未采集"), key)
        self.tensor_select.setCurrentIndex(max(0, self.tensor_select.findData(previous)))
        self.tensor_select.blockSignals(False)
        self.tensor_changed()
        attention = value.get("attention")
        self.fill_table(self.attention_table, attention.get("matrix") if attention else None)
        if attention and attention.get("type") == "linear":
            self.attention_note.setText(f"线性注意力递归状态 / 形状 {attention['shape']}\n值头 {attention['value_head']} / 查询键头 {attention['key_head']}；行列为键和值通道。该层无 Softmax 权重矩阵。状态对应本次前向末位置。")
        else:
            self.attention_note.setText("真实全注意力 / 形状 " + str(attention["shape"]) + f"\n共享 KV 头 {attention['kv_head']}，行是查询绝对位置，列是键位置" if attention else "该位置注意力未执行或未采集")
        if attention:
            self.attention_table.setVerticalHeaderLabels([str(p) for p in attention["query_positions"]])
        neuron = value.get("neuron")
        self.neuron_context.setText(f"L{s.layer_index + 1} N{s.neuron_index} · T{s.token_position}\n" + ("真实 MLP 门控后激活" if neuron else "未执行或未采集"))
        self.neuron_grid.blockSignals(True)
        self.neuron_top.blockSignals(True)
        self.neuron_grid.clearContents()
        self.neuron_top.clearContents()
        if neuron:
            maximum = neuron.get("maximum") or 1
            for offset, activation in enumerate(neuron["grid"]):
                item = QTableWidgetItem(f"N{neuron['start'] + offset}\n" + number(activation, 3))
                item.setData(Qt.UserRole, neuron["start"] + offset)
                item.setTextAlignment(Qt.AlignCenter)
                if activation is not None:
                    color = QColor("#2258b5" if activation >= 0 else "#bb6715")
                    color.setAlphaF(min(.6, .08 + .52 * abs(activation) / maximum))
                    item.setBackground(color)
                self.neuron_grid.setItem(offset // 8, offset % 8, item)
            for row, entry in enumerate(neuron["rank"]):
                for col, text in enumerate((str(row + 1), str(entry["index"]), number(entry["value"]))):
                    self.neuron_top.setItem(row, col, QTableWidgetItem(text))
            self.neuron_summary.setText(f"全体通道：正 {neuron['positive']} / 负 {neuron['negative']} / 零 {neuron['zero']}\n非数 {neuron['nan']} / 无穷 {neuron['inf']} / 色标最大绝对值 {maximum:.5f}")
            self.neuron_calculation.setText("门控投影 g = " + number(neuron.get("gate")) + "\nSiLU(g) = " + number(neuron.get("silu")) + "\n上投影 u = " + number(neuron.get("up")) + "\n原始乘积 = " + number(neuron.get("baseline")) + "\n干预后激活 m = " + number(neuron.get("value")))
            self.neuron_contribution.setText("对输出维度 0 的贡献：Wdown[0,j] × m[j] = " + number(neuron.get("contribution")) + "\n这是实际乘积，不是最终Token概率。")
        else:
            self.neuron_summary.setText("未执行或未采集")
            self.neuron_calculation.setText("未执行或未采集")
            self.neuron_contribution.setText("未执行或未采集")
        self.neuron_grid.blockSignals(False)
        self.neuron_top.blockSignals(False)
        weight = value.get("weight")
        self.edit_comparison.setText("原始权重：" + number(weight["original"], 7) + "\n当前权重：" + number(weight["value"], 7) + "\n版本 " + weight["weights_revision"] if self.edit_kind.currentData() in ("gate", "up", "down") and weight else "原始激活：" + number(neuron.get("baseline") if neuron else None) + "\n当前激活：" + number(neuron.get("value") if neuron else None))
        if value.get("lens"):
            self.show_probabilities(value["lens"]["candidates"], True)
        elif value.get("probabilities"):
            self.show_probabilities(value["probabilities"])
        else:
            self.probability_table.clearContents()
            self.probability_note.setText("该前向尚无已保存分布")
        self.show_stage()

    def show_probabilities(self, candidates, derived=False):
        """实际采样分布与派生投影分开标记，原始分数独立显示。"""
        self.fill_table(self.probability_table, [[c["text"] or "特殊Token " + str(c["id"]), f"{c['probability'] * 100:.3f}%", number(c.get("raw_logit"), 3)] for c in candidates])
        self.probability_table.setHorizontalHeaderLabels(["Token", "派生概率" if derived else "采样概率", "原始分数"])
        self.probability_note.setText("派生数据 / 额外执行中间层投影" if derived else "全词表实际采样分布前十项 / 原始分数未经采样处理")

    def tensor_changed(self, *_):
        """输入和输出默认提供切片，其他张量按引用请求。"""
        key = self.tensor_select.currentData()
        self.fill_table(self.tensor_matrix, self.snapshot.get("input_matrix" if key == "input" else "matrix") if key in ("input", "output") else None)

    def read_slice(self):
        """解析各轴范围并交给运行器执行有界读取。"""
        ref = self.snapshot.get("refs", {}).get(self.tensor_select.currentData())
        if not ref:
            self.show_error("所选张量未采集")
            return
        try:
            ranges = []
            for text in self.slice_ranges.text().split(","):
                parts = [int(v) for v in text.strip().split(":")]
                if len(parts) not in (2, 3):
                    raise ValueError()
                ranges.append(dict(start=parts[0], stop=parts[1], step=parts[2] if len(parts) == 3 else 1))
            self.worker.send("tensor.slice", {"tensor_ref": ref, "ranges": ranges, "max_elements": 4096})
        except ValueError:
            self.show_error("请输入各轴范围，例如 0:1,0:4,0:8")

    def scalar_selected(self, row, col):
        """查看快照实际标量，显示完整精度。"""
        values = self.snapshot.get("matrix")
        if values and row < len(values) and col < len(values[row]):
            self.scalar_label.setText(f"真实切片元素 [{row},{col}] = " + number(values[row][col], 8))
            if self.selection.operation in ("q", "k", "v", "o", "gate", "up", "down", "linear_qkv", "linear_z", "linear_b", "linear_a", "linear_out") and self.loaded:
                self.worker.send("scalar.query", asdict(self.selection) | {"row": row + self.snapshot.get("matrix_row_start", 0), "column": col})

    def neuron_cell(self, row, col, *_):
        """最后不足 32 个通道时空格不可选。"""
        item = self.neuron_grid.item(row, col)
        if item and item.data(Qt.UserRole) is not None:
            self.neuron_id.setValue(item.data(Qt.UserRole))

    def neuron_rank(self, row, col, *_):
        """排名使用全体通道排序结果。"""
        ranks = self.snapshot.get("neuron", {}).get("rank", [])
        if 0 <= row < len(ranks):
            self.neuron_id.setValue(ranks[row]["index"])

    def strongest(self):
        """定位绝对激活最强的实际通道。"""
        ranks = self.snapshot.get("neuron", {}).get("rank", [])
        if ranks:
            self.neuron_id.setValue(ranks[0]["index"])

    def edit_payload(self):
        """提交固定目标和期望版本，避免旧界面覆盖新权重。"""
        return dict(layer_index=self.selection.layer_index, neuron_index=self.selection.neuron_index, token_position=self.selection.token_position,
                    dimension=self.edit_dimension.value(), kind=self.edit_kind.currentData(), value=self.edit_value.value(), expected_weights_revision=self.weights_revision)

    def apply_edit(self):
        """真实编辑串行应用，历史快照不可变。"""
        self.follow.setChecked(False)
        self.worker.send("patch.apply", self.edit_payload())
        self.status.setText("修改已提交，等待运行器确认")

    def restore_edit(self):
        """恢复当前层通道权重及当前 Token 激活。"""
        self.worker.send("patch.restore", self.edit_payload())

    def open_trace(self):
        """读取离线目录，不要求模型权重存在。"""
        if not self.active:
            path = QFileDialog.getExistingDirectory(self, "打开包含 manifest.json 的记录目录", str(self.data_root / "traces"))
            if path:
                self.worker.send("trace.open", {"path": path})

    def export_trace(self):
        """新目录导出完整记录，禁止覆盖旧目录。"""
        if not self.active and self.trace_path:
            path, _ = QFileDialog.getSaveFileName(self, "指定新的记录目录名", str(Path.home() / "Desktop/模型运行记录"))
            if path:
                self.worker.send("trace.export", {"destination": path})

    def export_model(self):
        """导出真实参数变更，原模型保持原样。"""
        if not self.active and self.loaded:
            path, _ = QFileDialog.getSaveFileName(self, "指定新的模型目录名", str(Path(self.model_path).with_name(Path(self.model_path).name + "-修改版")))
            if path:
                self.worker.send("model.export", {"destination": path})

    def show_error(self, text):
        """中文状态提示和完整工具提示，日志留在用户目录。"""
        self.status.setText(text)
        self.status.setToolTip(text)
        self.run_state.setText("操作失败，请查看底部提示")

    def worker_stopped(self, code):
        """模型崩溃时停止生成状态，独立 GPU 监控继续。"""
        if not self.closing:
            self.loaded = False
            self.set_active(False)
            self.query_id = None
            self.allocator.setText("运行器断开 / 分配器指标不可用")
            self.show_error(f"运行器已退出（代码 {code}），日志位于 {self.data_root}")

    def toggle_fullscreen(self):
        """进入全屏时记住此前窗口状态。"""
        if self.isFullScreen():
            self.exit_fullscreen()
        else:
            self.was_maximized = self.isMaximized()
            self.showFullScreen()
            self.titlebar.full_button.setText("退出全屏 F11")

    def exit_fullscreen(self):
        """退出后恢复最大化或普通窗口。"""
        if self.isFullScreen():
            self.showMaximized() if self.was_maximized else self.showNormal()
            self.titlebar.full_button.setText("全屏 F11")

    def toggle_maximized(self):
        """最大化和还原使用 Qt 原生行为。"""
        self.showNormal() if self.isMaximized() else self.showMaximized()

    def nativeEvent(self, event_type, message):
        """无边框保留 Windows 边缘缩放，按物理像素命中。"""
        if sys.platform == "win32" and not self.isMaximized() and not self.isFullScreen():
            from ctypes import wintypes
            msg = wintypes.MSG.from_address(int(message))
            if msg.message == 0x84:
                rect = wintypes.RECT()
                ctypes.windll.user32.GetWindowRect(int(self.winId()), ctypes.byref(rect))
                x, y = ctypes.c_short(msg.lParam & 0xffff).value, ctypes.c_short((msg.lParam >> 16) & 0xffff).value
                border = max(6, round(6 * self.devicePixelRatioF()))
                left, right, top, bottom = x < rect.left + border, x >= rect.right - border, y < rect.top + border, y >= rect.bottom - border
                hit = 13 if top and left else 14 if top and right else 16 if bottom and left else 17 if bottom and right else 10 if left else 11 if right else 12 if top else 15 if bottom else 0
                if hit:
                    return True, hit
        return super().nativeEvent(event_type, message)

    def closeEvent(self, event):
        """先取消并收尾，只回收本程序拥有的子进程。"""
        self.closing = True
        self.timer.stop()
        self.worker.stop()
        self.monitor.stop()
        event.accept()
