#include "uiscale.h"
#include "workbench.h"
#include "features.h"
#include "chattokenview.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QContextMenuEvent>
#include <QSignalSpy>
#include <QStyleOptionButton>
#include <QPushButton>
#include <QProgressBar>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QAction>
#include <memory>

using namespace Qt::StringLiterals;

void Workbench::startFeatureVerification(const QString &directory) {
    // 在独立微型模型上验证真实页面、请求、写盘和导出重载，不触碰用户模型。
    struct State { QString phase=u"load"_s; QJsonObject checks; bool done=false; };
    const auto root=QDir(directory).absolutePath();
    QDir().mkpath(root);
    verifying=true;
    auto state=std::make_shared<State>();
    auto finish=[this,root,state](QString error=QString{}){
        if(state->done)return;
        state->done=true;
        for(auto it=state->checks.begin();it!=state->checks.end();++it)
            if(!it.value().toBool()&&error.isEmpty())error=it.key();
        QFile file(root+u"/新功能验证.json"_s);
        if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(QJsonObject{{u"pass"_s,error.isEmpty()},{u"phase"_s,state->phase},{u"checks"_s,state->checks},{u"error"_s,error}}).toJson());
        close();QApplication::exit(error.isEmpty()?0:1);
    };
    auto later=[this](std::function<void()> callback){QTimer::singleShot(180,this,callback);};
    auto click=[this](const QString &text){
        for(auto *control:features->findChildren<QPushButton*>())
            if(control->text()==text&&control->isVisible()){control->click();return true;}
        return false;
    };
    connect(this,&Workbench::received,this,[=](const QJsonObject &message){
        if(state->done)return;
        const auto name=message.value(u"name"_s).toString();
        const auto payload=message.value(u"payload"_s).toObject();
        if(message.value(u"kind"_s).toString()==u"error"_s){finish(payload.value(u"message"_s).toString());return;}
        if(name==u"model.ready"_s&&state->phase==u"load"_s){
            if(sessionId.isEmpty())newSession(false);
            state->phase=u"search"_s;
            later([=]{features->tokenQuery->setText(u"你好"_s);railButtons[6]->click();});
        }else if(name==u"feature.result"_s){
            const auto action=payload.value(u"action"_s).toString();
            if(action==u"search"_s&&state->phase==u"search"_s){
                state->checks[u"left_rail_and_real_vocabulary"_s]=centerViews->currentIndex()==2&&features->tokenTable->rowCount()==1&&railButtons[6]->isChecked();
                state->phase=u"vector"_s;
                later([=]{const auto index=features->tokenTable->model()->index(0,0);QTest::mouseClick(features->tokenTable->viewport(),Qt::LeftButton,Qt::NoModifier,features->tokenTable->visualRect(index).center());});
            }else if(action==u"vector"_s&&state->phase==u"vector"_s){
                state->checks[u"real_vector_slice"_s]=features->vectorTable->rowCount()==16;
                state->phase=u"preview"_s;
                later([=]{click(u"修改向量"_s);features->editStart->setValue(0);features->editStop->setValue(2);features->editValues->setPlainText(u"0.25"_s);click(u"预览修改"_s);});
            }else if(action==u"vector.preview"_s&&state->phase==u"preview"_s){
                state->checks[u"preview_values"_s]=payload.value(u"after"_s).toArray()==QJsonArray{.25,.25};
                state->phase=u"drafts"_s;
                later([=]{grab().save(root+u"/向量修改预览.png"_s);if(!click(u"保存草稿"_s)){finish(u"草稿按钮不可访问"_s);return;}railButtons[7]->click();});
            }else if(action==u"versions"_s&&state->phase==u"drafts"_s){
                state->checks[u"draft_persisted_without_apply"_s]=features->draftTable->rowCount()==1&&features->patchTable->rowCount()==0&&QFile::exists(projectDirectory+u"/features/drafts.json"_s);
                state->phase=u"draft_preview"_s;
                later([=]{features->draftTable->selectRow(0);click(u"预览草稿"_s);});
            }else if(action==u"vector.preview"_s&&state->phase==u"draft_preview"_s){
                state->phase=u"apply"_s;
                later([=]{if(!click(u"保存到模型"_s))finish(u"预览保存按钮不可访问"_s);});
            }else if(action==u"vector.apply"_s&&state->phase==u"apply"_s){
                state->checks[u"version_created"_s]=payload.value(u"patches"_s).toArray().size()==1;
                state->phase=u"verify_vector"_s;
                later([=]{openFeature(1,false);features->queryVector();});
            }else if(action==u"vector"_s&&state->phase==u"verify_vector"_s){
                state->checks[u"actual_vector_changed"_s]=payload.value(u"values"_s).toArray()[0].toDouble()==.25;
                const auto scene=captureSession();
                restoreSession(scene,false);
                state->checks[u"feature_scene_restored"_s]=centerViews->currentIndex()==2&&features->currentPage()==1&&railButtons[6]->isChecked();
                grab().save(root+u"/词表与向量.png"_s);
                state->phase=u"versions"_s;
                later([=]{railButtons[7]->click();});
            }else if(action==u"versions"_s&&state->phase==u"versions"_s){
                state->checks[u"version_page"_s]=features->patchTable->rowCount()==1;
                grab().save(root+u"/补丁与版本.png"_s);
                state->phase=u"undo"_s;
                later([=]{
                    activateWindow();features->patchTable->setFocus();QTest::qWait(20);updateHistoryActions();
                    if(!undoAction->isEnabled()){finish(u"编辑菜单未启用可撤销的模型修改"_s);return;}
                    undoAction->trigger();
                });
            }else if(action==u"versions.action"_s&&state->phase==u"undo"_s){
                state->checks[u"undo_disabled_patch"_s]=!payload.value(u"patches"_s).toArray()[0].toObject().value(u"enabled"_s).toBool();
                state->phase=u"redo"_s;
                later([=]{features->patchTable->setFocus();QTest::keyClick(features->patchTable,Qt::Key_Y,Qt::ControlModifier);});
            }else if(action==u"versions.action"_s&&state->phase==u"redo"_s){
                state->checks[u"redo_enabled_patch"_s]=payload.value(u"patches"_s).toArray()[0].toObject().value(u"enabled"_s).toBool();
                state->phase=u"run"_s;
                later([=]{setMode(u"observe"_s);runPrompt(u"你好"_s,2);});
            }else if(action==u"activation"_s&&state->phase==u"analysis"_s){
                state->checks[u"all_layer_real_activation"_s]=payload.value(u"layers"_s).toInt()==2&&features->activationTable->rowCount()>0;
                grab().save(root+u"/本轮激活.png"_s);
                features->activationTable->selectRow(0);
                click(u"定位到计算图"_s);
                const auto candidate=payload.value(u"rows"_s).toArray()[0].toObject();
                state->checks[u"activation_locates_exact_neuron"_s]=centerViews->currentIndex()==0&&layer==candidate.value(u"layer"_s).toInt()&&neuron->value()==candidate.value(u"neuron"_s).toInt()&&tokenPosition->value()==candidate.value(u"position"_s).toInt();
                grab().save(root+u"/激活定位.png"_s);
                state->phase=u"regression"_s;
                later([=]{railButtons[8]->click();features->addSample({{u"input"_s,u"你好"_s},{u"target"_s,u"用户"_s},{u"criterion"_s,u"probability"_s},{u"threshold"_s,0.0},{u"max_new_tokens"_s,2}});click(u"运行对照"_s);});
            }else if(action==u"regression"_s&&state->phase==u"regression"_s){
                state->checks[u"regression_saved"_s]=features->reportTable->rowCount()==1&&QFile::exists(payload.value(u"path"_s).toString());
                grab().save(root+u"/回归验证.png"_s);
                state->phase=u"export"_s;
                later([=]{features->request({{u"action"_s,u"export.verify"_s},{u"destination"_s,root+u"/验收副本"_s},{u"samples"_s,features->samples()}});});
            }else if(action==u"export.verify"_s&&state->phase==u"export"_s){
                state->checks[u"export_reloaded_and_verified"_s]=payload.value(u"export"_s).toObject().value(u"passed"_s).toBool();
                state->phase=u"weight_plain"_s;
                later([=]{railButtons[9]->click();});
            }
        }else if(name==u"file.result"_s&&state->phase==u"weight_plain"_s){
            state->checks[u"权重入口自动打开当前模型"_s]=QFileInfo(payload.value(u"path"_s).toString()).canonicalFilePath()==QFileInfo(QDir(modelPath).filePath(u"model.safetensors"_s)).canonicalFilePath();
            grab().save(root+u"/当前模型权重.png"_s);
            // 独立测试模型增加有效分片索引，验证优先选择索引而非任意分片。
            QJsonObject map;for(const auto &value:payload.value(u"rows"_s).toArray())map[value.toObject().value(u"name"_s).toString()]=u"model.safetensors"_s;
            QFile index(QDir(modelPath).filePath(u"model.safetensors.index.json"_s));
            if(index.exists()||!index.open(QIODevice::WriteOnly)){finish(u"测试索引不能创建"_s);return;}
            index.write(QJsonDocument(QJsonObject{{u"metadata"_s,QJsonObject{}},{u"weight_map"_s,map}}).toJson());index.close();
            state->phase=u"weight_index"_s;later([=]{railButtons[9]->click();});
        }else if(name==u"file.result"_s&&state->phase==u"weight_index"_s){
            state->checks[u"权重入口优先打开分片索引"_s]=payload.value(u"path"_s).toString().endsWith(u"model.safetensors.index.json"_s)&&payload.value(u"total"_s).toInt()>0;
            finish();
        }else if(name==u"run.finished"_s&&state->phase==u"run"_s){
            state->phase=u"analysis"_s;
            later([=]{railButtons[5]->click();click(u"重新分析"_s);});
        }
    });
    QTimer::singleShot(90000,this,[=]{finish(u"新功能验证超时"_s);});
    resize(1600,1000);
    uiScale->setFactor(1);
    if(!createProjectAt(root+u"/测试项目"_s,u"新功能验证"_s,root+u"/测试模型"_s,2,u"cpu"_s))finish(u"无法创建测试项目"_s);
}

QString Workbench::verifyInterfaceScale(const QString &directory, QJsonObject &checks) {
    // 快捷键从真实输入框和画布发出，检查控件、自绘区域和模型状态没有被替换。
    const auto originalText = prompt->text();
    const auto originalRule = ruleValue->value();
    const auto process = worker.pid();
    const auto originalSelection = selection();
    uiScale->setFactor(1);
    const double graphScale = graph->transform().m11();
    prompt->setText(u"缩放状态验证"_s);
    ruleValue->setValue(.375);
    prompt->setFocus();
    QTest::keyClick(prompt, Qt::Key_Equal, Qt::ControlModifier);
    if (uiScale->factor() != 1.1 || titlebar->height() != 64 || strip->height() != 84 ||
        std::abs(graph->transform().m11() / graphScale - 1.1) > 1e-9)
        return u"放大快捷键未同步缩放控件和计算图"_s;
    QTest::keyClick(prompt, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    if (uiScale->factor() != 1.2)
        return u"主键盘加号快捷键失败"_s;
    QTest::keyClick(graph, Qt::Key_Plus, Qt::ControlModifier | Qt::KeypadModifier);
    QTest::keyClick(graph, Qt::Key_Minus, Qt::ControlModifier | Qt::KeypadModifier);
    if (uiScale->factor() != 1.2)
        return u"小键盘缩放快捷键失败"_s;
    checks[u"scale_keyboard_and_geometry"_s] = true;
    uiScale->setFactor(1.3);
    QTest::qWait(30);
    grab().save(directory + u"/原生界面130.png"_s);
    {
        // 自绘区域在放大后仍按同一逻辑坐标命中，防止只放大图像却点错数据。
        Heatmap heat(this);
        heat.resize(260, 182);
        heat.setMatrix(QJsonArray{QJsonArray{1, 2}, QJsonArray{3, 4}});
        double selected = 0;
        connect(&heat, &Heatmap::cellSelected, &heat,
                [&selected](int, int, QJsonValue value) { selected = value.toDouble(); });
        heat.show();
        QTest::mouseClick(&heat, Qt::LeftButton, Qt::NoModifier, QPoint(191, 110));
        if (selected != 4)
            return u"缩放后张量热图点击坐标错误"_s;
        LayerStrip path(this);
        path.resize(520, 99);
        path.configure(QJsonObject{{u"num_hidden_layers"_s, 4}});
        int chosen = -1;
        connect(&path, &LayerStrip::layerSelected, &path, [&chosen](int value) { chosen = value; });
        path.show();
        QTest::mouseClick(&path, Qt::LeftButton, Qt::NoModifier, QPoint(300, 52));
        if (chosen != 2)
            return u"缩放后模型层路径点击坐标错误"_s;
        Timeline track(this);
        track.resize(780, 200);
        track.configure(4);
        track.startForward(u"f0"_s, u"base"_s, 10);
        QString chosenFrame;
        int chosenPosition = -1;
        connect(&track, &Timeline::selected, &track,
                [&](const QString &frame, int, const QString &, int position) {
                    chosenFrame = frame;
                    chosenPosition = position;
                });
        track.show();
        QTest::mouseClick(&track, Qt::LeftButton, Qt::NoModifier, QPoint(250, 90));
        if (chosenFrame != u"f0"_s || chosenPosition != 10)
            return u"缩放后时间轴点击坐标错误"_s;
    }
    checks[u"scale_heatmap_path_timeline_hit_testing"_s] = true;
    configureModel();
    auto *dialog = findChild<QDialog *>(u"modelSettingsDialog"_s);
    checks[u"scale_dialog_observed"_s] =
        QJsonObject{{u"minimum"_s, dialog ? dialog->minimumWidth() : -1},
                    {u"factor"_s, dialog ? dialog->property("uiScale").toDouble() : -1}};
    if (!dialog || dialog->minimumWidth() != 806 || dialog->property("uiScale").toDouble() != 1.3)
        return u"新建设置窗口未继承界面倍率"_s;
    QTest::keyClick(dialog, Qt::Key_Minus, Qt::ControlModifier);
    if (uiScale->factor() != 1.2 || dialog->minimumWidth() != 744)
        return u"设置窗口内的全局缩放失败"_s;
    dialog->close();
    QTest::qWait(10);
    checks[u"scale_dialog"_s] = true;
    uiScale->setFactor(5);
    if (uiScale->factor() != 2 || titlebar->height() != 116)
        return u"界面放大上限错误"_s;
    QTest::qWait(30);
    grab().save(directory + u"/原生界面200.png"_s);
    uiScale->setFactor(.1);
    if (uiScale->factor() != .7 || titlebar->height() != 41)
        return u"界面缩小下限错误"_s;
    QTest::qWait(30);
    grab().save(directory + u"/原生界面70.png"_s);
    for (int i = 0; i < 8; ++i) {
        QTest::keyClick(prompt, Qt::Key_Plus, Qt::ControlModifier);
        QTest::keyClick(prompt, Qt::Key_Minus, Qt::ControlModifier);
    }
    QTest::keyClick(ruleValue, Qt::Key_0, Qt::ControlModifier);
    if (uiScale->factor() != 1 || titlebar->height() != 58 || strip->height() != 76 ||
        std::abs(graph->transform().m11() - graphScale) > 1e-9)
        return u"恢复默认或反复缩放产生尺寸漂移"_s;
    if (worker.pid() != process || selection() != originalSelection || prompt->text() != u"缩放状态验证"_s ||
        ruleValue->value() != .375)
        return u"缩放丢失输入、选择或进程状态"_s;
    checks[u"scale_bounds_reset_no_drift"_s] = true;
    checks[u"scale_input_and_selection_preserved"_s] = true;
    // 再启动一个真实分发程序，读取其启动倍率，避免仅验证当前进程的缓存。
    uiScale->setFactor(1.3);
    const auto probe = directory + u"/缩放重启检查.json"_s;
    QFile::remove(probe);
    QProcess child;
    // Windows 子进程环境用 Unicode 显式传递，避免中文隔离路径经窄字符环境继承后乱码。
    auto probeEnvironment = QProcessEnvironment::systemEnvironment();
    probeEnvironment.insert(u"MODEL_OBSERVATORY_SETTINGS_DIR"_s, QFileInfo(settings.fileName()).absolutePath() + u"/.."_s);
    child.setProcessEnvironment(probeEnvironment);
    child.start(QCoreApplication::applicationFilePath(), {u"--probe-scale"_s, probe});
    if (!child.waitForFinished(15000) || child.exitCode() != 0)
        return u"缩放重启检查进程失败"_s;
    QFile result(probe);
    if (!result.open(QIODevice::ReadOnly) ||
        QJsonDocument::fromJson(result.readAll()).object().value(u"factor"_s).toDouble() != 1.3)
        return u"重新启动后未恢复界面倍率"_s;
    checks[u"scale_restart_preference"_s] = true;
    uiScale->setFactor(1);
    prompt->setText(originalText);
    ruleValue->setValue(originalRule);
    return {};
}

void Workbench::startScaleVerification(const QString &directory) {
    // 不加载模型的快速界面回归；验证结束恢复用户倍率且不覆盖布局。
    verifying = true;
    shell->setCurrentIndex(1);
    const double previous = uiScale->factor();
    const auto root = QDir(directory).absolutePath();
    QDir().mkpath(root);
    QJsonObject checks;
    const auto error = verifyInterfaceScale(root, checks);
    uiScale->setFactor(previous);
    QFile file(root + u"/缩放验证.json"_s);
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(
                       QJsonObject{{u"pass"_s, error.isEmpty()}, {u"checks"_s, checks}, {u"error"_s, error}})
                       .toJson());
    close();
    QApplication::exit(error.isEmpty() ? 0 : 1);
}

void Workbench::startProjectVerification(const QString &directory) {
    // 此处验证原生界面与磁盘现场；真实计算和确定性恢复由后端模型测试独立验证。
    verifying = true;
    // 固定本用例的嵌入前提，避免重复验收继承上次测试的独立窗口偏好。
    setConversationEmbedded(true);
    const auto root = QDir(directory).absolutePath();
    QDir().mkpath(root);
    QJsonObject checks;
    QString error;
    const double previousScale = uiScale->factor();
    auto check = [&](const QString &key, bool value) {
        checks[key] = value;
        if (!value && error.isEmpty())
            error = u"项目验证失败："_s + key;
    };
    auto json = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    };
    auto confirm = [this] {
        QTest::qWait(20);
        for (auto *dialog : findChildren<QDialog *>())
            if (dialog->isVisible())
                for (auto *button : dialog->findChildren<QPushButton *>())
                    if (button->text() == u"确认"_s) {
                        QTest::mouseClick(button, Qt::LeftButton);
                        QTest::qWait(20);
                        return;
                    }
    };
    check(u"launcher_before_workspace"_s, shell->currentIndex() == 0 && !processesStarted);
    check(u"launcher_keeps_conversation_hidden"_s, !conversationWindow->isVisible());
    showConversation();
    check(u"launcher_cannot_open_conversation"_s, !conversationWindow->isVisible());
    check(u"initial_conversation_mode"_s, mode == u"normal"_s && conversationMode->currentData() == u"normal"_s);
    grab().save(root + u"/项目选择页.png"_s);
    newProject();
    QTest::qWait(30);
    for (auto *dialog : findChildren<QDialog *>())
        if (dialog->isVisible()) {
            dialog->grab().save(root + u"/新建项目.png"_s);
            dialog->reject();
        }
    check(u"create_custom_directory_model_limit"_s,
          createProjectAt(root + u"/项目"_s, u"项目现场验证"_s, root + u"/验证模型"_s, 333, u"cpu"_s));
    check(u"enter_project_embeds_conversation"_s, !projectDirectory.isEmpty() && conversationEmbedded && conversationPanel->isVisible());
    showConversation();
    check(u"independent_chat_window"_s, conversationWindow->isWindow() && conversationWindow->isVisible() &&
          !center->isAncestorOf(chat));
    conversationWindow->close();
    check(u"conversation_close_embeds"_s, !conversationWindow->isVisible() && conversationPanel->parentWidget() == conversationHost);
    showConversation();
    QTest::keyClick(prompt, Qt::Key_Plus, Qt::ControlModifier);
    check(u"conversation_global_scale"_s, qAbs(uiScale->factor() - previousScale - .1) < .001);
    uiScale->setFactor(previousScale);
    loading = true;
    beginModelLoading();
    check(u"loading_dialog_immediate"_s, loadingDialog && loadingDialog->isVisible() && loadingProgress->maximum() == 0);
    updateModelLoading({{u"message"_s, u"校验模型文件"_s}, {u"total"_s, 100}, {u"completed"_s, 37}});
    check(u"loading_actual_progress"_s, loadingProgress->value() == 37);
    loadingDialog->grab().save(root + u"/模型加载进度.png"_s);
    finishModelLoading();
    loading = false;
    check(u"loading_dialog_closed"_s, loadingDialog == nullptr);
    messages = {};
    for (int i = 0; i < 90; ++i)
        messages.append(QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"历史消息 "_s + QString::number(i)}});
    assistantText = u"流式开始"_s;
    renderChat();
    auto *bar = chat->verticalScrollBar();
    static_cast<ChatTokenView *>(chat)->setReadingPosition(bar->maximum());
    for (int i = 0; i < 30; ++i) {
        assistantText += u"\n新增输出 "_s + QString::number(i);
        renderChat();
    }
    check(u"stream_follows_bottom"_s, bar->maximum() > 0 && bar->value() == bar->maximum());
    static_cast<ChatTokenView *>(chat)->setReadingPosition(bar->maximum() / 3);
    const int reading = bar->value();
    for (int i = 0; i < 15; ++i) {
        assistantText += u"\n继续生成"_s;
        renderChat();
    }
    check(u"stream_preserves_reading_position"_s, bar->value() == reading);
    static_cast<ChatTokenView *>(chat)->setReadingPosition(bar->maximum());
    assistantText += u"\n回到底部后恢复跟随"_s;
    renderChat();
    check(u"stream_resumes_following"_s, bar->value() == bar->maximum());
    assistantThought = u"这是测试思考文本"_s;
    responseThinking = true;
    responseThinkingComplete = false;
    renderChat();
    check(u"thinking_separate_from_answer"_s, chat->toPlainText().contains(assistantThought) && chat->toPlainText().contains(u"流式开始"_s));
    collapsedThoughts.insert(messages.size());
    renderChat();
    check(u"thinking_can_collapse"_s, !chat->toPlainText().contains(assistantThought) && chat->toPlainText().contains(u"流式开始"_s));
    messages = {};
    assistantText.clear();
    assistantThought.clear();
    collapsedThoughts.clear();
    responseThinking = false;

    if (projectDirectory.isEmpty())
        error = u"无法创建测试项目"_s;
    else {
        check(u"new_project_allows_no_sessions"_s, sessionId.isEmpty() && sessionList->count() == 0);
        newSession();
        const auto first = sessionId;
        check(u"new_session_defaults_to_chat"_s, mode == u"normal"_s &&
              conversationMode->currentData() == u"normal"_s && json(sessionDirectory() + u"/state.json"_s).value(u"mode"_s) == u"normal"_s);
        ruleLayer->setRange(0, 1);
        ruleNeuron->setMaximum(39);
        ruleLayer->setValue(0);
        check(u"editor_layers_start_at_zero"_s, ruleLayer->minimum() == 0 && ruleLayer->value() == 0);
        ruleNeuron->setValue(3);
        ruleKind->setCurrentIndex(ruleKind->findData(u"zero"_s));
        addRule();
        temporaryRules = QJsonArray{rules->item(0, 0)->data(Qt::UserRole).toJsonObject()};
        ruleNeuron->setValue(7);
        ruleValue->setValue(.625);
        messages = QJsonArray{QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"保留上下文"_s}}};
        assistantText = u"尚未生成完的回复"_s;
        thinkingTemperature->setValue(.9);
        prompt->setText(u"未发送的草稿"_s);
        branch = u"branch-12345678"_s;
        branchTexts[branch] = u"编辑分支的结果"_s;
        uiScale->setFactor(1.2);
        navSplit->setSizes({280, 800});
        graph->setTransform(QTransform::fromScale(.8, .8));
        renderChat();
        showConversation();
        conversationWindow->grab().save(root + u"/独立对话窗口.png"_s);
        saveProject();
        const auto saved = captureSession();
        const auto appliedRules = temporaryRules;
        const auto savedInfo = info;
        loaded = true;
        info[u"model_revision"_s] = u"验证模型甲"_s;
        rememberProjectRules();
        info[u"model_revision"_s] = u"验证模型乙"_s;
        restoreProjectRules();
        check(u"different_model_rules_isolated"_s, temporaryRules.isEmpty());
        info[u"model_revision"_s] = u"验证模型甲"_s;
        restoreProjectRules();
        check(u"returning_model_restores_own_rules"_s, temporaryRules == appliedRules);
        info = savedInfo;
        loaded = false;
        restoreProjectRules();
        const auto originalProject = projectDirectory;
        createProjectAt(root + u"/第二项目"_s, u"规则隔离验证"_s, root + u"/验证模型"_s, 333, u"cpu"_s);
        check(u"different_project_rules_isolated"_s, temporaryRules.isEmpty());
        openProjectAt(originalProject, false);
        check(u"returning_project_restores_own_rules"_s, temporaryRules == appliedRules);
        const auto savedSizes = navSplit->sizes();
        check(u"project_files_and_limit"_s, QFile::exists(sessionDirectory() + u"/state.json"_s) &&
              json(projectDirectory + u"/project.json"_s).value(u"max_tokens"_s).toInt() == 333);
        setMode(u"observe"_s);
        newSession();
        check(u"new_session_does_not_inherit_observation"_s, mode == u"normal"_s && conversationMode->currentIndex() == 0);
        conversationMode->showPopup();
        QTest::qWait(30);
        conversationMode->view()->window()->grab().save(root + u"/下拉列表.png"_s);
        conversationMode->hidePopup();
        check(u"new_session_isolated"_s, sessionId != first && messages.isEmpty() && !temporaryRules.isEmpty());
        const double browsingGraphScale = graph->transform().m11();
        activateSession(first);
        check(u"switch_restores_context_and_rules"_s, messages == saved.value(u"messages"_s).toArray() &&
              temporaryRules == saved.value(u"temporary_rules"_s).toArray() && rules->rowCount() == 1);
        check(u"draft_and_partial_reply_restored"_s, prompt->text() == u"未发送的草稿"_s &&
              ruleNeuron->value() == 7 && ruleValue->value() == .625 && chat->toPlainText().contains(assistantText));
        check(u"branch_restored"_s, branch == u"branch-12345678"_s);
        // 缩放属于应用偏好，会话切换应保留当前画布倍率，不恢复旧会话布局。
        check(u"scale_restored"_s, qAbs(uiScale->factor() - 1.2) < .001 && qAbs(graph->transform().m11() - browsingGraphScale) < .001);
        check(u"layout_restored"_s, qAbs(navSplit->sizes().first() - savedSizes.first()) <= 3);
        // 先发真实右键按下与抬起，再发系统菜单事件，覆盖列表刷新造成的命中错位。
        showSessionManager(); QTest::qWait(20);
        const auto contextMessages = messages;
        const auto contextDraft = prompt->text();
        auto *target = sessionList->item(1);
        const auto targetTitle = target->text();
        QSignalSpy selectionChanges(sessionList, &QListWidget::currentItemChanged);
        const auto point = sessionList->visualItemRect(target).center();
        QTest::mouseClick(sessionList->viewport(), Qt::RightButton, Qt::NoModifier, point);
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, sessionList->viewport()->mapToGlobal(point));
        QApplication::sendEvent(sessionList->viewport(), &context);
        QTest::qWait(20);
        check(u"right_click_does_not_read_or_switch_session"_s, selectionChanges.isEmpty() && sessionId == first &&
              messages == contextMessages && prompt->text() == contextDraft);
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        check(u"right_click_opens_menu"_s, menu && menu->actions().size() == 2);
        if (menu) {
            menu->actions().first()->trigger();
            menu->close();
            bool correctTarget = false;
            for (auto *dialog : findChildren<QDialog *>())
                if (dialog->isVisible() && dialog->windowTitle() == u"修改主题名"_s) {
                    correctTarget = dialog->findChild<QLineEdit *>()->text() == targetTitle;
                    dialog->reject();
                }
            check(u"right_click_rename_targets_hit_session"_s, correctTarget && sessionId == first);
        }
        // 检查实际绘制的指示器，透明区与深色笔画必须同时存在，避免只剩色块。
        check(u"checkmark_resource_decodes"_s, !QPixmap(u":/ui/checkmark.xpm"_s).isNull());
        for (double factor : {.7, 1., 1.3, 2.}) {
            uiScale->setFactor(factor);
            for (auto *box : {thinkingMode, follow}) {
                const bool wasChecked = box->isChecked(), wasEnabled = box->isEnabled();
                for (bool enabled : {true, false}) {
                    box->setEnabled(enabled);
                    box->setChecked(true);
                    QStyleOptionButton option;
                    option.initFrom(box);
                    const auto indicator = box->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, box);
                    const auto image = box->grab().toImage().copy(indicator);
                    int ink = 0, fill = 0;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto color = image.pixelColor(x, y);
                            ink += color.red() < 70 && color.green() < 90 && color.blue() < 100;
                            fill += color.red() > 100 && color.green() > 140 && color.blue() > 150;
                        }
                    check(u"checkbox_tick_"_s + box->text() + QString::number(factor) + QString::number(enabled), ink > 2 && fill > 2);
                    box->grab().save(root + u"/复选框_"_s + box->text() + QString::number(factor) + QString::number(enabled) + u".png"_s);
                }
                box->setChecked(wasChecked);
                box->setEnabled(wasEnabled);
            }
        }
        uiScale->setFactor(1.2);
        const auto path = projectDirectory;
        showLauncher();
        check(u"leave_project_hides_conversation"_s, !conversationWindow->isVisible());
        openProjectAt(path, false);
        check(u"reenter_project_opens_conversation"_s, conversationWindow->isVisible());
        check(u"reopen_project_restores_scene"_s, sessionId == first && !temporaryRules.isEmpty() &&
              prompt->text() == u"未发送的草稿"_s && maximum->value() == 333);
        check(u"thinking_temperature_saved_with_project"_s, qAbs(thinkingTemperature->value() - .9) < .001);
        grab().save(root + u"/项目现场恢复.png"_s);
        showStorage();
        QTest::qWait(30);
        for (auto *dialog : findChildren<QDialog *>())
            if (dialog->isVisible()) {
                dialog->grab().save(root + u"/项目存储管理.png"_s);
                dialog->reject();
            }
        // 真实点击清空/删除确认，核对关联文件也按选择清理。
        QDir().mkpath(sessionDirectory() + u"/records/test"_s);
        QFile marker(sessionDirectory() + u"/records/test/marker"_s);
        marker.open(QIODevice::WriteOnly);
        marker.write("test");
        marker.close();
        clearSession();
        confirm();
        check(u"clear_session_removes_history_and_records"_s, messages.isEmpty() && !temporaryRules.isEmpty() &&
              !QFile::exists(sessionDirectory() + u"/records/test/marker"_s));
        const int count = project.value(u"sessions"_s).toArray().size();
        clearSession(true);
        confirm();
        bool containsOld = false;
        for (auto value : project.value(u"sessions"_s).toArray())
            containsOld |= value.toObject().value(u"id"_s).toString() == first;
        check(u"delete_session_updates_catalog"_s, !containsOld && sessionId != first &&
              project.value(u"sessions"_s).toArray().size() == count - 1 && !temporaryRules.isEmpty());
        // 显式清除后访问旧会话，不能重新导入旧规则。
        temporaryRules = {};
        weightsRevision = u"w2"_s;
        writeProjectState(false);
        const auto otherId = project.value(u"sessions"_s).toArray().first().toObject().value(u"id"_s).toString();
        activateSession(otherId);
        check(u"cleared_project_rules_not_revived"_s, temporaryRules.isEmpty());
        clearSession(true);
        confirm();
        check(u"delete_last_session_keeps_empty"_s, sessionId.isEmpty() && sessionList->count() == 0 && !prompt->isEnabled());
        const auto emptyProject = projectDirectory;
        showLauncher();
        openProjectAt(emptyProject, false);
        check(u"reopen_empty_project_does_not_create_session"_s, sessionId.isEmpty() && sessionList->count() == 0);
        newSession();
        check(u"manual_new_session_from_empty"_s, !sessionId.isEmpty() && sessionList->count() == 1);
        showError(u"验证错误信息必须弹窗"_s);
        QTest::qWait(30);
        bool visible = false;
        for (auto *dialog : findChildren<QMessageBox *>())
            visible |= dialog->isVisible() && dialog->text() == u"验证错误信息必须弹窗"_s;
        check(u"errors_have_chinese_dialog"_s, visible);
        for (auto *dialog : findChildren<QMessageBox *>())
            dialog->accept();
        receive({{u"name"_s, u"run.finished"_s}, {u"payload"_s, QJsonObject{
            {u"reason"_s, u"length"_s}, {u"outcome"_s, u"completed"_s}, {u"text"_s, u"达到上限"_s}}}});
        check(u"generation_limit_has_visible_reason"_s, runLabel->text() == u"已达生成上限"_s && !captureWarning->isHidden());
    }
    QFile report(root + u"/项目验证.json"_s);
    if (report.open(QIODevice::WriteOnly))
        report.write(QJsonDocument(QJsonObject{{u"pass"_s, error.isEmpty()}, {u"checks"_s, checks}, {u"error"_s, error}}).toJson());
    uiScale->setFactor(previousScale);
    close();
    QApplication::exit(error.isEmpty() ? 0 : 1);
}

void Workbench::startProjectRuntimeVerification(const QString &directory) {
    // 用真实进程串起项目、临时编辑、暂停保存、模型重载和运行恢复，覆盖跨层接口。
    struct State {
        QString phase = u"load"_s, session;
        QJsonObject checks, boundary;
        bool sampled = false, finished = false;
        int executed = 0;
    };
    verifying = true;
    verifyProjectRuntime = true;
    const auto root = QDir(directory).absolutePath();
    auto state = std::make_shared<State>();
    const auto previous = uiScale->factor();
    auto finish = [this, root, state, previous](QString error = QString{}) {
        for (auto it = state->checks.constBegin(); it != state->checks.constEnd(); ++it)
            if (it.value().isBool() && !it.value().toBool() && error.isEmpty())
                error = u"检查失败："_s + it.key();
        if (state->finished)
            return;
        state->finished = true;
        QFile output(root + u"/项目运行验证.json"_s);
        if (output.open(QIODevice::WriteOnly))
            output.write(QJsonDocument(QJsonObject{{u"pass"_s, error.isEmpty()}, {u"phase"_s, state->phase},
                {u"checks"_s, state->checks}, {u"error"_s, error}}).toJson());
        uiScale->setFactor(previous);
        close();
        QApplication::exit(error.isEmpty() ? 0 : 1);
    };
    auto later = [this](std::function<void()> callback) { QTimer::singleShot(100, this, callback); };
    connect(this, &Workbench::received, this, [=](const QJsonObject &message) {
        if (state->finished)
            return;
        const auto name = message.value(u"name"_s).toString();
        const auto payload = message.value(u"payload"_s).toObject();
        if (message.value(u"kind"_s).toString() == u"error"_s) {
            finish(payload.value(u"message"_s).toString());
            return;
        }
        if (name == u"edit.applied"_s) {
            bool notified = false;
            for (auto *dialog : findChildren<QMessageBox *>())
                if (dialog->isVisible() && dialog->icon() == QMessageBox::Information) {
                    notified = dialog->text() == (temporaryRules.isEmpty() ? u"临时编辑已清除"_s : u"临时编辑已应用，将用于后续输入"_s);
                    dialog->accept();
                }
            state->checks[u"edit_success_dialog"_s] = notified;
        }
        if (name == u"model.ready"_s && state->phase == u"load"_s) {
            state->phase = u"edit"_s;
            later([this] {
                newSession();
                ruleLayer->setValue(0);
                ruleNeuron->setValue(3);
                ruleKind->setCurrentIndex(ruleKind->findData(u"zero"_s));
                ruleScope->setCurrentIndex(ruleScope->findData(u"all"_s));
                addRule();
                applyRules();
            });
        } else if (name == u"edit.applied"_s && state->phase == u"edit"_s) {
            state->checks[u"real_temporary_edit"_s] = !temporaryRules.isEmpty();
            state->phase = u"clear"_s;
            later([this] { rules->setRowCount(0); ruleNeuron->setValue(7); applyRules(); });
        } else if (name == u"edit.applied"_s && state->phase == u"clear"_s) {
            state->checks[u"empty_list_clears_without_inserting_draft"_s] = temporaryRules.isEmpty() && rules->rowCount() == 0 && ruleNeuron->value() == 7;
            state->phase = u"reapply"_s;
            later([this] { ruleNeuron->setValue(3); addRule(); applyRules(); });
        } else if (name == u"edit.applied"_s && state->phase == u"reapply"_s) {
            state->phase = u"run"_s;
            later([this, state] {
                setMode(u"step"_s);
                maximum->setValue(4);
                showConversation();
                QTest::qWait(30);
                prompt->setText(u"你好"_s);
                prompt->setFocus();
                QTest::keyClick(prompt, Qt::Key_Return, Qt::ControlModifier);
                state->checks[u"input_remains_enabled_during_generation"_s] = prompt->isEnabled();
                state->checks[u"input_focus_after_enter"_s] = prompt->hasFocus();
                prompt->setText(u"下一条消息草稿"_s);
            });
        } else if (name == u"token.sampled"_s && state->phase == u"run"_s) {
            state->sampled = true;
        } else if (name == u"step.waiting"_s && state->phase == u"run"_s) {
            if (state->sampled) {
                state->phase = u"save"_s;
                state->executed = payload.value(u"executed"_s).toInt();
                state->boundary = payload;
                state->session = sessionId;
                prompt->setText(u"保存运行时的草稿"_s);
                uiScale->setFactor(1.3);
                later([this] { saveProject(); });
            } else {
                later([this] { releaseStep(); });
            }
        } else if (name == u"checkpoint.saved"_s && state->phase == u"save"_s) {
            state->checks[u"checkpoint_committed"_s] = QFile::exists(checkpointPath);
            grab().save(root + u"/真实项目暂停保存.png"_s);
            state->phase = u"cancel"_s;
            later([this] { cancel(); });
        } else if (name == u"run.finished"_s && state->phase == u"cancel"_s) {
            state->phase = u"reload"_s;
            later([this] { openProjectAt(projectDirectory); });
        } else if (name == u"session.activated"_s && state->phase == u"reload"_s) {
            if (sessionId != state->session || temporaryRules.isEmpty() || checkpointPath.isEmpty() ||
                prompt->text() != u"保存运行时的草稿"_s || qAbs(uiScale->factor() - 1.3) > .001) {
                finish(u"模型重载后项目现场丢失"_s);
                return;
            }
            state->checks[u"reloaded_model_and_session_scene"_s] = true;
            state->phase = u"restore"_s;
            later([this] { resumeSavedRun(); });
        } else if (name == u"step.waiting"_s && state->phase == u"restore"_s) {
            if (payload.value(u"executed"_s).toInt() != state->executed ||
                payload.value(u"operation_id"_s) != state->boundary.value(u"operation_id"_s) ||
                payload.value(u"forward_id"_s) != state->boundary.value(u"forward_id"_s)) {
                finish(u"恢复位置与保存的真实运算边界不一致"_s);
                return;
            }
            state->checks[u"restored_exact_operation_boundary"_s] = true;
            state->phase = u"one"_s;
            grab().save(root + u"/真实项目恢复原边界.png"_s);
            later([this] { releaseStep(); });
        } else if (name == u"step.waiting"_s && state->phase == u"one"_s) {
            if (payload.value(u"executed"_s).toInt() != state->executed + 1) {
                finish(u"恢复后单步放行超过一次运算"_s);
                return;
            }
            state->checks[u"restored_single_step"_s] = true;
            state->phase = u"complete"_s;
            later([this] { setMode(u"observe"_s); });
        } else if (name == u"run.finished"_s && state->phase == u"complete"_s) {
            if (payload.value(u"outcome"_s).toString() != u"completed"_s) {
                finish(u"恢复后未正常完成生成"_s);
                return;
            }
            state->checks[u"continue_after_restore"_s] = true;
            state->phase = u"isolate"_s;
            later([this] { newSession(); });
        } else if (name == u"session.activated"_s && state->phase == u"isolate"_s) {
            if (payload.value(u"temporary_rules"_s).toArray().isEmpty() || !messages.isEmpty()) {
                finish(u"新会话未保留项目规则或未清空消息"_s);
                return;
            }
            state->checks[u"real_project_rules_survive_new_session"_s] = true;
            finish();
        }
    });
    connect(&worker, &Transport::failed, this, [finish](const QString &error) { finish(error); });
    if (!createProjectAt(root + u"/项目"_s, u"真实项目运行验证"_s, root + u"/验证模型"_s, 4, u"cpu"_s))
        finish(u"无法创建真实运行测试项目"_s);
    QTimer::singleShot(120000, this, [finish] { finish(u"项目运行验证超时"_s); });
}

void Workbench::startVerification(const QString &directory) {
    // 真实窗口、真实运行器端到端验证；结果与截图写入独立证据目录。
    struct State {
        QString phase = u"load4"_s, backup;
        QJsonObject checks;
        QJsonArray errors;
        bool done = false;
        int samples = 0, released = 0;
        double original = 0;
    };
    verifying = true;
    auto s = std::make_shared<State>();
    QString root = QDir(directory).absolutePath();
    QDir().mkpath(root);
    // 验证运行使用独立项目，绝不回退到系统盘记录目录。
    shell->setCurrentIndex(1);
    const auto testProject = root + u"/验证项目"_s;
    if (!QFile::exists(testProject + u"/project.json"_s)) {
        const bool enabled = runtimeEnabled;
        runtimeEnabled = false;
        createProjectAt(testProject, u"原生验证项目"_s, u"D:/models/Qwen3.5-4B"_s, 2);
        runtimeEnabled = enabled;
    } else {
        openProjectAt(testProject, false);
    }
    auto storage = project.value(u"storage"_s).toObject();
    storage[u"policy"_s] = u"full"_s;
    storage[u"run_mib"_s] = 1024;
    project[u"storage"_s] = storage;
    writeProjectState(false);
    ensureProcesses();
    QFile::remove(root + u"/原生验证事件.jsonl"_s);
    auto save = [this, root](const QString &name) { grab().save(root + u"/"_s + name + u".png"_s); };
    const double previousScale = uiScale->factor();
    auto finish = [this, s, root, previousScale](bool success, const QString &reason = QString{}) {
        // 任一检查失败保留当时界面，不以正常退出代替断言。
        if (s->done)
            return;
        for (auto it = s->checks.constBegin(); it != s->checks.constEnd(); ++it)
            if (it.value().isBool() && !it.value().toBool()) {
                success = false;
                s->errors.append(u"检查失败："_s + it.key());
            }
        s->done = true;
        if (!reason.isEmpty())
            s->errors.append(reason);
        QFile file(root + u"/原生验证.json"_s);
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(QJsonObject{{u"pass"_s, success},
                                                 {u"phase"_s, s->phase},
                                                 {u"checks"_s, s->checks},
                                                 {u"errors"_s, s->errors},
                                                 {u"monitor_samples"_s, s->samples}})
                           .toJson());
        grab().save(root + (success ? u"/验证完成.png"_s : u"/验证失败.png"_s));
        uiScale->setFactor(previousScale);
        close();
        QApplication::exit(success ? 0 : 1);
    };
    auto later = [this](std::function<void()> action) { QTimer::singleShot(180, this, action); };
    auto inspect = [this](int index, const QString &op, int channel = 0) {
        follow->setChecked(false);
        forward = u"f0"_s;
        tokenPosition->setValue(std::max(0, int(tokens.size()) - 1));
        neuron->setValue(channel);
        choose(index, op);
    };
    connect(&monitor, &Transport::message, this, [s](const QJsonObject &m) {
        if (m.value(u"name"_s).toString() == u"telemetry.sample"_s)
            ++s->samples;
    });
    connect(&worker, &Transport::failed, this, [finish](const QString &text) { finish(false, text); });
    connect(this, &Workbench::received, this, [=](const QJsonObject &m) {
        if (s->done)
            return;
        const auto name = m.value(u"name"_s).toString();
        const auto p = m.value(u"payload"_s).toObject();
        if (name != u"operation.completed"_s && name != u"allocator.sample"_s) {
            QFile log(root + u"/原生验证事件.jsonl"_s);
            auto logged = m;
            logged[u"verification_phase"_s] = s->phase;
            if (name == u"view.snapshot"_s)
                logged[u"payload"_s] =
                    QJsonObject{{u"selection"_s, p.value(u"selection"_s)},
                                {u"status"_s, p.value(u"status"_s)},
                                {u"neuron"_s, p.value(u"neuron"_s).toObject().value(u"value"_s)},
                                {u"current_selection"_s, selection()}};
            if (log.open(QIODevice::Append))
                log.write(QJsonDocument(logged).toJson(QJsonDocument::Compact) + '\n');
        }
        if (m.value(u"kind"_s).toString() == u"error"_s) {
            finish(false, p.value(u"message"_s).toString());
            return;
        }
        if (name == u"edit.applied"_s)
            for (auto *dialog : findChildren<QMessageBox *>())
                if (dialog->icon() == QMessageBox::Information)
                    dialog->accept();
        if (name == u"edit.applied"_s && (s->phase == u"temp4"_s || s->phase == u"clear4"_s))
            later([this] { runPrompt(u"按当前规则回答：你好。"_s); });
        if (name == u"model.ready"_s) {
            int count = p.value(u"config"_s).toObject().value(u"num_hidden_layers"_s).toInt();
            if (s->phase == u"load4"_s) {
                if (count != 32 || neuron->maximum() != 9215) {
                    finish(false, u"四十亿模型维度错误"_s);
                    return;
                }
                s->checks[u"4b_dimensions"_s] = true;
                s->checks[u"4b_thinking_supported"_s] = thinkingMode->isEnabled();
                s->checks[u"loading_dialog_closes_on_ready"_s] = loadingDialog == nullptr;
                s->phase = u"run4"_s;
                maximum->setValue(2);
                temperature->setValue(0);
                later([this] { runPrompt(u"中国的首都是哪里？简短回答。"_s); });
            } else if (s->phase == u"load3"_s) {
                if (count != 36 || neuron->maximum() != 11007 || !snapshot.isEmpty()) {
                    finish(false, u"三十亿模型切换残留旧状态"_s);
                    return;
                }
                s->checks[u"3b_dimensions"_s] = true;
                s->checks[u"3b_thinking_disabled"_s] = !thinkingMode->isEnabled() && !thinkingMode->isChecked();
                s->phase = u"run3"_s;
                maximum->setValue(2);
                later([this] {
                    setMode(u"observe"_s);
                    runPrompt(u"中国的首都是哪里？简短回答。"_s);
                });
            } else if (s->phase == u"reload4"_s) {
                if (count != 32 || neuron->maximum() != 9215) {
                    finish(false, u"再次切换四十亿模型失败"_s);
                    return;
                }
                s->checks[u"4b_3b_4b_switch"_s] = true;
                s->phase = u"think4"_s;
                later([this] {
                    maximum->setValue(16);
                    thinkingMode->setChecked(true);
                    setMode(u"normal"_s);
                    runPrompt(u"请思考：17乘以23是多少？"_s);
                });
            } else if (s->phase == u"loadSmall"_s || s->phase == u"reloadSaved"_s ||
                       s->phase == u"reloadRestored"_s) {
                s->phase = s->phase == u"loadSmall"_s     ? u"runSmall"_s
                           : s->phase == u"reloadSaved"_s ? u"runSaved"_s
                                                          : u"runRestored"_s;
                maximum->setValue(1);
                later([this] { runPrompt(u"你好"_s); });
            }
        } else if (name == u"run.finished"_s) {
            if (p.value(u"outcome"_s).toString() == u"failed"_s) {
                finish(false, u"真实计算失败"_s);
                return;
            }
            if (s->phase == u"run4"_s) {
                s->checks[u"4b_real_chat"_s] = p;
                s->phase = u"full4"_s;
                later([inspect] { inspect(15, u"attention"_s); });
            } else if (s->phase == u"temp4"_s) {
                s->phase = u"checkTemp4"_s;
                later([inspect] { inspect(13, u"mlp"_s, 10); });
            } else if (s->phase == u"clear4"_s) {
                s->phase = u"step4"_s;
                later([this] {
                    setMode(u"step"_s);
                    maximum->setValue(1);
                    runPrompt(u"继续。"_s);
                });
            } else if (s->phase == u"cancelStep"_s) {
                if (p.value(u"outcome"_s).toString() != u"cancelled"_s) {
                    finish(false, u"单步取消失败"_s);
                    return;
                }
                s->checks[u"step_release_cancel"_s] = true;
                s->phase = u"load3"_s;
                later([this] { loadModel(u"D:/models/models/Qwen__Qwen2.5-3B-Instruct"_s); });
            } else if (s->phase == u"run3"_s) {
                s->checks[u"3b_real_chat"_s] = p;
                s->phase = u"full3"_s;
                later([inspect] { inspect(15, u"attention"_s); });
            } else if (s->phase == u"think4"_s) {
                const auto thought = p.value(u"reasoning_content"_s).toString();
                // 富文本会合并连续空格；只校验可见文字，不能把排版差异误判为内容丢失。
                if (thought.isEmpty() || !chat->toPlainText().simplified().contains(thought.simplified())) {
                    finish(false, u"真实四十亿模型没有显示思考输出"_s);
                    return;
                }
                s->checks[u"4b_real_thinking_stream"_s] = true;
                conversationWindow->grab().save(root + u"/4B独立对话与思考.png"_s);
                thinkingMode->setChecked(false);
                setMode(u"observe"_s);
                s->phase = u"loadSmall"_s;
                later([this, root] { loadModel(root + u"/保存验证模型"_s, u"cpu"_s); });
            } else if (s->phase == u"runSmall"_s || s->phase == u"runSaved"_s ||
                       s->phase == u"runRestored"_s) {
                s->phase = s->phase == u"runSmall"_s   ? u"smallBaseline"_s
                           : s->phase == u"runSaved"_s ? u"savedValue"_s
                                                       : u"restoredValue"_s;
                later([inspect] { inspect(0, u"mlp"_s, 3); });
            } else if (s->phase == u"saveReplay"_s || s->phase == u"restoreReplay"_s) {
                s->phase = s->phase == u"saveReplay"_s ? u"reloadSaved"_s : u"reloadRestored"_s;
                later([this, root] { loadModel(root + u"/保存验证模型"_s, u"cpu"_s); });
            }
        } else if (name == u"view.snapshot"_s && p.value(u"selection"_s).toObject() == selection()) {
            auto attn = p.value(u"attention"_s).toObject();
            auto n = p.value(u"neuron"_s).toObject();
            auto weight = p.value(u"weight"_s).toObject();
            if (s->phase == u"full4"_s && operation == u"attention"_s && !attn.isEmpty()) {
                s->checks[u"4b_full_attention"_s] = true;
                save(u"4B原生计算图"_s);
                s->phase = u"linear4"_s;
                later([inspect] { inspect(13, u"linear_delta"_s); });
            } else if (s->phase == u"linear4"_s && operation == u"linear_delta"_s &&
                       attn.value(u"type"_s).toString() == u"linear"_s) {
                s->checks[u"4b_recurrent_state"_s] = true;
                save(u"4B原生线性注意力"_s);
                changeView(2);
                save(u"4B原生递归状态"_s);
                inspector->setCurrentIndex(1);
                tensorSelect->setCurrentIndex(tensorSelect->findData(u"recurrent_state"_s));
                s->phase = u"slice4"_s;
                requestSlice();
            } else if (s->phase == u"down4"_s && operation == u"down"_s &&
                       p.value(u"status"_s).toString() == u"available"_s) {
                s->phase = u"scalar4"_s;
                scalarQuery();
            } else if (s->phase == u"overview4"_s && !p.value(u"layer_overview"_s).toArray().isEmpty()) {
                s->checks[u"activation_overview"_s] = p.value(u"layer_overview"_s).toArray().size() == 32;
                save(u"4B原生逐层激活"_s);
                s->phase = u"temp4"_s;
                later([this] {
                    changeView(0);
                    inspector->setCurrentIndex(2);
                    editTarget->setCurrentIndex(0);
                    ruleLayer->setValue(14);
                    ruleNeuron->setValue(10);
                    ruleKind->setCurrentIndex(ruleKind->findData(u"zero"_s));
                    ruleScope->setCurrentIndex(ruleScope->findData(u"all"_s));
                    addRule();
                    ruleLayer->setValue(13);
                    ruleNeuron->setValue(20);
                    ruleKind->setCurrentIndex(ruleKind->findData(u"steer"_s));
                    ruleValue->setValue(-.1);
                    addRule();
                    applyRules();
                });
            } else if (s->phase == u"checkTemp4"_s && operation == u"mlp"_s && n.contains(u"value"_s) &&
                       p.value(u"selection"_s).toObject() == selection() &&
                       selection().value(u"layer_index"_s).toInt() == 13 && neuron->value() == 10) {
                if (n.value(u"value"_s).toDouble() != 0) {
                    finish(false, u"临时规则未将目标激活置零"_s);
                    return;
                }
                s->checks[u"multi_rule_temporary_edit"_s] = true;
                const auto before = selection();
                const auto pid = worker.pid();
                const auto frames = timeline->frames.size();
                const auto count = rules->rowCount();
                QTest::keyClick(ruleValue, Qt::Key_Plus, Qt::ControlModifier);
                QTest::keyClick(ruleValue, Qt::Key_Plus, Qt::ControlModifier);
                QTest::keyClick(ruleValue, Qt::Key_Plus, Qt::ControlModifier);
                if (selection() != before || worker.pid() != pid || timeline->frames.size() != frames ||
                    rules->rowCount() != count || count != 2 || active) {
                    finish(false, u"界面缩放丢失编辑分支或规则"_s);
                    return;
                }
                save(u"4B全局缩放130神经元编辑"_s);
                QTest::keyClick(ruleValue, Qt::Key_0, Qt::ControlModifier);
                s->checks[u"scale_preserves_real_edits"_s] = true;
                save(u"4B原生神经元编辑"_s);
                s->phase = u"clear4"_s;
                later([this] { clearEdit->click(); });
            } else if (s->phase == u"full3"_s && operation == u"attention"_s && !attn.isEmpty()) {
                s->checks[u"3b_attention"_s] = true;
                save(u"3B原生计算图"_s);
                s->phase = u"reload4"_s;
                later([this] { loadModel(u"D:/models/Qwen3.5-4B"_s); });
            } else if (s->phase == u"smallBaseline"_s && operation == u"mlp"_s && neuron->value() == 3 &&
                       weight.contains(u"value"_s)) {
                s->original = weight.value(u"value"_s).toDouble();
                s->phase = u"saveReplay"_s;
                later([this] {
                    inspector->setCurrentIndex(2);
                    editTarget->setCurrentIndex(1);
                    ruleLayer->setValue(1);
                    ruleNeuron->setValue(3);
                    ruleKind->setCurrentIndex(ruleKind->findData(u"zero"_s));
                    addRule();
                    applyRules();
                });
            } else if (s->phase == u"savedValue"_s && operation == u"mlp"_s && neuron->value() == 3 &&
                       weight.contains(u"value"_s)) {
                if (weight.value(u"value"_s).toDouble() != 0) {
                    finish(false, u"保存后重新加载丢失参数修改"_s);
                    return;
                }
                s->checks[u"save_reload"_s] = true;
                s->phase = u"restoreReplay"_s;
                later([this, s] {
                    editing = true;
                    setActive(false);
                    worker.send(
                        u"model.restore_backup"_s,
                        {{u"backup_path"_s, s->backup}, {u"expected_weights_revision"_s, weightsRevision}});
                });
            } else if (s->phase == u"restoredValue"_s && operation == u"mlp"_s && neuron->value() == 3 &&
                       weight.contains(u"value"_s)) {
                if (weight.value(u"value"_s).toDouble() != s->original) {
                    finish(false, u"备份恢复后参数不一致"_s);
                    return;
                }
                s->checks[u"backup_restore_reload"_s] = true;
                finish(true);
            }
        } else if (name == u"tensor.slice.result"_s && s->phase == u"slice4"_s) {
            if (p.value(u"values"_s).toArray().isEmpty()) {
                finish(false, u"张量切片为空"_s);
                return;
            }
            s->checks[u"tensor_slice"_s] = true;
            save(u"4B原生张量切片"_s);
            s->phase = u"down4"_s;
            later([inspect] { inspect(13, u"down"_s); });
        } else if (name == u"scalar.result"_s && s->phase == u"scalar4"_s) {
            if (p.value(u"terms"_s).toInt() != 9216) {
                finish(false, u"真实下投影内积项数错误"_s);
                return;
            }
            s->checks[u"scalar_explanation"_s] = true;
            s->phase = u"overview4"_s;
            changeView(1);
        } else if (name == u"edit.applied"_s && s->phase == u"saveReplay"_s) {
            s->backup = p.value(u"backup_path"_s).toString();
            if (s->backup.isEmpty())
                finish(false, u"保存未生成备份"_s);
        } else if (name == u"step.waiting"_s && s->phase == u"step4"_s) {
            if (!s->released) {
                s->released = 1;
                const auto before = boundary;
                const auto pid = worker.pid();
                QTest::keyClick(graph, Qt::Key_Plus, Qt::ControlModifier);
                QTest::keyClick(graph, Qt::Key_Minus, Qt::ControlModifier);
                if (!active || mode != u"step"_s || boundary != before || worker.pid() != pid) {
                    finish(false, u"界面缩放改变了模型暂停状态"_s);
                    return;
                }
                s->checks[u"scale_preserves_real_pause"_s] = true;
                save(u"4B原生单步等待"_s);
                later([this] { releaseStep(); });
            } else {
                if (p.value(u"executed"_s).toInt() != 1) {
                    finish(false, u"一次放行执行了多个运算"_s);
                    return;
                }
                s->phase = u"cancelStep"_s;
                const int before = s->samples;
                QTimer::singleShot(1400, this, [=] {
                    if (s->samples <= before) {
                        finish(false, u"暂停时监控没有更新"_s);
                        return;
                    }
                    s->checks[u"telemetry_while_paused"_s] = true;
                    cancel();
                });
            }
        }
    });
    resize(1800, 1125);
    resetLayout();
    QTimer::singleShot(200, this, [=] {
        const auto error = verifyInterfaceScale(root, s->checks);
        if (!error.isEmpty()) {
            finish(false, error);
            return;
        }
        // 使用真实鼠标事件验证边界拖动，检查横纵光标语义。
        int before = navSplit->sizes().first();
        auto *handle = navSplit->handle(1);
        auto point = handle->rect().center();
        QTest::mouseMove(handle, point);
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(handle, point + QPoint(45, 0), 40);
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, point + QPoint(45, 0));
        bool resized = navSplit->sizes().first() != before;
        s->checks[u"splitter_drag"_s] = resized;
        s->checks[u"splitter_cursors"_s] = handle->cursor().shape() == Qt::SplitHCursor &&
                                           leftSplit->handle(1)->cursor().shape() == Qt::SplitVCursor;
        if (!resized) {
            finish(false, u"鼠标拖动分区未改变尺寸"_s);
            return;
        }
        resetLayout();
        save(u"原生工作区"_s);
        loadModel(u"D:/models/Qwen3.5-4B"_s);
    });
    QTimer::singleShot(600000, this, [finish] { finish(false, u"端到端验证超时"_s); });
}
