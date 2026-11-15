#include "visuals.h"
#include "uiscale.h"
#include <QGraphicsItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolTip>
#include <QVariant>
#include <QWheelEvent>
#include <cmath>

using namespace Qt::StringLiterals;

QString operationName(const QString &key) {
    // 底层操作标识在界面统一转换为中文。
    static const QMap<QString, QString> names{{u"embed"_s, u"Token嵌入"_s},
                                              {u"norm"_s, u"输入归一化"_s},
                                              {u"q"_s, u"查询投影"_s},
                                              {u"k"_s, u"键投影"_s},
                                              {u"v"_s, u"值投影"_s},
                                              {u"q_norm"_s, u"查询头归一化"_s},
                                              {u"k_norm"_s, u"键头归一化"_s},
                                              {u"rope"_s, u"旋转位置编码"_s},
                                              {u"attention"_s, u"因果注意力"_s},
                                              {u"attention_gate"_s, u"注意力输出门控"_s},
                                              {u"o"_s, u"注意力输出投影"_s},
                                              {u"attention_residual"_s, u"注意力残差"_s},
                                              {u"norm2"_s, u"前馈归一化"_s},
                                              {u"gate"_s, u"门控投影"_s},
                                              {u"up"_s, u"上投影"_s},
                                              {u"silu"_s, u"门控激活"_s},
                                              {u"mlp"_s, u"门控乘积"_s},
                                              {u"down"_s, u"下投影"_s},
                                              {u"residual"_s, u"前馈残差"_s},
                                              {u"final_norm"_s, u"最终归一化"_s},
                                              {u"logits"_s, u"词表输出"_s},
                                              {u"sample"_s, u"采样与解码"_s},
                                              {u"linear_qkv"_s, u"查询键值联合投影"_s},
                                              {u"linear_z"_s, u"输出门投影"_s},
                                              {u"linear_b"_s, u"更新率投影"_s},
                                              {u"linear_a"_s, u"衰减投影"_s},
                                              {u"linear_conv"_s, u"因果卷积与激活"_s},
                                              {u"linear_beta"_s, u"更新率"_s},
                                              {u"linear_decay"_s, u"状态衰减"_s},
                                              {u"linear_delta"_s, u"门控增量递归"_s},
                                              {u"linear_norm"_s, u"归一化与输出门控"_s},
                                              {u"linear_out"_s, u"线性注意力输出"_s}};
    return names.value(key, u"未选择运算"_s);
}

QString operationFormula(const QString &key) {
    // 公式与真实运行器的操作目录相对应。
    static const QMap<QString, QString> formulas{
        {u"embed"_s, u"X = E[Token编号]"_s},
        {u"norm"_s, u"Xn = RMSNorm(H)"_s},
        {u"q"_s, u"Q = Xn Wqᵀ + bq"_s},
        {u"k"_s, u"K = Xn Wkᵀ + bk"_s},
        {u"v"_s, u"V = Xn Wvᵀ + bv"_s},
        {u"rope"_s, u"Q′, K′ = RoPE(Q, K)"_s},
        {u"attention"_s, u"A = Softmax(Q′K′ᵀ / √d + 掩码)\nO = A V"_s},
        {u"o"_s, u"Y = concat(O) Woᵀ"_s},
        {u"attention_residual"_s, u"Hmid = H + Y"_s},
        {u"norm2"_s, u"Xf = RMSNorm(Hmid)"_s},
        {u"gate"_s, u"g = Xf Wgateᵀ"_s},
        {u"up"_s, u"u = Xf Wupᵀ"_s},
        {u"silu"_s, u"s = SiLU(g)"_s},
        {u"mlp"_s, u"m = SiLU(g) ⊙ u"_s},
        {u"down"_s, u"M = m Wdownᵀ"_s},
        {u"residual"_s, u"Hout = Hmid + M"_s},
        {u"final_norm"_s, u"Y = RMSNorm(Hlast)"_s},
        {u"logits"_s, u"z = Y Wvocabᵀ"_s},
        {u"linear_qkv"_s, u"QKV = X Wqkvᵀ"_s},
        {u"linear_conv"_s, u"QKV′ = SiLU(CausalConv(QKV))"_s},
        {u"linear_decay"_s, u"g = −exp(Alog) · Softplus(a + dt)"_s},
        {u"linear_delta"_s, u"St = decay(St−1) + Δt\nOt = qtᵀ St"_s},
        {u"linear_norm"_s, u"Z = RMSNorm(O) ⊙ SiLU(z)"_s},
        {u"linear_out"_s, u"Y = Z Woutᵀ"_s}};
    return formulas.value(key, u"选择运算查看真实输入与输出"_s);
}

QString valueText(const QJsonValue &value, int precision) {
    // 缺失与非有限值不能伪装成零。
    return value.isDouble() && std::isfinite(value.toDouble())
               ? QString::number(value.toDouble(), 'f', precision)
               : u"未采集"_s;
}

QColor heatColor(double value, double maximum, bool positiveOnly) {
    // 有符号激活使用蓝橙配色，并以数值和图例补充颜色含义。
    const double ratio = std::clamp(std::abs(value) / std::max(maximum, 1e-12), 0.0, 1.0);
    const QColor base(u"#202b35"_s), target(value < 0 && !positiveOnly ? u"#d49b69"_s : u"#9acbdd"_s);
    return QColor::fromRgbF(base.redF() + (target.redF() - base.redF()) * ratio,
                            base.greenF() + (target.greenF() - base.greenF()) * ratio,
                            base.blueF() + (target.blueF() - base.blueF()) * ratio);
}

namespace {
class Node final : public QGraphicsItem {
  public:
    // 节点只保存真实采集预览；未运行时不绘制演示数值。
    Node(const QString &key, const QString &title, const QRectF &rect, bool compact = false)
        : key(key), title(title), rect(rect), compact(compact) {
        setData(0, key);
        setCursor(Qt::PointingHandCursor);
    }
    QRectF boundingRect() const override {
        return rect.adjusted(-4, -4, 4, 4);
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(current    ? QColor(u"#92cedd"_s)
                       : selected ? QColor(u"#c0d5e7"_s)
                                  : QColor(u"#46505e"_s),
                       selected ? 1.8 : 1));
        p->setBrush(current ? QColor(u"#304550"_s) : QColor(u"#282d34"_s));
        p->drawRoundedRect(rect, 4, 4);
        p->setPen(QColor(u"#a1adbc"_s));
        QFont font(u"Microsoft YaHei UI"_s, compact ? 8 : 8);
        p->setFont(font);
        if (!compact && rect.width() >= 65)
            p->drawText(rect.adjusted(10, 7, -8, -7), Qt::AlignTop,
                        key == u"mlp"_s ? u"神经元编辑边界"_s
                        : current       ? u"运算 / 执行位置"_s
                                        : u"运算节点"_s);
        font.setPointSize(compact ? 10 : 11);
        p->setFont(font);
        p->setPen(QColor(u"#d0d8e5"_s));
        const bool residual = rect.width() < 65;
        if (residual) {
            font.setPointSize(22);
            p->setFont(font);
            p->drawText(rect, Qt::AlignCenter, u"+"_s);
            return;
        }
        p->drawText(rect.adjusted(10, compact ? 7 : 27, -8, -2), Qt::AlignTop, title);
        font.setFamily(u"Consolas"_s);
        font.setPointSize(8);
        p->setFont(font);
        p->setPen(QColor(u"#a6b7ca"_s));
        const QString formula = operationFormula(key).section(QChar(u'\n'), 0, 0);
        if (!compact)
            p->drawText(rect.adjusted(10, 48, -8, -2), Qt::AlignTop,
                        p->fontMetrics().elidedText(formula, Qt::ElideRight, int(rect.width() - 20)));
        if (!preview.isEmpty() && rect.height() >= 100) {
            double maximum = 0;
            for (auto row : preview)
                for (auto value : row.toArray())
                    maximum = std::max(maximum, std::abs(value.toDouble()));
            int rows = preview.size(), cols = preview.first().toArray().size();
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c) {
                    const auto value = preview[r].toArray()[c];
                    p->fillRect(QRectF(rect.x() + 10 + c * (rect.width() - 20) / cols, rect.y() + 69 + r * 11,
                                       (rect.width() - 20) / cols - 1, 9),
                                value.isDouble() ? heatColor(value.toDouble(), maximum)
                                                 : QColor(u"#333943"_s));
                }
        }
        if (!compact) {
            QStringList parts;
            for (auto size : shape)
                parts << QString::number(size.toInt());
            p->setPen(QColor(u"#93a4ba"_s));
            p->drawText(rect.adjusted(10, 0, -8, -7), Qt::AlignBottom,
                        parts.isEmpty() ? u"尚未采集"_s : parts.join(u" × "_s));
        }
        if (done) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(u"#a9c5c4"_s));
            p->drawEllipse(rect.topRight() + QPointF(-9, 9), 2, 2);
        }
    }
    QString key, title;
    QRectF rect;
    QJsonArray preview, shape;
    bool compact = false, selected = false, current = false, done = false;
};
} // namespace

GraphView::GraphView(QWidget *parent) : QGraphicsView(parent) {
    // 原生图形场景支持键鼠平移缩放，不嵌入浏览器。
    setScene(&scene);
    setFrameShape(QFrame::NoFrame);
    setRenderHint(QPainter::Antialiasing);
    setTransformationAnchor(AnchorUnderMouse);
    setDragMode(ScrollHandDrag);
    setBackgroundBrush(QColor(u"#1d1f23"_s));
    setSceneRect(-20, -20, 1130, 560);
    UiScale::minimumWidth(this, 250);
    UiScale::minimumHeight(this, 180);
    setFocusPolicy(Qt::StrongFocus);
}

QString GraphView::visualKey(const QString &key) const {
    // 原型把头归一化与输出门控合并显示，但检查器仍可选择实际子运算。
    if (key == u"q_norm"_s || key == u"k_norm"_s)
        return u"rope"_s;
    if (key == u"attention_gate"_s)
        return u"o"_s;
    if (key == u"linear_a"_s || key == u"linear_b"_s || key == u"linear_beta"_s)
        return u"linear_decay"_s;
    if (key == u"linear_z"_s)
        return u"linear_qkv"_s;
    return key;
}

void GraphView::setLayer(bool isLinear, bool isHybrid, int index) {
    // 坐标沿用已确认原型，查询键值分支、残差旁路与前馈拓扑分别绘制。
    if (layer == index && linear == isLinear && hybrid == isHybrid)
        return;
    layer = index;
    linear = isLinear;
    hybrid = isHybrid;
    scene.clear();
    nodes.clear();
    edges.clear();
    auto node = [this](const QString &key, const QString &title, double x, double y, double w, double h,
                       bool compact = false) {
        auto *item = new Node(key, title, QRectF(0, 0, w, h), compact);
        item->setPos(x, y);
        scene.addItem(item);
        nodes[key] = item;
    };
    auto edge = [this](const QString &key, std::initializer_list<QPointF> points, bool dashed = false) {
        QPainterPath path;
        bool first = true;
        for (auto point : points) {
            if (first) {
                path.moveTo(point);
                first = false;
            } else
                path.lineTo(point);
        }
        auto *item = scene.addPath(path, QPen(QColor(dashed ? u"#4a5361"_s : u"#7d8b9a"_s), 1,
                                              dashed ? Qt::DashLine : Qt::SolidLine));
        item->setZValue(-1);
        if (!dashed) {
            auto last = *(points.end() - 1);
            QPolygonF arrow;
            arrow << last << last + QPointF(-5, -3) << last + QPointF(-5, 3);
            scene.addPolygon(arrow, Qt::NoPen, QBrush(QColor(u"#9babba"_s)));
        }
        edges[key] = item;
    };
    auto text = [this](const QString &value, double x, double y) {
        auto *item = scene.addSimpleText(value, QFont(u"Microsoft YaHei UI"_s, 9));
        item->setBrush(QColor(u"#96a8bb"_s));
        item->setPos(x, y);
    };
    text(linear ? u"线性注意力 / 递归状态更新"_s : u"全注意力 / 查询键值聚合"_s, 30, 12);
    text(u"前馈网络 / 神经元通道"_s, 30, 295);
    text(u"输入残差旁路"_s, 865, 38);
    text(u"前馈残差 / 进入下一层"_s, 850, 485);
    node(u"norm"_s, u"输入归一化"_s, 30, 140, 128, 96);
    if (linear) {
        node(u"linear_qkv"_s, u"查询键值投影"_s, 211, 140, 148, 96);
        node(u"linear_conv"_s, u"因果卷积"_s, 402, 140, 138, 96);
        node(u"linear_decay"_s, u"更新率与衰减"_s, 402, 46, 160, 56, true);
        node(u"linear_delta"_s, u"门控增量递归"_s, 592, 125, 176, 130);
        node(u"linear_norm"_s, u"归一化与门控"_s, 805, 140, 138, 96);
        node(u"linear_out"_s, u"输出投影"_s, 974, 140, 104, 96);
        edge(u"norm"_s, {{158, 182}, {211, 182}});
        edge(u"linear_qkv"_s, {{359, 182}, {402, 182}});
        edge(u"linear_conv"_s, {{540, 182}, {592, 182}});
        edge(u"linear_decay"_s, {{562, 74}, {680, 74}, {680, 125}});
        edge(u"linear_delta"_s, {{768, 182}, {805, 182}});
        edge(u"linear_norm"_s, {{943, 182}, {974, 182}});
        edge(u"linear_out"_s, {{1078, 182}, {1090, 182}, {1090, 258}, {1054, 258}});
    } else {
        auto *group =
            scene.addRect(201, 65, 168, 224, QPen(QColor(u"#414b59"_s), 1), QBrush(QColor(u"#23272d"_s)));
        group->setZValue(-2);
        text(u"独立投影分支"_s, 211, 70);
        node(u"q"_s, u"查询 Q"_s, 211, 96, 148, 52, true);
        node(u"k"_s, u"键 K"_s, 211, 157, 148, 52, true);
        node(u"v"_s, u"值 V"_s, 211, 218, 148, 52, true);
        node(u"rope"_s, hybrid ? u"头归一化 / RoPE"_s : u"旋转位置编码"_s, 402, 116, 138, 96);
        node(u"attention"_s, u"因果注意力"_s, 592, 125, 176, 130);
        node(u"o"_s, hybrid ? u"输出门控与投影"_s : u"注意力输出投影"_s, 805, 140, 138, 96);
        edge(u"norm"_s, {{158, 182}, {181, 182}, {181, 122}, {211, 122}});
        edge(u"norm-k"_s, {{181, 182}, {211, 182}});
        edge(u"norm-v"_s, {{181, 182}, {181, 244}, {211, 244}});
        edge(u"q"_s, {{359, 122}, {381, 122}, {381, 148}, {402, 148}});
        edge(u"k"_s, {{359, 182}, {402, 182}});
        edge(u"rope"_s, {{540, 164}, {566, 164}, {566, 174}, {592, 174}});
        edge(u"v"_s, {{359, 244}, {566, 244}, {566, 215}, {592, 215}});
        edge(u"attention"_s, {{768, 182}, {805, 182}});
        edge(u"o"_s, {{943, 182}, {974, 182}, {974, 258}, {1010, 258}});
    }
    node(u"attention_residual"_s, u"+"_s, 1010, 236, 44, 44);
    node(u"norm2"_s, u"前馈归一化"_s, 30, 350, 128, 96);
    auto *mlpGroup =
        scene.addRect(201, 319, 168, 148, QPen(QColor(u"#414b59"_s)), QBrush(QColor(u"#23272d"_s)));
    mlpGroup->setZValue(-2);
    text(u"前馈投影分支"_s, 211, 322);
    node(u"gate"_s, u"门控投影"_s, 211, 348, 148, 48, true);
    node(u"up"_s, u"上投影"_s, 211, 408, 148, 48, true);
    node(u"silu"_s, u"门控激活"_s, 402, 328, 138, 96);
    node(u"mlp"_s, u"门控乘积"_s, 592, 337, 176, 130);
    node(u"down"_s, u"下投影"_s, 805, 350, 138, 96);
    node(u"residual"_s, u"+"_s, 1010, 376, 44, 44);
    edge(u"attention_residual"_s, {{1032, 280}, {1032, 306}, {12, 306}, {12, 398}, {30, 398}});
    edge(u"norm2"_s, {{158, 398}, {184, 398}, {184, 372}, {211, 372}});
    edge(u"norm2-up"_s, {{184, 398}, {184, 432}, {211, 432}});
    edge(u"gate"_s, {{359, 372}, {402, 372}});
    edge(u"silu"_s, {{540, 372}, {566, 372}, {566, 390}, {592, 390}});
    edge(u"up"_s, {{359, 432}, {566, 432}, {566, 420}, {592, 420}});
    edge(u"mlp"_s, {{768, 398}, {805, 398}});
    edge(u"down"_s, {{943, 398}, {1010, 398}});
    edge(u"skip1"_s, {{12, 182}, {12, 51}, {1032, 51}, {1032, 236}}, true);
    edge(u"skip2"_s, {{12, 398}, {12, 480}, {1032, 480}, {1032, 420}}, true);
}

void GraphView::selectOperation(const QString &operation, bool center) {
    // 选中查看与当前执行状态分别着色，浏览历史不会推动模型。
    const auto key = visualKey(operation);
    for (auto *item : nodes) {
        auto *node = static_cast<Node *>(item);
        node->selected = node->key == key;
        node->update();
    }
    if (center && nodes.contains(key))
        ensureVisible(nodes[key], 70, 50);
}

void GraphView::updateOperation(const QString &operation, const QJsonObject &payload, bool current) {
    // 同一图节点可能聚合多个子运算，仅显示当前真实子运算的预览。
    if (current)
        for (auto *item : nodes) {
            auto *node = static_cast<Node *>(item);
            node->current = false;
            node->update();
        }
    const auto key = visualKey(operation);
    if (!nodes.contains(key))
        return;
    auto *node = static_cast<Node *>(nodes[key]);
    node->done = true;
    node->current = current;
    node->preview = payload.value(u"preview"_s).toArray();
    node->shape = payload.value(u"preview_shape"_s).toArray(payload.value(u"shape"_s).toArray());
    node->update();
    if (edges.contains(key))
        edges[key]->setPen(QPen(QColor(current ? u"#9fd7e5"_s : u"#9aabba"_s), current ? 2 : 1));
}

void GraphView::clearRun() {
    // 前向或分支切换时清除完成标记，防止旧数据冒充当前执行。
    for (auto *item : nodes) {
        auto *node = static_cast<Node *>(item);
        node->preview = {};
        node->shape = {};
        node->done = false;
        node->current = false;
        node->update();
    }
}

void GraphView::fitGraph() {
    // 小窗口保留节点可读尺寸，余下区域允许平移访问。
    resetTransform();
    const double scale = std::clamp(std::min(viewport()->width() / (1120.0 * interfaceScale),
                                             viewport()->height() / (545.0 * interfaceScale)),
                                    0.72, 1.15);
    QGraphicsView::scale(scale * interfaceScale, scale * interfaceScale);
    centerOn(540, 260);
    emit zoomChanged(qRound(scale * 100));
}
void GraphView::changeZoom(double factor) {
    // 限制缩放范围，避免节点消失或极端放大。
    const double next = std::clamp(transform().m11() / interfaceScale * factor, 0.45, 2.5);
    resetTransform();
    scale(next * interfaceScale, next * interfaceScale);
    emit zoomChanged(qRound(next * 100));
}
void GraphView::setInterfaceScale(double factor) {
    // 全局界面缩放独立于画布倍率，并保留当前浏览中心和节点状态。
    const auto center = mapToScene(viewport()->rect().center());
    const double canvas = transform().m11() / interfaceScale;
    interfaceScale = factor;
    resetTransform();
    scale(canvas * factor, canvas * factor);
    centerOn(center);
}
void GraphView::setPan(bool enabled) {
    setDragMode(enabled ? ScrollHandDrag : NoDrag);
}
void GraphView::drawBackground(QPainter *p, const QRectF &rect) {
    // 点阵背景匹配原型，避免高密度网格压过数据。
    p->fillRect(rect, QColor(u"#1d1f23"_s));
    p->setPen(QColor(u"#2e323a"_s));
    for (int x = int(std::floor(rect.left() / 24)) * 24; x < rect.right(); x += 24)
        for (int y = int(std::floor(rect.top() / 24)) * 24; y < rect.bottom(); y += 24)
            p->drawPoint(QPointF(x, y));
}
void GraphView::wheelEvent(QWheelEvent *event) {
    // 滚轮缩放围绕鼠标位置，按住 Shift 则横向浏览。
    if (event->modifiers().testFlag(Qt::ShiftModifier))
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - event->angleDelta().y());
    else
        changeZoom(event->angleDelta().y() > 0 ? 1.12 : 1 / 1.12);
    event->accept();
}
void GraphView::mousePressEvent(QMouseEvent *event) {
    // 点击节点只修改选择；空白处仍使用原生画布拖动。
    if (event->button() == Qt::LeftButton)
        for (auto *item : items(event->pos()))
            if (item->data(0).isValid()) {
                emit selected(item->data(0).toString());
                break;
            }
    QGraphicsView::mousePressEvent(event);
}

double ScaledWidget::interfaceScale() const {
    const auto value = property("uiScale");
    return value.isValid() ? value.toDouble() : 1;
}
int ScaledWidget::logicalWidth() const {
    return qRound(width() / interfaceScale());
}
int ScaledWidget::logicalHeight() const {
    return qRound(height() / interfaceScale());
}
QRect ScaledWidget::logicalRect() const {
    return QRect(0, 0, logicalWidth(), logicalHeight());
}
void ScaledWidget::preparePainter(QPainter &painter) const {
    // 字体在基准坐标中绘制，避免继承已缩放字体后再次放大。
    painter.scale(interfaceScale(), interfaceScale());
    QFont font(u"Microsoft YaHei UI"_s);
    font.setPixelSize(12);
    painter.setFont(font);
}

Heatmap::Heatmap(QWidget *parent) : ScaledWidget(parent) {
    UiScale::minimumHeight(this, 140);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setAccessibleName(u"张量热图，使用方向键选择单元"_s);
}
void Heatmap::setMatrix(const QJsonArray &values, const QString &description) {
    // 切片始终附带实际来源说明。
    matrix = values;
    caption = description;
    selectedRow = selectedColumn = -1;
    update();
}
void Heatmap::paintEvent(QPaintEvent *) {
    // 自适应单元格大小，非有限或缺失单元采用中性色。
    QPainter p(this);
    preparePainter(p);
    p.fillRect(logicalRect(), QColor(u"#1e232a"_s));
    p.setPen(QColor(u"#97a9be"_s));
    if (matrix.isEmpty() || matrix.first().toArray().isEmpty()) {
        p.drawText(logicalRect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                   u"尚未采集\n运行模型后显示真实张量切片"_s);
        return;
    }
    const int rows = matrix.size(), cols = matrix.first().toArray().size();
    double maximum = 0;
    for (auto row : matrix)
        for (auto value : row.toArray())
            if (value.isDouble())
                maximum = std::max(maximum, std::abs(value.toDouble()));
    const QRectF area(5, 5, logicalWidth() - 10, logicalHeight() - 34);
    double cw = area.width() / cols, ch = area.height() / rows;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const auto values = matrix[r].toArray();
            const auto value = c < values.size() ? values[c] : QJsonValue();
            QRectF cell(area.x() + c * cw, area.y() + r * ch, cw - 1, ch - 1);
            p.fillRect(cell, value.isDouble() ? heatColor(value.toDouble(), maximum, positiveOnly)
                                              : QColor(u"#48424a"_s));
            if (r == selectedRow && c == selectedColumn) {
                p.setPen(QPen(QColor(u"#d2edf4"_s), 2));
                p.drawRect(cell.adjusted(1, 1, -1, -1));
            }
        }
    p.setPen(QColor(u"#a8bbd0"_s));
    p.drawText(
        QRect(7, logicalHeight() - 25, logicalWidth() - 14, 22), Qt::AlignVCenter,
        p.fontMetrics().elidedText(caption.isEmpty() ? u"负值：橙色　零值：深色　正值：蓝色"_s : caption,
                                   Qt::ElideRight, logicalWidth() - 14));
}
void Heatmap::choose(int row, int column) {
    // 同时支持键盘与鼠标，数值反馈不依赖颜色。
    if (matrix.isEmpty() || matrix.first().toArray().isEmpty())
        return;
    selectedRow = std::clamp(row, 0, int(matrix.size() - 1));
    selectedColumn = std::clamp(column, 0, int(matrix.first().toArray().size() - 1));
    const auto value = matrix[selectedRow].toArray()[selectedColumn];
    emit cellSelected(selectedRow, selectedColumn, value);
    update();
}
void Heatmap::mousePressEvent(QMouseEvent *event) {
    if (matrix.isEmpty())
        return;
    setFocus();
    choose(int(((event->position().y() / interfaceScale()) - 5) /
               std::max(1.0, (logicalHeight() - 34.0) / matrix.size())),
           int(((event->position().x() / interfaceScale()) - 5) /
               std::max(1.0, (logicalWidth() - 10.0) / matrix.first().toArray().size())));
}
void Heatmap::keyPressEvent(QKeyEvent *event) {
    int r = std::max(0, selectedRow), c = std::max(0, selectedColumn);
    switch (event->key()) {
    case Qt::Key_Left:
        --c;
        break;
    case Qt::Key_Right:
        ++c;
        break;
    case Qt::Key_Up:
        --r;
        break;
    case Qt::Key_Down:
        ++r;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    choose(r, c);
}

LayerStrip::LayerStrip(QWidget *parent) : ScaledWidget(parent) {
    UiScale::fixedHeight(this, 76);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(u"模型层计算路径"_s);
}
void LayerStrip::configure(const QJsonObject &config) {
    count = config.value(u"num_hidden_layers"_s).toInt();
    types = config.value(u"layer_types"_s).toArray();
    selected = 0;
    executed = -1;
    update();
}
void LayerStrip::setPosition(int value, int current) {
    selected = value;
    executed = current;
    update();
}
void LayerStrip::paintEvent(QPaintEvent *) {
    // 路径条同时标记浏览层与运行层，全部尺寸来自当前模型配置。
    QPainter p(this);
    preparePainter(p);
    p.fillRect(logicalRect(), QColor(u"#25282d"_s));
    p.setPen(QColor(u"#afbbcd"_s));
    p.drawText(12, 17, u"模型计算路径"_s);
    p.drawText(QRect(logicalWidth() - 250, 0, 238, 23), Qt::AlignRight | Qt::AlignVCenter,
               u"已执行　当前层　待执行"_s);
    if (!count) {
        p.drawText(12, 48, u"请选择并加载本地模型"_s);
        return;
    }
    const double cw = (logicalWidth() - 160.0) / count;
    p.drawText(12, 45, u"Token嵌入"_s);
    p.drawText(logicalWidth() - 66, 45, u"词表输出"_s);
    for (int i = 0; i < count; ++i) {
        QRectF cell(81 + i * cw, 30, cw - 3, 21);
        p.setPen(QPen(QColor(i == selected ? u"#d0e4ed"_s : u"#525b6c"_s), i == selected ? 2 : 1));
        p.setBrush(QColor(i == executed                                                      ? u"#70b3c6"_s
                          : i < executed                                                     ? u"#4b5363"_s
                          : i < types.size() && types[i].toString() == u"linear_attention"_s ? u"#3e3b4c"_s
                                                                                             : u"#303c47"_s));
        p.drawRect(cell);
        if (i == 0 || i == count - 1 || (i + 1) % 4 == 0) {
            p.setPen(QColor(u"#96a5bd"_s));
            p.drawText(QRectF(cell.x() - 5, 55, cw + 8, 17), Qt::AlignCenter, QString::number(i));
        }
    }
}
void LayerStrip::mousePressEvent(QMouseEvent *event) {
    if (!count)
        return;
    if ((event->position().x() / interfaceScale()) < 81) {
        emit endpointSelected(u"embed"_s);
        return;
    }
    if ((event->position().x() / interfaceScale()) >= logicalWidth() - 79) {
        emit endpointSelected(u"logits"_s);
        return;
    }
    if (count && (event->position().x() / interfaceScale()) >= 81 &&
        (event->position().x() / interfaceScale()) < logicalWidth() - 79)
        emit layerSelected(std::clamp(
            int(((event->position().x() / interfaceScale()) - 81) / ((logicalWidth() - 160.0) / count)), 0,
            count - 1));
}

Timeline::Timeline(QWidget *parent) : ScaledWidget(parent) {
    UiScale::minimumHeight(this, 147);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(u"真实执行时间轴，点击Token定位计算"_s);
}
void Timeline::configure(int count) {
    layers = count;
    clear();
}
void Timeline::clear() {
    frames.clear();
    scroll = 0;
    selectedId.clear();
    update();
}
TimelineFrame *Timeline::frame(const QString &id, const QString &b) {
    for (auto &value : frames)
        if (value.id == id && value.branch == b)
            return &value;
    return nullptr;
}
void Timeline::startForward(const QString &id, const QString &b, int position) {
    if (!frame(id, b))
        frames.append({id, b, {}, position, {}, {}});
    // 时间轴保留近期计算轮次，避免长对话使界面现场和内存无限增长。
    while (frames.size() > 128)
        frames.removeFirst();
    branch = b;
    selectedId = id;
    scroll = std::max(0, int(frames.size()) - std::max(1, (logicalWidth() - 145) / 155));
    update();
}
void Timeline::operation(const QString &id, const QString &b, int layer, const QString &op, double duration) {
    if (auto *value = frame(id, b)) {
        const auto key = QString::number(layer) + u":"_s + op;
        value->done.insert(key);
        value->durations[key] = duration;
        update();
    }
}
void Timeline::token(const QString &id, const QString &b, const QString &text, int position) {
    if (auto *value = frame(id, b)) {
        value->token = text;
        // 采样输出位置与本次前向的输入位置相差一，导航仍定位到输入。
        value->sampledPosition = position;
        value->done.insert(u"sample"_s);
        update();
    }
}
void Timeline::setBranch(const QString &value) {
    branch = value;
    scroll = 0;
    update();
}
void Timeline::selectFrame(const QString &id) {
    selectedId = id;
    update();
}
void Timeline::paintEvent(QPaintEvent *) {
    // 每一列是真实前向，每一格对应实际层；不以动画速度替代计算进度。
    QPainter p(this);
    preparePainter(p);
    p.fillRect(logicalRect(), QColor(u"#23262b"_s));
    const int labelWidth = 145, rowHeight = std::max(23, (logicalHeight() - 2) / 5);
    const QStringList labels{u"近期 128 轮前向"_s, u"Token嵌入"_s, u"注意力计算"_s, u"前馈网络"_s,
                             u"采样与解码"_s};
    const QList<QColor> colors{QColor(u"#6e849b"_s), QColor(u"#63778e"_s), QColor(u"#7685a6"_s),
                               QColor(u"#9f88ab"_s), QColor(u"#80b5a4"_s)};
    for (int row = 0; row < 5; ++row) {
        p.setPen(QColor(u"#38404b"_s));
        p.drawLine(0, (row + 1) * rowHeight, logicalWidth(), (row + 1) * rowHeight);
        p.setPen(QColor(u"#b0bed0"_s));
        p.drawText(QRect(13, row * rowHeight, labelWidth - 16, rowHeight), Qt::AlignVCenter, labels[row]);
    }
    QVector<const TimelineFrame *> visible;
    for (const auto &value : frames)
        if (value.branch == branch)
            visible.append(&value);
    if (visible.isEmpty()) {
        p.setPen(QColor(u"#8594a8"_s));
        p.drawText(QRect(labelWidth, 0, logicalWidth() - labelWidth, logicalHeight()), Qt::AlignCenter,
                   u"运行后显示Token、层与运算的真实执行轨迹"_s);
        return;
    }
    for (int index = scroll; index < visible.size(); ++index) {
        int x = labelWidth + (index - scroll) * 155;
        if (x >= logicalWidth())
            break;
        const auto &value = *visible[index];
        if (value.id == selectedId)
            p.fillRect(QRect(x, 0, 155, logicalHeight()), QColor(u"#2e3c48"_s));
        p.setPen(QColor(u"#526173"_s));
        p.drawLine(x, 0, x, logicalHeight());
        p.setPen(QColor(u"#b6c9df"_s));
        p.drawText(QRect(x + 8, 0, 140, rowHeight), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(
                       QString::number(value.sampledPosition >= 0 ? value.sampledPosition : value.position) +
                           u"  "_s + (value.token.isEmpty() ? u"计算中"_s : value.token),
                       Qt::ElideRight, 138));
        for (int row = 1; row < 5; ++row) {
            if (row == 1 || row == 4) {
                bool done = row == 1 ? value.done.contains(u"-1:embed"_s) : value.done.contains(u"sample"_s);
                p.fillRect(QRect(x + 6, row * rowHeight + 7, 143, rowHeight - 14),
                           done ? colors[row].darker(170) : QColor(u"#2a2e35"_s));
            } else
                for (int layer = 0; layer < layers; ++layer) {
                    bool done = value.done.contains(QString::number(layer) +
                                                    (row == 2 ? u":attention_residual"_s : u":residual"_s));
                    double cw = 143.0 / std::max(layers, 1);
                    p.fillRect(QRectF(x + 6 + layer * cw, row * rowHeight + 7, std::max(1.0, cw - 1),
                                      rowHeight - 14),
                               done ? colors[row].darker(150) : QColor(u"#30343d"_s));
                }
        }
        if (value.id == selectedId) {
            p.setPen(QPen(QColor(u"#a7d6e6"_s), 2));
            p.drawLine(x + 1, 0, x + 1, logicalHeight());
        }
    }
}
void Timeline::mousePressEvent(QMouseEvent *event) {
    // 轨道点击定位到真实token与层，查看不会继续执行模型。
    if ((event->position().x() / interfaceScale()) < 145)
        return;
    QVector<const TimelineFrame *> visible;
    for (const auto &value : frames)
        if (value.branch == branch)
            visible.append(&value);
    int index = int(((event->position().x() / interfaceScale()) - 145) / 155) + scroll;
    if (index >= visible.size())
        return;
    const auto &value = *visible[index];
    int row = int((event->position().y() / interfaceScale()) / std::max(23, (logicalHeight() - 2) / 5));
    int layer =
        std::clamp(int(std::fmod((event->position().x() / interfaceScale()) - 145, 155) / 155 * layers), 0,
                   std::max(0, layers - 1));
    selectedId = value.id;
    update();
    emit selected(value.id, layer,
                  row == 1   ? u"embed"_s
                  : row == 2 ? u"attention_residual"_s
                  : row == 3 ? u"mlp"_s
                             : u"logits"_s,
                  value.position);
}
void Timeline::wheelEvent(QWheelEvent *event) {
    scroll = std::max(0, scroll + (event->angleDelta().y() < 0 ? 1 : -1));
    scroll = std::min(scroll, std::max(0, int(frames.size()) - 1));
    update();
    event->accept();
}
ResourcePlot::ResourcePlot(QWidget *parent) : ScaledWidget(parent) {
    UiScale::fixedHeight(this, 30);
}
void ResourcePlot::sample(int value) {
    samples.append(value);
    while (samples.size() > 48)
        samples.removeFirst();
    update();
}
void ResourcePlot::paintEvent(QPaintEvent *) {
    QPainter p(this);
    preparePainter(p);
    p.fillRect(logicalRect(), QColor(u"#24272d"_s));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(u"#768b99"_s));
    if (samples.isEmpty())
        return;
    double cw = logicalWidth() / 48.0;
    for (int i = 0; i < samples.size(); ++i) {
        double h = std::max(1.0, samples[i] * (logicalHeight() - 2) / 100.0);
        p.drawRect(QRectF(i * cw, logicalHeight() - h, cw - 1, h));
    }
}
