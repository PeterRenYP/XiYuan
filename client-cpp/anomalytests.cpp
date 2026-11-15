#include "anomalyengine.h"
#include "anomalycontroller.h"
#include <QApplication>
#include <QDialog>
#include <QJsonDocument>
#include <QFontDatabase>
#include <QDir>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>

using namespace Qt::StringLiterals;

class AnomalyTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase();
    void repetitionRecoveryIgnoreAndIdentity();
    void activationBaselineAndQuarantine();
    void attentionAndRepresentation();
    void latencyPauseAndHeartbeat();
    void dialogsRespectUserActions();
};

void AnomalyTests::initTestCase() {
    // 无窗口平台显式加载中文字体，使验收截图与真实界面使用同一字体。
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + u"/Fonts/msyh.ttc"_s);
    QApplication::setFont(QFont(u"Microsoft YaHei UI"_s, 11));
}

namespace {
// 测试直接驱动检测时钟，不依靠长时间等待，也不向产品路径加入模拟数据。
QJsonObject frame(const QString &kind) {
    return {{u"kind"_s, kind}, {u"run_id"_s, u"r1"_s}, {u"branch_id"_s, u"base"_s}};
}
QJsonObject start(const QString &directory = {}) {
    auto value = frame(u"start"_s); value[u"profile"_s] = u"profile1"_s;
    value[u"project_root"_s] = directory; value[u"max_tokens"_s] = 10000; return value;
}
int count(const QList<QJsonObject> &events, const QString &type) {
    int found = 0;
    for (const auto &event : events) if (event.value(u"name"_s) == u"anomaly.detected"_s &&
        !event.value(u"payload"_s).toObject().value(u"suppressed"_s).toBool() && event.value(u"payload"_s).toObject().value(u"type"_s) == type) ++found;
    return found;
}
QJsonObject tensor(const QString &operation, double rms, const QJsonArray &vector = {}) {
    auto value = frame(u"tensor"_s); value[u"operation"_s] = operation; value[u"layer"_s] = 0;
    value[u"step"_s] = 10; value[u"position"_s] = 100; value[u"phase"_s] = u"decode"_s;
    QJsonArray values{rms, rms * 2, 0, 0}; for (const auto &element : vector) values.append(element);
    if (vector.isEmpty()) values.append(1.);
    value[u"values"_s] = values; return value;
}
void tokens(AnomalyEngine &engine, int first, int count, bool repeat, qint64 now = 0, double elapsed = 20) {
    for (int i = first; i < first + count; ++i) {
        auto value = frame(u"token"_s); value[u"step"_s] = i; value[u"id"_s] = repeat ? 7 : i;
        value[u"channel"_s] = u"answer"_s; value[u"piece"_s] = u"字"_s; value[u"elapsed_ms"_s] = elapsed;
        value[u"input_tokens"_s] = 100; engine.accept(value, now + i * 20);
    }
}
}

void AnomalyTests::repetitionRecoveryIgnoreAndIdentity() {
    QList<QJsonObject> events; AnomalyEngine engine([&](const auto &event) { events.append(event); });
    engine.accept(start(), 0); tokens(engine, 0, 64, true);
    QCOMPARE(count(events, u"repetition"_s), 1);
    auto feedback = frame(u"feedback"_s); feedback[u"ignore"_s] = true; feedback[u"type"_s] = u"repetition"_s;
    engine.accept(feedback, 1300); tokens(engine, 64, 16, false); tokens(engine, 80, 64, true);
    // 被忽略类型不再次提示，其他类型与新运行仍可检测。
    QCOMPARE(count(events, u"repetition"_s), 1);
    auto old = frame(u"finish"_s); old[u"run_id"_s] = u"old"_s; engine.accept(old, 3000); QVERIFY(engine.running());
    engine.accept(start(), 4000); tokens(engine, 0, 64, true, 4000);
    QCOMPARE(count(events, u"repetition"_s), 2);
    engine.accept(start(), 6000); tokens(engine, 0, 40, true, 6000); tokens(engine, 40, 20, false, 6000); tokens(engine, 60, 40, true, 6000);
    QCOMPARE(count(events, u"repetition"_s), 4);
}

void AnomalyTests::activationBaselineAndQuarantine() {
    QTemporaryDir directory; QList<QJsonObject> events;
    AnomalyEngine engine([&](const auto &event) { events.append(event); });
    engine.accept(start(directory.path()), 0);
    for (int i = 0; i < 40; ++i) engine.accept(tensor(u"mlp"_s, 1), i);
    auto finish = frame(u"finish"_s); finish[u"outcome"_s] = u"completed"_s; finish[u"thinking_complete"_s] = true;
    engine.accept(finish, 1000); QVERIFY(engine.baselineCount() > 0);
    AnomalyEngine restored([&](const auto &event) { events.append(event); }); restored.accept(start(directory.path()), 2000);
    for (int i = 0; i < 3; ++i) restored.accept(tensor(u"mlp"_s, 100), 2000 + i);
    QCOMPARE(count(events, u"activation_growth"_s), 0);
    restored.accept(tensor(u"mlp"_s, 100), 2004); QCOMPARE(count(events, u"activation_growth"_s), 1);
    for (int i = 0; i < 4; ++i) restored.accept(tensor(u"mlp"_s, .001), 2010 + i);
    QCOMPARE(count(events, u"activation_decay"_s), 1);
    auto other = start(directory.path()); other[u"profile"_s] = u"new-weights"_s; restored.accept(other, 3000);
    for (int i = 0; i < 8; ++i) restored.accept(tensor(u"mlp"_s, 100), 3010 + i);
    QCOMPARE(count(events, u"activation_growth"_s), 1);
}

void AnomalyTests::attentionAndRepresentation() {
    QList<QJsonObject> events; AnomalyEngine engine([&](const auto &event) { events.append(event); }); engine.accept(start(), 0);
    auto attention = frame(u"tensor"_s); attention[u"operation"_s] = u"attention"_s; attention[u"layer"_s] = 0;
    attention[u"position"_s] = 100; attention[u"phase"_s] = u"decode"_s; attention[u"heads"_s] = QJsonArray{0, 1};
    for (int i = 0; i < 40; ++i) {
        attention[u"values"_s] = QJsonArray{QJsonArray{.8,.1,1,1,.9,.1}, QJsonArray{.8,.1,2,1,.1,.9}}; engine.accept(attention, i);
    }
    for (int i = 0; i < 10; ++i) {
        attention[u"values"_s] = QJsonArray{QJsonArray{.01,.999,0,1,1,0}, QJsonArray{.01,.999,0,1,1,0}}; engine.accept(attention, 100+i);
    }
    QCOMPARE(count(events, u"attention_collapse"_s), 1); QCOMPARE(count(events, u"head_convergence"_s), 1);
    for (int i = 0; i < 42; ++i) engine.accept(tensor(u"residual"_s, 1, i % 2 ? QJsonArray{1,0,0,0} : QJsonArray{0,1,0,0}), 200+i);
    for (int i = 0; i < 16; ++i) engine.accept(tensor(u"residual"_s, 1, QJsonArray{1,0,0,0}), 300+i);
    QCOMPARE(count(events, u"representation_decay"_s), 1);
}

void AnomalyTests::latencyPauseAndHeartbeat() {
    QList<QJsonObject> events; AnomalyEngine engine([&](const auto &event) { events.append(event); }); engine.accept(start(), 0);
    tokens(engine, 0, 40, false); tokens(engine, 40, 5, false, 0, 200);
    QCOMPARE(count(events, u"slow_generation"_s), 1);
    auto progress = frame(u"progress"_s); progress[u"waiting"_s] = true;
    engine.accept(progress, 2000); engine.tick(20000); engine.tick(21000);
    QCOMPARE(count(events, u"no_progress"_s), 0); QCOMPARE(count(events, u"unresponsive"_s), 0);
    engine.accept(progress, 22000); engine.tick(22000); engine.tick(23000); engine.tick(24000);
    progress[u"waiting"_s] = false; engine.accept(progress, 25000);
    for (int i = 26000; i < 60000; i += 1000) { engine.accept(progress, i); engine.tick(i); }
    QCOMPARE(count(events, u"no_progress"_s), 1);
    engine.tick(130000); engine.tick(131000); QCOMPARE(count(events, u"unresponsive"_s), 1);
}

void AnomalyTests::dialogsRespectUserActions() {
    QWidget owner; owner.resize(800,600); owner.show(); AnomalyController controller(&owner);
    QSignalSpy stopped(&controller,&AnomalyController::stopRequested);
    auto begin = QJsonObject{{u"name"_s,u"run.started"_s},{u"run_id"_s,u"r1"_s},{u"branch_id"_s,u"base"_s}};
    controller.runEvent(begin);
    auto detection = QJsonObject{{u"name"_s,u"anomaly.detected"_s},{u"run_id"_s,u"r1"_s},{u"branch_id"_s,u"base"_s},
        {u"payload"_s,QJsonObject{{u"type"_s,u"repetition"_s}}}};
    controller.handle(detection); controller.handle(detection); QCOMPARE(controller.pendingCount(),1);
    auto *dialog=owner.findChild<QDialog *>(u"anomalyDialog"_s); QVERIFY(dialog); QVERIFY(!dialog->isModal());
    QCOMPARE(dialog->windowModality(),Qt::NonModal);
    QVERIFY(dialog->findChild<QPushButton *>(u"anomalyStop"_s));
    QTest::mouseClick(dialog->findChild<QPushButton *>(u"anomalyIgnoreOnce"_s),Qt::LeftButton);
    QCOMPARE(stopped.count(),0);QCOMPARE(controller.pendingCount(),0);
    controller.handle(detection);QCOMPARE(controller.pendingCount(),1);
    // 关闭的窗口延迟销毁，按可见状态找到新提示，避免命中旧对象。
    for(auto *candidate:owner.findChildren<QDialog *>(u"anomalyDialog"_s))if(candidate->isVisible())dialog=candidate;
    auto other=detection; other[u"payload"_s]=QJsonObject{{u"type"_s,u"activation_growth"_s}};
    controller.handle(other); QCOMPARE(controller.pendingCount(),2);
    QTest::mouseClick(dialog->findChild<QPushButton *>(u"anomalyIgnore"_s),Qt::LeftButton);
    QCOMPARE(controller.pendingCount(),1); controller.handle(detection); QCOMPARE(controller.pendingCount(),1);
    controller.runEvent(begin); controller.handle(detection); QCOMPARE(controller.pendingCount(),1);
    auto end=begin;end[u"name"_s]=u"run.finished"_s;end[u"payload"_s]=QJsonObject{{u"outcome"_s,u"completed"_s}};
    controller.runEvent(end); QCOMPARE(controller.pendingCount(),0); controller.handle(other); QCOMPARE(controller.pendingCount(),0);
    controller.runEvent(begin);controller.handle(detection);
    for(auto *candidate:owner.findChildren<QDialog *>(u"anomalyDialog"_s))if(candidate->isVisible())dialog=candidate;
    QTest::mouseClick(dialog->findChild<QPushButton *>(u"anomalyStop"_s),Qt::LeftButton);
    QCOMPARE(stopped.count(),1);QCOMPARE(stopped.first()[0].toString(),u"r1"_s);QCOMPARE(stopped.first()[1].toString(),u"base"_s);
    auto stale=detection;stale[u"run_id"_s]=u"old"_s;const int records=controller.recordCount();controller.handle(stale);QCOMPARE(controller.recordCount(),records);
}

QTEST_MAIN(AnomalyTests)
#include "anomalytests.moc"
