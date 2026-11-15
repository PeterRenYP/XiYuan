#include "workbench.h"
#include "features.h"
#include "neurons.h"
#include "uiscale.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QSpinBox>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTextBrowser>
#include <QMenu>
#include <QAction>
#include <QTextEdit>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeWidget>
#include <QLineEdit>
#include <QScrollBar>

using namespace Qt::StringLiterals;

void Workbench::startPrototypeVerification(const QString &directory) {
    // 用生产会话存储和事件回放验证界面迁移，测试数据只写入隔离目录。
    verifying = true;
    const auto root = QDir(directory).absolutePath();
    QDir().mkpath(root + u"/model"_s);
    QFile config(root + u"/model/config.json"_s);
    config.open(QIODevice::WriteOnly);
    config.write("{\"model_type\":\"qwen2\",\"max_position_embeddings\":8192}");
    config.close();
    QJsonObject checks;
    const auto check = [&checks](const QString &name, bool passed) { checks[name] = passed; };
    check(u"验收设置写入隔离目录"_s, QDir::fromNativeSeparators(settings.fileName()).startsWith(root + QChar(u'/')));
    check(u"创建隔离项目"_s, createProjectAt(root + u"/project"_s, u"新版界面验收"_s, root + u"/model"_s, 256, u"cpu"_s));
    // 已存在的测试项目不能覆盖；创建失败立即退出，避免后续检查落到空项目路径。
    if (!checks.value(u"创建隔离项目"_s).toBool()) {
        QFile report(root + u"/界面验收.json"_s); report.open(QIODevice::WriteOnly);
        report.write(QJsonDocument(QJsonObject{{u"passed"_s,false},{u"checks"_s,checks}}).toJson());
        closing = true; QCoreApplication::exit(3); return;
    }
    // 固定模型结构仅用于布局验收，不注入激活值或冒充真实推理结果。
    QJsonArray types, operations;
    const QJsonArray catalog{QJsonArray{u"norm"_s,u"输入归一化"_s},QJsonArray{u"qkv"_s,u"查询键值投影"_s},
        QJsonArray{u"attn_out"_s,u"注意力输出投影"_s},QJsonArray{u"post_norm"_s,u"前馈归一化"_s},
        QJsonArray{u"gate"_s,u"门控投影"_s},QJsonArray{u"act"_s,u"门控乘积"_s},QJsonArray{u"down"_s,u"下投影"_s}};
    for (int i = 0; i < 32; ++i) { types.append(i % 4 == 3 ? u"full_attention"_s : u"linear_attention"_s); operations.append(catalog); }
    modelReady({{u"name"_s,u"界面验收模型"_s},{u"path"_s,root + u"/model"_s},{u"model_type"_s,u"qwen3_5"_s},
        {u"dtype"_s,u"bfloat16"_s},{u"model_revision"_s,u"ui-fixture"_s},{u"operation_catalog"_s,operations},
        {u"config"_s,QJsonObject{{u"num_hidden_layers"_s,32},{u"intermediate_size"_s,9216},{u"hidden_size"_s,2560},
            {u"num_attention_heads"_s,16},{u"layer_types"_s,types}}}}, false);
    if (sessionId.isEmpty()) newSession(false);
    const auto first = sessionId;
    loaded = true;
    setActive(false);
    setConversationEmbedded(true);
    changeView(0);
    resize(1600, 1050);
    QTest::qWait(120);
    const int emptyWidth=conversationHost->width();
    check(u"右侧四个操作页签保留"_s, inspector->count() == 4);
    check(u"嵌入对话位于中心与右侧之间"_s,
          mainSplit->indexOf(conversationHost) == 1 && mainSplit->count() == 3 && conversationPanel->isVisible());
    prompt->setText(u"第一会话草稿\n第二行"_s);
    messages = {QJsonObject{{u"role"_s,u"user"_s},{u"content"_s,u"已保存消息"_s}}};
    renderChat();
    QTest::qWait(40);
    showCaptureNotice(u"width-check"_s,QString(500,QChar(u'测')));QTest::qWait(40);
    check(u"空会话与首条消息及长提示保持相同宽度"_s,conversationHost->width()==emptyWidth);
    clearCaptureNotices();
    auto initialSizes=mainSplit->sizes();
    auto *chatHandle=mainSplit->handle(1);
    QTest::mousePress(chatHandle,Qt::LeftButton,Qt::NoModifier,chatHandle->rect().center());
    QTest::mouseMove(chatHandle,chatHandle->rect().center()-QPoint(45,0),20);
    QTest::mouseRelease(chatHandle,Qt::LeftButton,Qt::NoModifier,chatHandle->rect().center()-QPoint(45,0));QTest::qWait(30);
    const int resizedWidth=conversationHost->width();renderChat();QTest::qWait(30);
    check(u"拖动会话宽度后消息刷新不重置"_s,resizedWidth!=emptyWidth&&conversationHost->width()==resizedWidth);
    mainSplit->setSizes(initialSizes);conversationWidth=380;
    const auto *originalPrompt = prompt;
    const auto *originalChat = chat;
    showConversation();
    QTest::qWait(40);
    check(u"独立窗口复用原对话对象"_s, conversationWindow->isVisible() && prompt == originalPrompt && chat == originalChat);
    conversationWindow->close();
    QTest::qWait(40);
    check(u"关闭独立窗口返回嵌入"_s, !conversationWindow->isVisible() && conversationPanel->parentWidget() == conversationHost);
    check(u"返回嵌入保留多行草稿"_s, prompt->text() == u"第一会话草稿\n第二行"_s);
    newSession(false);
    const auto second = sessionId;
    prompt->setText(u"第二会话草稿"_s);
    activateSession(first);
    QTest::qWait(30);
    check(u"切换会话恢复第一份草稿"_s, prompt->text() == u"第一会话草稿\n第二行"_s);
    activateSession(second);
    QTest::qWait(30);
    check(u"紧凑选择器与真实会话一致"_s, sessionPicker->currentData().toString() == second && prompt->text() == u"第二会话草稿"_s);
    const auto savedDraft = prompt->text();
    showNeurons();
    QTest::qWait(60);
    check(u"神经元保留原画布与详情"_s, neurons->canvas->isVisible() && neurons->findChild<QLabel *>(u"neuronIdentity"_s));
    check(u"神经元对话位于原详情左边"_s, conversationHost->parentWidget() == neurons->conversationArea() && conversationPanel->isVisible());
    showConversation(); conversationWindow->close();
    check(u"神经元弹出返回保持草稿"_s, conversationHost->parentWidget() == neurons->conversationArea() && prompt->text() == savedDraft);
    grab().save(root + u"/神经元与嵌入对话.png"_s);
    changeView(0);
    showRulesPage();
    check(u"干预草稿不复制数据"_s, ruleEditor->parentWidget() == rulesPage && centerViews->currentWidget() == rulesPage);
    openFeature(0, false);
    check(u"离开干预页归还编辑器"_s, ruleEditor->parentWidget() != rulesPage && timelineTabs->count() == 2);
    check(u"分析页隐藏无关模型树与时间轴"_s, !navigator->isVisible() && !timelinePanel->isVisible());
    grab().save(root + u"/激活分析.png"_s);
    showHistoryPage();
    check(u"运行记录为独立页面"_s, centerViews->currentWidget() == historyPage);
    auto *recordList = historyPage->findChild<QListWidget *>(u"prototypeHistory"_s);
    check(u"无记录时显示空状态"_s, recordList->count() == 1 && recordList->item(0)->text() == u"暂无运行记录"_s);
    // 两条清单仅测试记录页读取与筛选，不提供张量或虚构推理指标。
    for (const auto &category : {u"standard"_s, u"none"_s}) {
        const auto recordRoot = projectDirectory + u"/sessions/records/"_s + category;
        QDir().mkpath(recordRoot);
        QFile manifest(recordRoot + u"/manifest.json"_s); manifest.open(QIODevice::WriteOnly);
        manifest.write(QJsonDocument(QJsonObject{{u"run_id"_s,category},{u"capture_policy"_s,category},
            {u"messages"_s,QJsonArray{QJsonObject{{u"role"_s,u"user"_s},{u"content"_s,u"记录界面验收"_s}}}}}).toJson());
    }
    refreshHistoryRecords();
    check(u"运行记录读取项目清单并显示卡片"_s, recordList->count() == 2 && recordList->itemWidget(recordList->item(0)));
    auto *recordFilter = historyPage->findChild<QComboBox *>(u"historyFilter"_s);
    recordFilter->setCurrentIndex(1);
    check(u"观测筛选仅显示观测记录"_s, recordList->count() == 1 && recordList->item(0)->data(Qt::UserRole).toString().endsWith(u"/standard"_s));
    recordFilter->setCurrentIndex(2);
    check(u"对话筛选仅显示普通记录"_s, recordList->count() == 1 && recordList->item(0)->data(Qt::UserRole).toString().endsWith(u"/none"_s));
    recordFilter->setCurrentIndex(0);
    grab().save(root + u"/运行记录.png"_s);
    for (int index = 1; index < 4; ++index) {
        openFeature(index, false); QTest::qWait(25);
        grab().save(root + QStringList{u""_s,u"/词表与向量.png"_s,u"/补丁与版本.png"_s,u"/回归验证.png"_s}.value(index));
    }
    // 验证折叠后隐藏的样例条件仍可编辑，取消不回写，保存才落到原行。
    features->addSample({{u"input"_s,u"原输入"_s},{u"expected"_s,u"原条件"_s}});
    features->sampleTable->setCurrentCell(0,1);
    features->editSample(); QTest::qWait(10);
    auto findSampleDialog = [this]() -> QDialog * {
        for (auto *dialog : features->findChildren<QDialog *>())
            if (dialog->isVisible() && dialog->windowTitle() == u"编辑样例"_s) return dialog;
        return nullptr;
    };
    auto *sampleDialog = findSampleDialog();
    if (sampleDialog) { sampleDialog->findChildren<QLineEdit *>().first()->setText(u"取消输入"_s); sampleDialog->reject(); }
    check(u"取消样例编辑不改变原数据"_s, sampleDialog && features->sampleTable->item(0,1)->text() == u"原输入"_s);
    features->editSample(); QTest::qWait(10); sampleDialog = findSampleDialog();
    if (sampleDialog) {
        auto fields = sampleDialog->findChildren<QLineEdit *>();
        fields[0]->setText(u"保存输入"_s); fields[2]->setText(u"保存条件"_s);
        for (auto *button : sampleDialog->findChildren<QPushButton *>()) if (button->text() == u"保存"_s) button->click();
    }
    check(u"保存样例编辑更新输入与隐藏条件"_s, sampleDialog && features->sampleTable->item(0,1)->text() == u"保存输入"_s && features->sampleTable->item(0,4)->text() == u"保存条件"_s);
    showRulesPage(); QTest::qWait(25); grab().save(root + u"/干预规则.png"_s);
    restoreRulePanels();
    showMcpInstallation(); QTest::qWait(25);
    auto *installation = findChild<QDialog *>(u"mcpInstallationDialog"_s);
    auto *installationPrompt = installation ? installation->findChild<QPlainTextEdit *>(u"mcpInstallationPrompt"_s) : nullptr;
    check(u"帮助提供安装MCP对话框"_s, installation && installationPrompt);
    if (installationPrompt) {
        const auto content = installationPrompt->toPlainText();
        // 安装版使用随包服务，源码版使用虚拟环境；两种入口都必须可生成并验证。
        const auto bundledMcp = QCoreApplication::applicationDirPath() + u"/Xiyuan-MCP.exe"_s;
        const auto expectedEntry = QFileInfo::exists(bundledMcp) ? u"Xiyuan-MCP.exe"_s : u"run_mcp.py"_s;
        check(u"安装提示词包含实际入口和验证步骤"_s, content.contains(expectedEntry) && content.contains(u"codex mcp add model_observatory"_s) && content.contains(u"observatory_get_state"_s));
        check(u"安装提示词不将控制桥当作HTTP服务"_s, !content.contains(u"--url"_s) && !content.contains(u"http://127.0.0.1"_s));
        const auto clipboardBefore = QApplication::clipboard()->text();
        installation->findChild<QPushButton *>(u"copyMcpInstallationPrompt"_s)->click();
        check(u"复制按钮复制完整安装提示词"_s, QApplication::clipboard()->text() == content);
        QApplication::clipboard()->setText(clipboardBefore);
        installation->grab().save(root + u"/安装MCP.png"_s); installation->close();
    }
    showAbout();
    QTest::qWait(20);
    auto *about = findChild<QDialog *>(u"aboutDialog"_s);
    check(u"关于读取真实应用版本"_s, about && about->findChild<QLabel *>(u"applicationVersion"_s)->text() == QCoreApplication::applicationVersion());
    if (about) { about->grab().save(root + u"/关于析元.png"_s); about->close(); }
    check(u"本轮版本为1.0.4"_s,QCoreApplication::applicationVersion()==u"1.0.4"_s);
    auto *brand=findChild<QToolButton *>(u"prototypeBrand"_s);
    check(u"左上角恢复产品Logo"_s,brand&&!brand->icon().isNull()&&brand->toolButtonStyle()==Qt::ToolButtonTextBesideIcon);
    auto *fileMenu=findChild<QPushButton *>(u"fileMenuButton"_s);QStringList fileActions;
    if(fileMenu&&fileMenu->menu())for(auto *action:fileMenu->menu()->actions())fileActions.append(action->text());
    check(u"文件菜单合并新建打开保存"_s,fileActions.contains(u"新建项目"_s)&&fileActions.contains(u"打开项目"_s)&&fileActions.contains(u"保存\tCtrl+S"_s));
    auto *sessionActions=findChild<QPushButton *>(u"chatSessionActions"_s);QStringList sessionCommands;
    if(sessionActions&&sessionActions->menu())for(auto *action:sessionActions->menu()->actions())sessionCommands.append(action->text());
    check(u"会话管理恢复三个明确入口"_s,sessionActions&&sessionActions->isVisible()&&sessionCommands.contains(u"修改会话标题"_s)&&sessionCommands.contains(u"删除会话"_s)&&sessionCommands.contains(u"删除所有会话"_s));

    auto *rail=findChild<QWidget *>(u"rail"_s);
    bool redundant=false,removedNavigation=false;
    for(auto *button:titlebar->parentWidget()->findChildren<QPushButton *>())
        if(button->isVisible()&&(button->text()==u"嵌入对话"_s||button->text()==u"独立对话"_s||button->text()==u"切换模型"_s))redundant=true;
    for(auto *button:rail->findChildren<QPushButton *>())if(button->isVisible()&&(button->text()==u"干预规则"_s||button->text()==u"回归验证"_s))removedNavigation=true;
    check(u"移除三个冗余按钮及两个左侧入口"_s,!redundant&&!removedNavigation);
    auto *navigation=rail->findChild<QPushButton *>(u"toggleNavigation"_s);
    check(u"导航按钮明确表达收起动作"_s,navigation&&navigation->text()==u"收起导航"_s&&!navigation->icon().isNull());
    navigation->click();check(u"收起后提示展开导航"_s,navigation->toolTip()==u"展开导航"_s);navigation->click();
    activateWindow();QTest::qWait(20);prompt->setFocus();QTest::qWait(20);prompt->setText(QString{});QTest::keyClicks(prompt,u"undo check"_s);QTest::qWait(10);
    updateHistoryActions();undoAction->trigger();
    check(u"编辑菜单撤销文本不执行模型操作"_s,prompt->text().isEmpty()&&!editing);
    updateHistoryActions();redoAction->trigger();check(u"编辑菜单重做文本"_s,prompt->text()==u"undo check"_s);
    QTest::keyClick(prompt,Qt::Key_Z,Qt::ControlModifier);QTest::keyClick(prompt,Qt::Key_Y,Qt::ControlModifier);
    check(u"输入框撤销重做快捷键无冲突"_s,prompt->text()==u"undo check"_s&&!editing);
    const auto existingRules=ruleDraft();showIntervention();QTest::qWait(30);
    check(u"控制菜单打开近正方形共享干预编辑器"_s,interventionDialog&&interventionDialog->isVisible()&&ruleEditor->parentWidget()==interventionDialog&&qAbs(interventionDialog->width()-interventionDialog->height())<120);
    if(interventionDialog){interventionDialog->grab().save(root+u"/神经元干预.png"_s);interventionDialog->close();QTest::qWait(20);}
    check(u"关闭干预弹窗归还同一草稿"_s,ruleEditor->parentWidget()!=interventionDialog&&ruleDraft()==existingRules);
    const QJsonObject temporary{{u"target"_s,u"temporary"_s},{u"enabled"_s,true},{u"layer_index"_s,0},{u"neuron_index"_s,1},{u"kind"_s,u"scale"_s},{u"scope"_s,u"all"_s},{u"value"_s,.5}};
    showRulePreview({{u"rules"_s,QJsonArray{temporary}},{u"targets"_s,QJsonArray{}},{u"temporary_count"_s,1},{u"model_count"_s,0}});QTest::qWait(20);
    auto *preview=findChild<QDialog *>(u"rulePreviewDialog"_s);
    check(u"临时干预预览展示具体规则及生效时机"_s,preview&&preview->findChild<QTableWidget *>(u"rulePreviewSummary"_s)->rowCount()==1&&preview->findChild<QLabel *>(u"rulePreviewExplanation"_s)->text().contains(u"后续"_s));
    if(preview){preview->grab().save(root+u"/临时干预应用预览.png"_s);preview->reject();QTest::qWait(20);}
    // 确定的 UTF-16 边界覆盖补充平面字符与共享字节片段；这里只验证选择请求，不伪造激活。
    FeaturePanel selectionPanel;selectionPanel.context(projectDirectory,true,true,u"base"_s);
    QJsonArray samples{QJsonObject{{u"channel"_s,u"answer"_s},{u"position"_s,10},{u"text_start"_s,0},{u"text_end"_s,1}},
        QJsonObject{{u"channel"_s,u"answer"_s},{u"position"_s,11},{u"text_start"_s,1},{u"text_end"_s,3}},
        QJsonObject{{u"channel"_s,u"answer"_s},{u"position"_s,12},{u"text_start"_s,1},{u"text_end"_s,3}},
        QJsonObject{{u"channel"_s,u"answer"_s},{u"position"_s,13},{u"text_start"_s,3},{u"text_end"_s,4}}};
    selectionPanel.setActivationSessions({QJsonObject{{u"id"_s,u"历史会话"_s},{u"title"_s,u"历史会话"_s},{u"replies"_s,QJsonArray{QJsonObject{
        {u"content"_s,QString::fromUtf8("甲\xf0\xa0\x80\x80乙")},{u"available"_s,true},{u"trace_path"_s,u"选择的历史记录"_s},{u"run_id"_s,u"历史运行"_s},{u"branch_id"_s,u"历史分支"_s},{u"output_tokens"_s,samples}}}}}});
    auto cursor=selectionPanel.analysisContent->textCursor();cursor.setPosition(1);cursor.setPosition(3,QTextCursor::KeepAnchor);selectionPanel.analysisContent->setTextCursor(cursor);selectionPanel.analysisContentScope->setCurrentIndex(1);
    QSignalSpy selectedRequest(&selectionPanel,&FeaturePanel::requested);selectionPanel.analyzeActivation();
    const auto selectedPayload=selectedRequest.isEmpty()?QJsonObject{}:selectedRequest.last().first().toJsonObject();
    check(u"指定文本准确映射共享Token且保留来源分支"_s,selectedPayload.value(u"positions"_s).toArray()==QJsonArray{11,12}&&selectedPayload.value(u"branch_id"_s).toString()==u"历史分支"_s&&sessionId==second);
    // 仅注入表格状态验证页面生命周期，数值正确性由真实张量后端测试覆盖。
    selectionPanel.result({{u"action"_s,u"activation"_s},{u"analysis_id"_s,u"界面快照"_s},{u"page"_s,3},{u"page_count"_s,5},{u"total"_s,250},
        {u"rows"_s,QJsonArray{QJsonObject{{u"layer"_s,1},{u"neuron"_s,7},{u"score"_s,2.0},{u"samples"_s,2}}}}});
    selectionPanel.activationTable->selectRow(0);selectionPanel.activationSearch->setText(u"查询保持"_s);
    const auto savedReport=selectionPanel.activationReport;const auto savedCursor=selectionPanel.analysisContent->textCursor();
    selectionPanel.open(1,false);selectionPanel.setActivationSessions(selectionPanel.analysisSessions);selectionPanel.open(0,false);
    check(u"页面切换及来源刷新不丢分析现场"_s,selectionPanel.activationReport==savedReport&&selectionPanel.activationTable->currentRow()==0&&
        selectionPanel.activationSearch->text()==u"查询保持"_s&&selectionPanel.activationPageNumber->value()==3&&
        selectionPanel.analysisContent->textCursor().selectionStart()==savedCursor.selectionStart()&&selectionPanel.analysisContent->textCursor().selectionEnd()==savedCursor.selectionEnd());
    // 注入阶段事件检查控件状态；真实后台事件与取消由运行器测试覆盖。
    selectionPanel.resize(780,680);selectionPanel.show();
    selectionPanel.pending(true,u"activation"_s);
    check(u"分析准备阶段显示滚动进度且停止按钮可用"_s,selectionPanel.analysisProgress->maximum()==0&&
        !selectionPanel.analysisProgressBox->isHidden()&&selectionPanel.cancelButton->isEnabled());
    selectionPanel.progress({{u"message"_s,u"正在计算统计"_s},{u"completed"_s,120},{u"total"_s,500},{u"unit"_s,u"条"_s}});
    check(u"已知总量按真实计数显示阶段百分比"_s,selectionPanel.analysisProgress->value()==240&&
        selectionPanel.analysisProgressText->text().contains(u"120 / 500"_s));
    QTest::qWait(1100);
    check(u"无新事件时耗时仍更新且界面事件循环可响应"_s,selectionPanel.analysisProgressText->text().contains(u"0分 1秒"_s)&&
        selectionPanel.analysisProgressTimer->isActive());
    selectionPanel.grab().save(root+u"/激活分析进度.png"_s);
    selectionPanel.progress({{u"message"_s,u"正在等待采集数据落盘"_s}});
    check(u"未知总量不伪造百分比"_s,selectionPanel.analysisProgress->maximum()==0&&
        selectionPanel.analysisProgressText->text().contains(u"等待采集数据落盘"_s));
    QSignalSpy cancelProgress(&selectionPanel,&FeaturePanel::cancelled);selectionPanel.cancelButton->click();
    selectionPanel.progress({{u"message"_s,u"正在计算统计"_s},{u"completed"_s,500},{u"total"_s,500}});
    check(u"停止只发送一次并保持等待反馈"_s,cancelProgress.count()==1&&!selectionPanel.cancelButton->isEnabled()&&
        selectionPanel.analysisProgressText->text().contains(u"正在停止"_s));
    selectionPanel.failure(u"任务已取消，保留原有分析结果"_s);
    check(u"取消后保留已有结果且不显示完成"_s,selectionPanel.activationReport==savedReport&&
        selectionPanel.analysisProgress->value()==0&&!selectionPanel.analysisProgressTimer->isActive()&&selectionPanel.pages->isEnabled());
    selectionPanel.pending(true,u"activation.query"_s);
    selectionPanel.result(savedReport);
    check(u"成功响应后才显示完成并停止计时"_s,selectionPanel.analysisProgress->value()==1000&&
        selectionPanel.analysisProgress->format()==u"已完成"_s&&!selectionPanel.analysisProgressTimer->isActive());
    selectionPanel.progress({{u"message"_s,u"过期进度"_s},{u"total"_s,5}});
    check(u"结束后的进度不覆盖完成状态"_s,selectionPanel.analysisProgress->value()==1000);
    selectionPanel.pending(true,u"activation.open"_s);selectionPanel.failure(u"分析文件不可读"_s);
    check(u"分析失败恢复操作并保留旧数据"_s,selectionPanel.activationReport==savedReport&&
        selectionPanel.analysisProgress->value()==0&&selectionPanel.cancelButton->isHidden());
    selectionPanel.hide();
    check(u"激活分析提供观测模式注释"_s,features->findChild<QLabel *>(u"activationModeNote"_s)->text()==u"仅支持开启了观测模式的输出使用"_s);
    const auto maximumBefore = maximum->value();
    configureModel();
    QTest::qWait(20);
    auto *settingsDialog = findChild<QDialog *>(u"modelSettingsDialog"_s);
    if (settingsDialog) {
        settingsDialog->findChild<QSpinBox *>(u"settingsMaximum"_s)->setValue(32);
        settingsDialog->close();
    }
    check(u"取消设置不修改输出上限"_s, maximum->value() == maximumBefore);
    changeView(0);
    prompt->clear();
    QTest::keyClicks(prompt, u"line one"_s);
    QTest::keyClick(prompt, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(prompt, u"line two"_s);
    check(u"控制加回车只换行"_s, prompt->text() == u"line one\nline two"_s);
    // 单独检查输入控件，避免测试发送动作启动运行器。
    ChatPrompt shortcutPrompt; shortcutPrompt.show();
    QSignalSpy sends(&shortcutPrompt, &ChatPrompt::returnPressed);
    QTest::keyClick(&shortcutPrompt, Qt::Key_Return);
    check(u"回车只触发一次发送"_s, sends.count() == 1);
    QTest::keyClick(&shortcutPrompt,Qt::Key_Return,Qt::ControlModifier);
    check(u"控制加回车不发送且插入换行"_s,sends.count()==1&&shortcutPrompt.text()==u"\n"_s);
    shortcutPrompt.close();
    messages={QJsonObject{{u"role"_s,u"user"_s},{u"content"_s,u"请展示消息气泡与多行内容。"_s}},QJsonObject{{u"role"_s,u"assistant"_s},{u"content"_s,u"消息气泡已恢复。\n\n支持 **Markdown**、代码块与 Token 分析。"_s}}};renderChat();
    check(u"用户和助手消息保留不同底色"_s,chat->toHtml().contains(u"#344f60"_s)&&chat->toHtml().contains(u"#2d343d"_s));
    resetLayout();
    QTest::qWait(80);
    graph->fitGraph();
    grab().save(root + u"/计算图与嵌入对话.png"_s);
    const auto projectBeforeHome = projectDirectory;
    settings.setValue(u"recent_projects"_s, QStringList{projectDirectory}); refreshRecentProjects();
    shell->setCurrentIndex(0); QTest::qWait(25);
    check(u"首页保留项目和对话操作区"_s, projectDirectory == projectBeforeHome && loaded && homeStage->widget(0)->isVisible() && homeStage->indexOf(conversationHost) == 1 && prototypeInspector->isVisible());
    grab().save(root + u"/项目首页.png"_s);
    changeView(0); QTest::qWait(25);
    check(u"离开首页恢复右侧操作区"_s, mainSplit->indexOf(prototypeInspector) == 2 && conversationPanel->isVisible());
    resize(1240, 820); QTest::qWait(40);
    graph->fitGraph();
    check(u"最小窗口三分区保持可见"_s, width() <= 1240 && conversationHost->isVisible() && prototypeInspector->isVisible() && leftSplit->width() >= 350);
    grab().save(root + u"/最小窗口.png"_s);
    resize(1600, 1050);
    showConversation();
    QTest::qWait(30);
    conversationWindow->grab().save(root + u"/独立对话.png"_s);
    conversationWindow->close();
    bool passed = true;
    for (auto it = checks.begin(); it != checks.end(); ++it) passed &= it.value().toBool();
    QFile report(root + u"/界面验收.json"_s);
    report.open(QIODevice::WriteOnly);
    report.write(QJsonDocument(QJsonObject{{u"passed"_s,passed},{u"checks"_s,checks}}).toJson());
    report.close();
    closing = true;
    QCoreApplication::exit(passed ? 0 : 3);
}
