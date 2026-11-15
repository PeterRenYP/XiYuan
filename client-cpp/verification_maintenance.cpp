// 1.0.2 的菜单、长文本滚动、提示样式与真实记录分析验收；仅使用独立测试目录。
#include "workbench.h"
#include "chattokenview.h"
#include "uiscale.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>
#include <QWheelEvent>
#include <memory>

using namespace Qt::StringLiterals;

void Workbench::startMaintenanceVerification(const QString &directory) {
    // 前半段操作真实 Qt 控件，后半段通过原运行器管道读取真实小模型的记录。
    struct State { QJsonObject checks, details; bool done = false; QString originalSession, targetToken; };
    auto state = std::make_shared<State>();
    const auto root = QDir(directory).absolutePath();
    QDir().mkpath(root);
    verifying = true;
    const auto check = [state](const QString &name, bool passed) { state->checks[name] = passed; };
    const auto finish = [this, state, root](QString error = {}) {
        if (state->done) return;
        state->done = true;
        for (auto it = state->checks.begin(); it != state->checks.end(); ++it)
            if (!it.value().toBool() && error.isEmpty()) error = it.key();
        QFile report(root + u"/专项界面验证.json"_s);
        if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(QJsonObject{
            {u"pass"_s, error.isEmpty()}, {u"checks"_s, state->checks}, {u"details"_s, state->details}, {u"error"_s, error}}).toJson());
        conversationWindow->grab().save(root + u"/最终界面.png"_s);
        close();
        QApplication::exit(error.isEmpty() ? 0 : 1);
    };
    QFile fixtureFile(QDir(root).absoluteFilePath(u"../后端生成记录.json"_s));
    if (!fixtureFile.open(QIODevice::ReadOnly)) { finish(u"缺少后端生成记录"_s); return; }
    const auto fixture = QJsonDocument::fromJson(fixtureFile.readAll()).object();
    check(u"启动页不显示对话"_s, shell->currentIndex() == 0 && !conversationWindow->isVisible() && !processesStarted);
    if (!createProjectAt(root + u"/项目"_s, u"专项验证项目"_s, fixture.value(u"model_path"_s).toString(), 8, u"cpu"_s)) {
        finish(u"创建专项项目失败"_s); return;
    }
    check(u"进入项目后显示对话"_s, conversationPanel->isVisible());
    showConversation(); showSessionManager();
    newSession(false);
    state->originalSession = sessionId;
    setMode(u"step"_s);
    newSession(false);
    check(u"调试后新会话仍默认对话"_s, mode == u"normal"_s && conversationMode->currentData() == u"normal"_s);

    // 滚动列表并变更缩放，右键命中必须始终对应所点行且不触发现场加载。
    for (int i = 0; i < 12; ++i) newSession(false);
    const auto selected = sessionId;
    for (double factor : {.7, 1., 1.5, 2.}) {
        uiScale->setFactor(factor);
        QTest::qWait(10);
        for (int row : {0, 6, sessionList->count() - 2}) {
            auto *item = sessionList->item(row);
            const auto title = item->text();
            sessionList->scrollToItem(item);
            const auto point = sessionList->visualItemRect(item).center();
            QSignalSpy changes(sessionList, &QListWidget::currentItemChanged);
            QTest::mouseClick(sessionList->viewport(), Qt::RightButton, Qt::NoModifier, point);
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, sessionList->viewport()->mapToGlobal(point));
            QApplication::sendEvent(sessionList->viewport(), &event);
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            bool correct = menu && sessionId == selected && changes.isEmpty();
            if (menu) {
                menu->actions().first()->trigger(); menu->close();
                bool dialogFound = false;
                for (auto *dialog : findChildren<QDialog *>())
                    if (dialog->isVisible() && dialog->windowTitle() == u"修改主题名"_s) {
                        dialogFound = dialog->findChild<QLineEdit *>()->text() == title;
                        dialog->reject();
                    }
                correct &= dialogFound;
            }
            check(u"滚动缩放右键目标_"_s + QString::number(factor) + u"_"_s + QString::number(row), correct);
        }
    }
    uiScale->setFactor(1);
    // 不可读取的非当前会话仍能右键打开菜单，证明没有隐式读取聊天现场。
    auto *inactive = sessionList->item(0);
    const auto inactivePath = projectDirectory + u"/sessions/"_s + inactive->data(Qt::UserRole).toString() + u"/state.json"_s;
    QFile savedFile(inactivePath); savedFile.open(QIODevice::ReadOnly); const auto savedBytes = savedFile.readAll(); savedFile.close();
    savedFile.open(QIODevice::WriteOnly); savedFile.write("invalid-json"); savedFile.close();
    sessionList->scrollToItem(inactive);
    const auto hit = sessionList->visualItemRect(inactive).center();
    QTest::mouseClick(sessionList->viewport(), Qt::RightButton, Qt::NoModifier, hit);
    QContextMenuEvent invalidEvent(QContextMenuEvent::Mouse, hit, sessionList->viewport()->mapToGlobal(hit));
    QApplication::sendEvent(sessionList->viewport(), &invalidEvent);
    auto *invalidMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    check(u"右键不读取损坏的会话文件"_s, invalidMenu && sessionId == selected);
    if (invalidMenu) invalidMenu->close();
    savedFile.open(QIODevice::WriteOnly); savedFile.write(savedBytes); savedFile.close();

    // 长文档持续重排时分别验证底部跟随、翻阅历史与重新跟随。
    auto *view = static_cast<ChatTokenView *>(chat);
    auto *bar = view->verticalScrollBar();
    QString longText;
    for (int i = 0; i < 1200; ++i) longText += u"<p>长上下文第 "_s + QString::number(i) + u" 行，保留阅读现场并持续输出新内容。</p>"_s;
    view->setConversationHtml(longText, true);
    QTest::qWait(10);
    bool bottom = true, readingStable = true, resumed = true;
    for (int i = 0; i < 12; ++i) {
        longText += u"<p>流式追加</p>"_s; view->setConversationHtml(longText); QTest::qWait(1);
        bottom &= bar->maximum() - bar->value() <= 2;
    }
    // 原生滚轮事件覆盖已排队的自动恢复，不通过内部状态直接伪造滚动结果。
    const QPointF wheelPoint(100, 100);
    QWheelEvent wheel(wheelPoint, view->viewport()->mapToGlobal(wheelPoint.toPoint()), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &wheel);
    QTest::qWait(1);
    const int reading = bar->value();
    check(u"长文档可以滚离底部"_s, reading < bar->maximum() - 12);
    for (int i = 0; i < 12; ++i) {
        longText += u"<p>阅读期间继续生成</p>"_s; view->setConversationHtml(longText); QTest::qWait(1);
        readingStable &= std::abs(bar->value() - reading) <= 2;
    }
    view->setConversationHtml(longText);
    bar->triggerAction(QAbstractSlider::SliderToMaximum);
    QTest::qWait(1);
    for (int i = 0; i < 12; ++i) {
        longText += u"<p>回到底部后继续生成</p>"_s; view->setConversationHtml(longText); QTest::qWait(1);
        resumed &= bar->maximum() - bar->value() <= 2;
    }
    check(u"长上下文流式输出持续跟随"_s, bottom);
    check(u"翻阅历史不会被新输出拉走"_s, readingStable);
    check(u"回到底部后不再跳回旧位置"_s, resumed);
    state->details[u"长文档最大滚动值"_s] = bar->maximum();

    // 在界面保存两项无限制，再重新读取项目文件，覆盖前端保存的零容量分支。
    showStorage();
    for (auto *dialog : findChildren<QDialog *>())
        if (dialog->isVisible() && dialog->windowTitle() == u"项目存储管理"_s) {
            const auto fields = dialog->findChildren<QSpinBox *>();
            if (fields.size() >= 2) { fields[0]->setValue(0); fields[1]->setValue(0); }
            for (auto *button : dialog->findChildren<QPushButton *>()) if (button->text() == u"保存存储设置"_s) button->click();
        }
    QFile projectFile(projectDirectory + u"/project.json"_s); projectFile.open(QIODevice::ReadOnly);
    const auto storage = QJsonDocument::fromJson(projectFile.readAll()).object().value(u"storage"_s).toObject(); projectFile.close();
    check(u"无限容量可通过界面保存"_s, storage.value(u"total_mib"_s).toInt(-1) == 0 && storage.value(u"run_mib"_s).toInt(-1) == 0);

    // 将真实生成记录复制到当前隔离项目；菜单与运行器读取的来源均为该副本。
    const auto sourceTrace = fixture.value(u"trace_path"_s).toString();
    const auto traceCopy = sessionDirectory() + u"/records/"_s + QFileInfo(sourceTrace).fileName();
    QDirIterator files(sourceTrace, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto source = files.next();
        const auto destination = traceCopy + u"/"_s + QDir(sourceTrace).relativeFilePath(source);
        QDir().mkpath(QFileInfo(destination).absolutePath()); QFile::copy(source, destination);
    }
    auto started = fixture.value(u"started"_s).toObject();
    auto startedPayload = started.value(u"payload"_s).toObject(); startedPayload[u"path"_s] = traceCopy; started[u"payload"_s] = startedPayload;
    messages = QJsonArray{QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"你好"_s}}};
    pendingPrompt = u"你好"_s;
    renderChat();
    tokenSplit->setChecked(true);
    receive(started);
    for (const auto &event : fixture.value(u"events"_s).toArray())
        if (event.toObject().value(u"name"_s).toString() != u"run.finished"_s) receive(event.toObject());
    // 验证合并后的实时刷新，等待实际文本与锚点出现在当前会话。
    QElapsedTimer streamWait;
    streamWait.start();
    while (chatTokenTargets.isEmpty() && streamWait.elapsed() < 1000)
        QTest::qWait(20);
    check(u"拆分显示边框与独立命中"_s, view->showTokenBorders && !chatTokenTargets.isEmpty());
    conversationWindow->grab().save(root + u"/Token拆分.png"_s);
    const auto withBorders = chat->grab().toImage();
    view->showTokenBorders = false;
    const auto withoutBorders = chat->grab().toImage();
    view->showTokenBorders = true;
    check(u"Token边框实际绘制"_s, withBorders != withoutBorders);
    tokenSplit->setChecked(false);
    check(u"普通排版仍保留Token右键身份"_s, !chatTokenTargets.isEmpty());
    bool normalUnderlined=false,normalAnchors=false,normalColor=true;
    for(auto block=chat->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){const auto fragment=it.fragment();if(fragment.isValid()&&fragment.charFormat().anchorHref().startsWith(u"token:"_s)){normalAnchors=true;normalUnderlined|=fragment.charFormat().fontUnderline();normalColor=normalColor&&fragment.charFormat().foreground().color()==chat->palette().color(QPalette::Text);}}
    check(u"不勾选Token拆分时正文没有自动下划线"_s,normalAnchors&&!normalUnderlined&&!view->showTokenBorders);
    check(u"普通对话使用正文色而非链接蓝色"_s,normalAnchors&&normalColor);
    conversationWindow->grab().save(root+u"/普通对话无下划线.png"_s);
    tokenSplit->setChecked(true);

    receive({{u"name"_s, u"capture.degraded"_s}, {u"payload"_s, QJsonObject{{u"message"_s, u"磁盘保留空间不足，已停止张量采集"_s}}}});
    auto finished = fixture.value(u"finished"_s).toObject();
    auto finishedPayload = finished.value(u"payload"_s).toObject(); finishedPayload[u"reason"_s] = u"eos"_s; finished[u"payload"_s] = finishedPayload;
    receive(finished);
    check(u"真实回复保存逐Token数据"_s, !responseTokens.isEmpty() && !messages.last().toObject().value(u"output_tokens"_s).toArray().isEmpty());
    const auto savedSamples = messages.last().toObject().value(u"output_tokens"_s).toArray();
    const auto savedReplySession = sessionId;
    newSession(false);
    activateSession(savedReplySession);
    check(u"会话重开后保留Token数据和菜单"_s, messages.last().toObject().value(u"output_tokens"_s).toArray() == savedSamples && !chatTokenTargets.isEmpty());
    check(u"结束与采集提示同时保留"_s, captureWarning->text().contains(u"模型已结束回答"_s) && captureWarning->text().contains(u"磁盘保留空间不足"_s) && !captureWarning->text().contains(u"允许额度"_s));
    const auto notice = captureNoticeFrame->grab().toImage().pixelColor(4, captureNoticeFrame->height() / 2);
    check(u"提示为黄色底框及感叹号"_s, notice.red() > 180 && notice.green() > 130 && notice.blue() < 160 && captureNoticeFrame->findChild<QLabel *>(u"captureNoticeIcon"_s)->text() == u"!"_s);
    auto *sampleButton = resumeSaved;
    sampleButton->show(); sampleButton->setFocusPolicy(Qt::NoFocus); chat->setFocus();
    QTest::mouseMove(chat->viewport(), QPoint(15, 15));
    const auto buttonImage = sampleButton->grab().toImage();
    const auto buttonColor = buttonImage.pixelColor(5, buttonImage.height() / 2);
    const auto borderColor = buttonImage.pixelColor(0, buttonImage.height() / 2);
    check(u"按钮无需悬停已有底色和边框"_s, buttonColor == QColor(u"#303944"_s) && borderColor == QColor(u"#596b7c"_s));
    state->details[u"按钮常态底色"_s] = buttonColor.name();
    state->details[u"按钮常态边框"_s] = borderColor.name();
    conversationWindow->grab().save(root + u"/提示与按钮.png"_s);

    // 通过实际文字位置发右键事件，菜单打开时再刷新回复，点击分析仍指向原 token。
    QString anchor; QPoint tokenPoint;
    for (auto block = chat->document()->begin(); block.isValid() && anchor.isEmpty(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().anchorHref().startsWith(u"token:"_s)) {
                anchor = fragment.charFormat().anchorHref();
                QTextCursor cursor(chat->document()); cursor.setPosition(fragment.position());
                chat->setTextCursor(cursor); chat->ensureCursorVisible();
                tokenPoint = chat->cursorRect(cursor).center() + QPoint(2, 0); break;
            }
        }
    if (anchor.isEmpty()) { finish(u"未找到实际 Token 命中区域"_s); return; }
    const auto target = chatTokenTargets.value(anchor);
    state->targetToken = QString::number(target.value(u"token"_s).toObject().value(u"id"_s).toInt());
    QContextMenuEvent tokenEvent(QContextMenuEvent::Mouse, tokenPoint, chat->viewport()->mapToGlobal(tokenPoint));
    QApplication::sendEvent(chat->viewport(), &tokenEvent);
    auto *tokenMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    bool panelVisible = false;
    for (auto *dialog : findChildren<QDialog *>()) panelVisible |= dialog->isVisible() && dialog->windowTitle() == u"输出 Token 分析"_s;
    check(u"右键仅显示分析菜单"_s, tokenMenu && tokenMenu->actions().size() == 1 && tokenMenu->actions().first()->text() == u"分析"_s && !panelVisible && !processesStarted);
    if (!tokenMenu) { finish(u"Token右键菜单未打开"_s); return; }
    renderChat();
    const auto currentSession = sessionId;
    // 切换会话后菜单仍持有旧回复快照，后端须按原记录读取。
    activateSession(state->originalSession);
    const auto browsingSession = sessionId;
    runtimeEnabled = true;
    connect(this, &Workbench::received, this, [=](const QJsonObject &message) {
        if (state->done || message.value(u"name"_s).toString() != u"token.analysis"_s) return;
        const auto report = message.value(u"payload"_s).toObject();
        check(u"分析未切换正在查看的会话"_s, sessionId == browsingSession && sessionId != currentSession);
        check(u"菜单刷新后仍分析原Token"_s, report.value(u"token"_s).toObject().value(u"id"_s) == target.value(u"token"_s).toObject().value(u"id"_s));
        check(u"分析读取原始生成与层数据"_s, report.value(u"evidence"_s).toString() == u"原始生成记录"_s && !report.value(u"layers"_s).toArray().isEmpty());
        QTimer::singleShot(0, this, [=] {
            bool shown = false;
            for (auto *dialog : findChildren<QDialog *>())
                if (dialog->isVisible() && dialog->windowTitle() == u"输出 Token 分析"_s) {
                    const auto text = dialog->findChild<QTextBrowser *>()->toPlainText();
                    shown = text.contains(u"自动分析"_s) && text.contains(u"输出通道贡献"_s) && text.contains(u"原始生成记录"_s);
                    dialog->grab().save(root + u"/Token分析面板.png"_s);
                }
            check(u"点击分析后展示真实分析面板"_s, shown);
            finish();
        });
    });
    tokenMenu->actions().first()->trigger(); tokenMenu->close();
    QTimer::singleShot(30000, this, [finish] { finish(u"等待分析面板超时"_s); });
}
