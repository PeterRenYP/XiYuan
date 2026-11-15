#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcess>

class Transport final : public QObject {
    Q_OBJECT
  public:
    // 每个运行器有独立进程与字节缓冲，界面线程仅接收协议事件。
    explicit Transport(const QString &mode, QObject *parent = nullptr);
    void start();
    QString send(const QString &name, const QJsonObject &payload = {});
    void stop();
    void setStorageRoot(const QString &directory);
    void setAnomalyEndpoint(const QJsonObject &endpoint) { anomalyEndpoint = endpoint; }
    qint64 pid() const {
        return process.processId();
    }
    static QString projectRoot();
  signals:
    void message(const QJsonObject &value);
    void failed(const QString &text);
    void stopped(int code);

  private:
    void readOutput();
    void readLog();
    QProcess process;
    QByteArray buffer;
    QMap<QString,QByteArray> transfers;
    QMap<QString,int> transferIndices;
    QString mode, logRoot;
    QJsonObject anomalyEndpoint;
};
