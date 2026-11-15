#pragma once
#include <QObject>
#include <QString>

class QWidget;

// 统一缩放控件尺寸、布局和样式；系统 DPI 仍由 Qt 自行处理。
class UiScale final : public QObject {
    Q_OBJECT
  public:
    explicit UiScale(QWidget *root, const QString &style);
    double factor() const {
        return current;
    }
    void setFactor(double value);
    static void fixedHeight(QWidget *widget, int value);
    static void fixedWidth(QWidget *widget, int value);
    static void minimumHeight(QWidget *widget, int value);
    static void minimumWidth(QWidget *widget, int value);
  signals:
    void changed(double factor);

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    void apply(QWidget *widget);
    void applyTree(QWidget *widget);
    QWidget *root;
    QString baseStyle;
    double current = 1;
    bool applying = false;
};
