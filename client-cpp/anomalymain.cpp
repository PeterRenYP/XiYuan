#include "anomalyengine.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {
    // 独立监控仅启动 QtCore/Network 事件循环，不初始化模型、图形界面或 CUDA。
    QCoreApplication application(argc, argv);
    QElapsedTimer clock; clock.start();
    auto write = [](const QJsonObject &message) {
        const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); std::fflush(stdout);
    };
    AnomalyEngine engine(write);
    const auto secret = QUuid::createUuid().toString(QUuid::Id128);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) return 2;
    int connections = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &application, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            if (++connections > 4) { --connections; socket->abort(); socket->deleteLater(); continue; }
            socket->setReadBufferSize(65536);
            auto buffer = std::make_shared<QByteArray>();
            auto authenticated = std::make_shared<bool>(false);
            auto identity = std::make_shared<QJsonObject>();
            // 未认证连接不允许长期占用槽位。
            QTimer::singleShot(1000, socket, [socket, authenticated] { if (!*authenticated) socket->abort(); });
            QObject::connect(socket, &QTcpSocket::disconnected, &application, [&, socket, identity] {
                --connections;
                if (!identity->isEmpty()) { auto lost = *identity; lost[u"kind"_s] = u"transport_lost"_s; engine.accept(lost, clock.elapsed()); }
                socket->deleteLater();
            });
            QObject::connect(socket, &QTcpSocket::readyRead, &application, [&, socket, buffer, authenticated, identity] {
                buffer->append(socket->readAll());
                if (buffer->size() > 131072) { socket->abort(); return; }
                while (buffer->contains('\n')) {
                    const auto end = buffer->indexOf('\n'); const auto line = buffer->left(end); buffer->remove(0, end + 1);
                    if (line.size() > 32768) { socket->abort(); return; }
                    QJsonParseError error;
                    const auto message = QJsonDocument::fromJson(line, &error).object();
                    if (error.error != QJsonParseError::NoError) { socket->abort(); return; }
                    if (!*authenticated) {
                        if (message.value(u"secret"_s).toString() != secret) { socket->abort(); return; }
                        *authenticated = true;
                        if (message.value(u"kind"_s).toString() == u"hello"_s) continue;
                    }
                    if (message.value(u"kind"_s).toString() == u"start"_s)
                        *identity = {{u"run_id"_s, message.value(u"run_id"_s)}, {u"branch_id"_s, message.value(u"branch_id"_s)}};
                    engine.accept(message, clock.elapsed());
                }
            });
        }
    });
    QTimer timer; timer.setInterval(500);
#ifdef Q_OS_WIN
    const auto args = application.arguments(); const int parentIndex = args.indexOf(u"--parent"_s);
    HANDLE parent = parentIndex >= 0 ? OpenProcess(SYNCHRONIZE, FALSE, args.value(parentIndex + 1).toULong()) : nullptr;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
    QObject::connect(&timer, &QTimer::timeout, &application, [&] {
#ifdef Q_OS_WIN
        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) { application.quit(); return; }
#endif
        engine.tick(clock.elapsed());
    });
    timer.start();
    write({{u"name"_s, u"monitor.ready"_s}, {u"payload"_s, QJsonObject{{u"port"_s, int(server.serverPort())}, {u"secret"_s, secret}}}});
    const auto result = application.exec();
#ifdef Q_OS_WIN
    if (parent) CloseHandle(parent);
#endif
    return result;
}
