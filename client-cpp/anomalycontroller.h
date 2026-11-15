#pragma once
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTcpSocket>

class QDialog;
class QWidget;

// 管理异常提示；仅用户点击停止时，将带原运行身份的取消请求交给工作台。
class AnomalyController : public QObject {
    Q_OBJECT
  public:
    explicit AnomalyController(QWidget *parent);
    ~AnomalyController() override;
    void start();
    QJsonObject endpoint() const { return connection; }
    void runEvent(const QJsonObject &message, const QString &projectDirectory = {});
    void handle(const QJsonObject &message);
    void showRecords();
    int pendingCount() const { return pending.size(); }
    int recordCount() const { return records.size(); }
  signals:
    void stopRequested(const QString &runId, const QString &branchId);
    void ready(const QJsonObject &endpoint);
    void changed(const QString &text);
  private:
    void readOutput();
    void send(QJsonObject message);
    void clearDialogs();
    void record(const QJsonObject &event);
    QWidget *owner;
    QProcess process;
    QTcpSocket control;
    QByteArray buffer;
    QJsonObject connection;
    QString run, branch, logDirectory;
    bool active = false, covered = false;
    QSet<QString> ignored;
    QMap<QString, QPointer<QDialog>> pending;
    QList<QJsonObject> records;
};
