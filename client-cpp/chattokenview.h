#pragma once
#include <QPointer>
#include <QTextBrowser>
#include <functional>

class QTextFrame;

// 拆分模式仍使用原生文本布局，边框与右键命中共用实际字符位置。
class ChatTokenView final : public QTextBrowser {
  public:
    explicit ChatTokenView(QWidget *parent = nullptr);
    void setConversationHtml(const QString &html, bool forceBottom = false);
    void setStreamingHtml(const QString &html);
    void setReadingPosition(int value);
    std::function<void(const QString &, const QPoint &)> tokenMenu;
    bool showTokenBorders = false;

  protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void beginContentUpdate(bool forceBottom);
    void endContentUpdate(int firstPosition);
    void rememberReadingPosition();
    void restoreReadingPosition();
    void queueScrollRestore();
    bool followingOutput = true, replacingContent = false, adjustingScroll = false, restoreQueued = false;
    int readingCharacter = 0, readingOffset = 0, horizontalPosition = 0;
    QPointer<QTextFrame> streamingFrame;
};
