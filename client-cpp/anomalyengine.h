#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <functional>

// 纯 CPU 检测状态只存在于独立监控进程；不持有模型或 GPU 句柄。
class AnomalyEngine {
  public:
    explicit AnomalyEngine(std::function<void(const QJsonObject &)> output);
    void accept(const QJsonObject &message, qint64 now);
    void tick(qint64 now);
    bool running() const { return active; }
    int baselineCount() const { return baselines.size(); }
  private:
    struct Gate { int bad = 0, good = 0; bool active = false; QString type; };
    struct Reference { QList<double> values; };
    struct Window { QList<double> values; QList<QList<double>> vectors; int position = -1, fixed = 0; };
    void condition(const QString &key, const QString &type, bool bad, const QJsonObject &evidence,
                   int required = 4);
    void tensor(const QJsonObject &message);
    void token(const QJsonObject &message, qint64 now);
    void finish(const QJsonObject &message, qint64 now);
    void status(qint64 now);
    void emitEvent(const QString &name, const QJsonObject &payload);
    QString referenceKey(const QString &metric, int layer, const QString &phase, int context) const;
    QList<double> reference(const QString &key) const;
    void learn(const QString &key, double value, bool suspect);
    void loadReferences(const QString &directory);
    void saveReferences();
    std::function<void(const QJsonObject &)> output;
    QMap<QString, Reference> baselines, candidates;
    QMap<QString, Gate> gates;
    QMap<QString, Window> windows;
    QSet<QString> ignored, announced;
    QList<int> recentTokens;
    QString reasoning, answer, run, branch, profile, directory;
    QJsonObject metadata;
    bool active = false, waiting = false, hadAlert = false, transportGap = false;
    int lastStep = -1, reasoningTokens = 0, validTensorSamples = 0, dropped = 0, sampled = 0, eligible = 0;
    int droppedTokens = 0;
    int slowStreak = 0;
    qint64 started = 0, lastHeartbeat = 0, lastProgress = 0, lastStatus = 0;
    QString progressIdentity;
};

QString anomalyName(const QString &type);
