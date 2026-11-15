#include "chattokenview.h"
#include <QContextMenuEvent>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

ChatTokenView::ChatTokenView(QWidget *parent) : QTextBrowser(parent) {
    // 链接只提供 token 身份，不执行导航或自动发起分析。
    setOpenLinks(false);
    setOpenExternalLinks(false);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!replacingContent && !adjustingScroll && !restoreQueued)
            rememberReadingPosition();
    });
    connect(verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int value) {
        setReadingPosition(value);
    });
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int) {
        // 此信号在 value 更新前发出，使用已经调整好的 sliderPosition 判断意图。
        setReadingPosition(verticalScrollBar()->sliderPosition());
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
        if (!replacingContent) queueScrollRestore();
    });
}

void ChatTokenView::rememberReadingPosition() {
    // 用户操作更新阅读锚点；文档重排造成的临时归零不能修改此状态。
    const auto *bar = verticalScrollBar();
    followingOutput = bar->maximum() - bar->value() <= 12;
    const auto cursor = cursorForPosition(QPoint(0, 1));
    readingCharacter = cursor.position();
    readingOffset = cursorRect(cursor).top();
    horizontalPosition = horizontalScrollBar()->value();
}

void ChatTokenView::restoreReadingPosition() {
    // 使用可见字符及其行内偏移恢复，长文档延迟布局后仍能保持阅读现场。
    adjustingScroll = true;
    auto *bar = verticalScrollBar();
    if (followingOutput) {
        bar->setValue(bar->maximum());
    } else {
        QTextCursor cursor(document());
        cursor.setPosition(std::clamp(readingCharacter, 0, document()->characterCount() - 1));
        bar->setValue(bar->value() + cursorRect(cursor).top() - readingOffset);
    }
    horizontalScrollBar()->setValue(horizontalPosition);
    adjustingScroll = false;
}

void ChatTokenView::queueScrollRestore() {
    // 合并排版回调，只恢复最新的用户意图，不捕获已经过期的滚动数值。
    if (restoreQueued) return;
    restoreQueued = true;
    QTimer::singleShot(0, this, [this] {
        restoreQueued = false;
        restoreReadingPosition();
    });
}

void ChatTokenView::setConversationHtml(const QString &html, bool forceBottom) {
    // 会话切换、发送和显示设置变化时重建已提交的历史。
    beginContentUpdate(forceBottom);
    streamingFrame.clear();
    setHtml(html);
    endContentUpdate(0);
}

void ChatTokenView::setStreamingHtml(const QString &html) {
    // 流式回复放在独立尾部，保留历史文档、表格及 Token 锚点。
    if (html.isEmpty() && !streamingFrame)
        return;
    beginContentUpdate(false);
    if (!streamingFrame) {
        QTextCursor end(document());
        end.movePosition(QTextCursor::End);
        streamingFrame = end.insertFrame(QTextFrameFormat());
    }
    auto cursor = streamingFrame->firstCursorPosition();
    const int firstPosition = cursor.position();
    cursor.beginEditBlock();
    cursor.setPosition(streamingFrame->lastPosition(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.insertHtml(html);
    cursor.endEditBlock();
    endContentUpdate(firstPosition);
}

void ChatTokenView::beginContentUpdate(bool forceBottom) {
    // 流式更新前保留意图，避免 setHtml 的临时范围被误判为用户翻到了顶部。
    if (!restoreQueued && !followingOutput) rememberReadingPosition();
    if (forceBottom) followingOutput = true;
    replacingContent = true;
    document()->setLayoutEnabled(false);
}

void ChatTokenView::endContentUpdate(int firstPosition) {
    // Qt 导入 HTML 时会给身份锚点补上链接下划线，导入后显式清除。
    // 合并格式修改后再启用布局，避免每个 Token 触发一次整段排版。
    QList<QPair<int,int>> tokenRanges;
    for (auto block = document()->findBlock(firstPosition); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().anchorHref().startsWith(u"token:"_s))
                tokenRanges.append({fragment.position(), fragment.length()});
        }
    }
    QTextCharFormat plainToken;plainToken.setUnderlineStyle(QTextCharFormat::NoUnderline);plainToken.setFontUnderline(false);
    // 普通阅读使用正文色，避免身份锚点被 Qt 渲染成默认蓝色链接。
    if(!showTokenBorders)plainToken.setForeground(palette().color(QPalette::Text));
    QTextCursor cursor(document());
    cursor.beginEditBlock();
    for (const auto &range : tokenRanges) {
        cursor.setPosition(range.first);
        cursor.setPosition(range.first + range.second, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(plainToken);
    }
    cursor.endEditBlock();
    document()->setLayoutEnabled(true);
    document()->documentLayout()->documentSize();
    replacingContent = false;
    restoreReadingPosition();
    queueScrollRestore();
}

void ChatTokenView::setReadingPosition(int value) {
    // 恢复保存位置和主动发送消息也必须更新同一滚动意图，不能只改滚动条。
    adjustingScroll = true;
    verticalScrollBar()->setValue(value);
    adjustingScroll = false;
    rememberReadingPosition();
}

void ChatTokenView::wheelEvent(QWheelEvent *event) {
    // 即使文档正在等待延迟排版，滚轮操作仍立即覆盖此前的自动跟随状态。
    QTextBrowser::wheelEvent(event);
    rememberReadingPosition();
}

void ChatTokenView::keyPressEvent(QKeyEvent *event) {
    // 翻页、方向键与首尾跳转和鼠标滚动采用同一规则。
    QTextBrowser::keyPressEvent(event);
    if (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown || event->key() == Qt::Key_Up ||
        event->key() == Qt::Key_Down || event->key() == Qt::Key_Home || event->key() == Qt::Key_End || event->key() == Qt::Key_Space)
        rememberReadingPosition();
}

void ChatTokenView::contextMenuEvent(QContextMenuEvent *event) {
    // 在菜单弹出前取到不可变身份，流式刷新不会把命中移动到另一个 token。
    const auto anchor = anchorAt(event->pos());
    if (anchor.startsWith(u"token:"_s) && tokenMenu) {
        tokenMenu(anchor, event->globalPos());
        event->accept();
        return;
    }
    QTextBrowser::contextMenuEvent(event);
}

void ChatTokenView::paintEvent(QPaintEvent *event) {
    // QTextBrowser 的 HTML 不支持可靠的行内边框，因此在原生字符布局上描边。
    QTextBrowser::paintEvent(event);
    if (!showTokenBorders)
        return;
    QPainter painter(viewport());
    painter.setPen(QColor(u"#7892a3"_s));
    painter.setBrush(Qt::NoBrush);
    const int firstVisible = cursorForPosition(QPoint(0, 0)).blockNumber() - 1;
    const int lastVisible = cursorForPosition(QPoint(viewport()->width() - 1, viewport()->height() - 1)).blockNumber() + 1;
    for (auto block = document()->findBlockByNumber(std::max(0, firstVisible)); block.isValid() && block.blockNumber() <= lastVisible; block = block.next()) {
        const auto *layout = block.layout();
        if (!layout)
            continue;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().anchorHref().startsWith(u"token:"_s))
                continue;
            const int start = fragment.position() - block.position();
            const int end = start + fragment.length();
            for (int i = 0; i < layout->lineCount(); ++i) {
                const auto line = layout->lineAt(i);
                const int left = std::max(start, line.textStart());
                const int right = std::min(end, line.textStart() + line.textLength());
                if (left >= right)
                    continue;
                QTextCursor a(document());
                a.setPosition(block.position() + left);
                const auto caret = cursorRect(a);
                // 换行末尾的光标属于下一行，横坐标改用该行自身的布局计算。
                const qreal width = std::abs(line.cursorToX(right) - line.cursorToX(left));
                const qreal x = caret.x() - std::max(qreal(0), line.cursorToX(left) - line.cursorToX(right));
                painter.drawRect(QRectF(x - 1, caret.y() + 1, std::max(qreal(4), width) + 1, std::max(3, caret.height() - 2)));
            }
        }
    }
}
