#include "mcpbridge.h"
#include "workbench.h"
#include "features.h"
#include "neurons.h"
#include "weightfilewindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QListWidget>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <memory>

using namespace Qt::StringLiterals;

namespace {
constexpr int FrameLimit = 1024 * 1024;

// 延迟到工作台构造完成再挂接，避免修改主入口和对话实现文件。
void startBridge() {
    QTimer::singleShot(0, qApp, [] {
        const auto args = QCoreApplication::arguments();
        if (args.contains(u"--disable-mcp"_s))
            return;
        for (const auto &arg : args)
            if ((arg.startsWith(u"--verify"_s) || arg.startsWith(u"--probe"_s)) && !args.contains(u"--mcp"_s))
                return;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<Workbench *>(widget)) {
                new McpBridge(window);
                break;
            }
    });
}
Q_COREAPP_STARTUP_FUNCTION(startBridge)

// 接口中的所有索引均从零开始，缺省值必须与工作台当前选择一致。
QJsonObject merged(QJsonObject base, const QJsonObject &changes) {
    for (auto it = changes.begin(); it != changes.end(); ++it)
        base[it.key()] = it.value();
    return base;
}
}

McpBridge::McpBridge(Workbench *workbench) : QObject(workbench), window(workbench) {
    // 随机端口与每次启动独立令牌，仅监听本机回环地址。
    token = QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
    instanceId = QUuid::createUuid().toString(QUuid::Id128);
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        qWarning("MCP 本地控制桥启动失败");
        return;
    }
    auto directory = qEnvironmentVariable("MODEL_OBSERVATORY_MCP_DIR");
    if (directory.isEmpty()) {
        auto local = qEnvironmentVariable("LOCALAPPDATA");
        if (local.isEmpty())
            local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        directory = local + u"/ModelObservatory/mcp"_s;
    }
    QDir().mkpath(directory);
    descriptorPath = directory + u"/"_s + QString::number(QCoreApplication::applicationPid()) + u".json"_s;
    QSaveFile file(descriptorPath);
    const QJsonObject descriptor{{u"version"_s, 1}, {u"pid"_s, double(QCoreApplication::applicationPid())},
        {u"instance_id"_s, instanceId}, {u"port"_s, server.serverPort()}, {u"token"_s, token},
        {u"executable"_s, QCoreApplication::applicationFilePath()},
        {u"started_at"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    const auto descriptorData = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(descriptorData) != descriptorData.size() || !file.commit()) {
        server.close();
        qWarning("MCP 连接信息写入失败");
        return;
    }
    QFile::setPermissions(descriptorPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    connectionLabel = new QLabel(u"MCP：可连接"_s, window);
    connectionLabel->setObjectName(u"mcpConnectionStatus"_s);
    connectionLabel->setToolTip(u"本机控制桥已启动，MCP 操作会实时同步到当前工作区"_s);
    window->statusBar()->addPermanentWidget(connectionLabel);
    connect(&server, &QTcpServer::newConnection, this, &McpBridge::acceptConnections);
    connect(window, &Workbench::received, this, &McpBridge::record);
    connect(&window->monitor, &Transport::message, this, [this](const QJsonObject &message) {
        if (message.value(u"name"_s).toString() == u"telemetry.sample"_s)
            setProperty("telemetry", message.value(u"payload"_s).toObject().toVariantMap());
    });
    connect(&window->worker, &Transport::stopped, this, [this](int code) {
        const auto replies = pending;
        pending.clear();
        for (const auto &reply : replies)
            reply(failure(u"运行器已退出："_s + QString::number(code), u"RUNTIME_EXITED"_s));
    });
    connect(&window->worker, &Transport::failed, this, [this](const QString &text) {
        // 通信失败也要结束等待中的请求，不能只在进程退出时清理。
        const auto replies=pending;pending.clear();
        for(const auto &reply:replies)reply(failure(text,u"RUNTIME_FAILED"_s));
    });
}

McpBridge::~McpBridge() {
    // 正常退出立即移除连接信息；异常退出的旧描述符由客户端探测并忽略。
    if (!descriptorPath.isEmpty())
        QFile::remove(descriptorPath);
}

QString McpBridge::localEndpoint() const {
    // 仅展示回环地址和端口，不把连接令牌放进可复制提示词。
    return server.isListening() ? u"127.0.0.1:"_s + QString::number(server.serverPort()) : QString{};
}

QString McpBridge::connectionDirectory() const {
    // 标准管道服务从此目录动态发现实例，重启后不依赖旧端口。
    return QFileInfo(descriptorPath).absolutePath();
}

QJsonObject McpBridge::failure(const QString &text, const QString &code) {
    return {{u"ok"_s, false}, {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message"_s, text}}}};
}

void McpBridge::acceptConnections() {
    // 每条连接只处理一个有界请求，防止慢连接和多帧重放占满界面线程。
    while (server.hasPendingConnections()) {
        auto *socket = server.nextPendingConnection();
        if (findChildren<QTcpSocket *>().size() > 32) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        socket->setReadBufferSize(FrameLimit + 1);
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        auto completed = std::make_shared<bool>(false);
        QPointer<QTcpSocket> guarded(socket);
        Reply reply = [guarded, completed](const QJsonObject &value) {
            if (*completed || !guarded || guarded->state() != QAbstractSocket::ConnectedState)
                return;
            *completed = true;
            auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
            if (data.size() > FrameLimit)
                data = QJsonDocument(failure(u"响应过大，请缩小查询范围"_s, u"RESPONSE_TOO_LARGE"_s)).toJson(QJsonDocument::Compact);
            guarded->write(data + '\n');
            guarded->disconnectFromHost();
        };
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(30000, socket, [reply] { reply(failure(u"控制请求超时"_s, u"TIMEOUT"_s)); });
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled, reply] {
            if (*handled)
                return;
            buffer->append(socket->readAll());
            if (buffer->size() > FrameLimit) {
                *handled = true;
                reply(failure(u"请求超过大小限制"_s, u"FRAME_TOO_LARGE"_s));
                return;
            }
            const auto newline = buffer->indexOf('\n');
            if (newline < 0)
                return;
            *handled = true;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(buffer->left(newline), &error);
            const auto request = document.object();
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                reply(failure(u"请求不是有效 JSON 对象"_s, u"INVALID_JSON"_s));
                return;
            }
            if (request.value(u"token"_s).toString() != token) {
                reply(failure(u"本地控制令牌无效"_s, u"UNAUTHORIZED"_s));
                return;
            }
            if (!request.value(u"params"_s).isObject()) {
                reply(failure(u"参数必须为对象"_s, u"INVALID_PARAMS"_s));
                return;
            }
            const auto method = request.value(u"method"_s).toString();
            if (method == u"state"_s || method == u"events"_s || method == u"telemetry"_s) {
                dispatch(method, request.value(u"params"_s).toObject(), reply);
                return;
            }
            connectionLabel->setText(u"MCP：正在操作"_s);
            dispatch(method, request.value(u"params"_s).toObject(), [this, reply](const QJsonObject &response) {
                // 请求接收、操作提交和最终结果分别可见，连接探测不会覆盖它们。
                const bool ok = response.value(u"ok"_s).toBool();
                const auto data = response.value(u"data"_s).toObject();
                const auto request = data.value(u"request_id"_s).toString();
                const bool submitted = data.value(u"accepted"_s).toBool() && !request.isEmpty();
                if (submitted) {
                    if (externalRequests.size() >= 32) externalRequests.clear();
                    externalRequests.insert(request);
                }
                connectionLabel->setText(!ok ? u"MCP：操作失败"_s :
                                         submitted ? u"MCP：已提交"_s : u"MCP：操作完成"_s);
                if (!ok)
                    connectionLabel->setToolTip(response.value(u"error"_s).toObject().value(u"message"_s).toString());
                else
                    connectionLabel->setToolTip(u"MCP 操作已同步到当前工作区"_s);
                reply(response);
            });
        });
    }
}

QJsonObject McpBridge::state() const {
    // 只返回轻量状态；大历史、模型详情和张量分别查询。
    return {{u"instance_id"_s, instanceId}, {u"pid"_s, double(QCoreApplication::applicationPid())},
        {u"project_directory"_s, window->projectDirectory}, {u"project_name"_s, window->project.value(u"name"_s)},
        {u"session_id"_s, window->sessionId}, {u"model_path"_s, window->modelPath},
        {u"loaded"_s, window->loaded}, {u"loading"_s, window->loading}, {u"active"_s, window->active},
        {u"editing"_s, window->editing}, {u"offline"_s, window->offline},
        {u"mode"_s, window->mode}, {u"status_text"_s, window->statusLabel->text()},
        {u"run_id"_s, window->runId}, {u"weights_revision"_s, window->weightsRevision},
        {u"model_revision"_s, window->info.value(u"model_revision"_s)},
        {u"boundary"_s, window->boundary}, {u"selection"_s, window->selection()},
        {u"trace_path"_s, window->tracePath}, {u"checkpoint_path"_s, window->checkpointPath},
        {u"last_backup"_s, window->lastBackup}, {u"message_count"_s, window->messages.size()},
        {u"thinking"_s, window->thinkingMode->isChecked()},
        {u"temperature"_s, window->temperature->value()}, {u"thinking_temperature"_s, window->thinkingTemperature->value()},
        {u"max_tokens"_s, window->maximum->value()}, {u"max_tokens_limit"_s, window->maximum->maximum()},
        {u"running_mode"_s, window->runningMode}, {u"session_count"_s, window->sessionList->count()},
        {u"conversation_visible"_s, window->conversationWindow->isVisible()},
        {u"conversation_minimized"_s, window->conversationWindow->isMinimized()},
        {u"neuron_visible"_s, window->neurons->isVisible()},
        {u"neuron_selection"_s, window->neurons->selection()},
        {u"neuron_active_beams"_s, window->neurons->isVisible() ? window->neurons->canvas->activeBeamCount() : 0},
        {u"edit_rule_count"_s, window->rules->rowCount()},
        {u"file_workspace"_s, window->fileWindow?window->fileWindow->state():QJsonObject{}},
        {u"debug_points"_s,window->debugPoints},
        {u"capabilities"_s,QJsonArray{u"activation"_s,u"vectors"_s,u"versions"_s,u"rules_draft"_s,u"comparison"_s,u"regression"_s,u"debugging"_s,u"weight_files"_s}},
        {u"event_cursor"_s, double(cursor)}};
}

QJsonObject McpBridge::accepted(const QString &request) const {
    return {{u"ok"_s, true}, {u"data"_s, QJsonObject{{u"accepted"_s, true},
        {u"request_id"_s, request}, {u"event_cursor"_s, double(cursor)}, {u"state"_s, state()}}}};
}

QString McpBridge::idleError(bool requireModel) const {
    if (window->closing)
        return u"客户端正在关闭"_s;
    if (window->active || window->loading || window->editing || window->restoringSession)
        return u"请先等待当前任务结束，或停止运行后重试"_s;
    if (requireModel && (!window->loaded || window->offline))
        return u"请先加载本地模型"_s;
    return {};
}

void McpBridge::runtimeQuery(const QString &name, const QJsonObject &payload, const Reply &reply) {
    // 响应按请求编号独立分发，不占用工作台自身的快照请求编号。
    if (!window->processesStarted || pending.size() >= 16) {
        reply(failure(u"运行器尚未启动或查询数量过多"_s, u"RUNTIME_UNAVAILABLE"_s));
        return;
    }
    const auto id = window->worker.send(name, payload);
    // MCP 读取与界面按钮使用同一结果控件，请求编号阻止旧结果覆盖新切片。
    if (name == u"tensor.slice"_s || name == u"scalar.query"_s) {
        window->changeView(0);
        window->inspector->setCurrentIndex(1);
        if (name == u"tensor.slice"_s) window->sliceId = id;
        else window->scalarId = id;
    }
    pending[id] = reply;
    QTimer::singleShot(20000, this, [this, id] {
        if (pending.contains(id))
            pending.take(id)(failure(u"数据查询超时"_s, u"TIMEOUT"_s));
    });
}

void McpBridge::record(const QJsonObject &message) {
    // 查询结果直接返回；事件环按字节和数量双重限额，游标溢出明确告知调用方。
    const auto id = message.value(u"request_id"_s).toString();
    if(message.value(u"name"_s).toString()==u"file.result"_s||message.value(u"name"_s).toString()==u"feature.result"_s||message.value(u"kind"_s).toString()==u"error"_s){
        window->featureReports[id]=message;
        window->featureReportOrder.removeAll(id);window->featureReportOrder.append(id);
        while(window->featureReportOrder.size()>16)window->featureReports.remove(window->featureReportOrder.takeFirst());
    }
    if (pending.contains(id)) {
        const auto reply = pending.take(id);
        if (message.value(u"kind"_s).toString() == u"error"_s)
            reply({{u"ok"_s, false}, {u"error"_s, message.value(u"payload"_s)}});
        else
            reply({{u"ok"_s, true}, {u"data"_s, message.value(u"payload"_s)}});
        return;
    }
    const auto name = message.value(u"name"_s).toString();
    if (externalRequests.contains(id) &&
        (message.value(u"kind"_s).toString() == u"error"_s ||
         name == u"feature.result"_s || name == u"file.result"_s || name == u"edit.applied"_s ||
         name == u"run.finished"_s || name == u"model.ready"_s || name == u"model.exported"_s ||
         name == u"trace.opened"_s || name == u"trace.exported"_s || name == u"trace.closed"_s ||
         name == u"session.activated"_s || name == u"model.cancelled"_s || name == u"step.released"_s)) {
        externalRequests.remove(id);
        connectionLabel->setText(message.value(u"kind"_s).toString() == u"error"_s ?
                                 u"MCP：操作失败"_s : u"MCP：操作完成"_s);
    }
    // 生成事件使用运行编号，结束时让状态栏跟上实际执行结果。
    if (name == u"run.started"_s && externalRequests.remove(window->actionId))
        externalRunId = message.value(u"run_id"_s).toString();
    if (name == u"run.finished"_s && !externalRunId.isEmpty() &&
        message.value(u"run_id"_s).toString() == externalRunId) {
        externalRunId.clear();
        const auto outcome = message.value(u"payload"_s).toObject().value(u"outcome"_s).toString();
        connectionLabel->setText(outcome == u"failed"_s ? u"MCP：运行失败"_s :
                                 outcome == u"cancelled"_s ? u"MCP：运行已停止"_s : u"MCP：运行完成"_s);
    }
    if (name == u"run.started"_s) {
        sampledTokens = {};
        sampledTracePath = window->tracePath;
    } else if (name == u"token.sampled"_s) {
        // 工作台的 tokens 只保存输入token，桥另存本次生成token以支持完整位置查询。
        auto sample = message.value(u"payload"_s).toObject();
        sample.remove(u"candidates"_s);
        sample[u"generated"_s] = true;
        sampledTokens.append(sample);
        return;
    }
    if (!(name.startsWith(u"run."_s) || name.startsWith(u"model."_s) || name.startsWith(u"text."_s) ||
          name.startsWith(u"step."_s) || name.startsWith(u"edit."_s) || name.startsWith(u"branch."_s) ||
          name.startsWith(u"checkpoint."_s) || name.startsWith(u"session."_s) || name.startsWith(u"trace."_s) ||
          name.startsWith(u"feature."_s)||name.startsWith(u"file."_s)||name.startsWith(u"debug."_s)||
          message.value(u"kind"_s).toString() == u"error"_s))
        return;
    auto event = message;
    event[u"cursor"_s] = double(++cursor);
    auto bytes = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
    if (bytes > 256 * 1024) {
        event[u"payload"_s] = QJsonObject{{u"truncated"_s, true}, {u"message"_s, u"事件过大，请使用详情查询"_s}};
        bytes = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
    }
    events.append(event);
    eventBytes += bytes;
    while (events.size() > 512 || eventBytes > 2 * FrameLimit) {
        eventBytes -= QJsonDocument(events.first().toObject()).toJson(QJsonDocument::Compact).size();
        events.removeFirst();
    }
}

QJsonObject McpBridge::selectObservation(const QJsonObject &params, bool changeCapture) {
    // 算子检查器与神经元页共用目标身份，先验证参数再切换可见页面。
    const auto current = window->selection();
    const auto neuronView = window->neurons->selection();
    const auto config = window->info.value(u"config"_s).toObject();
    const bool showNeuron = params.contains(u"neuron_index"_s);
    const bool useNeuron = showNeuron && window->neurons->isVisible();
    const int layer = params.value(u"layer_index"_s).toInt(
        useNeuron ? neuronView.value(u"layer_index"_s).toInt() : window->layer);
    const int neuron = params.value(u"neuron_index"_s).toInt(window->neuron->value());
    auto operation = params.value(u"operation"_s).toString(window->operation);
    const int position = params.value(u"token_position"_s).toInt(window->tokenPosition->value());
    const int dimension = params.value(u"dimension"_s).toInt(window->dimension->value());
    const auto types = config.value(u"layer_types"_s).toArray();
    const bool linear = layer >= 0 && layer < types.size() && types[layer].toString() == u"linear_attention"_s;
    const int heads = config.value(linear ? u"linear_num_value_heads"_s : u"num_attention_heads"_s).toInt(1);
    const int head = params.value(u"head"_s).toInt(std::clamp(current.value(u"head"_s).toInt(), 0, std::max(0, heads - 1)));
    bool foundOperation = operation == u"embed"_s || operation == u"final_norm"_s || operation == u"logits"_s;
    if (layer >= 0 && layer < window->catalog.size())
        for (const auto &value : window->catalog[layer].toArray()) {
            const auto entry = value.toArray();
            foundOperation |= !entry.isEmpty() && entry[0].toString() == operation;
        }
    if (!foundOperation && !params.contains(u"operation"_s)) {
        operation = u"norm"_s;
        foundOperation = true;
    }
    if (window->loading || window->editing || (!window->loaded && !window->offline) ||
        layer < 0 || layer >= config.value(u"num_hidden_layers"_s).toInt() || !foundOperation ||
        neuron < 0 || neuron >= config.value(u"intermediate_size"_s).toInt() ||
        dimension < 0 || dimension >= config.value(u"hidden_size"_s).toInt() ||
        position < 0 || (changeCapture && position > window->tokenPosition->maximum()) || head < 0 || head >= heads)
        return failure(u"观测位置无效或模型尚未就绪"_s, u"INVALID_SELECTION"_s);

    bool positionChanged = params.contains(u"token_position"_s);
    if (params.contains(u"branch_id"_s)) {
        const int index = window->branchSelect->findData(params.value(u"branch_id"_s).toString());
        if (index >= 0 && window->branchSelect->currentIndex() != index) {
            window->branchSelect->setCurrentIndex(index);
            positionChanged = true;
        }
    }
    if (params.contains(u"forward_id"_s)) {
        const int index = window->forwardSelect->findData(params.value(u"forward_id"_s).toString());
        if (index >= 0 && window->forwardSelect->currentIndex() != index) {
            window->forwardSelect->setCurrentIndex(index);
            positionChanged = true;
        }
    }
    window->follow->setChecked(false);
    window->choose(layer, operation, changeCapture && layer != window->layer);
    {
        QSignalBlocker n(window->neuron), t(window->tokenPosition), d(window->dimension), h(window->headSelect);
        window->neuron->setValue(neuron);
        if (params.contains(u"token_position"_s))
            window->tokenPosition->setValue(position);
        window->dimension->setValue(dimension);
        window->headSelect->setCurrentIndex(window->headSelect->findData(head));
    }
    if (showNeuron) {
        window->showNeurons();
        window->neurons->context(window->branch, window->forward, window->tokenPosition->value());
        if (!useNeuron || !neuronView.value(u"has_selection"_s).toBool() ||
            neuronView.value(u"layer_index"_s).toInt() != layer ||
            neuronView.value(u"neuron_index"_s).toInt() != neuron)
            window->neurons->choose(layer, neuron, true);
        else if (positionChanged)
            window->queryNeurons();
    } else if (window->workspaceViews->currentWidget() != window->mainSplit ||
               window->centerViews->currentIndex() == 2) {
        window->changeView(0);
    }
    if (window->isMinimized()) window->showNormal();
    window->raise();
    window->activateWindow();
    window->dirty = true;
    return window->selection();
}

void McpBridge::dispatch(const QString &method, const QJsonObject &p, const Reply &reply) {
    auto result = [&reply](const QJsonValue &value) { reply({{u"ok"_s, true}, {u"data"_s, value}}); };
    if(method==u"feature.result"_s){
        const auto id=p.value(u"request_id"_s).toString();
        if(!window->featureReports.contains(id)){reply(failure(u"结果尚未完成或已过期，请读取事件及状态"_s,u"RESULT_UNAVAILABLE"_s));return;}
        const auto message=window->featureReports.value(id);
        if(message.value(u"kind"_s).toString()==u"error"_s){reply({{u"ok"_s,false},{u"error"_s,message.value(u"payload"_s)}});return;}
        auto data=message.value(u"payload"_s).toObject();
        const auto field=p.value(u"field"_s).toString();
        if(!field.isEmpty()){
            // 支持 rows.0.slice.before 等路径，逐步查看大型对照报告。
            QJsonValue value=data;
            for(const auto &part:field.split(QChar(u'.'))){
                bool indexed=false;const int index=part.toInt(&indexed);
                if(value.isArray()&&indexed&&index>=0&&index<value.toArray().size())value=value.toArray()[index];
                else if(value.isObject()&&value.toObject().contains(part))value=value.toObject().value(part);
                else{reply(failure(u"结果不包含所选字段"_s,u"INVALID_FIELD"_s));return;}
            }
            if(value.isArray()){QJsonArray page;const auto rows=value.toArray();const int offset=std::max(0,p.value(u"offset"_s).toInt());const int limit=std::clamp(p.value(u"limit"_s).toInt(20),1,100);for(int i=offset;i<std::min(int(rows.size()),offset+limit);++i)page.append(rows[i]);data={{u"items"_s,page},{u"total"_s,rows.size()},{u"offset"_s,offset}};}
            else data={{field,value}};
        }else for(auto it=data.begin();it!=data.end();++it)if(it.value().isArray()&&it.value().toArray().size()>20)it.value()=QJsonObject{{u"total"_s,it.value().toArray().size()},{u"message"_s,u"请指定字段分页读取"_s}};
        if(QJsonDocument(data).toJson(QJsonDocument::Compact).size()>700*1024){reply(failure(u"结果过大，请指定字段并缩小分页数量"_s,u"RESULT_TOO_LARGE"_s));return;}
        result(data);return;
    }
    if(method.startsWith(u"file."_s)){
        const QSet<QString> allowed{u"file.open"_s,u"file.list"_s,u"file.slice"_s,u"file.preview"_s,u"file.apply"_s,u"file.compare"_s,u"file.restore"_s};
        if(!allowed.contains(method)){reply(failure(u"未知的文件操作"_s));return;}
        if(window->fileWindow&&window->fileWindow->busy()){reply(failure(u"文件操作尚未完成"_s,u"BUSY"_s));return;}
        if(!window->fileWindow){
            if(method!=u"file.open"_s){reply(failure(u"请先打开权重文件"_s));return;}
            window->fileWindow=new WeightFileWindow(window);
            connect(window->fileWindow,&WeightFileWindow::received,window,&Workbench::received);
        }
        window->fileWindow->show();
        const auto id=window->fileWindow->submit(method,p,true);
        if(id.isEmpty()){reply(failure(u"文件写入未提交；若当前模型已在项目中加载，请从项目修改或关闭模型后重试"_s,u"FILE_BUSY"_s));return;}
        reply(accepted(id));return;
    }
    if(method==u"debug.configure"_s||method==u"debug.continue"_s){
        if(!window->loaded||window->offline){reply(failure(u"请先在项目中加载模型"_s));return;}
        if(method==u"debug.configure"_s)runtimeQuery(method,p,reply);
        else{window->boundary={};const auto id=window->worker.send(u"run.change_mode"_s,{{u"mode"_s,u"observe"_s}});reply(accepted(id));}
        return;
    }
    if(method.startsWith(u"feature."_s)||method==u"rules.draft"_s){
        const auto action=method==u"rules.draft"_s?method:method.mid(8);
        const QSet<QString> allowed{u"search"_s,u"vector"_s,u"similar"_s,u"vector.preview"_s,u"vector.apply"_s,u"vocabulary.preview"_s,u"vocabulary.apply"_s,
            u"activation"_s,u"activation.detail"_s,u"activation.query"_s,u"activation.open"_s,u"versions"_s,u"versions.action"_s,u"versions.export"_s,u"versions.preview"_s,u"versions.apply"_s,
            u"rules.preview"_s,u"rules.slice"_s,u"rules.apply"_s,u"rules.draft"_s,u"comparison"_s,u"regression"_s,u"export.verify"_s,u"reports"_s,u"report.read"_s,
            u"samples.read"_s,u"samples.save"_s,u"drafts.read"_s,u"drafts.save"_s};
        if(!allowed.contains(action)){reply(failure(u"未知的功能操作"_s));return;}
        const bool traceOnly=action.startsWith(u"activation"_s)||action==u"reports"_s||action==u"report.read"_s;
        if(const auto error=idleError(!traceOnly);!error.isEmpty()){reply(failure(error));return;}
        if(!window->processesStarted){reply(failure(u"运行器尚未启动"_s));return;}
        // 切换到功能所在页面，使用同一个结果处理器更新表格和状态。
        if (action.startsWith(u"activation"_s))
            window->openFeature(0, false);
        else if (action.startsWith(u"versions"_s) || action.startsWith(u"drafts."_s))
            window->openFeature(2, false);
        else if (action == u"regression"_s || action == u"export.verify"_s ||
                 action.startsWith(u"samples."_s) || action == u"reports"_s || action == u"report.read"_s)
            window->openFeature(3, false);
        else if (action.startsWith(u"rules."_s)) {
            window->changeView(0);
            window->inspector->setCurrentIndex(2);
        } else if (action == u"comparison"_s) {
            window->changeView(0);
            window->timelineTabs->setCurrentIndex(1);
        } else {
            window->openFeature(1, false);
        }
        auto payload=p;payload[u"action"_s]=action;payload[u"project_root"_s]=window->projectDirectory;
        if(!payload.contains(u"branch_id"_s))payload[u"branch_id"_s]=window->branch;
        // 激活分析也按页返回，避免全量结果超出通信帧；运行身份与界面查询保持一致。
        if(action==u"activation"_s)payload[u"paged"_s]=true;
        if(action==u"activation"_s||action==u"activation.query"_s||action==u"activation.open"_s)
            payload[u"client_revision"_s]=++window->features->activationRevision;
        // 预览窗口携带与 MCP 相同的参数和版本，后续提交仍由已有按钮或 MCP 发起。
        if (action.endsWith(u".preview"_s)) {
            payload[u"expected_weights_revision"_s] = window->weightsRevision;
            window->features->previewRequest = payload;
        }
        if (action == u"rules.preview"_s) {
            window->pendingRulePreview = payload;
            window->editing = true;
            window->setActive(false);
            window->ruleRequestId = window->worker.send(u"feature.request"_s, payload);
            reply(accepted(window->ruleRequestId));
            return;
        }
        window->editing=true;window->setActive(false);
        window->features->pending(true);
        window->featureRequestId=window->worker.send(u"feature.request"_s,payload);reply(accepted(window->featureRequestId));return;
    }
    if (method == u"state"_s) {
        result(state());
        return;
    }
    if (method == u"model.info"_s) {
        result(window->info);
        return;
    }
    if (method == u"telemetry"_s) {
        result(QJsonObject::fromVariantMap(property("telemetry").toMap()));
        return;
    }
    if (method == u"history"_s || method == u"tokens"_s) {
        const bool history = method == u"history"_s;
        auto source = history ? window->messages : window->tokens;
        if (method == u"tokens"_s && !sampledTracePath.isEmpty() && sampledTracePath == window->tracePath)
            for (const auto &sample : sampledTokens)
                source.append(sample);
        const auto offset = p.value(u"offset"_s).toInt(0);
        const auto limit = p.value(u"limit"_s).toInt(20);
        if (offset < 0 || limit < 1 || limit > 100) {
            reply(failure(u"分页参数无效"_s, u"INVALID_PARAMS"_s));
            return;
        }
        QJsonArray page;
        qsizetype pageBytes = 0;
        for (int i = offset; i < source.size() && page.size() < limit; ++i) {
            auto item = source[i].toObject();
            // 长回复的详细 Token 数据通过 Token 工具读取，避免一条消息撑满协议帧。
            auto size = QJsonDocument(item).toJson(QJsonDocument::Compact).size();
            if (history && size > 512 * 1024) {
                const auto tokens = item.take(u"output_tokens"_s).toArray();
                if (!tokens.isEmpty()) item[u"output_token_count"_s] = tokens.size();
                size = QJsonDocument(item).toJson(QJsonDocument::Compact).size();
            }
            if (!page.isEmpty() && pageBytes + size > 600 * 1024) break;
            page.append(item);
            pageBytes += size;
        }
        const auto rendered = window->chat->toPlainText();
        result(QJsonObject{{u"items"_s, page}, {u"total"_s, source.size()},
            {u"next_offset"_s, offset + page.size()}, {u"active"_s, window->active},
            {u"text"_s, window->assistantText}, {u"reasoning_content"_s, window->assistantThought},
            {u"rendered_text"_s, rendered.right(32768)},
            {u"rendered_text_truncated"_s, rendered.size() > 32768},
            {u"thinking_complete"_s, window->responseThinkingComplete}});
        return;
    }
    if (method == u"sessions"_s) {
        result(QJsonObject{{u"items"_s, window->project.value(u"sessions"_s)}, {u"current"_s, window->sessionId}});
        return;
    }
    if (method == u"rules"_s) {
        result(QJsonObject{{u"rules"_s, window->temporaryRules}, {u"weights_revision"_s, window->weightsRevision},
            {u"draft"_s,window->ruleDraft()},{u"applied"_s,window->appliedRuleDraft},
            {u"last_backup"_s, window->lastBackup}});
        return;
    }
    if (method == u"events"_s) {
        const auto after = p.value(u"after_cursor"_s).toInteger(0);
        const auto limit = p.value(u"limit"_s).toInt(50);
        if (after < 0 || after > cursor || limit < 1 || limit > 100) {
            reply(failure(u"事件游标或分页数量无效，请重新查询状态"_s, u"INVALID_CURSOR"_s));
            return;
        }
        const auto first = events.isEmpty() ? cursor + 1 : events.first().toObject().value(u"cursor"_s).toInteger();
        QJsonArray page;
        qint64 next = after, bytes = 0;
        for (const auto &value : events) {
            const auto event = value.toObject();
            if (event.value(u"cursor"_s).toInteger() <= after)
                continue;
            const auto size = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
            if (page.size() >= limit || bytes + size > 768 * 1024)
                break;
            bytes += size;
            page.append(event);
            next = event.value(u"cursor"_s).toInteger();
        }
        result(QJsonObject{{u"items"_s, page}, {u"next_cursor"_s, double(next)},
            {u"latest_cursor"_s, double(cursor)}, {u"dropped"_s, after < first - 1}, {u"instance_id"_s, instanceId}});
        return;
    }
    if (method == u"view.query"_s || method == u"scalar.query"_s || method == u"tensor.slice"_s) {
        if (method != u"tensor.slice"_s && (!window->loaded && !window->offline)) {
            reply(failure(u"请先加载模型或打开观测记录"_s));
            return;
        }
        if (method != u"tensor.slice"_s) {
            const auto selection = selectObservation(p, false);
            if (selection.contains(u"error"_s)) {
                reply(selection);
                return;
            }
        }
        runtimeQuery(method, method == u"tensor.slice"_s ? p : merged(window->selection(), p), reply);
        return;
    }
    if (method == u"view.select"_s) {
        const auto selection = selectObservation(p);
        if (selection.contains(u"error"_s)) reply(selection);
        else result(selection);
        return;
    }
    if (method == u"neurons.show"_s) {
        if (window->loading || window->editing || (!window->loaded && !window->offline)) {
            reply(failure(u"请先加载模型或打开观测记录"_s));
            return;
        }
        const auto selection = p.value(u"global"_s).toBool() ? QJsonObject{} : p;
        if (selection.contains(u"layer_index"_s) || selection.contains(u"neuron_index"_s)) {
            auto target = selection;
            target[u"neuron_index"_s] = selection.value(u"neuron_index"_s).toInt(window->neuron->value());
            const auto selected = selectObservation(target, false);
            if (selected.contains(u"error"_s)) { reply(selected); return; }
        } else {
            window->showNeurons();
            if (window->isMinimized()) window->showNormal();
            window->raise();
            window->activateWindow();
        }
        if (p.value(u"global"_s).toBool())
            window->neurons->clearSelection();
        if (p.contains(u"live"_s) && window->loaded && !window->offline)
            window->neurons->setLive(p.value(u"live"_s).toBool());
        result(state());
        return;
    }
    if (method == u"run.cancel"_s) {
        if (!window->processesStarted) {
            reply(failure(u"运行器尚未启动"_s));
            return;
        }
        window->cancel();
        reply(accepted());
        return;
    }
    if (method == u"run.mode"_s) {
        const auto mode = p.value(u"mode"_s).toString();
        if (mode != u"normal"_s && mode != u"observe"_s && mode != u"step"_s) {
            reply(failure(u"运行模式无效"_s, u"INVALID_PARAMS"_s));
            return;
        }
        window->setMode(mode);
        if (!window->projectDirectory.isEmpty() &&
            (!window->conversationWindow->isVisible() || window->conversationWindow->isMinimized())) {
            // 外部操作沿用用户选择的嵌入或独立呈现方式。
            if(window->conversationEmbedded) window->placeConversation(); else window->showConversation();
        }
        reply(accepted());
        return;
    }
    if (method == u"run.step"_s) {
        if(window->boundary.isEmpty()){reply(failure(u"当前没有暂停边界"_s));return;}
        const auto id=window->worker.send(u"step.release"_s,{{u"expected_boundary_id"_s,p.value(u"boundary_id"_s)},{u"expected_state_revision"_s,p.value(u"state_revision"_s)}});
        window->boundary={};window->setActive(true);reply(accepted(id));
        return;
    }
    if (method == u"project.save"_s && window->active) {
        if (window->projectDirectory.isEmpty() || window->loading || window->editing) {
            reply(failure(u"当前不能保存项目"_s));
            return;
        }
        // 运行中的保存必须走安全计算边界，不能直接序列化不稳定现场。
        window->saveProject(true);
        reply(accepted());
        return;
    }
    const bool needsModel = method == u"run.send"_s || method == u"edit.apply"_s || method == u"edit.clear"_s ||
        method == u"model.export"_s || method == u"model.restore_backup"_s || method == u"run.resume"_s;
    if (const auto error = idleError(needsModel); !error.isEmpty()) {
        reply(failure(error, u"BUSY_OR_UNAVAILABLE"_s));
        return;
    }
    if(method==u"session.rename"_s){
        const auto id=p.value(u"session_id"_s).toString(),title=p.value(u"title"_s).toString().trimmed();
        if(title.isEmpty()){reply(failure(u"主题名不能为空"_s));return;}
        auto entries=window->project.value(u"sessions"_s).toArray();bool found=false;
        const auto previous=window->project;
        for(int i=0;i<entries.size();++i){auto entry=entries[i].toObject();if(entry.value(u"id"_s).toString()==id){entry[u"title"_s]=title;entries[i]=entry;found=true;break;}}
        if(!found){reply(failure(u"会话不存在"_s));return;}
        window->project[u"sessions"_s]=entries;
        if(!window->writeProjectState(false)){window->project=previous;reply(failure(u"主题名保存失败"_s));return;}
        window->refreshSessions();reply(accepted());return;
    }else if(method==u"session.clear"_s||method==u"session.delete"_s){
        if(window->sessionId.isEmpty()||window->projectDirectory.isEmpty()){reply(failure(u"当前没有会话"_s));return;}
        const bool remove=method==u"session.delete"_s;
        window->pendingSessionId=remove?window->sessionId:QString{};
        window->pendingSession={{u"cleanup_path"_s,window->sessionDirectory()},{u"remove_session"_s,remove},{u"delete_records"_s,p.value(u"delete_records"_s).toBool(true)}};
        const auto id=window->worker.send(u"trace.close"_s);reply(accepted(id));return;
    }else if (method == u"project.create"_s || method == u"project.open"_s) {
        const auto path = p.value(u"directory"_s).toString();
        if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
            reply(failure(u"请提供项目目录的绝对路径"_s, u"INVALID_PARAMS"_s));
            return;
        }
        const bool success = method == u"project.create"_s
            ? window->createProjectAt(path, p.value(u"name"_s).toString(), p.value(u"model_path"_s).toString(),
                p.value(u"max_tokens"_s).toInt(0), p.value(u"device"_s).toString(u"cuda:0"_s))
            : window->openProjectAt(path, window->runtimeEnabled);
        reply(success ? accepted(window->actionId) : failure(window->statusLabel->text(), u"PROJECT_FAILED"_s));
    } else if (method == u"project.save"_s) {
        if (window->projectDirectory.isEmpty()) {
            reply(failure(u"请先打开项目"_s));
            return;
        }
        reply(window->writeProjectState(true) ? accepted() : failure(window->statusLabel->text(), u"SAVE_FAILED"_s));
    } else if (method == u"session.new"_s || method == u"session.switch"_s) {
        if (window->projectDirectory.isEmpty()) {
            reply(failure(u"请先打开项目"_s));
            return;
        }
        const auto previous = window->sessionId;
        if (method == u"session.new"_s) {
            window->newSession();
        } else {
            const auto id = p.value(u"session_id"_s).toString();
            bool found = false;
            for (const auto &entry : window->project.value(u"sessions"_s).toArray())
                found |= entry.toObject().value(u"id"_s).toString() == id;
            if (!found) {
                reply(failure(u"会话不存在"_s, u"SESSION_NOT_FOUND"_s));
                return;
            }
            if (previous == id) {
                reply(accepted());
                return;
            }
            window->activateSession(id);
        }
        if (previous != window->sessionId) {
            // 外部操作沿用用户选择的嵌入或独立呈现方式。
            if(window->conversationEmbedded) window->placeConversation(); else window->showConversation();
            reply(accepted(window->sessionActivationId));
        } else {
            reply(failure(window->statusLabel->text(), u"SESSION_FAILED"_s));
        }
    } else if (method == u"model.load"_s) {
        const auto path = p.value(u"path"_s).toString();
        if (window->projectDirectory.isEmpty() || !QDir::isAbsolutePath(path) || !QFile::exists(path + u"/config.json"_s)) {
            reply(failure(u"请先打开项目，并提供含 config.json 的模型绝对路径"_s, u"INVALID_PARAMS"_s));
            return;
        }
        if (QDir(path).canonicalPath() == QDir(window->modelPath).canonicalPath())
            window->pendingSession = window->captureSession();
        else {
            window->newSession(false);
            window->pendingSession = {};
        }
        window->loadModel(path, p.value(u"device"_s).toString(u"cuda:0"_s));
        reply(accepted(window->actionId));
    } else if (method == u"run.send"_s) {
        const auto text = p.value(u"text"_s).toString();
        const int maximum = p.value(u"max_tokens"_s).toInt(window->maximum->value());
        const bool thinking = p.value(u"thinking"_s).toBool(window->thinkingMode->isChecked());
        const auto temperature = p.value(u"temperature"_s).toDouble(thinking ? window->thinkingTemperature->value() : window->temperature->value());
        if (window->projectDirectory.isEmpty() || text.trimmed().isEmpty() || text.size() > 65536 ||
            maximum < 1 || maximum > window->maximum->maximum() || !std::isfinite(temperature) || temperature < 0 || temperature > 2 ||
            (thinking && window->info.value(u"capabilities"_s).toObject().value(u"thinking_mode"_s).toString() != u"supported"_s)) {
            reply(failure(u"消息、生成参数或思考模式无效，请检查模型能力"_s, u"INVALID_PARAMS"_s));
            return;
        }
        (thinking ? window->thinkingTemperature : window->temperature)->setValue(temperature);
        window->thinkingMode->setChecked(thinking);
        if (!window->conversationWindow->isVisible() || window->conversationWindow->isMinimized()) {
            // 外部操作沿用用户选择的嵌入或独立呈现方式。
            if(window->conversationEmbedded) window->placeConversation(); else window->showConversation();
        }
        // MCP 的显式上限仅作用本次请求，不覆盖界面保存的默认值。
        window->runPrompt(text, maximum);
        reply(window->active ? accepted(window->actionId) : failure(window->statusLabel->text()));
    } else if (method == u"run.resume"_s) {
        window->resumeSavedRun();reply(window->active?accepted():failure(u"当前没有可恢复的运行现场"_s));
        return;
    } else if (method == u"edit.apply"_s || method == u"edit.clear"_s || method == u"model.restore_backup"_s) {
        // 修改必须携带调用方读到的版本，后端还会完整校验规则并生成权重备份。
        if (p.value(u"expected_model_revision"_s).toString() != window->info.value(u"model_revision"_s).toString()) {
            reply(failure(u"当前模型已变化，请重新读取模型状态"_s, u"STALE_MODEL"_s));
            return;
        }
        if (p.value(u"expected_weights_revision"_s).toString() != window->weightsRevision) {
            reply(failure(u"模型版本已变化，请重新读取状态"_s, u"STALE_WEIGHTS"_s));
            return;
        }
        if (method == u"edit.apply"_s && (!p.value(u"rules"_s).isArray() || p.value(u"rules"_s).toArray().isEmpty() ||
            p.value(u"rules"_s).toArray().size() > 128 ||
            (p.value(u"target"_s).toString() != u"temporary"_s && p.value(u"target"_s).toString() != u"model"_s))) {
            reply(failure(u"请选择编辑目标，并提供 1 至 128 条规则"_s, u"INVALID_PARAMS"_s));
            return;
        }
        window->changeView(0);
        window->inspector->setCurrentIndex(2);
        window->editing = true;
        window->externalEditPending = true;
        window->setActive(false);
        window->actionId = window->worker.send(method, p);
        reply(accepted(window->actionId));
    } else if (method == u"model.export"_s || method == u"trace.export"_s || method == u"trace.open"_s) {
        if (!window->processesStarted || (method != u"model.export"_s && method != u"trace.open"_s && window->tracePath.isEmpty())) {
            reply(failure(u"尚无运行器或可导出的记录"_s));
            return;
        }
        // 离线打开和导出也占用串行运行器，完成前禁止开始下一项修改。
        if (method == u"trace.open"_s)
            window->changeView(0);
        window->editing = true;
        window->setActive(false);
        window->actionId = window->worker.send(method, p);
        reply(accepted(window->actionId));
    } else {
        reply(failure(u"不支持的控制方法："_s + method, u"METHOD_NOT_FOUND"_s));
    }
}
