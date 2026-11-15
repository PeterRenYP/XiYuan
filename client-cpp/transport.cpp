#include "transport.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUuid>

using namespace Qt::StringLiterals;

Transport::Transport(const QString &mode, QObject *parent) : QObject(parent), mode(mode) {
    // 错误输出进入独立日志，不能污染标准输出上的 NDJSON。
    logRoot = projectRoot() + u"/data/logs"_s;
    connect(&process, &QProcess::readyReadStandardOutput, this, &Transport::readOutput);
    connect(&process, &QProcess::readyReadStandardError, this, &Transport::readLog);
    connect(&process, &QProcess::errorOccurred, this,
            [this] { buffer.clear();transfers.clear();transferIndices.clear();emit failed(u"运行器异常："_s + process.errorString()); });
    connect(&process, &QProcess::finished, this, [this](int code) { buffer.clear();transfers.clear();transferIndices.clear();emit stopped(code); });
}

QString Transport::projectRoot() {
    // 开发目录由启动参数指定；分发版始终优先使用同目录运行器。
    const QString configured = qEnvironmentVariable("MODEL_OBSERVATORY_ROOT");
    if (!configured.isEmpty())
        return configured;
    QDir path(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        if (path.exists(u"main.py"_s))
            return path.absolutePath();
        path.cdUp();
    }
    return QCoreApplication::applicationDirPath();
}

void Transport::start() {
    buffer.clear();transfers.clear();transferIndices.clear();
    // 使用本地离线模式；完整 UTF-8 行解码避免中文跨管道读取时损坏。
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto &pair :
         {qMakePair(u"PYTHONUTF8"_s, u"1"_s), qMakePair(u"PYTHONIOENCODING"_s, u"utf-8"_s),
          qMakePair(u"HF_HUB_OFFLINE"_s, u"1"_s), qMakePair(u"TOKENIZERS_PARALLELISM"_s, u"false"_s)})
        env.insert(pair.first, pair.second);
    process.setProcessEnvironment(env);
    const QString runtime = QCoreApplication::applicationDirPath() + u"/ModelObservatory-Runtime.exe"_s;
    if (QFile::exists(runtime)) {
        process.setProgram(runtime);
        process.setArguments({u"--"_s + mode});
    } else {
        const QString root = projectRoot();
        process.setProgram(root + u"/.venv/Scripts/python.exe"_s);
        process.setArguments({u"-u"_s, root + u"/main.py"_s, u"--"_s + mode});
        process.setWorkingDirectory(root);
    }
    process.start();
}

QString Transport::send(const QString &name, const QJsonObject &payload) {
    // 所有异步查询保留独立请求编号，过期响应由工作台丢弃。
    const QString request = QUuid::createUuid().toString(QUuid::Id128);
    auto actualPayload = payload;
    // 界面与 MCP 都经过同一入口，确保每次生成取得当前监控连接。
    if (mode == u"worker"_s && (name == u"run.start"_s || name == u"run.restore"_s))
        actualPayload[u"anomaly_monitor"_s] = anomalyEndpoint;
    QJsonObject wire{{u"protocol_version"_s, u"1.0"_s},
                     {u"producer_id"_s, u"desktop-cpp"_s},
                     {u"kind"_s, u"command"_s},
                     {u"name"_s, name},
                     {u"message_id"_s, QUuid::createUuid().toString(QUuid::Id128)},
                     {u"request_id"_s, request},
                     {u"run_id"_s, QJsonValue::Null},
                     {u"branch_id"_s, QJsonValue::Null},
                     {u"context"_s, QJsonValue::Null},
                     {u"seq"_s, 0},
                     {u"monotonic_ns"_s, 0},
                     {u"timestamp_utc"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                     {u"payload"_s, actualPayload}};
    process.write(QJsonDocument(wire).toJson(QJsonDocument::Compact) + '\n');
    return request;
}

void Transport::readOutput() {
    // 每帧上限为 1 MiB；只在换行后解析完整 JSON 对象。
    buffer += process.readAllStandardOutput();
    constexpr qsizetype limit = 1024 * 1024;
    if (buffer.size() > limit && !buffer.contains('\n')) {
        emit failed(u"运行器通信帧超过上限"_s);
        process.kill();
        return;
    }
    qsizetype newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (line.trimmed().isEmpty())
            continue;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (line.size() > limit || error.error != QJsonParseError::NoError || !document.isObject() ||
            document.object().value(u"protocol_version"_s).toString() != u"1.0"_s) {
            emit failed(u"运行器通信格式错误"_s);
            continue;
        }
        auto decoded=document.object();
        if(decoded.value(u"name"_s).toString()==u"result.chunk"_s){
            // 严格按序重组有界报告，半份结果不能提前解锁操作。
            const auto p=decoded.value(u"payload"_s).toObject();
            const auto id=p.value(u"transfer_id"_s).toString();
            const int index=p.value(u"index"_s).toInt(-1),total=p.value(u"total"_s).toInt();
            if(id.isEmpty()||total<1||total>172||index!=transferIndices.value(id,0)||index>=total||transfers.size()>4){
                transfers.remove(id);transferIndices.remove(id);emit failed(u"报告分帧顺序无效"_s);continue;
            }
            transfers[id]+=QByteArray::fromBase64(p.value(u"data"_s).toString().toLatin1());
            if(transfers[id].size()>64*1024*1024){transfers.remove(id);transferIndices.remove(id);emit failed(u"报告超过读取上限"_s);continue;}
            transferIndices[id]=index+1;
            if(index+1<total)continue;
            decoded=QJsonDocument::fromJson(transfers.take(id),&error).object();transferIndices.remove(id);
            if(error.error!=QJsonParseError::NoError){emit failed(u"报告重组失败"_s);continue;}
        }
        emit message(decoded);
    }
}

void Transport::readLog() {
    // 每种进程写独立文件，便于定位库日志和模型加载错误。
    QDir().mkpath(logRoot);
    const auto filename = logRoot + u"/"_s + mode + u".log"_s;
    const auto data = process.readAllStandardError().right(1024 * 1024);
    // 每种运行器最多保留两份一兆字节日志，异常也不会无限写盘。
    if (QFileInfo(filename).size() + data.size() > 1024 * 1024) {
        QFile::remove(filename + u".old"_s);
        QFile::rename(filename, filename + u".old"_s);
    }
    QFile log(filename);
    if (log.open(QIODevice::Append))
        log.write(data);
}

void Transport::setStorageRoot(const QString &directory) {
    // 日志随项目存储，系统盘仅保留少量应用偏好。
    logRoot = directory + u"/logs"_s;
}

void Transport::stop() {
    // 先协作退出；超时只结束本客户端拥有的进程树。
    if (process.state() == QProcess::NotRunning)
        return;
    if (mode == u"worker"_s) {
        send(u"shutdown"_s);
        if (process.waitForFinished(2500))
            return;
    }
    process.terminate();
    if (!process.waitForFinished(1000)) {
#ifdef Q_OS_WIN
        QProcess killer;
        killer.start(u"taskkill"_s, {u"/PID"_s, QString::number(process.processId()), u"/T"_s, u"/F"_s});
        killer.waitForFinished(5000);
#endif
        process.kill();
        process.waitForFinished(1000);
    }
}
