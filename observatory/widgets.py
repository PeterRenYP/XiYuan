"""原生窗口标题栏、层结构投影与统一浅色主题。"""
from __future__ import annotations

import math

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget

ACCENT = "#1e4fa6"


def label(text="", role="", size=13):
    """创建中文、纯文本标签，禁止把用户输入当作富文本。"""
    result = QLabel(text)
    result.setTextFormat(Qt.PlainText)
    result.setProperty("role", role)
    result.setWordWrap(True)
    font = result.font()
    font.setPixelSize(size)
    result.setFont(font)
    result.setStyleSheet(f"font-size:{size}px;")
    return result


def button(text, callback=None, primary=False):
    """创建键盘可达的原生操作按钮。"""
    result = QPushButton(text)
    result.setMinimumHeight(33)
    if primary:
        result.setProperty("role", "primary")
    if callback:
        result.clicked.connect(callback)
    return result


def box(parent, horizontal=False, margins=10, spacing=8):
    """统一布局留白和间距。"""
    result = QHBoxLayout(parent) if horizontal else QVBoxLayout(parent)
    result.setContentsMargins(margins, margins, margins, margins)
    result.setSpacing(spacing)
    return result


def panel(title=""):
    """创建带可选标题的白色功能面板。"""
    widget = QFrame()
    widget.setObjectName("panel")
    layout = box(widget)
    if title:
        layout.addWidget(label(title, "heading", 14))
    return widget, layout


class TitleBar(QFrame):
    """保留系统移动与双击最大化行为的无边框标题栏。"""

    def __init__(self, window):
        super().__init__()
        self.window = window
        self.setObjectName("titlebar")
        self.setFixedHeight(44)
        row = box(self, True, 8)
        row.addWidget(label("析元", "heading", 16))
        subtitle = label("本地模型 · 真实推理与观测", "muted")
        subtitle.setWordWrap(False)
        subtitle.setMinimumWidth(220)
        row.addWidget(subtitle)
        row.addStretch()
        self.full_button = button("全屏 F11", window.toggle_fullscreen)
        row.addWidget(self.full_button)
        for text, tip, action in (("—", "最小化", window.showMinimized), ("□", "最大化或还原", window.toggle_maximized), ("×", "关闭", window.close)):
            control = button(text, action)
            control.setFixedWidth(38)
            control.setToolTip(tip)
            control.setAccessibleName(tip)
            row.addWidget(control)

    def mousePressEvent(self, event):
        """通过系统窗口管理器移动窗口，正确处理多屏 DPI。"""
        if event.button() == Qt.LeftButton and not self.window.isFullScreen() and self.window.windowHandle():
            self.window.windowHandle().startSystemMove()

    def mouseDoubleClickEvent(self, event):
        """标题栏双击切换最大化。"""
        if event.button() == Qt.LeftButton:
            self.window.toggle_maximized()


class LayerScene(QWidget):
    """动态层栈只展示结构；点击仅改变查看位置。"""
    selected = Signal(int)

    def __init__(self):
        super().__init__()
        self.count, self.layer = 0, None
        self.yaw, self.pitch = -.6, -.25
        self.points, self.drag = [], None
        self.setMinimumSize(210, 220)
        self.setFocusPolicy(Qt.StrongFocus)
        self.setAccessibleName("模型层结构，方向键选择层；空间距离不表示语义关系")

    def paintEvent(self, event):
        """按实际层数投影结构，不伪装成参数级三维重建。"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.fillRect(self.rect(), QColor("#ffffff"))
        width, height = self.width(), self.height()
        half = min(width * .27, 100)
        gap = min(6, (height - 60) / max(1, self.count))

        def project(x, y, z):
            """把旋转后的三维点变换到当前视口。"""
            x1 = x * math.cos(self.yaw) - z * math.sin(self.yaw)
            z1 = x * math.sin(self.yaw) + z * math.cos(self.yaw)
            y1 = y * math.cos(self.pitch) - z1 * math.sin(self.pitch)
            return QPointF(width * .45 + x1, height * .5 + y1)

        self.points = []
        for index in range(self.count):
            y = (index - (self.count - 1) / 2) * gap
            points = [project(x, y, z) for x, z in ((-half, -half * .4), (half, -half * .4), (half, half * .4), (-half, half * .4))]
            active = index == self.layer
            painter.setPen(QPen(QColor(ACCENT if active else "#b8c5d5"), 1.7 if active else .7))
            painter.setBrush(QColor("#dce8fb" if active else "#ffffff"))
            painter.drawPolygon(QPolygonF(points))
            center = project(0, y, 0)
            self.points.append((index, center))
            if active or index in (0, self.count - 1):
                painter.drawText(points[1] + QPointF(5, 4), f"第 {index + 1} 层")
        painter.setPen(QColor("#45566f"))
        painter.drawText(10, 19, f"{self.count} 层结构参考 / 拖动旋转" if self.count else "等待加载实际模型结构")
        painter.drawText(10, height - 10, "空间位置不代表语义距离")

    def mousePressEvent(self, event):
        """记录旋转起点，不触发计算。"""
        if event.button() == Qt.LeftButton:
            self.drag = event.position(), self.yaw, self.pitch
            self.setFocus()

    def mouseMoveEvent(self, event):
        """拖动只修改视角。"""
        if self.drag:
            delta = event.position() - self.drag[0]
            self.yaw = self.drag[1] + delta.x() * .008
            self.pitch = max(-.7, min(.7, self.drag[2] + delta.y() * .008))
            self.update()

    def mouseReleaseEvent(self, event):
        """短距离点击选择最近层。"""
        if self.drag and self.points and (event.position() - self.drag[0]).manhattanLength() < 6:
            index, _ = min(self.points, key=lambda item: (item[1] - event.position()).manhattanLength())
            self.selected.emit(index)
        self.drag = None

    def keyPressEvent(self, event):
        """方向键选择层或旋转。"""
        if event.key() in (Qt.Key_Up, Qt.Key_Down):
            if not self.count:
                return
            self.selected.emit(max(0, min(self.count - 1, (self.layer or 0) + (1 if event.key() == Qt.Key_Down else -1))))
        elif event.key() in (Qt.Key_Left, Qt.Key_Right):
            self.yaw += .1 if event.key() == Qt.Key_Right else -.1
            self.update()
        else:
            super().keyPressEvent(event)


STYLE = """
QWidget { background:#e8edf3; color:#142033; font-family:'Microsoft YaHei UI','Segoe UI'; font-size:13px; }
QFrame#panel { background:white; border:1px solid #b8c5d5; border-radius:5px; }
QFrame#titlebar { background:white; border-bottom:1px solid #b8c5d5; }
QLabel { background:transparent; border:none; }
QLabel[role=heading] { font-weight:600; }
QLabel[role=muted] { color:#45566f; }
QLabel[role=accent] { color:#1e4fa6; }
QPushButton { background:#f1f5fa; border:1px solid #b8c5d5; border-radius:4px; padding:5px 9px; }
QPushButton:hover, QPushButton:checked { background:#dce8fb; border-color:#1e4fa6; color:#1e4fa6; }
QPushButton[role=primary] { background:#2258b5; border-color:#1e4fa6; color:white; }
QPushButton:disabled { color:#8792a3; background:#edf0f4; }
QLineEdit,QTextEdit,QTextBrowser,QComboBox,QSpinBox,QDoubleSpinBox { background:white; border:1px solid #9aaec6; border-radius:3px; padding:5px; }
QListWidget,QTableWidget { background:white; border:1px solid #b8c5d5; gridline-color:#d1dbe7; selection-background-color:#dce8fb; selection-color:#142033; }
QListWidget::item { min-height:25px; }
QHeaderView::section { background:#f1f5fa; color:#45566f; padding:4px; border:none; }
QTabWidget::pane { background:white; border:none; }
QTabBar::tab { background:white; color:#45566f; padding:7px 10px; border-bottom:2px solid #b8c5d5; }
QTabBar::tab:selected { color:#1e4fa6; border-bottom-color:#1e4fa6; }
QSplitter::handle { background:#d4deeb; }
QSplitter::handle:hover { background:#9aaec6; }
QScrollArea { background:white; border:none; }
QPushButton:focus,QLineEdit:focus,QComboBox:focus,QSpinBox:focus,QTableWidget:focus { border:1px solid #1e4fa6; }
QToolTip { background:white; color:#142033; border:1px solid #63758d; padding:5px; }
"""
