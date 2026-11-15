#include "uiscale.h"
#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QTextDocument>
#include <QTextEdit>
#include <QTreeView>
#include <QVariant>
#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace {
// 基准值只记录一次，避免连续放大缩小产生累计误差。
QVariant baseline(QObject *object, const char *name, const QVariant &value) {
    if (!object->property(name).isValid())
        object->setProperty(name, value);
    return object->property(name);
}
void scaleLayout(QLayout *layout, double factor) {
    if (!layout)
        return;
    const auto margins =
        baseline(layout, "scaleMargins", QVariant::fromValue(layout->contentsMargins())).value<QMargins>();
    layout->setContentsMargins(qRound(margins.left() * factor), qRound(margins.top() * factor),
                               qRound(margins.right() * factor), qRound(margins.bottom() * factor));
    const int spacing = baseline(layout, "scaleSpacing", layout->spacing()).toInt();
    if (spacing >= 0)
        layout->setSpacing(qRound(spacing * factor));
    for (int i = 0; i < layout->count(); ++i) {
        auto *item = layout->itemAt(i);
        if (item->layout())
            scaleLayout(item->layout(), factor);
        if (auto *spacer = item->spacerItem()) {
            const auto key = (u"scaleSpacer"_s + QString::number(i)).toLatin1();
            const auto size = baseline(layout, key.constData(), spacer->sizeHint()).toSize();
            const auto policy = spacer->sizePolicy();
            spacer->changeSize(qRound(size.width() * factor), qRound(size.height() * factor),
                               policy.horizontalPolicy(), policy.verticalPolicy());
        }
    }
    layout->invalidate();
}
} // namespace

UiScale::UiScale(QWidget *root, const QString &style) : QObject(root), root(root), baseStyle(style) {
    // 应用级过滤器让输入框、画布和模态设置窗口都能使用相同快捷键。
    applyTree(root);
    qApp->installEventFilter(this);
}
void UiScale::fixedHeight(QWidget *widget, int value) {
    widget->setProperty("scaleFixedHeight", value);
    widget->setFixedHeight(value);
}
void UiScale::fixedWidth(QWidget *widget, int value) {
    widget->setProperty("scaleFixedWidth", value);
    widget->setFixedWidth(value);
}
void UiScale::minimumHeight(QWidget *widget, int value) {
    widget->setProperty("scaleMinimumHeight", value);
    widget->setMinimumHeight(value);
}
void UiScale::minimumWidth(QWidget *widget, int value) {
    widget->setProperty("scaleMinimumWidth", value);
    widget->setMinimumWidth(value);
}
void UiScale::setFactor(double value) {
    // 以百分之十为步长，范围 70% 到 200%；重算自基准样式而非当前样式。
    if (!std::isfinite(value))
        return;
    current = std::clamp(qRound(value * 10) / 10.0, 0.7, 2.0);
    applying = true;
    QString style;
    qsizetype offset = 0;
    const QRegularExpression pixels(uR"((\d+(?:\.\d+)?)px)"_s);
    auto matches = pixels.globalMatch(baseStyle);
    while (matches.hasNext()) {
        const auto match = matches.next();
        style += baseStyle.mid(offset, match.capturedStart() - offset);
        style += QString::number(qRound(match.captured(1).toDouble() * current)) + u"px"_s;
        offset = match.capturedEnd();
    }
    style += baseStyle.mid(offset);
    // 弹出菜单和对话框继承同一应用样式，已存在的内容与业务对象保持原样。
    qApp->setStyleSheet(style);
    QFont font(u"Microsoft YaHei UI"_s);
    font.setPixelSize(qRound(14 * current));
    qApp->setFont(font);
    applyTree(root);
    applying = false;
    // 显式沿用主入口选择的格式，验收时使用隔离目录而非 Windows 注册表。
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, u"ModelObservatory"_s, u"NativeWorkbench"_s);
    settings.setValue(u"ui_scale"_s, current);
    settings.sync();
    emit changed(current);
}
void UiScale::applyTree(QWidget *widget) {
    // 控件只更新几何约束，不替换对象，因此选择、文本和滚动状态均保留。
    apply(widget);
    for (auto *child : widget->findChildren<QWidget *>(QString{}, Qt::FindDirectChildrenOnly))
        applyTree(child);
}
void UiScale::apply(QWidget *widget) {
    const auto scaled = [this, widget](const char *key) {
        return qRound(widget->property(key).toInt() * current);
    };
    if (widget->property("scaleMinimumWidth").isValid())
        widget->setMinimumWidth(scaled("scaleMinimumWidth"));
    if (widget->property("scaleMinimumHeight").isValid())
        widget->setMinimumHeight(scaled("scaleMinimumHeight"));
    if (widget->property("scaleFixedWidth").isValid())
        widget->setFixedWidth(scaled("scaleFixedWidth"));
    if (widget->property("scaleFixedHeight").isValid())
        widget->setFixedHeight(scaled("scaleFixedHeight"));
    scaleLayout(widget->layout(), current);
    if (auto *splitter = qobject_cast<QSplitter *>(widget)) {
        const int size = baseline(widget, "scaleHandle", splitter->handleWidth()).toInt();
        splitter->setHandleWidth(std::max(3, qRound(size * current)));
    }
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        const auto size = baseline(widget, "scaleIcon", button->iconSize()).toSize();
        button->setIconSize(QSize(qRound(size.width() * current), qRound(size.height() * current)));
    }
    if (auto *tree = qobject_cast<QTreeView *>(widget)) {
        const int size = baseline(widget, "scaleIndent", tree->indentation()).toInt();
        tree->setIndentation(qRound(size * current));
    }
    if (auto *header = qobject_cast<QHeaderView *>(widget)) {
        const int size = baseline(widget, "scaleSection", header->defaultSectionSize()).toInt();
        header->setDefaultSectionSize(qRound(size * current));
    }
    if (auto *text = qobject_cast<QTextEdit *>(widget))
        text->document()->setDefaultFont(qApp->font());
    widget->setProperty("uiScale", current);
    widget->updateGeometry();
    widget->update();
}
bool UiScale::eventFilter(QObject *object, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(object);
    // 对话框是独立顶层窗口，按对象所有权判断归属，不能用窗口内的祖先判断。
    auto *owner = object;
    while (owner && owner != root)
        owner = owner->parent();
    if (!widget || !owner)
        return false;
    if (event->type() == QEvent::Show && !applying) {
        // 延迟创建的模型设置窗口和菜单，在首次显示之前补上当前倍率。
        applying = true;
        applyTree(widget);
        applying = false;
    }
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers().testFlag(Qt::ControlModifier) && !key->modifiers().testFlag(Qt::AltModifier) &&
            !key->modifiers().testFlag(Qt::MetaModifier)) {
            const auto code = key->key();
            if (code == Qt::Key_Plus || code == Qt::Key_Equal || code == Qt::Key_Minus || code == Qt::Key_0) {
                if (event->type() == QEvent::KeyPress)
                    setFactor(code == Qt::Key_0 ? 1 : current + (code == Qt::Key_Minus ? -0.1 : 0.1));
                key->accept();
                return true;
            }
        }
    }
    return false;
}
