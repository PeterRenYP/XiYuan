#include "anomalycontroller.h"
#include "anomalyengine.h"
#include "transport.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

AnomalyController::AnomalyController(QWidget *parent) : QObject(parent), owner(parent) {
    // 主界面只读结果；异常进程失效不会改变生成状态。
    connect(&process, &QProcess::readyReadStandardOutput, this, &AnomalyController::readOutput);
    connect(&process, &QProcess::readyReadStandardError, this, [this] { process.readAllStandardError(); });
    connect(&process, &QProcess::errorOccurred, this, [this] {
        connection = {}; emit ready({}); emit changed(u"异常监控不可用，生成继续"_s);
    });
    connect(&process, &QProcess::finished, this, [this] {
        connection = {}; control.abort(); emit ready({});
        emit changed(u"异常监控已退出，生成继续"_s);
    });
}

AnomalyController::~AnomalyController() {
    // 只关闭本对象拥有的监控进程，退出时不等待模型计算。
    process.kill(); process.waitForFinished(1000);
}

void AnomalyController::start() {
    // 仅启动本对象拥有的独立程序，不为其加载 Python 推理环境。
    if (process.state() != QProcess::NotRunning) return;
    auto path = QCoreApplication::applicationDirPath() + u"/Xiyuan-Monitor.exe"_s;
    if (!QFile::exists(path)) path = Transport::projectRoot() + u"/work/client-cpp/Xiyuan-Monitor.exe"_s;
    process.setProgram(path);
    process.setArguments({u"--parent"_s, QString::number(QCoreApplication::applicationPid())});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    buffer.clear(); connection = {}; process.start();
    emit changed(u"异常监控启动中"_s);
}

void AnomalyController::send(QJsonObject message) {
    // 控制通道只发送用户反馈和结束通知；拥堵时直接降级，不等待写出。
    if (control.state() != QAbstractSocket::ConnectedState || control.bytesToWrite() > 65536) return;
    message[u"run_id"_s] = run; message[u"branch_id"_s] = branch;
    control.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void AnomalyController::readOutput() {
    // 按行解析有界结果缓冲，不在界面线程执行检测算法。
    buffer += process.readAllStandardOutput();
    if (buffer.size() > 131072) { buffer.clear(); emit changed(u"异常监控消息超出上限"_s); return; }
    while (buffer.contains('\n')) {
        const auto end = buffer.indexOf('\n'); const auto line = buffer.left(end); buffer.remove(0, end + 1);
        handle(QJsonDocument::fromJson(line).object());
    }
}

void AnomalyController::clearDialogs() {
    // 生成结束或身份变化使待决问题失效，直接关闭，不触发任何生成命令。
    const auto dialogs = pending; pending.clear();
    for (const auto &dialog : dialogs) if (dialog) dialog->close();
}

void AnomalyController::runEvent(const QJsonObject &message, const QString &projectDirectory) {
    // 同步运行生命周期与用户主动暂停状态，不改变模型执行状态。
    const auto name = message.value(u"name"_s).toString();
    if (name == u"run.started"_s) {
        clearDialogs(); ignored.clear(); records.clear(); active = true; covered = false;
        run = message.value(u"run_id"_s).toString(); branch = message.value(u"branch_id"_s).toString();
        logDirectory = projectDirectory.isEmpty() ? QString() : QDir(projectDirectory).filePath(u"monitor"_s);
        emit changed(connection.isEmpty() ? u"异常监控未连接，本次检测不可用"_s : u"异常监控运行中，统计参考积累中"_s);
    } else if (name == u"run.finished"_s && message.value(u"run_id"_s).toString() == run && message.value(u"branch_id"_s).toString() == branch) {
        // 结束通知先关闭窗口，随后监控只生成记录，避免过期弹窗。
        active = false; clearDialogs();
        const auto payload = message.value(u"payload"_s).toObject();
        send({{u"kind"_s, u"finish"_s}, {u"outcome"_s, payload.value(u"outcome"_s)},
              {u"reason"_s, payload.value(u"reason"_s)}, {u"thinking_complete"_s, payload.value(u"thinking_complete"_s)}});
        emit changed(covered ? u"本次监控结束，异常记录 "_s + QString::number(records.size()) + u" 条"_s : u"本次异常监控未连接，未执行检测"_s);
    } else if (name == u"worker.stopped"_s) {
        active = false; clearDialogs();
        send({{u"kind"_s, u"finish"_s}, {u"outcome"_s, u"failed"_s}, {u"reason"_s, u"worker_exit"_s}});
    } else if (active && name == u"step.waiting"_s) {
        send({{u"kind"_s, u"pause"_s}, {u"waiting"_s, true}});
    } else if (active && (name == u"step.accepted"_s || name == u"run.mode_changed"_s)) {
        send({{u"kind"_s, u"pause"_s}, {u"waiting"_s, false}});
    }
}

void AnomalyController::record(const QJsonObject &event) {
    // 内存与磁盘双重限额，不在生成进程中落盘，也不无限增长日志。
    auto entry = event; entry[u"time"_s] = QDateTime::currentDateTime().toString(Qt::ISODate);
    records.append(entry); while (records.size() > 256) records.removeFirst();
    if (logDirectory.isEmpty()) return;
    QDir().mkpath(logDirectory); const auto path = QDir(logDirectory).filePath(u"anomalies.jsonl"_s);
    if (QFile(path).size() > 2 * 1024 * 1024) { QFile::remove(path + u".old"_s); QFile::rename(path, path + u".old"_s); }
    QFile file(path); if (file.open(QIODevice::Append)) file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
}

void AnomalyController::handle(const QJsonObject &message) {
    // 结果路由、身份校验与提示去重均在界面侧完成。
    const auto name = message.value(u"name"_s).toString(); const auto payload = message.value(u"payload"_s).toObject();
    if (name == u"monitor.ready"_s) {
        connection = payload; control.abort();
        connect(&control, &QTcpSocket::connected, this, [this] {
            control.write(QJsonDocument(QJsonObject{{u"kind"_s, u"hello"_s}, {u"secret"_s, connection.value(u"secret"_s)}}).toJson(QJsonDocument::Compact) + '\n');
        }, Qt::SingleShotConnection);
        control.connectToHost(QHostAddress::LocalHost, quint16(connection.value(u"port"_s).toInt()));
        emit ready(connection); emit changed(u"异常监控就绪"_s); return;
    }
    if (message.value(u"run_id"_s).toString() != run || message.value(u"branch_id"_s).toString() != branch) return;
    if (name == u"anomaly.started"_s) { covered = true; return; }
    if (name == u"anomaly.status"_s) {
        if (!active) return;
        const int count = payload.value(u"valid_tensor_samples"_s).toInt();
        emit changed((payload.value(u"degraded"_s).toBool() ? u"检测覆盖不足"_s : u"异常监控运行中"_s) +
                     u" · 内部样本 "_s + QString::number(count) + u" · 基线指标 "_s + QString::number(payload.value(u"baseline_metrics"_s).toInt()));
        return;
    }
    if (name == u"anomaly.finished"_s) {
        if (payload.value(u"unfinished_thinking"_s).toBool()) record({{u"run_id"_s, run}, {u"branch_id"_s, branch},
            {u"payload"_s, QJsonObject{{u"type"_s, u"unfinished_thinking"_s}, {u"title"_s, anomalyName(u"unfinished_thinking"_s)}}}});
        active = false; clearDialogs(); emit changed(u"本次监控结束，异常记录 "_s + QString::number(records.size()) + u" 条"_s); return;
    }
    if (name != u"anomaly.detected"_s) return;
    record(message);
    const auto type = payload.value(u"type"_s).toString();
    if (!active || ignored.contains(type) || payload.value(u"suppressed"_s).toBool() || pending.contains(type)) return;
    // 非模态提示不暂停生成；只有明确点击停止才提交取消请求。
    auto *dialog = new QDialog(owner); dialog->setObjectName(u"anomalyDialog"_s);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::NonModal); dialog->setModal(false);
    dialog->setWindowTitle(u"生成异常提示"_s); dialog->setMinimumWidth(440);
    auto *layout = new QVBoxLayout(dialog);
    auto *text = new QLabel(u"检测到当前生成可能存在「"_s + anomalyName(type) + u"」问题。"_s);
    text->setWordWrap(true); text->setTextFormat(Qt::PlainText); layout->addWidget(text);
    auto *note = new QLabel(u"生成仍在进行。忽略本次仅关闭当前提示；保持忽略将在本轮生成内不再提示同类异常，下轮恢复。"_s);
    note->setWordWrap(true); layout->addWidget(note);
    auto *buttons = new QHBoxLayout;
    auto *stop = new QPushButton(u"停止生成"_s), *once = new QPushButton(u"忽略本次"_s), *ignore = new QPushButton(u"保持忽略"_s);
    stop->setObjectName(u"anomalyStop"_s); once->setObjectName(u"anomalyIgnoreOnce"_s); ignore->setObjectName(u"anomalyIgnore"_s);
    buttons->addWidget(stop); buttons->addWidget(once); buttons->addWidget(ignore); layout->addLayout(buttons);
    pending[type] = dialog;
    // 固定原运行身份，过期窗口不能停止或忽略新一轮生成。
    const auto dialogRun = run, dialogBranch = branch;
    connect(stop, &QPushButton::clicked, dialog, [this, dialog, dialogRun, dialogBranch] {
        if (active && run == dialogRun && branch == dialogBranch) emit stopRequested(dialogRun, dialogBranch);
        dialog->close();
    });
    connect(once, &QPushButton::clicked, dialog, &QDialog::close);
    connect(ignore, &QPushButton::clicked, dialog, [this, dialog, type, dialogRun, dialogBranch] {
        if (active && run == dialogRun && branch == dialogBranch) { ignored.insert(type); send({{u"kind"_s, u"feedback"_s}, {u"type"_s, type}, {u"ignore"_s, true}}); }
        dialog->close();
    });
    connect(dialog, &QDialog::finished, this, [this, type, dialog] { if (pending.value(type) == dialog) pending.remove(type); });
    // Qt 的屏幕可用区域使用逻辑像素，兼容多屏位置与不同缩放比例。
    dialog->adjustSize(); dialog->show();
    auto *screen = owner ? owner->screen() : QGuiApplication::primaryScreen();
    if (screen) dialog->move(dialog->pos() + screen->availableGeometry().center() - dialog->frameGeometry().center());

}

void AnomalyController::showRecords() {
    // 展示中文证据摘要；英文协议字段不直接出现在用户界面。
    auto *dialog = new QDialog(owner); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"本次生成异常记录"_s); dialog->resize(640, 440);
    auto *layout = new QVBoxLayout(dialog); auto *text = new QTextBrowser;
    QString html = u"<p>内部指标采用轮换采样；未告警不代表所有位置均正常。统计基线不保证样本正常。</p>"_s;
    for (const auto &entry : records) {
        const auto value = entry.value(u"payload"_s).toObject();
        html += u"<p><b>"_s + anomalyName(value.value(u"type"_s).toString()).toHtmlEscaped() + u"</b>　"_s + entry.value(u"time"_s).toString().toHtmlEscaped();
        if (value.contains(u"step"_s)) html += u"<br>生成步："_s + QString::number(value.value(u"step"_s).toInt() + 1);
        if (value.contains(u"layer"_s)) html += u"　层："_s + QString::number(value.value(u"layer"_s).toInt() + 1);
        if (value.contains(u"head"_s)) html += u"　注意力头："_s + QString::number(value.value(u"head"_s).toInt() + 1);
        for (const auto &field : {qMakePair(u"rms"_s,u"采样均方根"_s), qMakePair(u"baseline_rms"_s,u"基线均方根"_s),
             qMakePair(u"entropy"_s,u"归一化注意力熵"_s), qMakePair(u"peak"_s,u"峰值"_s), qMakePair(u"similarity"_s,u"相似度"_s),
             qMakePair(u"elapsed_ms"_s,u"Token耗时（毫秒）"_s), qMakePair(u"baseline_ms"_s,u"基线耗时（毫秒）"_s),
             qMakePair(u"consecutive"_s,u"连续命中次数"_s), qMakePair(u"silent_ms"_s,u"无进展时长（毫秒）"_s)})
            if (value.contains(field.first)) html += u"<br>"_s + field.second + u"："_s + QString::number(value.value(field.first).toDouble(), 'g', 6);
        html += u"</p>"_s;
    }
    if (records.isEmpty()) html += u"<p>本次暂无异常记录。</p>"_s;
    text->setHtml(html); layout->addWidget(text); dialog->show();
}
