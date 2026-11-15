// 会话滚动选择、后台生成归属、批量删除与冻结复选框的隔离回归。
#include "workbench.h"
#include "uiscale.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTest>
#include <QTextBrowser>
#include <QUuid>
#include <algorithm>
#include <memory>

using namespace Qt::StringLiterals;

void Workbench::startSessionVerification(const QString &directory) {
    // 所有文件和设置位于新测试目录；协议回放只验证前端事件归属，不冒充模型计算。
    verifying = true;
    const auto root = QDir(directory).absolutePath();
    QDir().mkpath(root + u"/模型"_s);
    QJsonObject checks, details;
    const auto check = [&checks](const QString &name, bool passed) { checks[name] = passed; };
    const auto read = [](const QString &path) {
        QFile file(path); file.open(QIODevice::ReadOnly);
        return QJsonDocument::fromJson(file.readAll()).object();
    };
    QFile config(root + u"/模型/config.json"_s);
    config.open(QIODevice::WriteOnly); config.write("{\"model_type\":\"qwen2\",\"max_position_embeddings\":8192}"); config.close();
    check(u"创建隔离项目"_s, createProjectAt(root + u"/项目"_s, u"会话修复回归"_s, root + u"/模型"_s, 3, u"cpu"_s));
    if (sessionId.isEmpty()) newSession(false);
    const auto original = sessionId;
    messages = {QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"正在生成的原会话"_s}}};
    writeProjectState(false);
    QJsonArray entries = project.value(u"sessions"_s).toArray();
    // 构造足够长的真实会话列表，文件读写使用生产现场格式。
    for (int i = 0; i < 64; ++i) {
        const auto id = QUuid::createUuid().toString(QUuid::Id128);
        entries.append(QJsonObject{{u"id"_s, id}, {u"title"_s, u"历史会话 "_s + QString::number(i)}});
        QDir().mkpath(projectDirectory + u"/sessions/"_s + id);
        QFile file(projectDirectory + u"/sessions/"_s + id + u"/state.json"_s);
        file.open(QIODevice::WriteOnly);
        file.write(QJsonDocument(QJsonObject{{u"messages"_s, QJsonArray{QJsonObject{{u"role"_s, u"user"_s},
            {u"content"_s, u"历史内容 "_s + QString::number(i)}}}}, {u"model_path"_s, modelPath}}).toJson());
    }
    project[u"sessions"_s] = entries; refreshSessions(); writeProjectState(false);
    conversationWindow->resize(1000, 620); showConversation(); QTest::qWait(60);
    showSessionManager(); QTest::qWait(30);
    sessionList->verticalScrollBar()->setValue(sessionList->verticalScrollBar()->maximum());
    QTest::qWait(20);
    QList<double> durations;
    bool selectionOk = true, stableItems = true, scrollOk = true;
    auto *firstItem = sessionList->item(0);
    for (int turn = 0; turn < 12; ++turn) {
        const int row = 50 + turn % 8;
        sessionList->scrollToItem(sessionList->item(row)); QTest::qWait(5);
        const auto point = sessionList->visualItemRect(sessionList->item(row)).center();
        const auto id = sessionList->item(row)->data(Qt::UserRole).toString();
        const int scroll = sessionList->verticalScrollBar()->value();
        QElapsedTimer timer; timer.start();
        QTest::mouseClick(sessionList->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
        durations.append(timer.nsecsElapsed() / 1000000.0);
        selectionOk &= sessionId == id && sessionList->currentItem() && sessionList->currentItem()->data(Qt::UserRole).toString() == id;
        stableItems &= sessionList->item(0) == firstItem;
        scrollOk &= sessionList->verticalScrollBar()->value() == scroll;
    }
    check(u"滚动后连续鼠标点击选中与内容一致"_s, selectionOk);
    check(u"切换保持原列表条目"_s, stableItems);
    check(u"切换保持列表滚动位置"_s, scrollOk);
    std::sort(durations.begin(), durations.end());
    details[u"切换耗时毫秒中位数"_s] = durations[durations.size() / 2];
    details[u"切换耗时毫秒最大值"_s] = durations.last();
    activateSession(original);
    const auto historical = entries[52].toObject().value(u"id"_s).toString();
    const auto historicalBefore = read(projectDirectory + u"/sessions/"_s + historical + u"/state.json"_s);
    loaded = true; prompt->setText(u"下一条草稿"_s); setActive(true);
    activateSession(historical);
    check(u"生成中允许选择历史会话且不改变任务归属"_s, active && sessionId == original && viewedSessionId == historical && sessionList->isEnabled());
    check(u"生成中显示所选历史内容并禁用冲突发送"_s, chat->toPlainText().contains(u"历史内容 51"_s) && !send->isEnabled() && !prompt->isEnabled());
    check(u"生成中禁止删除所有会话"_s, !deleteAllSessionsButton->isEnabled());
    receive({{u"name"_s, u"text.updated"_s}, {u"payload"_s, QJsonObject{{u"text"_s, u"原会话的后台输出"_s}}}});
    renderChat(true);
    check(u"后台流式输出不覆盖历史内容"_s, chat->toPlainText().contains(u"历史内容 51"_s) && !chat->toPlainText().contains(u"原会话的后台输出"_s));
    activateSession(original);
    check(u"切回生成会话立即显示最新内容与原草稿"_s, viewedSessionId.isEmpty() && active && chat->toPlainText().contains(u"原会话的后台输出"_s) && prompt->text() == u"下一条草稿"_s);
    activateSession(historical);
    receive({{u"name"_s, u"run.finished"_s}, {u"branch_id"_s, u"base"_s},
        {u"payload"_s, QJsonObject{{u"text"_s, u"原会话的最终输出"_s}, {u"reason"_s, u"eos"_s}}}});
    QTest::qWait(30);
    check(u"生成结束后保持当前查看会话并恢复发送"_s, !active && sessionId == historical && viewedSessionId.isEmpty() && send->isEnabled());
    const auto live = read(projectDirectory + u"/sessions/"_s + original + u"/state.json"_s);
    check(u"最终输出与草稿只保存到生成会话"_s, live.value(u"assistant_text"_s).toString() == u"原会话的最终输出"_s && live.value(u"input_draft"_s).toString() == u"下一条草稿"_s);
    check(u"历史会话文件未混入生成输出"_s, read(projectDirectory + u"/sessions/"_s + historical + u"/state.json"_s) == historicalBefore);
    loaded = false;
    // 用同一模型验证冻结列的勾选、横向移动和纵向同步。
    for (int i = 0; i < 30; ++i) appendRule({{u"layer_index"_s, i}, {u"neuron_index"_s, 12345},
        {u"kind"_s, u"scale"_s}, {u"scope"_s, u"all"_s}, {u"value"_s, .5}});
    inspector->setCurrentIndex(2); rules->show();
    for (double scale : {.7, 1., 1.5, 2.}) {
        uiScale->setFactor(scale);
        rules->setMinimumSize(0, 0); rules->setMaximumSize(16777215, 16777215);
        rules->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        for (int column = 0; column < 5; ++column) rules->setColumnWidth(column, 200);
        updateFrozenRules(); QTest::qWait(10);
        const int x = frozenRules->mapToGlobal(QPoint(0, 0)).x();
        rules->horizontalScrollBar()->setValue(rules->horizontalScrollBar()->maximum());
        QTest::qWait(10);
        check(u"冻结列横向位置固定 倍率 "_s + QString::number(scale), frozenRules->mapToGlobal(QPoint(0, 0)).x() == x && frozenRules->horizontalScrollBar()->value() == 0);
    }
    uiScale->setFactor(1.);
    rules->verticalScrollBar()->setValue(rules->verticalScrollBar()->maximum());
    check(u"冻结列与内容纵向滚动同步"_s, frozenRules->verticalScrollBar()->value() == rules->verticalScrollBar()->value());
    const auto topIndex = frozenRules->indexAt(QPoint(12, 12));
    if (topIndex.isValid()) {
        const auto rectangle = frozenRules->visualRect(topIndex);
        QTest::mouseClick(frozenRules->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(11, rectangle.center().y()));
    }
    check(u"鼠标点击冻结复选框修改同一规则草稿"_s, topIndex.isValid() && !ruleDraft()[topIndex.row()].toObject().value(u"enabled"_s).toBool());
    conversationWindow->grab().save(root + u"/会话列表.png"_s);
    rules->grab().save(root + u"/冻结规则列.png"_s);
    // 取消删除不能改变任何会话；确认后只删除本项目已登记的会话。
    const auto findDialog = [this]() {
        for (auto *dialog : conversationWindow->findChildren<QDialog*>()) {
            if (dialog->isVisible() && dialog->windowTitle() == u"删除所有会话"_s) return dialog;
        }
        return static_cast<QDialog*>(nullptr);
    };
    const auto click = [](QDialog *dialog, const QString &text) {
        if (!dialog) return false;
        for (auto *button : dialog->findChildren<QPushButton*>()) {
            if (button->text() == text) { button->click(); return true; }
        }
        return false;
    };
    deleteAllSessions(); check(u"删除所有会话提供确认对话框"_s, findDialog() != nullptr);
    click(findDialog(), u"取消"_s);
    check(u"取消批量删除保留全部会话"_s, project.value(u"sessions"_s).toArray().size() == entries.size());
    QFile sentinel(projectDirectory + u"/保留项目文件.txt"_s); sentinel.open(QIODevice::WriteOnly); sentinel.write("keep"); sentinel.close();
    deleteAllSessions(); click(findDialog(), u"确认删除"_s); QTest::qWait(30);
    check(u"确认批量删除清空会话索引与界面"_s, project.value(u"sessions"_s).toArray().isEmpty() && sessionId.isEmpty() && sessionList->count() == 0);
    bool removed = true;
    for (const auto &value : entries) removed &= !QFileInfo::exists(projectDirectory + u"/sessions/"_s + value.toObject().value(u"id"_s).toString());
    check(u"批量删除清理全部登记会话目录"_s, removed);
    check(u"批量删除保留模型与项目文件及规则草稿"_s, QFileInfo::exists(root + u"/模型/config.json"_s) && QFileInfo::exists(sentinel.fileName()) && rules->rowCount() == 30);
    check(u"空列表禁止再次批量删除"_s, !deleteAllSessionsButton->isEnabled());
    const auto empty = read(projectDirectory + u"/project.json"_s);
    check(u"空列表持久化且不创建替代会话"_s, empty.value(u"sessions"_s).toArray().isEmpty() && empty.value(u"current_session"_s).toString().isEmpty());
    bool passed = true;
    for (auto it = checks.begin(); it != checks.end(); ++it) passed &= it.value().toBool();
    QFile report(root + u"/会话修复验收.json"_s); report.open(QIODevice::WriteOnly);
    report.write(QJsonDocument(QJsonObject{{u"pass"_s, passed}, {u"checks"_s, checks}, {u"details"_s, details}}).toJson()); report.close();
    closing = true; close(); QApplication::exit(passed ? 0 : 1);
}

void Workbench::startSessionRuntimeVerification(const QString &directory) {
    // 独立小模型经真实进程生成，验证浏览期间 Token 事件归属和关闭记录后的批量删除。
    struct State {
        QString phase = u"加载"_s, live, history;
        QJsonObject checks, before;
        bool sampled = false, done = false;
        int samplesWhileBrowsing = 0;
    };
    const auto state = std::make_shared<State>();
    const auto root = QDir(directory).absolutePath(); QDir().mkpath(root);
    verifying = true;
    const auto finish = [this, state, root](QString error = {}) {
        if (state->done) return;
        state->done = true;
        for (auto it = state->checks.begin(); it != state->checks.end(); ++it)
            if (!it.value().toBool() && error.isEmpty()) error = it.key();
        QFile report(root + u"/真实生成切换验收.json"_s); report.open(QIODevice::WriteOnly);
        report.write(QJsonDocument(QJsonObject{{u"pass"_s, error.isEmpty()}, {u"error"_s, error},
            {u"checks"_s, state->checks}, {u"浏览期间真实采样次数"_s, state->samplesWhileBrowsing}}).toJson()); report.close();
        closing = true; close(); QApplication::exit(error.isEmpty() ? 0 : 1);
    };
    connect(this, &Workbench::received, this, [this, state, root, finish](const QJsonObject &message) {
        if (state->done) return;
        const auto name = message.value(u"name"_s).toString();
        if (message.value(u"kind"_s).toString() == u"error"_s) { finish(message.value(u"payload"_s).toObject().value(u"message"_s).toString()); return; }
        if (name == u"model.ready"_s && state->phase == u"加载"_s) {
            state->phase = u"准备历史"_s;
            QTimer::singleShot(50, this, [this, state] {
                newSession(); state->history = sessionId;
                messages = {QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"独立的历史会话数据"_s}}};
                writeProjectState(false); state->before = captureSession();
            });
        } else if (name == u"session.activated"_s && state->phase == u"准备历史"_s && !state->history.isEmpty()) {
            state->phase = u"准备生成"_s;
            QTimer::singleShot(0, this, [this, state] {
                newSession(); state->live = sessionId;
                // 连续切换真实运行器，只有最后一次激活可以恢复发送与启动生成。
                activateSession(state->history); activateSession(state->live);
            });
        } else if (name == u"session.activated"_s && state->phase == u"准备生成"_s && !state->live.isEmpty()) {
            state->phase = u"等待采样"_s;
            QTimer::singleShot(0, this, [this] {
                setMode(u"step"_s); runPrompt(u"你好"_s, 12); prompt->setText(u"真实生成时的下一条草稿"_s);
            });
        } else if (name == u"token.sampled"_s) {
            state->sampled = true;
            if (state->phase == u"后台生成"_s) {
                ++state->samplesWhileBrowsing;
                state->checks[u"真实采样继续归属原运行会话"_s] = sessionId == state->live && viewedSessionId == state->history && active;
                state->checks[u"真实生成期间历史聊天不被覆盖"_s] = chat->toPlainText().contains(u"独立的历史会话数据"_s);
                state->phase = u"停止"_s;
                cancel();
            }
        } else if (name == u"step.waiting"_s && state->phase == u"等待采样"_s) {
            QTimer::singleShot(0, this, [this, state, root] {
                if (!state->sampled) { releaseStep(); return; }
                activateSession(state->history);
                state->checks[u"真实任务生成中可切换历史会话"_s] = active && viewedSessionId == state->history && sessionId == state->live;
                conversationWindow->grab().save(root + u"/真实生成中查看历史.png"_s);
                state->phase = u"后台生成"_s;
                worker.send(u"run.change_mode"_s, {{u"mode"_s, u"normal"_s}});
            });
        } else if (name == u"run.finished"_s && (state->phase == u"停止"_s || state->phase == u"后台生成"_s)) {
            state->phase = u"结束"_s;
            QTimer::singleShot(50, this, [this, state, finish] {
                state->checks[u"生成结束保持历史会话选择"_s] = !active && viewedSessionId.isEmpty() && sessionId == state->history;
                QFile live(projectDirectory + u"/sessions/"_s + state->live + u"/state.json"_s); live.open(QIODevice::ReadOnly);
                const auto saved = QJsonDocument::fromJson(live.readAll()).object(); live.close();
                state->checks[u"真实输出及Token已保存到原会话"_s] = !saved.value(u"response_tokens"_s).toArray().isEmpty() &&
                    !saved.value(u"assistant_text"_s).toString().isEmpty() && saved.value(u"input_draft"_s).toString() == u"真实生成时的下一条草稿"_s;
                QFile history(projectDirectory + u"/sessions/"_s + state->history + u"/state.json"_s); history.open(QIODevice::ReadOnly);
                const auto prior = QJsonDocument::fromJson(history.readAll()).object(); history.close();
                state->checks[u"真实历史会话内容未被串写"_s] = prior.value(u"messages"_s) == state->before.value(u"messages"_s);
                state->checks[u"浏览期间存在真实后台采样"_s] = state->samplesWhileBrowsing > 0;
                state->phase = u"删除"_s; deleteAllSessions();
                for (auto *dialog : conversationWindow->findChildren<QDialog*>()) {
                    if (!dialog->isVisible() || dialog->windowTitle() != u"删除所有会话"_s) continue;
                    for (auto *button : dialog->findChildren<QPushButton*>())
                        if (button->text() == u"确认删除"_s) { button->click(); return; }
                }
                finish(u"未找到批量删除确认按钮"_s);
            });
        } else if (name == u"trace.closed"_s && state->phase == u"删除"_s) {
            state->checks[u"真实运行器关闭记录后批量删除成功"_s] = deletingSessionIds.isEmpty() && sessionList->count() == 0 &&
                !QFileInfo::exists(projectDirectory + u"/sessions/"_s + state->live) && !QFileInfo::exists(projectDirectory + u"/sessions/"_s + state->history);
            finish();
        }
    });
    QTimer::singleShot(60000, this, [finish] { finish(u"真实生成切换验证超时"_s); });
    const auto model = qEnvironmentVariable("SESSION_TEST_MODEL");
    if (model.isEmpty() || !createProjectAt(root + u"/项目"_s, u"真实生成切换验证"_s, model, 12, u"cpu"_s)) finish(u"缺少验证模型或创建项目失败"_s);
}
