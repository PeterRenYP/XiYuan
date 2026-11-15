#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <functional>

class Workbench;
class QLabel;

// 本机控制桥只操作现有工作台，不创建第二个模型或独立会话。
class McpBridge final : public QObject {
    Q_OBJECT
  public:
    explicit McpBridge(Workbench *workbench);
    ~McpBridge() override;
    QString localEndpoint() const;
    QString connectionDirectory() const;

  private:
    using Reply = std::function<void(const QJsonObject &)>;
    void acceptConnections();
    void dispatch(const QString &method, const QJsonObject &params, const Reply &reply);
    void runtimeQuery(const QString &name, const QJsonObject &payload, const Reply &reply);
    void record(const QJsonObject &message);
    QJsonObject selectObservation(const QJsonObject &params, bool changeCapture = true);
    QJsonObject state() const;
    QJsonObject accepted(const QString &request = {}) const;
    QString idleError(bool requireModel = false) const;
    static QJsonObject failure(const QString &text, const QString &code = QStringLiteral("INVALID_STATE"));

    Workbench *window;
    QTcpServer server;
    QLabel *connectionLabel = nullptr;
    QString token, instanceId, descriptorPath, externalRunId;
    QMap<QString, Reply> pending;
    QSet<QString> externalRequests;
    QJsonArray events;
    QJsonArray sampledTokens;
    QString sampledTracePath;
    qint64 cursor = 0, eventBytes = 0;
};
