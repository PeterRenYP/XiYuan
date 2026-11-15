#pragma once
#include <QGraphicsView>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QWidget>

QString operationName(const QString &key);
QString operationFormula(const QString &key);
QColor heatColor(double value, double maximum, bool positiveOnly = false);
QString valueText(const QJsonValue &value, int precision = 5);

class GraphView final : public QGraphicsView {
    Q_OBJECT
  public:
    explicit GraphView(QWidget *parent = nullptr);
    void setLayer(bool linear, bool hybrid, int layer);
    void selectOperation(const QString &operation, bool center = false);
    void updateOperation(const QString &operation, const QJsonObject &payload, bool current);
    void clearRun();
    void fitGraph();
    void changeZoom(double factor);
    void setPan(bool enabled);
    void setInterfaceScale(double factor);
  signals:
    void selected(const QString &operation);
    void zoomChanged(int percent);

  protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    QString visualKey(const QString &key) const;
    QGraphicsScene scene;
    QMap<QString, QGraphicsItem *> nodes;
    QMap<QString, QGraphicsPathItem *> edges;
    double interfaceScale = 1;
    bool linear = false, hybrid = false;
    int layer = -999;
};

// 自绘控件统一使用基准坐标；绘制与鼠标命中采用同一倍率。
class ScaledWidget : public QWidget {
  public:
    explicit ScaledWidget(QWidget *parent = nullptr) : QWidget(parent) {}

  protected:
    double interfaceScale() const;
    int logicalWidth() const;
    int logicalHeight() const;
    QRect logicalRect() const;
    void preparePainter(QPainter &painter) const;
};

class Heatmap final : public ScaledWidget {
    Q_OBJECT
  public:
    explicit Heatmap(QWidget *parent = nullptr);
    void setMatrix(const QJsonArray &matrix, const QString &caption = {});
    QJsonArray matrix;
    bool positiveOnly = false;
  signals:
    void cellSelected(int row, int column, QJsonValue value);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void choose(int row, int column);
    int selectedRow = -1, selectedColumn = -1;
    QString caption;
};

class LayerStrip final : public ScaledWidget {
    Q_OBJECT
  public:
    explicit LayerStrip(QWidget *parent = nullptr);
    void configure(const QJsonObject &config);
    void setPosition(int selected, int executed);
  signals:
    void layerSelected(int layer);
    void endpointSelected(const QString &operation);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    int count = 0, selected = 0, executed = -1;
    QJsonArray types;
};

struct TimelineFrame {
    QString id, branch, token;
    int position = 0;
    QSet<QString> done;
    QMap<QString, double> durations;
    int sampledPosition = -1;
};

class Timeline final : public ScaledWidget {
    Q_OBJECT
  public:
    explicit Timeline(QWidget *parent = nullptr);
    void configure(int layers);
    void startForward(const QString &id, const QString &branch, int position);
    void operation(const QString &id, const QString &branch, int layer, const QString &operation,
                   double duration);
    void token(const QString &id, const QString &branch, const QString &text, int position);
    void clear();
    void setBranch(const QString &branch);
    void selectFrame(const QString &id);
    QVector<TimelineFrame> frames;
  signals:
    void selected(const QString &forward, int layer, const QString &operation, int position);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

  private:
    TimelineFrame *frame(const QString &id, const QString &branch);
    int layers = 0, scroll = 0;
    QString branch = QStringLiteral("base"), selectedId;
};

class ResourcePlot final : public ScaledWidget {
  public:
    explicit ResourcePlot(QWidget *parent = nullptr);
    void sample(int value);

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    QList<int> samples;
};
