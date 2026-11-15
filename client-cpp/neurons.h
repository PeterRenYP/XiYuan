#pragma once
#include "visuals.h"
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QPolygonF>
#include <QTimer>
#include <QVector3D>

class QLabel;
class QLineEdit;
class QPushButton;
class QGridLayout;

// 所有节点编号来自模型实际维度或后端返回值；坐标只负责空间排布。
class NeuronCanvas final : public ScaledWidget {
    Q_OBJECT
  public:
    explicit NeuronCanvas(QWidget *parent = nullptr);
    void configure(const QJsonArray &layers);
    void clearActivity();
    void applyFrame(const QJsonObject &frame, bool animate = true);
    void setPaths(const QJsonArray &paths);
    void setNetworkPaths(int layer, const QJsonArray &paths);
    void clearNetworkPaths();
    void setActivityPaused(bool value);
    int activeBeamCount() const;
    void clearSelection();
    void select(int layer, int neuron, bool center = false);
    void setFlat(bool value);
    void setPan(bool value);
    void changeZoom(double factor);
    void resetView();
    void showAll();
    bool replay();
    QPointF nodePosition(int layer, int neuron) const;
    int nodeCount() const;
    bool flat() const { return flatView; }
  signals:
    void selected(int layer, int neuron);
    void selectionCleared();
    void zoomChanged(int percent);
    void propagated(int sourceLayer, int sourceNeuron, int targetLayer, int targetNeuron);
  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *) override;
  private:
    // 验收可读取在途状态，避免仅凭截图判断独立计时和刷新节奏。
    friend struct NeuronAnimationVerification;
    struct Node {
        int layer=0, neuron=0, slot=0;
        QVector3D position;
        double value=0, target=0, maximum=1;
        bool available=false, sampled=true;
        QString forward;
        int token=-1;
    };
    struct Path { int source=-1, target=-1; double coefficient=0; qint64 started=-1; int duration=1100; bool negative=false; };
    struct Projection { QPointF point; double depth=0, scale=1; };
    int ensureNode(int layer, int neuron);
    Projection project(const Node &node) const;
    void ensureProjection() const;
    void startPath(Path &path, qint64 now);
    bool inFlight(const Path &path, qint64 now) const;
    int hit(const QPointF &position);
    QPointF flatPoint(int layer, int neuron) const;
    QPointF flatScreenPoint(int layer, int neuron) const;
    void animate();
    void centerSelection();
    void launchNetwork(int layer);
    qint64 activityTime() const;
    quint64 key(int layer, int neuron) const;
    QVector<Node> nodes;
    QVector<Path> paths;
    QVector<Path> networkPaths;
    QHash<quint64,int> networkIndices;
    QMap<int,QVector<int>> layerPaths;
    QMap<int,QJsonArray> networkTopology;
    QMap<int,int> propagationCursor;
    QMap<quint64,int> indices;
    QJsonArray layers;
    QVector<QPolygonF> flatLayers;
    QTimer timer;
    QElapsedTimer clock;
    qint64 pausedAt=-1;
    int selectedIndex=-1;
    bool flatView=false, pan=false, dragging=false, moved=false;
    QPointF pressed, previous, offset;
    double yaw=-.24, pitch=.16, zoom=1;
    mutable QVector<Projection> projected;
    mutable QVector<int> depthOrder;
    mutable QSize projectionSize;
    mutable QPointF projectionOffset;
    mutable double projectionYaw=0, projectionPitch=0, projectionZoom=0;
    mutable bool projectionFlat=false;
    QPixmap beamGlows[4];
    double paintCostMs=0;
    int beamBudget=144;
};

// 原生页面复刻原型：层导航、双输入框、两种画布与右侧真实数值检查器。
class NeuronPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit NeuronPanel(QWidget *parent = nullptr);
    void configure(const QJsonObject &model, bool online);
    void beginRun(const QString &run, const QString &branch);
    void context(const QString &branch, const QString &forward, int position);
    void frame(const QJsonObject &payload);
    void snapshot(const QJsonObject &payload);
    void failure(const QString &message);
    void clearActivity();
    QJsonObject selection() const;
    void choose(int layer, int neuron, bool center);
    void clearSelection();
    void setLive(bool value);
    bool following() const { return live && online; }
    void restoreView(bool flat);
    bool flat() const { return canvas->flat(); }
    // 对话使用新增分区，原导航、画布和详情不重建。
    QWidget *conversationArea() const { return chatArea; }
    NeuronCanvas *canvas = nullptr;
  signals:
    void subscriptionChanged();
    void queryRequested();
  protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
  private:
    void locate();
    void renderDetails(const QJsonObject &detail);
    void renderHistory();
    void updateButtons();
    QJsonObject model, detail;
    QJsonArray layerInfo, pathInfo, history;
    QMap<int,QJsonObject> recentFrames;
    QString modelRevision, weightsRevision, runId, branchId=QStringLiteral("base"), forwardId=QStringLiteral("f0");
    int selectedLayer=0, selectedNeuron=0, tokenPosition=0;
    bool online=false, live=true, hasSelection=false;
    QLineEdit *layerInput=nullptr, *neuronInput=nullptr;
    QGridLayout *layerGrid=nullptr;
    QLabel *total=nullptr, *count=nullptr, *message=nullptr, *activity=nullptr, *identity=nullptr, *subtitle=nullptr;
    QLabel *activation=nullptr, *rank=nullptr, *gate=nullptr, *silu=nullptr, *up=nullptr, *middle=nullptr;
    QLabel *contextLabel=nullptr, *pathNote=nullptr, *location=nullptr, *historyText=nullptr;
    QWidget *meter=nullptr;
    QWidget *chatArea=nullptr;
    QPushButton *view3d=nullptr, *viewFlat=nullptr, *play=nullptr, *emitOnce=nullptr, *rotate=nullptr, *panButton=nullptr;
    QPushButton *pathSource=nullptr, *pathTarget=nullptr, *zoomLabel=nullptr, *locateButton=nullptr;
};
