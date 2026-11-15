#pragma once
#include <QKeyEvent>
#include <QPlainTextEdit>

// 多行对话输入保留会话存储所用的文本接口。
class ChatPrompt final : public QPlainTextEdit {
    Q_OBJECT
  public:
    explicit ChatPrompt(QWidget *parent = nullptr) : QPlainTextEdit(parent) {}
    QString text() const { return toPlainText(); }
    void setText(const QString &value) { setPlainText(value); }
  signals:
    void returnPressed();
  protected:
    // 回车发送，Ctrl+Enter 插入换行；由输入控件处理，避免窗口快捷键重复发送。
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            event->accept();
            if (event->modifiers().testFlag(Qt::ControlModifier)) insertPlainText(QStringLiteral("\n"));
            else emit returnPressed();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};
