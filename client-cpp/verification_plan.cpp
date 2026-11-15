// 计划 01—10 的原生界面验收，全部项目、设置及截图写入独立目录。
#include "workbench.h"
#include "uiscale.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTextBrowser>
#include <QTextTable>

using namespace Qt::StringLiterals;

void Workbench::startPlanUiVerification(const QString &directory){
    // 使用实际控件、文件保存和会话入口；生成状态另由真实 MCP 运行验证。
    verifying=true;const auto root=QDir(directory).absolutePath();QDir().mkpath(root);
    QJsonObject checks;QString error;
    const auto check=[&](bool passed,const QString &label){checks[label]=passed;if(!passed&&error.isEmpty())error=label;};
    const auto click=[](QWidget *parent,const QString &text){for(auto *button:parent->findChildren<QPushButton*>())if(button->isVisible()&&button->text()==text){button->click();return true;}return false;};
    const auto dialog=[this](const QString &title){for(auto *item:findChildren<QDialog*>())if(item->isVisible()&&item->windowTitle()==title)return item;return static_cast<QDialog*>(nullptr);};
    resize(1600,1000);showConversation();QTest::qWait(80);
    QDir().mkpath(root+u"/模型"_s);QFile config(root+u"/模型/config.json"_s);config.open(QIODevice::WriteOnly);config.write("{\"model_type\":\"qwen2\",\"max_position_embeddings\":128}");config.close();
    check(createProjectAt(root+u"/项目"_s,u"界面验收"_s,root+u"/模型"_s,3,u"cpu"_s),u"创建隔离项目"_s);
    if(sessionId.isEmpty())newSession(false);
    const auto first=sessionId;setMode(u"observe"_s);writeProjectState(false);
    changeView(1);inspector->setCurrentIndex(2);mainSplit->setSizes({1200,350});QTest::qWait(30);
    conversationWindow->move(50,60);const auto position=conversationWindow->pos();
    const auto split=mainSplit->saveState();newSession(false);const auto second=sessionId;
    check(mode==u"normal"_s&&conversationMode->currentData()==u"normal"_s,u"07 新会话默认对话"_s);
    check(inspector->currentIndex()==2&&centerViews->currentIndex()==1&&mainSplit->saveState()==split,u"03 新会话保留页签视图布局"_s);
    check(conversationWindow->pos()==position,u"04 新会话窗口不跳位"_s);
    activateSession(first);check(mode==u"observe"_s,u"07 已有会话恢复模式"_s);
    check(inspector->currentIndex()==2&&centerViews->currentIndex()==1&&conversationWindow->pos()==position,u"03 04 切换保留工作位置"_s);
    inspector->setCurrentIndex(1);conversationWindow->move(100,120);const auto moved=conversationWindow->pos();
    activateSession(second);activateSession(first);
    check(inspector->currentIndex()==1&&conversationWindow->pos()==moved,u"03 04 张量页与最近窗口位置保持"_s);
    // 旧会话即使包含屏外坐标与旧布局，也不能覆盖当前界面。
    auto old=captureSession();old[u"conversation_geometry"_s]=u"旧坐标"_s;old[u"inspector"_s]=0;old[u"center_view"_s]=0;
    restoreSession(old,false);check(inspector->currentIndex()==1&&centerViews->currentIndex()==1&&conversationWindow->pos()==moved,u"03 04 忽略旧会话界面坐标"_s);
    renameSession(second);auto *rename=dialog(u"修改主题名"_s);
    check(rename!=nullptr,u"08 修改非当前会话弹窗"_s);
    if(rename){rename->findChild<QLineEdit*>()->setText(u""_s);click(rename,u"保存"_s);check(rename->isVisible(),u"08 空主题名拦截"_s);rename->findChild<QLineEdit*>()->setText(u"另一个主题"_s);click(rename,u"保存"_s);}
    check(sessionId==first&&inspector->currentIndex()==1,u"08 修改其他主题不切会话"_s);
    bool persisted=false;for(const auto &v:project.value(u"sessions"_s).toArray())if(v.toObject().value(u"id"_s)==second)persisted=v.toObject().value(u"title"_s)==u"另一个主题"_s;
    check(persisted,u"08 主题名写入项目"_s);
    renameSession(second);rename=dialog(u"修改主题名"_s);if(rename){rename->findChild<QLineEdit*>()->setText(u"取消内容"_s);click(rename,u"取消"_s);}
    inspector->setCurrentIndex(2);clearSession(false);auto *clear=dialog(u"清空会话"_s);if(clear)click(clear,u"确认"_s);QTest::qWait(50);
    check(clear!=nullptr&&inspector->currentIndex()==2&&centerViews->currentIndex()==1,u"03 清空会话保留工作区"_s);
    // 真实弹窗保存、重复检测、修改自身与取消。
    editRule();ruleLayer->setValue(0);ruleNeuron->setValue(1);ruleScope->setCurrentIndex(1);ruleKind->setCurrentIndex(ruleKind->findData(u"scale"_s));ruleValue->setValue(.5);click(ruleDialog,u"确定"_s);
    check(rules->rowCount()==1,u"05 新增规则进入草稿"_s);
    rules->item(0,0)->setCheckState(Qt::Unchecked);editRule();ruleNeuron->setValue(1);click(ruleDialog,u"确定"_s);
    check(rules->rowCount()==1&&ruleDialog->isVisible()&&ruleError->text().contains(u"重复"_s),u"02 停用目标仍拦截重复"_s);ruleDialog->reject();
    editRule(0);ruleValue->setValue(.25);click(ruleDialog,u"确定"_s);
    check(rules->rowCount()==1&&ruleDraft()[0].toObject().value(u"value"_s).toDouble()==.25&&!ruleDraft()[0].toObject().value(u"enabled"_s).toBool(),u"02 修改自身并保留停用"_s);
    editRule();ruleNeuron->setValue(2);click(ruleDialog,u"确定"_s);editRule(0);ruleNeuron->setValue(2);click(ruleDialog,u"确定"_s);
    check(rules->rowCount()==2&&ruleDialog->isVisible(),u"02 修改成其他目标被拦截"_s);ruleDialog->reject();
    editRule();ruleNeuron->setValue(3);ruleDialog->reject();check(rules->rowCount()==2,u"05 取消不加入草稿"_s);
    editRule();editTarget->setCurrentIndex(1);ruleNeuron->setValue(1);projection->setCurrentIndex(0);click(ruleDialog,u"确定"_s);
    check(rules->rowCount()==3,u"02 临时与权重目标分别检查"_s);
    // 勾选图像必须与未勾选不同，并覆盖共享复选框和表格委托绘制。
    for(double scale:{.7,1.,1.3,2.}){
        uiScale->setFactor(scale);QTest::qWait(30);inspector->setCurrentIndex(2);
        rules->item(0,0)->setCheckState(Qt::Unchecked);QTest::qWait(10);const auto unchecked=rules->viewport()->grab().toImage();
        rules->item(0,0)->setCheckState(Qt::Checked);QTest::qWait(10);const auto checked=rules->viewport()->grab().toImage();
        check(unchecked!=checked,u"01 规则表格对勾 "_s+QString::number(qRound(scale*100))+u"%"_s);
        grab().save(root+u"/规则对勾-"_s+QString::number(qRound(scale*100))+u".png"_s);
    }
    uiScale->setFactor(1.);QCheckBox *box=follow;box->setChecked(false);const auto un=box->grab().toImage();box->setChecked(true);QTest::qWait(20);check(un!=box->grab().toImage(),u"01 普通复选框对勾"_s);
    const QString markdown=u"# 标题\n\n- 列表\n\n> 引用\n\n`行内代码`\n\n| 名称 | 数值 |\n| --- | --- |\n| 测试 | 1 |\n\n```python\n    print('缩进')\n```\n\n<script>alert('禁止')</script>\n\n普通符号 * _ < > &"_s;
    messages=QJsonArray{QJsonObject{{u"role"_s,u"user"_s},{u"content"_s,u"测试 Markdown"_s}},QJsonObject{{u"role"_s,u"assistant"_s},{u"content"_s,markdown}}};assistantText.clear();assistantThought.clear();renderChat();
    check(chat->toPlainText().contains(u"标题"_s)&&chat->toPlainText().contains(u"    print('缩进')"_s)&&messages[1].toObject().value(u"content"_s).toString()==markdown,u"06 渲染并保存原始 Markdown"_s);
    const auto html=chat->toHtml();check(!html.contains(u"#30434f"_s)&&!html.contains(u"#2b3038"_s)&&chat->toPlainText().contains(u"用户"_s)&&chat->toPlainText().contains(u"模型"_s),u"10 原型段落样式与角色标签"_s);
    assistantText=u"```python\n未闭合代码"_s;renderChat();assistantText=u"后续消息"_s;renderChat();check(chat->toPlainText().contains(u"后续消息"_s),u"06 不完整流式代码不破坏后续内容"_s);
    conversationWindow->resize(640,620);QTest::qWait(30);conversationWindow->grab().save(root+u"/Markdown窄窗口.png"_s);
    messages.append(QJsonObject{{u"role"_s,u"assistant"_s},{u"content"_s,u"```text\n"_s+QString(800,QChar(u'W'))+u"\n```\n\n| 列 | 数值 |\n| --- | --- |\n| "_s+QString(800,QChar(u'W'))+u" | 1 |"_s}});assistantText.clear();renderChat();QTest::qWait(30);
    check(chat->horizontalScrollBar()->maximum()>0,u"10 宽表格和代码可横向滚动"_s);conversationWindow->grab().save(root+u"/Markdown宽内容.png"_s);
    loaded=true;setActive(true);check(send->text()==u"停止"_s,u"09 生成显示停止"_s);cancel();check(send->text()==u"正在停止"_s&&!send->isEnabled(),u"09 停止中禁止重复点击"_s);setActive(false);check(send->text()==u"发送"_s,u"09 结束恢复发送"_s);loaded=false;
    conversationWindow->move(-50000,-50000);showConversation();check(QGuiApplication::primaryScreen()->availableGeometry().intersects(conversationWindow->frameGeometry()),u"04 屏外窗口可恢复"_s);
    writeProjectState(false);saveLayout();checks[u"应用设置保存"_s]=settings.contains(u"workspace_preferences"_s);
    QFile report(root+u"/计划界面验收.json"_s);if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{u"pass"_s,error.isEmpty()},{u"checks"_s,checks},{u"error"_s,error}}).toJson());
    closing=true;close();QApplication::exit(error.isEmpty()?0:1);
}
