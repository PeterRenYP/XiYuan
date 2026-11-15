#include "workbench.h"
#include "features.h"
#include "weightfilewindow.h"
#include "uiscale.h"
#include "chattokenview.h"
#include "neurons.h"
#include "anomalycontroller.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QLockFile>
#include <QUuid>
#include <QSaveFile>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#endif

using namespace Qt::StringLiterals;

namespace {
// 以下工厂只统一原型中的控件间距和字体，不封装业务状态。
QVBoxLayout *column(QWidget *widget, int margin = 0, int spacing = 0) {
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(spacing);
    return layout;
}
QHBoxLayout *row(QWidget *widget, int margin = 0, int spacing = 6) {
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(spacing);
    return layout;
}
QLabel *label(const QString &text, const QString &name = {}) {
    auto *value = new QLabel(text);
    value->setObjectName(name);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return value;
}
QPushButton *button(const QString &text, const std::function<void()> &action = {}) {
    auto *value = new QPushButton(text);
    value->setCursor(Qt::PointingHandCursor);
    if (action)
        QObject::connect(value, &QPushButton::clicked, value, action);
    return value;
}
QWidget *line() {
    auto *value = new QWidget;
    value->setObjectName(u"separator"_s);
    UiScale::fixedHeight(value, 1);
    return value;
}
QScrollArea *scrollArea(QWidget *widget) {
    auto *area = new QScrollArea;
    area->setWidget(widget);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return area;
}
QSplitter *splitter(Qt::Orientation orientation) {
    auto *value = new QSplitter(orientation);
    value->setHandleWidth(4);
    value->setChildrenCollapsible(false);
    return value;
}
QSpinBox *integer(int maximum, const QString &prefix = {}) {
    auto *value = new QSpinBox;
    value->setRange(0, maximum);
    value->setPrefix(prefix);
    value->setKeyboardTracking(false);
    return value;
}
QComboBox *combo(std::initializer_list<QPair<QString, QString>> values) {
    auto *value = new QComboBox;
    for (const auto &pair : values)
        value->addItem(pair.first, pair.second);
    return value;
}
QString shapeText(const QJsonArray &array) {
    QStringList values;
    for (auto value : array)
        values << QString::number(value.toInt());
    return values.isEmpty() ? u"未采集"_s : values.join(u" × "_s);
}
QString branchName(const QString &value) {
    return value == u"base"_s ? u"原始分支"_s : u"编辑分支 "_s + value.right(8);
}
QString forwardName(const QString &value) {
    return value == u"f0"_s ? u"输入预填充"_s : u"缓存解码 "_s + value.mid(1);
}
QIcon icon(const QString &key) {
    // 工具栏图标使用原生矢量描绘，保持与原型相同的细线风格。
    QPixmap image(96, 96);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.scale(4, 4);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(u"#a9bdc9"_s), 1.3));
    if (key == u"graph"_s) {
        p.drawRect(3, 3, 7, 7);
        p.drawRect(14, 14, 7, 7);
        p.drawLine(10, 6, 18, 6);
        p.drawLine(18, 6, 18, 14);
        p.drawLine(6, 10, 6, 18);
        p.drawLine(6, 18, 14, 18);
    } else if (key == u"neurons"_s) {
        // 四个胞体与弧形分叉交错，避免缩小时出现杂乱的放射线。
        const QList<QPointF> cells{{5,7},{16.5,4.5},{11.5,14},{20,18}};
        for (const auto &edge : QList<QPair<int,int>>{{0,1},{0,2},{1,2},{2,3},{1,3}}) {
            const auto a=cells[edge.first],b=cells[edge.second];
            QPainterPath branch(a);branch.quadTo((a+b)/2+QPointF(1.6,-2.2),b);p.drawPath(branch);
        }
        p.drawLine(QPointF(4.3,5),QPointF(3.4,2.5));p.drawLine(QPointF(3.4,2.5),QPointF(1.8,2.8));
        p.drawLine(QPointF(10.8,16),QPointF(8.5,20.5));p.drawLine(QPointF(8.5,20.5),QPointF(5.5,21));p.drawLine(QPointF(8.5,20.5),QPointF(9,23));
        p.drawLine(QPointF(21.5,18.5),QPointF(23,21));
        p.setBrush(QColor(u"#25282d"_s));for(const auto &cell:cells)p.drawEllipse(cell,1.9,1.9);
        p.setPen(QPen(QColor(u"#b5deeb"_s),1.3));p.setBrush(QColor(u"#344f60"_s));p.drawEllipse(cells[2],2.1,2.1);
    } else if (key == u"grid"_s) {
        for (int x : {4, 14})
            for (int y : {4, 14})
                p.drawRect(x, y, 6, 6);
    } else if (key == u"chart"_s) {
        p.drawLine(4, 3, 4, 21);
        p.drawLine(4, 21, 22, 21);
        QPolygonF points;
        points << QPointF(6, 17) << QPointF(11, 9) << QPointF(16, 13) << QPointF(21, 5);
        p.drawPolyline(points);
    } else if (key == u"chat"_s) {
        p.drawRect(3, 4, 18, 13);
        p.drawLine(3, 17, 3, 22);
        p.drawLine(3, 22, 9, 17);
        p.drawLine(7, 9, 17, 9);
        p.drawLine(7, 13, 14, 13);
    } else if (key == u"folder"_s) {
        QPolygonF points;
        points << QPointF(3, 6) << QPointF(10, 6) << QPointF(12, 9) << QPointF(21, 9) << QPointF(21, 20)
               << QPointF(3, 20) << QPointF(3, 6);
        p.drawPolyline(points);
    } else if (key == u"play"_s) {
        QPolygonF points;
        points << QPointF(7, 5) << QPointF(19, 12) << QPointF(7, 19) << QPointF(7, 5);
        p.drawPolyline(points);
    } else if (key == u"step"_s) {
        QPolygonF points;
        points << QPointF(4, 5) << QPointF(15, 12) << QPointF(4, 19) << QPointF(4, 5);
        p.drawPolyline(points);
        p.drawLine(20, 5, 20, 19);
    } else if (key == u"stop"_s) {
        p.drawRect(6, 6, 12, 12);
    } else if (key == u"activation"_s) {
        QPolygonF points;
        points << QPointF(2, 14) << QPointF(7, 14) << QPointF(10, 5) << QPointF(14, 20) << QPointF(17, 11) << QPointF(22, 11);
        p.drawPolyline(points);
    } else if (key == u"vectors"_s) {
        p.drawEllipse(3, 3, 5, 5);
        p.drawEllipse(15, 3, 5, 5);
        p.drawEllipse(9, 16, 5, 5);
        p.drawLine(8, 6, 15, 6);
        p.drawLine(6, 8, 11, 16);
        p.drawLine(17, 8, 13, 16);
    } else if (key == u"versions"_s) {
        p.drawEllipse(4, 3, 5, 5);
        p.drawEllipse(15, 9, 5, 5);
        p.drawEllipse(4, 17, 5, 5);
        p.drawLine(6, 8, 6, 17);
        p.drawLine(8, 6, 15, 11);
    } else if (key == u"regression"_s) {
        p.drawRect(4, 3, 16, 18);
        p.drawLine(7, 9, 10, 12);
        p.drawLine(10, 12, 16, 6);
        p.drawLine(7, 17, 16, 17);
    } else if (key == u"history"_s) {
        p.drawArc(4, 4, 17, 17, 35 * 16, 300 * 16);
        p.drawLine(4, 3, 4, 9);
        p.drawLine(4, 9, 10, 9);
        p.drawLine(12, 7, 12, 13);
        p.drawLine(12, 13, 16, 15);
    } else {
        p.drawLine(5, 3, 5, 21);
        p.drawLine(12, 3, 12, 21);
        p.drawLine(19, 3, 19, 21);
        p.drawLine(2, 8, 8, 8);
        p.drawLine(9, 16, 15, 16);
        p.drawLine(16, 6, 22, 6);
    }
    return QIcon(image);
}
const QString workbenchStyle = uR"(
QWidget{background:#25282d;color:#c3cede;font-family:'Microsoft YaHei UI';font-size:12px;}
QMainWindow{background:#1c1e22;} QLabel{background:transparent;} QLabel#muted{color:#97a6bb;font-size:11px;} QLabel#heading{font-size:16px;color:#d3dfed;} QLabel#eyebrow{font-size:11px;color:#92a8c0;}
QWidget#titlebar{background:#1b1d21;} QLabel#brand{color:#b7d4e9;border:1px solid #708ca4;padding:2px 4px;font-size:15px;} QWidget#controlbar{background:#292c31;border-bottom:1px solid #111316;}
QWidget#rail{background:#22252a;border-right:1px solid #13161a;} QWidget#separator{background:#171a1e;} QWidget#panelHeader{background:#23262a;border-bottom:1px solid #15181d;}
QMenuBar{background:#23262b;border-bottom:1px solid #121418;padding:1px 8px;spacing:8px;} QMenuBar::item{padding:5px 10px;background:transparent;} QMenuBar::item:selected,QMenu::item:selected{background:#394b59;} QMenu{border:1px solid #4d5c6e;background:#252b34;padding:5px;} QMenu::item{padding:7px 28px;}
QPushButton{background:#303944;border:1px solid #596b7c;border-radius:3px;padding:5px 9px;min-height:19px;color:#dce6ef;} QPushButton:hover{background:#3c4c5b;border-color:#90adbf;} QPushButton:pressed{background:#233c4e;border-color:#a7c9db;} QPushButton:checked{background:#344f60;border-color:#8ab4ca;color:#e1f2fa;} QPushButton:focus{border-color:#b4d5e7;} QPushButton:disabled{background:#292e35;border-color:#414a55;color:#8592a1;} QPushButton#primary{background:#405f71;border:1px solid #8db5ca;color:#edf7fc;} QPushButton#primary:hover{background:#4d748b;} QPushButton#primary:pressed{background:#294a60;} QPushButton#primary:disabled{background:#303b45;border-color:#4e606e;color:#8899a7;} QPushButton#secondary{background:#303c49;border:1px solid #697f93;}
QToolButton{background:#303944;border:1px solid #596b7c;border-radius:3px;padding:4px;} QToolButton:hover{background:#3c4c5b;border-color:#90adbf;} QToolButton:pressed,QToolButton:checked{background:#344f60;border-color:#8ab4ca;} QToolButton:focus{border-color:#b4d5e7;} QToolButton:disabled{background:#292e35;border-color:#414a55;color:#8592a1;}
QPushButton#workspace{background:#2d3640;border:1px solid #526170;border-bottom:2px solid #526170;border-radius:3px;padding:5px 12px;} QPushButton#workspace:checked{background:#354b5c;border-color:#83a9bf;border-bottom:2px solid #b4dbea;}
QPushButton#workspace:hover,QPushButton#secondary:hover,QPushButton#launcherAction:hover{background:#3c4c5b;border-color:#90adbf;} QPushButton#workspace:pressed,QPushButton#secondary:pressed,QPushButton#launcherAction:pressed{background:#233c4e;border-color:#a7c9db;} QPushButton#primary:focus,QPushButton#workspace:focus,QPushButton#secondary:focus,QPushButton#launcherAction:focus{border-color:#b4d5e7;} QPushButton#workspace:disabled,QPushButton#secondary:disabled,QPushButton#launcherAction:disabled{background:#292e35;border-color:#414a55;color:#8592a1;}
QWidget#captureNoticeFrame{background:#f2d47a;border:1px solid #b9963d;border-radius:4px;} QLabel#captureWarning{color:#463510;background:transparent;padding:8px 10px;} QLabel#captureNoticeIcon{color:#463510;background:transparent;font-size:18px;font-weight:bold;padding:8px;}
QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox{background:#202329;border:1px solid #424c5a;border-radius:2px;padding:5px 6px;min-height:18px;selection-background-color:#466275;} QLineEdit:focus,QSpinBox:focus,QDoubleSpinBox:focus,QComboBox:focus{border:1px solid #a7d6e5;} QComboBox{background:#171d25;border:1px solid #68798c;padding-right:32px;color:#e0e8f1;}
QComboBox:hover{background:#293745;border-color:#9bb6ca;} QComboBox:focus,QComboBox:on{border:1px solid #a7d6e5;background:#243440;} QComboBox:disabled{color:#778493;border-color:#414b58;}
QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:top right;width:26px;border-left:1px solid #586b7f;background:#303e4d;}
QComboBox::down-arrow{image:url(:/ui/chevron-down.xpm);width:12px;height:8px;}
QComboBox QAbstractItemView{background:#18222e;color:#e0e8f1;border:1px solid #9bb4c7;outline:0;padding:4px;selection-background-color:#466e8a;selection-color:#ffffff;}
QComboBox QAbstractItemView::item{min-height:26px;padding:4px 8px;} QComboBox QAbstractItemView::item:hover{background:#324c62;} QComboBox QAbstractItemView::item:selected{background:#466e8a;color:#ffffff;} QSpinBox::up-button,QSpinBox::down-button,QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:15px;}
QTreeWidget,QListWidget,QTextBrowser,QTableWidget{background:#22252a;border:0;outline:0;selection-background-color:#344b5a;alternate-background-color:#282d34;} QTreeWidget::item{height:27px;} QTreeWidget::item:selected,QListWidget::item:selected{background:#344d5c;color:#d7eaf6;} QTreeWidget::item:hover{background:#2d3743;} QListWidget::item{padding:10px 7px;border-bottom:1px solid #333a45;}
QTabWidget::pane{border:0;} QTabBar::tab{background:#24272c;border:0;border-bottom:2px solid transparent;padding:9px 12px;color:#99a8bd;} QTabBar::tab:selected{color:#d2e5f3;border-bottom:2px solid #a9d8e8;} QTabBar::tab:hover{background:#303a46;}
QScrollArea{border:0;} QScrollBar:vertical{background:#202329;width:7px;margin:0;} QScrollBar::handle:vertical{background:#4b5664;min-height:24px;border-radius:2px;} QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;} QScrollBar:horizontal{background:#202329;height:7px;} QScrollBar::handle:horizontal{background:#4b5664;min-width:24px;} QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}
QSplitter::handle{background:#15181d;} QSplitter::handle:hover{background:#7aaaba;} QHeaderView::section{background:#2b313a;border:0;border-right:1px solid #414b59;padding:5px;color:#a5b7ce;} QTableWidget{gridline-color:#39414d;}
QProgressBar{background:#202329;border:0;max-height:4px;} QProgressBar::chunk{background:#8799b0;} QProgressBar#modelLoadingProgress{min-height:24px;max-height:24px;border:1px solid #687e91;text-align:center;color:#e6f2fa;background:#18212b;} QProgressBar#modelLoadingProgress::chunk{background:#466f83;} QCheckBox{spacing:5px;color:#a7b9cf;background:transparent;} QCheckBox::indicator{width:12px;height:12px;border:1px solid #697c8d;background:#252b32;} QCheckBox::indicator:checked{background:#9ac6d7;border:1px solid #b0dce7;image:url(:/ui/checkmark.xpm);}
QLabel#formula{font-family:'Consolas';background:#20252b;border:1px solid #455364;border-radius:2px;padding:12px;line-height:1.6;color:#b9d6f6;}
QLabel#status{font-size:10px;color:#b4c7dc;} QToolTip{background:#303d4c;color:#e3edf6;border:1px solid #829db6;padding:5px;} QStatusBar{background:#22252a;border-top:1px solid #111419;min-height:23px;} QDialog{background:#272d35;} QWidget#launcher{background:#191b1e;} QLabel#launcherBrand{font-size:30px;color:#e0e7ed;} QPushButton#launcherAction{background:#292d32;border:1px solid #3c444e;padding:14px;} QListWidget#recentProjects{background:transparent;}
)"_s;
} // namespace

Workbench::Workbench(bool startProcesses)
    : worker(u"worker"_s, this), monitor(u"monitor"_s, this),
      settings(QSettings::defaultFormat(), QSettings::UserScope, u"ModelObservatory"_s, u"NativeWorkbench"_s) {
    // 布局与模型历史独立保存，迁移不会覆盖旧客户端偏好。
    if (!qEnvironmentVariable("MODEL_OBSERVATORY_SETTINGS_DIR").isEmpty()) settings.setFallbacksEnabled(false);
    verifying = QCoreApplication::arguments().contains(u"--probe-scale"_s);
    setWindowTitle(u"析元"_s);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(1180, 760);
    resize(1800, 1125);
    if (auto *screen = QGuiApplication::primaryScreen()) {
        const auto size = screen->availableGeometry().size();
        resize(std::min(1800, size.width() - 24), std::min(1125, size.height() - 40));
    }
    // 首次启动由用户选择模型目录，后续沿用本人保存的路径。
    modelPath = settings.value(u"model_path"_s).toString();
    runtimeEnabled = startProcesses;
    build();
    // 异常提示不连接任何生成控制槽；所有用户选择只改变提醒状态。
    anomalies = new AnomalyController(this);
    // 只取消弹窗所属的当前运行，避免旧提示误停新会话。
    connect(anomalies, &AnomalyController::stopRequested, this, [this](const QString &id, const QString &runBranch) {
        if (active && runId == id && branch == runBranch) cancel();
    });
    auto *anomalyStatus = new QLabel(u"异常监控未启动"_s);
    anomalyStatus->setObjectName(u"anomalyStatus"_s);
    anomalyStatus->setToolTip(u"内部指标采用轮换采样，基线不足时仅执行保护规则。"_s);
    auto *anomalyRecords = new QPushButton(u"异常记录"_s);
    anomalyRecords->setObjectName(u"anomalyRecords"_s);
    statusBar()->addPermanentWidget(anomalyStatus);
    statusBar()->addPermanentWidget(anomalyRecords);
    connect(anomalies, &AnomalyController::changed, anomalyStatus, &QLabel::setText);
    connect(anomalies, &AnomalyController::ready, this, [this](const QJsonObject &endpoint) { worker.setAnomalyEndpoint(endpoint); });
    connect(anomalyRecords, &QPushButton::clicked, anomalies, &AnomalyController::showRecords);
    uiScale = new UiScale(this, workbenchStyle + prototypeWorkbenchStyle());
    connect(uiScale, &UiScale::changed, this, [this](double factor) {
        graph->setInterfaceScale(factor);
        interfaceZoom->setText(u"界面 "_s + QString::number(qRound(factor * 100)) + u"%"_s);
    });
    uiScale->setFactor(settings.value(u"ui_scale"_s, 1.0).toDouble());
    connect(&worker, &Transport::message, this, &Workbench::receive);
    connect(&worker, &Transport::failed, this, [this](const QString &text) {
        loading = false;
        editing = false;
        featureRequestId.clear();
        ruleRequestId.clear();comparisonRequestId.clear();previewSliceId.clear();
        if (features) features->failure(text);
        finishModelLoading();
        setActive(false);
        showError(text);
    });
    connect(&worker, &Transport::stopped, this, [this](int code) {
        anomalies->runEvent({{u"name"_s, u"worker.stopped"_s}});
        tokenAnalysisRequests.clear();
        if (!closing) {
            loaded = false;
            loading = false;
            finishModelLoading();
            editing = false;
            featureRequestId.clear();
            ruleRequestId.clear();comparisonRequestId.clear();previewSliceId.clear();
            if (features) features->failure(u"运行器已退出，请重新启动客户端"_s);
            setActive(false);
            showError(u"运行器已退出，退出码："_s + QString::number(code) + u"。请重新启动客户端。"_s);
        }
    });
    connect(&monitor, &Transport::message, this, &Workbench::metric);
    connect(&monitor, &Transport::failed, this, [this](const QString &text) {
        gpuName->setText(u"监控不可用"_s);
        showError(u"资源监控进程异常："_s + text);
    });
    auto *refresh = new QTimer(this);
    refresh->setInterval(130);
    connect(refresh, &QTimer::timeout, this, [this] {
        if (dirty) {
            dirty = false;
            if (active && follow->isChecked() && !execution.isEmpty()) {
                forward = execution.value(u"forward_id"_s).toString(forward);
                choose(std::max(0, executionLayer), execution.value(u"operation_id"_s).toString(operation),
                       false);
                dirty = false;
            }
            query();
        }
        if (active)
            timeLabel->setText(QString::number(elapsed.elapsed() / 1000.0, 'f', 2) + u" 秒"_s);
    });
    refresh->start();

    new QShortcut(QKeySequence(u"Ctrl+S"_s), this, [this] { saveProject(); });
    new QShortcut(QKeySequence(u"Ctrl+S"_s), conversationWindow, [this] { saveProject(); });
    new QShortcut(QKeySequence(u"Ctrl+Shift+N"_s), conversationWindow, [this] { newSession(); });
    new QShortcut(QKeySequence(u"Ctrl+Shift+N"_s), this, [this] { newSession(); });
    new QShortcut(QKeySequence(u"F11"_s), this, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    new QShortcut(QKeySequence(u"Escape"_s), this, [this] {
        if (isFullScreen())
            showNormal();
    });
    resetLayout();
    for (auto pair : {qMakePair(mainSplit, u"main_v2"_s), qMakePair(leftSplit, u"left"_s),
                      qMakePair(navSplit, u"nav"_s)})
        if (settings.contains(pair.second))
            pair.first->restoreState(settings.value(pair.second).toByteArray());
    renderSelection();
    sizeConversation();
    setActive(false);
    // 界面偏好独立于会话保存，重启后仍使用最后选择的操作页。
    const auto preferences=QJsonDocument::fromJson(settings.value(u"workspace_preferences"_s).toByteArray()).object();
    if(!preferences.isEmpty()){
        inspector->setCurrentIndex(preferences.value(u"inspector"_s).toInt());
        navigatorTabs->setCurrentIndex(preferences.value(u"navigator_tab"_s).toInt());
        timelineTabs->setCurrentIndex(preferences.value(u"timeline_tab"_s).toInt());
        search->setText(preferences.value(u"search"_s).toString());
        if(preferences.value(u"center_view"_s).toInt()==2)openFeature(std::clamp(preferences.value(u"feature_page"_s).toInt(),0,3),false);
        else if(preferences.value(u"center_view"_s).toInt()==3)showRulesPage();
        else if(preferences.value(u"center_view"_s).toInt()==4)showHistoryPage();
        else changeView(preferences.value(u"view_type"_s).toInt());
        neurons->restoreView(preferences.value(u"neuron_flat"_s).toBool());
        if(preferences.value(u"neuron_page"_s).toBool())showNeurons();
        inspector->setCurrentIndex(preferences.value(u"inspector"_s).toInt());
    }
    shell->setCurrentIndex(0);
    // 启动默认停留在项目选择器；模型进程只在打开项目时启动。
    auto *autosave = new QTimer(this);
    autosave->setInterval(15000);
    connect(autosave, &QTimer::timeout, this, [this] {
        if (!verifying && !active && !restoringSession && !projectDirectory.isEmpty())
            saveProject(false);
    });
    autosave->start();
    conversationWindow->restoreGeometry(settings.value(u"conversation_geometry"_s).toByteArray());
}
Workbench::~Workbench() {
    closing = true;
    worker.stop();
    monitor.stop();
    delete projectLock;
}

void Workbench::build() {
    // 整体层级严格跟随原型：标题、菜单、运行栏、工具工作区、状态栏。
    auto *root = new QWidget;
    auto *layout = column(root);
    // 极高倍率或小屏幕允许整体滚动，所有操作仍可通过鼠标或键盘访问。
    auto *workspaceScroll = new QScrollArea;
    workspaceScroll->setFrameShape(QFrame::NoFrame);
    workspaceScroll->setWidgetResizable(true);
    workspaceScroll->setWidget(root);

    maximum = integer(262143);
    maximum->setMinimum(1);
    maximum->setValue(maximum->maximum());
    maximum->setParent(this);
    temperature = new QDoubleSpinBox(this);
    temperature->setRange(0, 3);
    temperature->setValue(0.7);
    temperature->hide();
    thinkingTemperature = new QDoubleSpinBox(this);
    thinkingTemperature->setRange(0, 3);
    thinkingTemperature->setValue(1.0);
    thinkingTemperature->hide();
    titlebar = new QWidget;
    titlebar->setObjectName(u"titlebar"_s);
    UiScale::fixedHeight(titlebar, 30);
    titlebar->installEventFilter(this);
    auto *title = row(titlebar, 4, 10);
    auto *brandIcon=label({});brandIcon->setPixmap(qApp->windowIcon().pixmap(22,22));title->addWidget(brandIcon);
    title->addWidget(label(u"析元"_s));
    projectLabel = label(u"│　未命名观测项目"_s, u"muted"_s);
    title->addWidget(projectLabel);
    title->addStretch();
    title->addWidget(label(u"本地模型 · 原生工作区"_s, u"muted"_s));
    title->addStretch();
    for (auto pair : {qMakePair(u"—"_s, 0), qMakePair(u"□"_s, 1), qMakePair(u"×"_s, 2)}) {
        auto *control = button(pair.first, [this, which = pair.second] {
            if (which == 0)
                showMinimized();
            else if (which == 1)
                isMaximized() ? showNormal() : showMaximized();
            else
                close();
        });
        UiScale::fixedWidth(control, 36);
        UiScale::fixedHeight(control, 24);
        title->addWidget(control);
    }
    layout->addWidget(titlebar);
    auto *menuRow = new QWidget;
    auto *menuLayout = row(menuRow, 0, 0);
    auto *menus = new QMenuBar;
    auto *file = menus->addMenu(u"文件"_s);
    file->addAction(u"新建项目"_s, this, &Workbench::newProject);
    file->addAction(u"打开项目"_s, QKeySequence(u"Ctrl+O"_s), this, &Workbench::openProjectDialog);
    file->addAction(u"打开权重文件"_s,this,[this]{openWeightFile();});
    file->addAction(u"保存项目现场"_s, this, [this] { saveProject(); });
    file->addAction(u"关闭项目"_s, this, &Workbench::showLauncher);
    file->addAction(u"项目存储管理"_s, this, &Workbench::showStorage);
    file->addSeparator();
    file->addAction(u"模型与生成设置"_s, this, &Workbench::configureModel);
    file->addAction(u"打开离线记录"_s, this, &Workbench::openTrace);
    file->addAction(u"导出运行记录"_s, this, &Workbench::exportTrace);
    file->addAction(u"导出模型副本"_s, this, &Workbench::exportModel);
    file->addSeparator();
    file->addAction(u"退出"_s, this, &QWidget::close);
    auto *editMenu = menus->addMenu(u"编辑"_s);
    editMenu->addAction(u"临时编辑"_s, this, [this] {
        inspector->setCurrentIndex(2);
        editTarget->setCurrentIndex(0);
    });
    editMenu->addAction(u"修改模型"_s, this, [this] {
        inspector->setCurrentIndex(2);
        editTarget->setCurrentIndex(1);
    });
    editMenu->addAction(u"从备份恢复模型"_s, this, &Workbench::restoreBackup);
    auto *view = menus->addMenu(u"视图"_s);
    view->addAction(u"计算图"_s, this, [this] { changeView(0); });
    view->addAction(u"激活空间"_s, this, [this] { changeView(1); });
    view->addAction(u"注意力与状态"_s, this, [this] { changeView(2); });
    view->addAction(u"神经元"_s, this, &Workbench::showNeurons);
    view->addAction(u"适合画布"_s, this, [this] { graph->fitGraph(); });
    view->addSeparator();
    view->addAction(u"放大界面\tCtrl++"_s, this, [this] { uiScale->setFactor(uiScale->factor() + .1); });
    view->addAction(u"缩小界面\tCtrl+-"_s, this, [this] { uiScale->setFactor(uiScale->factor() - .1); });
    view->addAction(u"恢复界面大小\tCtrl+0"_s, this, [this] { uiScale->setFactor(1); });
    auto *run = menus->addMenu(u"运行"_s);
    // 回归验证移出左侧导航后仍可从应用菜单访问。
    run->addAction(u"回归验证"_s, this, [this] { openFeature(3); });
    run->addAction(u"断点与监视"_s, this, &Workbench::configureDebug);
    run->addAction(u"继续运行"_s, this, [this]{
        if(!active)return;
        boundary={};worker.send(u"run.change_mode"_s,{{u"mode"_s,u"observe"_s}});setActive(true);
    });
    run->addAction(u"单步执行"_s,QKeySequence(u"F10"_s),this,&Workbench::releaseStep);
    run->addAction(u"停止生成"_s, this, &Workbench::cancel);
    auto *window = menus->addMenu(u"窗口"_s);
    window->addAction(u"打开对话"_s, this, &Workbench::showConversation);
    window->addAction(u"重置布局"_s, this, &Workbench::resetLayout);
    window->addAction(u"调整分区尺寸"_s, this, [this] {
        // 数值入口补充拖动操作，便于键盘和精确尺寸调整。
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(u"调整分区尺寸"_s);
        auto *layout = column(dialog, 18, 10);
        auto *form = new QFormLayout;
        QList<QSpinBox *> fields;
        const QStringList names{u"左侧模型树宽度"_s, u"右侧检查器宽度"_s, u"底部时间轴高度"_s,
                                u"独立对话窗口高度"_s};
        const QList<int> sizes{navSplit->sizes().first(), mainSplit->sizes().last(),
                               leftSplit->sizes().last(), conversationWindow->height()};
        for (int i = 0; i < names.size(); ++i) {
            auto *control = integer(2000);
            control->setMinimum(100);
            control->setValue(sizes[i]);
            form->addRow(names[i], control);
            fields.append(control);
        }
        layout->addLayout(form);
        layout->addWidget(button(u"应用"_s, [this, dialog, fields] {
            navSplit->setSizes({fields[0]->value(), std::max(400, navSplit->width() - fields[0]->value())});
            const int chatWidth = conversationEmbedded ? conversationHost->width() : 0;
            if(mainSplit->count()==3)
                mainSplit->setSizes({std::max(350, mainSplit->width() - fields[1]->value() - chatWidth), chatWidth, fields[1]->value()});
            else mainSplit->setSizes({std::max(350, mainSplit->width() - fields[1]->value()), fields[1]->value()});
            leftSplit->setSizes(
                {std::max(300, leftSplit->height() - fields[2]->value()), fields[2]->value()});
            conversationWindow->resize(conversationWindow->width(), fields[3]->value());
            dialog->close();
        }));
        dialog->open();
    });
    window->addAction(u"全屏"_s, this, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    auto *help = menus->addMenu(u"帮助"_s);
    help->addAction(u"操作提示"_s, this, [this] {
        status(
            u"点击节点检查计算；滚轮缩放画布；Ctrl++ / Ctrl+- 缩放界面，Ctrl+0 复原；拖动边界调整分区；在对话窗口选择运行模式，Enter 发送。临时编辑不写入模型，保存到模型将备份并写回权重。"_s);
    });
    menuLayout->addWidget(menus, 1);
    menuLayout->addWidget(button(u"重置布局"_s, [this] { resetLayout(); }));
    layout->addWidget(menuRow);
    auto *controlbar = new QWidget;
    controlbar->setObjectName(u"controlbar"_s);
    UiScale::fixedHeight(controlbar, 47);
    auto *controls = row(controlbar, 8, 8);
    auto *modelButton = button(u"模型目录与切换"_s, [this] { configureModel(); });
    modelButton->setIcon(icon(u"folder"_s));
    controls->addWidget(modelButton);
    modelLabel = label(u"未加载模型"_s);
    controls->addWidget(modelLabel);
    modelMeta = label(u"请选择本地目录"_s, u"muted"_s);
    controls->addWidget(modelMeta);
    controls->addStretch();
    tokenInfo = label(u"等待运行"_s, u"muted"_s);
    controls->addWidget(tokenInfo);
    controls->addWidget(label(u"输出上限"_s, u"muted"_s));
    maximum->setSuffix(u" Token"_s);
    controls->addWidget(maximum);
    maximum->show();
    controls->addStretch();
    play = button(u"开始执行"_s, [this] { runPrompt(prompt->text()); });
    play->setIcon(icon(u"play"_s));
    play->setObjectName(u"primary"_s);
    controls->addWidget(play);
    step = button(u"单步  F10"_s, [this] { releaseStep(); });
    step->setIcon(icon(u"step"_s));
    controls->addWidget(step);
    step->setToolTip(u"在暂停边界放行一次计算"_s);
    stop = button(u"停止"_s, [this] { cancel(); });
    stop->setIcon(icon(u"stop"_s));
    controls->addWidget(stop);
    runLabel = label(u"未加载"_s, u"muted"_s);
    UiScale::minimumWidth(runLabel, 80);
    controls->addWidget(runLabel);
    layout->addWidget(controlbar);
    auto *body = new QWidget;
    auto *bodyRow = row(body, 0, 0);
    auto *rail = new QWidget;
    rail->setObjectName(u"rail"_s);
    UiScale::fixedWidth(rail, 40);
    auto *railLayout = column(rail, 2, 8);
    const QStringList railNames{u"计算图"_s, u"激活空间"_s, u"注意力与状态"_s, u"对话"_s, u"运行记录"_s,
        u"本轮激活"_s, u"词表与向量"_s, u"补丁与版本"_s, u"回归验证"_s, u"权重文件"_s, u"断点与监视"_s, u"神经元"_s};
    const QStringList railIcons{u"graph"_s, u"grid"_s, u"chart"_s, u"chat"_s, u"history"_s,
        u"activation"_s, u"vectors"_s, u"versions"_s, u"regression"_s, u"history"_s, u"step"_s, u"neurons"_s};
    for (int i = 0; i < railNames.size(); ++i) {
        auto *control = button({}, [this, i] {
            if (i < 3)
                changeView(i);
            else if (i == 3) {
                changeView(0);
                setConversationEmbedded(true);
                syncNavigation(3);
                prompt->setFocus();
            }
            else if (i == 4)
                navigatorTabs->setCurrentIndex(1);
            else if(i==9)openWeightFile();
            else if(i==10)configureDebug();
            else if(i==11)showNeurons();
            else
                openFeature(i - 5);
        });
        control->setIcon(icon(railIcons[i]));
        control->setIconSize(QSize(22, 22));
        UiScale::fixedWidth(control, 35);
        UiScale::fixedHeight(control, 35);
        control->setToolTip(railNames[i]);
        control->setAccessibleName(railNames[i]);
        control->setObjectName(u"rail-"_s + QString::number(i));
        if (i < 3 || (i >= 5 && i <= 8) || i == 11) control->setCheckable(true);
        control->setChecked(i == 0);
        railButtons.append(control);
        if(i==11)railLayout->insertWidget(3,control);
        else railLayout->addWidget(control);
        if (i == 2 || i == 4)
            railLayout->addWidget(line());
    }
    railLayout->addStretch();
    auto *configuration = button({}, [this] { configureModel(); });
    configuration->setIcon(icon(u"settings"_s));
    configuration->setToolTip(u"模型与生成设置"_s);
    configuration->setAccessibleName(u"模型与生成设置"_s);
    railLayout->addWidget(configuration);
    bodyRow->addWidget(rail);
    mainSplit = splitter(Qt::Horizontal);
    leftSplit = splitter(Qt::Vertical);
    navSplit = splitter(Qt::Horizontal);
    navigator = buildNavigator();
    center = buildCenter();
    navSplit->addWidget(navigator);
    navSplit->addWidget(center);
    navSplit->setStretchFactor(1, 1);
    leftSplit->addWidget(navSplit);
    timelinePanel = buildTimeline();
    leftSplit->addWidget(timelinePanel);
    leftSplit->setStretchFactor(0, 1);
    mainSplit->addWidget(leftSplit);
    mainSplit->addWidget(buildInspector());
    mainSplit->setStretchFactor(0, 1);
    workspaceViews = new QStackedWidget;
    workspaceViews->addWidget(mainSplit);
    neurons = new NeuronPanel;
    workspaceViews->addWidget(neurons);
    connect(neurons,&NeuronPanel::subscriptionChanged,this,&Workbench::syncNeurons,Qt::QueuedConnection);
    connect(neurons,&NeuronPanel::queryRequested,this,&Workbench::queryNeurons,Qt::QueuedConnection);
    bodyRow->addWidget(workspaceViews, 1);
    layout->addWidget(body, 1);
    statusLabel = label(u"请选择本地模型目录，加载后开始观测"_s, u"status"_s);
    selectionLabel = label(u"真实运行器 · 尚未连接模型"_s, u"status"_s);
    statusBar()->addWidget(statusLabel, 1);
    statusBar()->addPermanentWidget(selectionLabel);
    interfaceZoom = button(u"界面 100%"_s, [this] { uiScale->setFactor(1); });
    interfaceZoom->setToolTip(u"Ctrl++ 放大，Ctrl+- 缩小，Ctrl+0 恢复默认；点击恢复默认"_s);
    statusBar()->addPermanentWidget(interfaceZoom);
    // 工作区与启动页是原生堆叠页面，模型与控件对象无需反复创建。
    shell = new QStackedWidget;
    shell->addWidget(buildLauncher());
    workspaceScroll->setParent(nullptr);
    shell->addWidget(workspaceScroll);
    setCentralWidget(shell);
    shell->setCurrentIndex(0);
    applyPrototypeLayout();
}

QWidget *Workbench::buildNavigator() {
    // 左侧树、历史与显卡指标保持固定位置，允许用户拖动改变宽度。
    auto *panel = new QWidget;
    UiScale::minimumWidth(panel, 180);
    auto *layout = column(panel);
    navigatorTabs = new QTabWidget;
    navigatorTabs->setDocumentMode(true);
    auto *structure = new QWidget;
    auto *content = column(structure, 10, 9);
    search = new QLineEdit;
    search->setPlaceholderText(u"查找层或模块"_s);
    search->setAccessibleName(u"查找层或模块"_s);
    content->addWidget(search);
    content->addWidget(label(u"本地模型 / 文本解码器"_s, u"muted"_s));
    tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setColumnCount(2);
    tree->setIndentation(13);
    tree->setColumnWidth(0, 135);
    tree->header()->setStretchLastSection(true);
    tree->setAccessibleName(u"模型结构树"_s);
    content->addWidget(tree, 1);
    connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        choose(item->data(0, Qt::UserRole).toInt(), item->data(1, Qt::UserRole).toString());
    });
    connect(search, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *item = tree->topLevelItem(i);
            bool match = item->text(0).contains(text, Qt::CaseInsensitive) ||
                         item->text(1).contains(text, Qt::CaseInsensitive);
            for (int j = 0; j < item->childCount(); ++j) {
                auto *child = item->child(j);
                bool found = child->text(0).contains(text, Qt::CaseInsensitive);
                child->setHidden(!found && !match);
                match = match || found;
            }
            item->setHidden(!match);
        }
    });
    content->addWidget(label(u"线性注意力　/　全注意力"_s, u"muted"_s));
    auto *directory = button(u"模型目录与切换"_s, [this] { configureModel(); });
    directory->setIcon(icon(u"folder"_s));
    content->addWidget(directory);
    navigatorTabs->addTab(structure, u"模型结构"_s);
    history = new QListWidget;
    history->addItem(u"原始分支"_s);
    history->item(0)->setData(Qt::UserRole, u"base"_s);
    connect(history, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        branchSelect->setCurrentIndex(branchSelect->findData(item->data(Qt::UserRole)));
    });
    navigatorTabs->addTab(history, u"运行记录"_s);
    layout->addWidget(navigatorTabs, 1);
    layout->addWidget(line());
    auto *resources = new QWidget;
    auto *resourceLayout = column(resources, 12, 5);
    resourceLayout->addWidget(label(u"资源监控"_s));
    gpuMemory = label(u"显存　等待采样"_s, u"muted"_s);
    resourceLayout->addWidget(gpuMemory);
    memoryBar = new QProgressBar;
    memoryBar->setTextVisible(false);
    resourceLayout->addWidget(memoryBar);
    gpuUtilization = label(u"计算利用率　等待采样"_s, u"muted"_s);
    resourceLayout->addWidget(gpuUtilization);
    gpuPlot = new ResourcePlot;
    resourceLayout->addWidget(gpuPlot);
    gpuName = label(u"独立硬件采样 / 每秒更新"_s, u"muted"_s);
    gpuName->setWordWrap(true);
    resourceLayout->addWidget(gpuName);
    layout->addWidget(resources);
    return panel;
}

QWidget *Workbench::buildCenter() {
    // 中央画布和对话使用独立竖向分隔器，不把对话压进浮动窗口。
    auto *panel = new QWidget;
    UiScale::minimumWidth(panel, 400);
    auto *layout = column(panel);
    auto *document = new QWidget;
    document->setObjectName(u"panelHeader"_s);
    UiScale::fixedHeight(document, 33);
    auto *docRow = row(document, 8, 10);
    docRow->addWidget(label(u"观测会话 01"_s));
    docRow->addStretch();
    docRow->addWidget(label(u"真实采集 / 本地运行"_s, u"muted"_s));
    layout->addWidget(document);
    auto *viewbar = new QWidget;
    auto *views = row(viewbar, 3, 5);
    const QStringList names{u"计算图"_s, u"激活空间"_s, u"注意力"_s};
    for (int i = 0; i < 3; ++i) {
        auto *control = button(names[i], [this, i] { changeView(i); });
        control->setObjectName(u"workspace"_s);
        control->setCheckable(true);
        viewButtons.append(control);
        views->addWidget(control);
    }
    viewButtons[0]->setChecked(true);
    views->addStretch();
    views->addWidget(button(u"平移"_s, [this] { graph->setPan(true); }));
    views->addWidget(button(u"−"_s, [this] { graph->changeZoom(1 / 1.12); }));
    zoomLabel = button(u"100%"_s, [this] { graph->fitGraph(); });
    views->addWidget(zoomLabel);
    views->addWidget(button(u"+"_s, [this] { graph->changeZoom(1.12); }));
    views->addWidget(button(u"适合"_s, [this] { graph->fitGraph(); }));
    layout->addWidget(viewbar);
    strip = new LayerStrip;
    connect(strip, &LayerStrip::layerSelected, this, [this](int index) { choose(index, operation); });
    connect(strip, &LayerStrip::endpointSelected, this, [this](const QString &op) { choose(layer, op); });
    layout->addWidget(strip);
    layout->addWidget(line());
    auto *breadcrumb = new QWidget;
    auto *crumb = row(breadcrumb, 6, 6);
    positionLabel = label(u"模型 › 文本解码器"_s, u"muted"_s);
    crumb->addWidget(positionLabel, 1);
    follow = new QCheckBox(u"跟随执行"_s);
    follow->setChecked(true);
    crumb->addWidget(follow);
    connect(follow, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled && !execution.isEmpty()) {
            branch = execution.value(u"branch_id"_s).toString(branch);
            forward = execution.value(u"forward_id"_s).toString(forward);
            choose(std::max(0, executionLayer), execution.value(u"operation_id"_s).toString(operation),
                   false);
        }
    });
    layout->addWidget(breadcrumb);
    observationControls = {viewbar, strip, breadcrumb};
    centerViews = new QStackedWidget;
    graph = new GraphView;
    connect(graph, &GraphView::selected, this, [this](const QString &op) { choose(layer, op); });
    connect(graph, &GraphView::zoomChanged, this,
            [this](int percent) { zoomLabel->setText(QString::number(percent) + u"%"_s); });
    centerViews->addWidget(graph);
    auto *matrixPanel = new QWidget;
    auto *matrixLayout = column(matrixPanel, 18, 10);
    largeTitle = label(u"激活空间"_s, u"heading"_s);
    matrixLayout->addWidget(largeTitle);
    matrixLayout->addWidget(label(u"显示当前层与Token的真实切片。橙色为负值，蓝色为正值。"_s, u"muted"_s));
    largeHeat = new Heatmap;
    UiScale::minimumHeight(largeHeat, 180);
    matrixLayout->addWidget(largeHeat, 1);
    matrixLayout->addWidget(label(u"切换层、运算和注意力头可查看其他位置；点击单元查看数值。"_s, u"muted"_s));
    centerViews->addWidget(matrixPanel);
    features = new FeaturePanel;
    centerViews->addWidget(features);
    connect(features, &FeaturePanel::requested, this, [this](QJsonObject payload) {
        // 功能请求复用运行器版本与任务互斥，预览时记录的版本不能被覆盖。
        if (active || loading || editing) { features->failure(u"请等待当前任务结束"_s); return; }
        if (!payload.contains(u"expected_weights_revision"_s)) payload[u"expected_weights_revision"_s] = weightsRevision;
        editing = true;
        setActive(false);
        features->pending(true, payload.value(u"action"_s).toString());
        featureRequestId = worker.send(u"feature.request"_s, payload);
    });
    connect(features, &FeaturePanel::cancelled, this, [this] { worker.send(u"run.cancel"_s); });
    connect(features,&FeaturePanel::monitored,this,[this](const QJsonObject &row){
        if(active||loading||editing){features->failure(u"请等待当前任务结束"_s);return;}
        neuron->setValue(row.value(u"neuron"_s).toInt());choose(row.value(u"layer"_s).toInt(),u"mlp"_s);
        setProperty("candidateWatch",true);configureDebug();
    });
    connect(features, &FeaturePanel::located, this, [this](const QJsonObject &row, bool draft) {
        // 跳转定位保留来源的前向与位置；加入干预只生成草稿，不自动应用。
        if (active || loading || editing) { features->failure(u"请等待当前任务结束"_s); return; }
        const auto analysis = features->activationReport;
        if (tracePath.isEmpty() || QDir(analysis.value(u"trace"_s).toString()).absolutePath() != QDir(tracePath).absolutePath() ||
            analysis.value(u"branch_id"_s).toString() != branch) {
            features->failure(u"请先打开分析的来源记录和分支"_s);
            return;
        }
        follow->setChecked(false);
        forward = row.value(u"forward_id"_s).toString();
        forwardSelect->setCurrentIndex(forwardSelect->findData(forward));
        tokenPosition->setValue(row.value(u"position"_s).toInt());
        neuron->setValue(row.value(u"neuron"_s).toInt());
        choose(row.value(u"layer"_s).toInt(), u"mlp"_s);
        changeView(0);
        inspector->setCurrentIndex(draft ? 2 : 0);
        if (draft && loaded && !offline) {
            editTarget->setCurrentIndex(0);
            ruleLayer->setValue(row.value(u"layer"_s).toInt());
            ruleNeuron->setValue(row.value(u"neuron"_s).toInt());
            ruleKind->setCurrentIndex(ruleKind->findData(u"zero"_s));
            ruleScope->setCurrentIndex(ruleScope->findData(u"all"_s));
            ruleValue->setValue(0);
            addRule();
            status(u"已加入置零草稿；可改为缩放后应用，并用相同输入重跑对照"_s);
        } else if (draft) status(u"离线记录只支持定位，请先加载对应模型"_s);
        dirty = true;
    });
    layout->addWidget(centerViews, 1);
    // 对话使用独立普通窗口，不占用观测画布，也不阻塞主窗口操作。
    conversationWindow = new QWidget(this, Qt::Window | Qt::WindowMinimizeButtonHint |
                                              Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    conversationWindow->setWindowTitle(u"对话 · 析元"_s);
    conversationWindow->setObjectName(u"conversationWindow"_s);
    conversationWindow->setAttribute(Qt::WA_QuitOnClose, false);
    conversationWindow->resize(1100, 700);
    conversationWindow->setMinimumSize(800, 420);
    conversationWindow->installEventFilter(this);
    auto *windowLayout = column(conversationWindow);
    auto *conversationSplit = new QSplitter(Qt::Horizontal);
    auto *sessionPanel = new QWidget;
    auto *sessionLayout = column(sessionPanel, 10, 8);
    sessionLayout->addWidget(label(u"会话"_s));
    sessionLayout->addWidget(button(u"新建会话"_s, [this] { newSession(); }));
    deleteAllSessionsButton = button(u"删除所有会话"_s, [this] { deleteAllSessions(); });
    sessionLayout->addWidget(deleteAllSessionsButton);
    sessionList = new QListWidget;
    sessionList->setObjectName(u"sessionList"_s);
    sessionList->setAccessibleName(u"项目会话列表"_s);
    sessionList->setContextMenuPolicy(Qt::CustomContextMenu);
    // 右键由菜单处理，不能交给列表默认行为切换当前会话并重建条目。
    sessionList->viewport()->installEventFilter(this);
    sessionLayout->addWidget(sessionList, 1);
    connect(sessionList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString();
        // 等本次鼠标事件结束再切换，快速连选时仅处理最终仍被选中的会话。
        QTimer::singleShot(0, this, [this, id] {
            auto *selected = sessionList->currentItem();
            if (selected && selected->data(Qt::UserRole).toString() == id) activateSession(id);
        });
    });
    connect(sessionList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = sessionList->itemAt(position);
        if (!item || active || loading || editing)
            return;
        auto *menu = new QMenu(sessionList);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        const auto id = item->data(Qt::UserRole).toString();
        menu->addAction(u"修改会话标题"_s, this, [this, id] { renameSession(id); });
        menu->addAction(u"删除会话"_s, this, [this, id] { activateSession(id); clearSession(true); });
        menu->popup(sessionList->viewport()->mapToGlobal(position));
    });
    conversationSplit->addWidget(sessionPanel);
    auto *conversation = new QWidget;
    UiScale::minimumHeight(conversation, 115);
    auto *conversationLayout = column(conversation, 9, 5);
    auto *chatTitle = new QWidget;
    auto *chatTitleRow = row(chatTitle);
    chatTitleRow->addWidget(label(u"对话"_s));
    chatTitleRow->addWidget(button(u"清空"_s, [this] { clearSession(); }));
    resumeSaved = button(u"恢复已保存运行"_s, [this] { resumeSavedRun(); });
    resumeSaved->hide();
    chatTitleRow->addWidget(resumeSaved);
    chatTitleRow->addStretch();
    tokenSplit = new QCheckBox(u"Token拆分"_s);
    tokenSplit->setAccessibleName(u"按实际生成 Token 拆分并显示边框"_s);
    tokenSplit->setToolTip(u"拆分后右键某个 Token，点击“分析”查看其生成依据；字节续片保留独立编号。"_s);
    tokenSplit->setChecked(settings.value(u"token_split"_s, false).toBool());
    chatTitleRow->addWidget(tokenSplit);
    chatTitleRow->addWidget(button(u"最小化"_s, [this] { conversationWindow->showMinimized(); }));
    conversationLayout->addWidget(chatTitle);
    captureNoticeFrame = new QWidget;
    captureNoticeFrame->setObjectName(u"captureNoticeFrame"_s);
    auto *noticeRow = row(captureNoticeFrame, 3, 0);
    auto *noticeIcon = label(u"!"_s, u"captureNoticeIcon"_s);
    noticeIcon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    noticeRow->addWidget(noticeIcon);
    captureWarning = label({}, u"captureWarning"_s);
    captureWarning->setTextFormat(Qt::PlainText);
    captureWarning->setWordWrap(true);
    noticeRow->addWidget(captureWarning, 1);
    captureWarning->hide();
    captureNoticeFrame->hide();
    conversationLayout->addWidget(captureNoticeFrame);
    auto *tokenChat = new ChatTokenView;
    chat = tokenChat;
    // 合并密集的生成事件，给动画、停止按钮和 MCP 留出界面处理时间。
    chatRefresh.setSingleShot(true);
    chatRefresh.setInterval(80);
    connect(&chatRefresh, &QTimer::timeout, this, [this] { renderChat(true); });
    tokenChat->tokenMenu = [this](const QString &anchor, const QPoint &point) { showTokenMenu(anchor, point); };
    connect(tokenSplit, &QCheckBox::toggled, this, [this](bool checked) {
        settings.setValue(u"token_split"_s, checked);
        renderChat();
    });
    UiScale::minimumHeight(chat, 36);
    chat->setOpenExternalLinks(false);
    chat->setPlainText(u"请选择并加载模型。计算图、张量和执行时间轴将在运行后显示真实数据。"_s);
    conversationLayout->addWidget(chat, 1);
    auto *composer = new QWidget;
    auto *compose = row(composer);
    conversationMode = new QComboBox;
    conversationMode->setAccessibleName(u"下一条消息的运行模式"_s);
    conversationMode->addItem(u"对话"_s, u"normal"_s);
    conversationMode->addItem(u"观测"_s, u"observe"_s);
    conversationMode->setCurrentIndex(0);
    compose->addWidget(conversationMode);
    connect(conversationMode, &QComboBox::activated, this, [this](int index) {
        setMode(conversationMode->itemData(index).toString());
    });
    prompt = new ChatPrompt;
    prompt->setPlaceholderText(u"输入消息，Enter 发送并观测"_s);
    prompt->setAccessibleName(u"对话消息"_s);
    compose->addWidget(prompt, 1);
    send = button(u"发送"_s, [this] { if (active) cancel(); else runPrompt(prompt->text()); });
    send->setObjectName(u"primary"_s);
    compose->addWidget(send);
    connect(prompt, &ChatPrompt::returnPressed, this, [this] { runPrompt(prompt->text()); });
    conversationLayout->addWidget(composer);
    auto *thinkingOptions = new QWidget;
    auto *thinkingRow = row(thinkingOptions);
    thinkingMode = new QCheckBox(u"深度思考"_s);
    thinkingMode->setAccessibleName(u"启用模型原生深度思考"_s);
    thinkingRow->addWidget(thinkingMode);
    thinkingHint = label(u"加载模型后检查支持情况"_s, u"muted"_s);
    thinkingHint->setWordWrap(true);
    thinkingRow->addWidget(thinkingHint, 1);
    conversationLayout->addWidget(thinkingOptions);
    conversationSplit->addWidget(conversation);
    conversationSplit->setStretchFactor(1, 1);
    conversationSplit->setSizes({210, 870});
    windowLayout->addWidget(conversationSplit);
    chat->setOpenLinks(false);
    connect(chat, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() != u"thought"_s)
            return;
        const int index = url.path().toInt();
        auto &collapsed = viewedSessionId.isEmpty() ? collapsedThoughts : viewedCollapsedThoughts;
        if (collapsed.contains(index))
            collapsed.remove(index);
        else
            collapsed.insert(index);
        renderChat();
    });
    connect(largeHeat, &Heatmap::cellSelected, this, [this](int r, int c, QJsonValue value) {
        status(u"热图 行 "_s + QString::number(r) + u" / 列 "_s + QString::number(c) + u" = "_s +
               valueText(value, 7));
    });
    return panel;
}

QWidget *Workbench::buildTimeline() {
    // 时间轴与分支比较共享底部可调区域。
    auto *panel = new QWidget;
    UiScale::minimumHeight(panel, 145);
    auto *layout = column(panel);
    auto *toolbar = new QWidget;
    auto *controls = row(toolbar, 5, 7);
    controls->addWidget(button(u"执行时间轴"_s, [this] { timelineTabs->setCurrentIndex(0); }));
    auto *branches = button(u"分支对比"_s, [this] { timelineTabs->setCurrentIndex(1); });
    controls->addWidget(branches);
    controls->addStretch();
    timeLabel = label(u"0.00 秒"_s, u"muted"_s);
    controls->addWidget(timeLabel);
    controls->addWidget(button(u"回到起点"_s, [this] {
        follow->setChecked(false);
        forward = u"f0"_s;
        forwardSelect->setCurrentIndex(forwardSelect->findData(forward));
        choose(0, u"embed"_s);
    }));
    controls->addWidget(button(u"导出记录"_s, [this] { exportTrace(); }));
    layout->addWidget(toolbar);
    timelineTabs = new QTabWidget;
    timelineTabs->tabBar()->hide();
    timeline = new Timeline;
    timelineTabs->addTab(timeline, u"执行时间轴"_s);
    timelineTabs->addTab(buildComparison(), u"分支对比"_s);
    layout->addWidget(timelineTabs, 1);
    connect(timeline, &Timeline::selected, this,
            [this](const QString &id, int index, const QString &op, int position) {
                follow->setChecked(false);
                forward = id;
                forwardSelect->setCurrentIndex(forwardSelect->findData(id));
                tokenPosition->setValue(position);
                choose(index, op);
                timelineTabs->setCurrentIndex(0);
            });
    return panel;
}

QWidget *Workbench::buildInspector() {
    // 属性、张量、干预统一放入右侧，避免散落的编辑窗口。
    auto *panel = new QWidget;
    UiScale::minimumWidth(panel, 285);
    auto *layout = column(panel);
    auto *tabs = new QWidget;
    auto *tabRow = row(tabs, 0, 0);
    QList<QPushButton *> tabButtons;
    const QStringList tabTitles{u"属性"_s, u"张量"_s, u"干预"_s, u"详情"_s};
    for (int i = 0; i < tabTitles.size(); ++i) {
        auto *control = button(tabTitles[i], [this, i] { inspector->setCurrentIndex(i); });
        control->setObjectName(u"workspace"_s);
        control->setCheckable(true);
        control->setChecked(i == 0);
        tabRow->addWidget(control);
        tabButtons.append(control);
    }
    tabRow->addStretch();
    layout->addWidget(tabs);
    auto *identityPanel = new QWidget;
    inspectorIdentity = identityPanel;
    auto *identityLayout = column(identityPanel, 13, 6);
    eyebrow = label(u"尚未加载模型"_s, u"eyebrow"_s);
    identity = label(u"选择计算节点"_s, u"heading"_s);
    identityLayout->addWidget(eyebrow);
    identityLayout->addWidget(identity);
    layout->addWidget(identityPanel);
    layout->addWidget(line());
    inspector = new QTabWidget;
    inspector->setDocumentMode(true);
    inspector->tabBar()->hide();
    connect(inspector, &QTabWidget::currentChanged, this, [tabButtons](int index) {
        for (int i = 0; i < tabButtons.size(); ++i)
            tabButtons[i]->setChecked(i == index);
    });
    inspector->addTab(scrollArea(buildProperties()), u"属性"_s);
    inspector->addTab(scrollArea(buildTensor()), u"张量"_s);
    inspector->addTab(scrollArea(buildEditing()), u"干预"_s);
    featureDetails = new QTextBrowser;
    featureDetails->setPlainText(u"从左侧打开新功能，查看详情"_s);
    inspector->addTab(featureDetails, u"详情"_s);
    connect(features, &FeaturePanel::inspected, this, [this](const QString &text) {
        featureDetails->setPlainText(text);
        if (centerViews->currentIndex() == 2) inspector->setCurrentIndex(3);
    });
    layout->addWidget(inspector, 1);
    auto *footer = label(u"查看坐标独立于执行坐标"_s, u"muted"_s);
    footer->setContentsMargins(12, 6, 0, 6);
    layout->addWidget(footer);
    return panel;
}

QWidget *Workbench::buildProperties() {
    // 只显示当前完整选择身份的快照，旧查询响应不覆盖新选择。
    auto *panel = new QWidget;
    auto *layout = column(panel, 13, 10);
    layout->addWidget(label(u"计算表达式"_s));
    formula = label({}, u"formula"_s);
    formula->setWordWrap(true);
    layout->addWidget(formula);
    auto *form = new QFormLayout;
    form->setSpacing(7);
    branchSelect = new QComboBox;
    branchSelect->addItem(u"原始分支"_s, u"base"_s);
    forwardSelect = new QComboBox;
    forwardSelect->addItem(u"输入预填充"_s, u"f0"_s);
    operationSelect = new QComboBox;
    tokenPosition = integer(100000, u"位置 "_s);
    neuron = integer(0, u"通道 "_s);
    headSelect = new QComboBox;
    dimension = integer(0, u"维度 "_s);
    form->addRow(u"分支"_s, branchSelect);
    form->addRow(u"前向"_s, forwardSelect);
    form->addRow(u"运算"_s, operationSelect);
    form->addRow(u"Token位置"_s, tokenPosition);
    form->addRow(u"神经元"_s, neuron);
    form->addRow(u"注意力头"_s, headSelect);
    form->addRow(u"隐藏维度"_s, dimension);
    connect(branchSelect, &QComboBox::currentIndexChanged, this, [this] { switchBranch(); });
    connect(forwardSelect, &QComboBox::currentIndexChanged, this, [this] {
        if (forwardSelect->currentIndex() < 0)
            return;
        follow->setChecked(false);
        forward = forwardSelect->currentData().toString();
        // 切换前向时把绝对位置约束到该次实际输入，避免沿用下一token位置。
        for (auto value : forwards) {
            auto entry = value.toObject();
            if (entry.value(u"forward_id"_s).toString() != forward)
                continue;
            auto positions = entry.value(u"positions"_s).toArray();
            bool contains = false;
            for (auto position : positions)
                if (position.toInt() == tokenPosition->value())
                    contains = true;
            if (!contains && !positions.isEmpty())
                tokenPosition->setValue(positions.last().toInt());
        }
        dirty = true;
        timeline->selectFrame(forward);
        refreshGraph();
    });
    connect(operationSelect, &QComboBox::currentIndexChanged, this, [this] {
        if (operationSelect->currentIndex() >= 0)
            choose(layer, operationSelect->currentData().toString());
    });
    for (auto *control : {tokenPosition, neuron, dimension})
        connect(control, &QSpinBox::valueChanged, this, [this] {
            follow->setChecked(false);
            dirty = true;
        });
    connect(headSelect, &QComboBox::currentIndexChanged, this, [this] { dirty = true; });
    layout->addWidget(line());
    layout->addWidget(label(u"输入 / 输出"_s));
    shapeLabel = label(u"未采集"_s, u"muted"_s);
    shapeLabel->setWordWrap(true);
    layout->addWidget(shapeLabel);
    layout->addWidget(label(u"张量预览"_s));
    propertyHeat = new Heatmap;
    UiScale::fixedHeight(propertyHeat, 178);
    layout->addWidget(propertyHeat);
    layout->addWidget(button(u"展开张量切片"_s, [this] {
        inspector->setCurrentIndex(1);
        requestSlice();
    }));
    cellValue = label(u"点击单元查看数值"_s, u"muted"_s);
    cellValue->setWordWrap(true);
    layout->addWidget(cellValue);
    connect(propertyHeat, &Heatmap::cellSelected, this, [this](int r, int c, QJsonValue value) {
        cellValue->setText(u"行 "_s + QString::number(r) + u" / 列 "_s + QString::number(c) + u" = "_s +
                           valueText(value, 7));
    });
    layout->addWidget(line());
    layout->addWidget(label(u"数值概览"_s));
    stats = label(u"尚未采集"_s, u"muted"_s);
    stats->setWordWrap(true);
    layout->addWidget(stats);
    layout->addWidget(line());
    layout->addWidget(label(u"执行上下文"_s));
    contextLabel = label(u"尚无运行记录"_s, u"muted"_s);
    contextLabel->setWordWrap(true);
    layout->addWidget(contextLabel);
    layout->addWidget(label(u"查看坐标"_s));
    layout->addLayout(form);
    probabilities = new QTableWidget(0, 2);
    probabilities->setHorizontalHeaderLabels({u"候选Token"_s, u"概率"_s});
    probabilities->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    probabilities->verticalHeader()->hide();
    probabilities->setEditTriggers(QAbstractItemView::NoEditTriggers);
    UiScale::minimumHeight(probabilities, 140);
    layout->addWidget(probabilities);
    lens = new QCheckBox(u"查看本层词表投影"_s);
    connect(lens, &QCheckBox::toggled, this, [this] { dirty = true; });
    layout->addWidget(lens);
    layout->addWidget(label(u"词表投影属于额外观测计算。"_s, u"muted"_s));
    layout->addStretch();
    return panel;
}

QWidget *Workbench::buildTensor() {
    // 任意轴的切片范围独立填写，协议层严格限制元素数量。
    auto *panel = new QWidget;
    auto *layout = column(panel, 13, 10);
    layout->addWidget(label(u"张量切片"_s));
    tensorSelect = new QComboBox;
    layout->addWidget(tensorSelect);
    connect(tensorSelect, &QComboBox::currentIndexChanged, this, [this] { setupTensor(); });
    tensorHint = label(u"选择节点后读取可用张量"_s, u"muted"_s);
    tensorHint->setWordWrap(true);
    layout->addWidget(tensorHint);
    ranges = new QTableWidget(0, 3);
    ranges->setHorizontalHeaderLabels({u"起始"_s, u"结束"_s, u"步长"_s});
    ranges->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    UiScale::minimumHeight(ranges, 125);
    layout->addWidget(ranges);
    auto *sliceButton = button(u"应用切片"_s, [this] { requestSlice(); });
    sliceButton->setObjectName(u"secondary"_s);
    layout->addWidget(sliceButton);
    sliceHeat = new Heatmap;
    UiScale::minimumHeight(sliceHeat, 180);
    layout->addWidget(sliceHeat);
    connect(sliceHeat, &Heatmap::cellSelected, this, [this](int r, int c, QJsonValue value) {
        status(u"切片 行 "_s + QString::number(r) + u" / 列 "_s + QString::number(c) + u" = "_s +
               valueText(value, 8));
    });
    layout->addWidget(line());
    layout->addWidget(label(u"投影标量解释"_s));
    layout->addWidget(button(u"解释当前系数的完整内积"_s, [this] { scalarQuery(); }));
    scalarText = new QTextBrowser;
    UiScale::minimumHeight(scalarText, 170);
    layout->addWidget(scalarText);
    layout->addWidget(label(u"内积以单精度重算，与原计算精度的数值分别显示。"_s, u"muted"_s));
    layout->addStretch();
    return panel;
}


void Workbench::configureModel() {
    // 目录可以手工填写或原生浏览，点击加载后才切换真实模型。
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"模型与生成设置"_s);
    dialog->setObjectName(u"modelSettingsDialog"_s);
    UiScale::minimumWidth(dialog, 620);
    auto *layout = column(dialog, 20, 12);
    layout->addWidget(label(u"选择本地模型"_s, u"heading"_s));
    auto *path = new QLineEdit(modelPath);
    auto *recent = new QComboBox;
    auto paths = settings
                     .value(u"recent_models"_s, QStringList{})
                     .toStringList();
    for (const auto &entry : paths)
        recent->addItem(QDir(entry).dirName(), entry);
    layout->addWidget(recent);
    connect(recent, &QComboBox::activated, dialog,
            [recent, path](int index) { path->setText(recent->itemData(index).toString()); });
    auto *directoryRow = new QWidget;
    auto *directory = row(directoryRow);
    directory->addWidget(path, 1);
    directory->addWidget(button(u"浏览目录"_s, [dialog, path] {
        auto selected = QFileDialog::getExistingDirectory(dialog, u"选择模型目录"_s, path->text());
        if (!selected.isEmpty())
            path->setText(selected);
    }));
    layout->addWidget(label(u"模型目录"_s));
    layout->addWidget(directoryRow);
    auto *device = combo({{u"显卡计算"_s, u"cuda:0"_s}, {u"处理器计算"_s, u"cpu"_s}});
    auto *form = new QFormLayout;
    form->addRow(u"计算设备"_s, device);
    auto *maximumControl = integer(maximum->maximum());
    maximumControl->setMinimum(1);
    maximumControl->setValue(maximum->value());
    // 设置字段只在应用后提交，取消不会修改当前输出上限。
    maximumControl->setObjectName(u"settingsMaximum"_s);
    device->setCurrentIndex(std::max(0, device->findData(project.value(u"device"_s).toString())));
    auto *temperatureControl = new QDoubleSpinBox;
    temperatureControl->setRange(0, 3);
    temperatureControl->setDecimals(2);
    temperatureControl->setValue(temperature ? temperature->value() : 0.7);
    form->addRow(u"输出上限"_s, maximumControl);
    auto *thinkingControl = new QDoubleSpinBox;
    thinkingControl->setRange(0, 3);
    thinkingControl->setDecimals(2);
    thinkingControl->setValue(thinkingTemperature->value());
    form->addRow(u"普通对话温度"_s, temperatureControl);
    form->addRow(u"深度思考温度"_s, thinkingControl);
    layout->addLayout(form);
    auto *note = label(u"支持 Qwen2.5-3B 与 Qwen3.5-4B。切换后刷新计算结构、层类型和维度。数据在本机处理。"_s,
                       u"muted"_s);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QWidget;
    auto *actions = row(buttons);
    actions->addWidget(button(u"卸载模型"_s, [this, dialog] {
        if (active || loading || editing) {
            showError(u"请先停止当前任务"_s);
            return;
        }
        actionId = worker.send(u"model.unload"_s);
        dialog->close();
    }));
    actions->addStretch();
    actions->addWidget(button(u"取消"_s, [dialog] { dialog->close(); }));
    auto *load =
        button(u"应用设置"_s, [this, dialog, path, device, maximumControl, temperatureControl, thinkingControl] {
            if (active || loading || editing) {
                showError(u"请先停止当前任务"_s);
                return;
            }
            const bool sameModel = QDir(path->text()).canonicalPath() == QDir(modelPath).canonicalPath();
            const bool sameDevice = device->currentData().toString() == project.value(u"device"_s).toString();
            // 生成参数即时应用；模型或设备变化才重载，避免设置温度导致现场重建。
            if (sameModel && sameDevice && loaded) {
                maximumControl->interpretText();
                maximum->setValue(maximumControl->value());
                temperature->setValue(temperatureControl->value());
                thinkingTemperature->setValue(thinkingControl->value());
                writeProjectState(false);
                dialog->close();
                return;
            }
            if (sameModel) {
                pendingSession = captureSession();
            } else {
                newSession(false);
                pendingSession = {};
            }
            maximumControl->interpretText();
            maximum->setValue(maximumControl->value());
            temperature->setValue(temperatureControl->value());
            thinkingTemperature->setValue(thinkingControl->value());
            loadModel(path->text(), device->currentData().toString());
            dialog->close();
        });
    load->setObjectName(u"primary"_s);
    load->setEnabled(!active && !loading && !editing);
    actions->addWidget(load);
    layout->addWidget(buttons);
    dialog->open();
}

void Workbench::loadModel(const QString &path, const QString &device) {
    ensureProcesses();
    if (!projectDirectory.isEmpty()) {
        project[u"model_path"_s] = path;
        project[u"device"_s] = device;
    }
    // 切换期间禁用冲突操作，目录预检查失败保留当前模型。
    if (active || loading || editing) {
        showError(u"请先停止当前任务后切换模型"_s);
        return;
    }
    modelPath = QDir::cleanPath(path);
    loading = true;
    beginModelLoading();
    setActive(false);
    status(u"正在加载："_s + modelPath);
    runLabel->setText(u"加载中"_s);
    actionId = worker.send(u"model.load"_s,
                           {{u"path"_s, modelPath}, {u"device"_s, device}, {u"dtype"_s, u"auto"_s}});
}

void Workbench::modelReady(const QJsonObject &payload, bool restoreProject) {
    canUndoModel=false;canRedoModel=false;historyQueryId.clear();
    // 以模型实际配置重建全部维度，避免模型切换后残留旧选择。
    finishModelLoading();
    info = payload;
    updateThinkingSupport();
    catalog = payload.value(u"operation_catalog"_s).toArray();
    loading = false;
    loaded = true;
    offline = false;
    editing = false;
    active = false;
    layer = 0;
    executionLayer = -1;
    operation = u"norm"_s;
    forward = u"f0"_s;
    branch = u"base"_s;
    weightsRevision = payload.value(u"weights_revision"_s).toString(u"w0"_s);
    snapshot = {};
    boundary = {};
    execution = {};
    previews.clear();
    previewOrder.clear();
    messages = {};
    tokens = {};
    forwards = {};
    branchTexts.clear();
    assistantText.clear();
    assistantThought.clear();
    queryId.clear();
    sliceId.clear();
    scalarId.clear();
    tracePath.clear();
    lastBackup.clear();
    rules->setRowCount(0);
    modelPath = payload.value(u"path"_s).toString(modelPath);
    if (!verifying)
        settings.setValue(u"model_path"_s, modelPath);
    auto paths = settings.value(u"recent_models"_s).toStringList();
    paths.removeAll(modelPath);
    paths.prepend(modelPath);
    while (paths.size() > 10)
        paths.removeLast();
    if (!verifying)
        settings.setValue(u"recent_models"_s, paths);
    const auto config = info.value(u"config"_s).toObject();
    auto neuronModel=info;neuronModel[u"weights_revision"_s]=weightsRevision;
    neurons->configure(neuronModel,restoreProject);
    QTimer::singleShot(0,this,[this]{syncNeurons();queryNeurons();});
    const int layers = config.value(u"num_hidden_layers"_s).toInt();
    const int channels = config.value(u"intermediate_size"_s).toInt();
    const int hidden = config.value(u"hidden_size"_s).toInt();
    const bool hybrid = info.value(u"model_type"_s).toString() == u"qwen3_5"_s;
    modelLabel->setText(info.value(u"name"_s).toString());
    const auto precision = info.value(u"dtype"_s).toString();
    modelMeta->setText(QString::number(layers) + u" 层 / "_s + (hybrid ? u"混合注意力"_s : u"全注意力"_s) +
                       u" / "_s +
                       (precision == u"bfloat16"_s  ? u"脑浮点十六位"_s
                        : precision == u"float16"_s ? u"半精度"_s
                                                    : u"单精度"_s));
    tree->clear();
    for (int i = 0; i < layers; ++i) {
        const auto types = config.value(u"layer_types"_s).toArray();
        const bool linear = i < types.size() && types[i].toString() == u"linear_attention"_s;
        auto *item =
            new QTreeWidgetItem(tree, {u"第 "_s + QString::number(i).rightJustified(2, QChar(u'0')) + u" 层"_s,
                                       linear ? u"线性"_s : u"全注意力"_s});
        item->setData(0, Qt::UserRole, i);
        item->setData(1, Qt::UserRole, u"norm"_s);
        const QJsonArray operations = i < catalog.size() ? catalog[i].toArray() : QJsonArray{};
        for (auto entry : operations) {
            auto values = entry.toArray();
            if (values.size() < 2)
                continue;
            auto *child = new QTreeWidgetItem(item, {values[1].toString()});
            child->setData(0, Qt::UserRole, i);
            child->setData(1, Qt::UserRole, values[0].toString());
        }
    }
    {
        QSignalBlocker b(branchSelect);
        branchSelect->clear();
        branchSelect->addItem(u"原始分支"_s, u"base"_s);
    }
    {
        QSignalBlocker b(forwardSelect);
        forwardSelect->clear();
        forwardSelect->addItem(u"输入预填充"_s, u"f0"_s);
    }
    history->clear();
    history->addItem(u"原始分支"_s);
    history->item(0)->setData(Qt::UserRole, u"base"_s);
    neuron->setMaximum(std::max(0, channels - 1));
    ruleNeuron->setMaximum(std::max(0, channels - 1));
    dimension->setMaximum(std::max(0, hidden - 1));
    ruleDimension->setMaximum(std::max(0, hidden - 1));
    ruleLayer->setRange(0, std::max(0, layers - 1));
    ruleLayer->setValue(0);
    updateOutputLimit();
    neuron->setValue(0);
    dimension->setValue(0);
    ruleNeuron->setValue(0);
    tokenPosition->setValue(0);
    strip->configure(config);
    timeline->configure(layers);
    propertyHeat->setMatrix({});
    largeHeat->setMatrix({});
    sliceHeat->setMatrix({});
    renderSelection();
    setActive(false);
    status(u"模型已就绪："_s + info.value(u"name"_s).toString());
    runLabel->setText(u"就绪"_s);
    chat->setPlainText(u"模型已加载。输入消息开始真实推理。"_s);
    if (restoreProject && !projectDirectory.isEmpty() && (!verifying || verifyProjectRuntime)) {
        const auto saved = pendingSession;
        pendingSession = {};
        restoreSession(saved);
        writeProjectState(false);
    } else {
        QTimer::singleShot(100, graph, &GraphView::fitGraph);
    }
    // 打开工作区完成模型与会话恢复后，模型结构默认全部收起。
    tree->collapseAll();
}

void Workbench::setMode(const QString &value) {
    // 模式选择只用于下一次发送；保留验证入口以检查旧现场兼容。
    mode = value == u"step"_s ? value : value == u"normal"_s ? value : u"observe"_s;
    QSignalBlocker blocker(conversationMode);
    conversationMode->setCurrentIndex(mode == u"normal"_s ? 0 : 1);
    if (verifying && active)
        worker.send(u"run.change_mode"_s, {{u"mode"_s, mode}});
    setActive(active);
}

void Workbench::runPrompt(const QString &text, int outputOverride) {
    if (!viewedSessionId.isEmpty()) {
        showError(u"其他会话正在生成，请等待结束后发送"_s);
        return;
    }
    if (sessionActivationPending) {
        status(u"会话数据正在准备，请稍后发送"_s);
        return;
    }
    if (projectDirectory.isEmpty() && !verifying) {
        showError(u"请先创建或打开项目"_s);
        return;
    }
    // 对话使用真实模板与历史消息；每次生成独立记录，并清除旧选择缓存。
    if (!loaded || offline) {
        showError(u"请先加载本地模型"_s);
        return;
    }
    if (active || loading || editing) {
        showError(u"当前任务尚未结束"_s);
        return;
    }
    if (sessionId.isEmpty() && !verifying) {
        showError(u"请先点击左侧“新建会话”"_s);
        return;
    }
    if (text.trimmed().isEmpty()) {
        prompt->setFocus();
        status(u"请输入对话消息"_s);
        return;
    }
    checkpointPath.clear();
    checkpointCommitted = false;
    resumeSaved->hide();
    clearCaptureNotices();
    maximum->interpretText();
    runningMode = debugPoints.isEmpty() ? mode : u"observe"_s;
    const int requestedOutput = outputOverride > 0 ? outputOverride : maximum->value();
    pendingPrompt = text.trimmed();
    if (messages.isEmpty()) {
        auto entries = project.value(u"sessions"_s).toArray();
        for (int i = 0; i < entries.size(); ++i) {
            auto entry = entries[i].toObject();
            if (entry.value(u"id"_s).toString() == sessionId) {
                entry[u"title"_s] = pendingPrompt.left(36);
                entries[i] = entry;
            }
        }
        project[u"sessions"_s] = entries;
        refreshSessions();
    }
    messages.append(QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, pendingPrompt}, {u"mode"_s, runningMode}});
    assistantText.clear();
    assistantThought.clear();
    responseTokens = {};
    responseGeneration = {};
    responseReplyId.clear();
    responseThinking = thinkingMode->isEnabled() && thinkingMode->isChecked();
    responseThinkingComplete = !responseThinking;
    renderChat();
    static_cast<ChatTokenView *>(chat)->setReadingPosition(chat->verticalScrollBar()->maximum());
    prompt->clear();
    boundary = {};
    queryId.clear();
    snapshot = {};
    previews.clear();
    previewOrder.clear();
    branchTexts.clear();
    tokens = {};
    forwards = {};
    branch = u"base"_s;
    forward = u"f0"_s;
    executionLayer = -1;
    timeline->clear();
    graph->clearRun();
    propertyHeat->setMatrix({});
    largeHeat->setMatrix({});
    sliceHeat->setMatrix({});
    {
        QSignalBlocker b(branchSelect);
        branchSelect->clear();
        branchSelect->addItem(u"原始分支"_s, u"base"_s);
    }
    history->clear();
    history->addItem(u"原始分支"_s);
    history->item(0)->setData(Qt::UserRole, u"base"_s);
    follow->setChecked(true);
    setActive(true);
    elapsed.restart();
    runLabel->setText(u"运行中"_s);
    status(u"正在执行真实模型计算"_s);
    QJsonObject generation{{u"repetition_penalty"_s, 1.0}, {u"max_new_tokens"_s, requestedOutput},
                           {u"seed"_s, 42},
                           {u"temperature"_s, responseThinking ? thinkingTemperature->value() : temperature->value()},
                           {u"template_options"_s, QJsonObject{{u"enable_thinking"_s, responseThinking}}}};
    // 历史中的 token 证据只用于本地分析，不重复传入推理协议或聊天模板。
    QJsonArray contextMessages;
    for (const auto &value : messages) {
        const auto entry = value.toObject();
        contextMessages.append(QJsonObject{{u"role"_s, entry.value(u"role"_s)}, {u"content"_s, entry.value(u"content"_s)}});
    }
    actionId = worker.send(
        u"run.start"_s,
        {{u"messages"_s, contextMessages},
         {u"mode"_s, runningMode},
         {u"breakpoints"_s, debugPoints},
         {u"generation"_s, generation},
         {u"project_root"_s, projectDirectory},
         {u"trace_root"_s, sessionDirectory() + u"/records"_s},
         {u"capture"_s, QJsonObject{{u"policy"_s, project.value(u"storage"_s).toObject().value(u"policy"_s).toString(u"standard"_s)},
                                    {u"layer"_s, layer},
                                    {u"disk_budget_bytes"_s, 0}}}});
    prompt->setFocus(Qt::OtherFocusReason);
}

void Workbench::setActive(bool value) {
    // 统一控制冲突操作，单步等待时仍可浏览快照与资源指标。
    active = value;
    if(neurons){auto model=loaded||offline?info:QJsonObject{};model[u"weights_revision"_s]=weightsRevision;neurons->configure(model,loaded&&!offline);}
    const bool available = loaded && !offline && !loading && !editing && !sessionActivationPending;
    const bool hasSession = !sessionId.isEmpty();
    if (!active) stopping = false;
    const bool browsing = !viewedSessionId.isEmpty();
    send->setEnabled(!browsing && hasSession && ((active && !stopping) || (available && !active)));
    play->setEnabled(available && hasSession && !active);
    stop->setEnabled(active || loading);
    step->setEnabled(available && active && !boundary.isEmpty());
    maximum->setEnabled(!active && !loading && !editing);
    sessionList->setEnabled(!loading && !editing);
    if(sessionPicker) sessionPicker->setEnabled(!loading && !editing);
    deleteAllSessionsButton->setEnabled(!projectDirectory.isEmpty() && sessionList->count() > 0 && !active && !loading && !editing);
    thinkingMode->setEnabled(!active && !loading && info.value(u"capabilities"_s).toObject().value(u"thinking_mode"_s).toString() == u"supported"_s);
    apply->setEnabled(available && !active);
    clearEdit->setEnabled(available && !active);
    prompt->setEnabled(!browsing && available && hasSession);
    prompt->setPlaceholderText(browsing ? u"其他会话正在生成，当前可查看历史内容与 Token 数据"_s : hasSession ? u"输入消息，Enter 发送，Ctrl+Enter 换行；生成期间可编辑下一条消息"_s : u"请先新建会话"_s);
    send->setText(browsing ? u"其他会话生成中"_s : active ? stopping ? u"正在停止"_s : u"停止"_s : u"发送"_s);
    play->setText(u"开始执行"_s);
    if (features) features->context(projectDirectory, loaded && !offline, !active && !loading && !editing, branch);
    if (!active && browsing && !restoringSession) {
        // 原任务完成并保存后再正式激活正在查看的会话，不中断任务或串写历史。
        const auto id = viewedSessionId;
        QTimer::singleShot(0, this, [this, id] {
            if (!active && viewedSessionId == id && !loading && !editing) activateSession(id);
        });
    }
}

void Workbench::releaseStep() {
    // 只放行当前边界一次，并立即禁用按钮直到收到下一个等待事件。
    if (boundary.isEmpty()) {
        if (active)
            setMode(u"step"_s);
        else
            status(u"请在调试工作区启动一次运行"_s);
        return;
    }
    worker.send(u"step.release"_s, {{u"expected_boundary_id"_s, boundary.value(u"boundary_id"_s)},
                                    {u"expected_state_revision"_s, boundary.value(u"state_revision"_s)}});
    boundary = {};
    step->setEnabled(false);
    runLabel->setText(u"单步执行中"_s);
}
void Workbench::cancel() {
    // 取消命令独立于运算线程，暂停和加载期间均能响应。
    if (stopping) return;
    stopping = true;
    worker.send(u"run.cancel"_s, {{u"run_id"_s, runId}});
    if (active) { send->setText(u"正在停止"_s); send->setEnabled(false); }
    boundary = {};
    step->setEnabled(false);
    status(u"正在停止当前任务"_s);
}

QJsonObject Workbench::selection() const {
    // 所有视图共用完整身份，包含分支、前向、层、token与通道。
    return {{u"branch_id"_s, branch},
            {u"forward_id"_s, forward},
            {u"layer_index"_s, layer},
            {u"token_position"_s, tokenPosition->value()},
            {u"neuron_index"_s, neuron->value()},
            {u"operation"_s, operation},
            {u"head"_s, headSelect->currentData().toInt()},
            {u"dimension"_s, dimension->value()},
            {u"weight_kind"_s, u"down"_s},
            {u"lens"_s, lens->isChecked()},
            {u"overview"_s, centerViews->currentIndex() == 1 && viewButtons[1]->isChecked()}};
}

void Workbench::choose(int index, const QString &op, bool manual) {
    if (manual && active && processesStarted)
        worker.send(u"capture.select"_s, {{u"layer"_s, index}});
    // 用户浏览历史时解除跟随；自动跟随保留执行策略。
    const auto config = info.value(u"config"_s).toObject();
    if (!config.contains(u"num_hidden_layers"_s))
        return;
    if (manual)
        follow->setChecked(false);
    layer = std::clamp(index, 0, config.value(u"num_hidden_layers"_s).toInt() - 1);
    operation = op.isEmpty() ? u"norm"_s : op;
    renderSelection();
    dirty = true;
}

void Workbench::renderSelection() {
    // 当前节点的中文身份、公式与可用子运算同步更新。
    const auto config = info.value(u"config"_s).toObject();
    const auto types = config.value(u"layer_types"_s).toArray();
    const bool linear = layer < types.size() && types[layer].toString() == u"linear_attention"_s;
    const QString kind = linear ? u"线性注意力"_s : u"全注意力"_s;
    eyebrow->setText(u"第 "_s + QString::number(layer) + u" 层 / "_s + kind);
    identity->setText(operationName(operation));
    formula->setText(operationFormula(operation));
    positionLabel->setText(u"模型 › 文本解码器 › 第 "_s + QString::number(layer) + u" 层 / "_s + kind);
    selectionLabel->setText(u"第 "_s + QString::number(layer) + u" 层 / "_s + operationName(operation));
    {
        QSignalBlocker blocker(operationSelect);
        operationSelect->clear();
        operationSelect->addItem(u"Token嵌入"_s, u"embed"_s);
        if (layer < catalog.size())
            for (auto value : catalog[layer].toArray()) {
                auto entry = value.toArray();
                if (entry.size() > 1)
                    operationSelect->addItem(entry[1].toString(), entry[0].toString());
            }
        if (operationSelect->count() == 1) {
            const QStringList fallback{u"norm"_s,  u"q"_s,         u"k"_s,       u"v"_s,
                                       u"rope"_s,  u"attention"_s, u"o"_s,       u"attention_residual"_s,
                                       u"norm2"_s, u"gate"_s,      u"up"_s,      u"silu"_s,
                                       u"mlp"_s,   u"down"_s,      u"residual"_s};
            for (const auto &key : fallback)
                operationSelect->addItem(operationName(key), key);
        }
        operationSelect->addItem(u"最终归一化"_s, u"final_norm"_s);
        operationSelect->addItem(u"词表输出"_s, u"logits"_s);
        operationSelect->setCurrentIndex(operationSelect->findData(operation));
    }
    const int heads = config.value(linear ? u"linear_num_value_heads"_s : u"num_attention_heads"_s).toInt(1);
    if (headSelect->count() != heads) {
        QSignalBlocker blocker(headSelect);
        headSelect->clear();
        for (int i = 0; i < heads; ++i)
            headSelect->addItem(u"头 "_s + QString::number(i), i);
    }
    int progress = -1;
    for (const auto &frame : timeline->frames)
        if (frame.branch == branch && frame.id == forward) {
            if (frame.done.contains(u"-1:embed"_s))
                progress = 0;
            for (int i = 0; i < config.value(u"num_hidden_layers"_s).toInt(); ++i)
                if (frame.done.contains(QString::number(i) + u":residual"_s))
                    progress = std::max(progress, i + 1);
        }
    if (active && execution.value(u"forward_id"_s).toString() == forward &&
        execution.value(u"branch_id"_s).toString() == branch)
        progress = std::max(progress, executionLayer);
    strip->setPosition(layer, progress);
    {
        QSignalBlocker blocker(branchSelect);
        const int index = branchSelect->findData(branch);
        if (index >= 0)
            branchSelect->setCurrentIndex(index);
    }
    {
        QSignalBlocker blocker(forwardSelect);
        const int index = forwardSelect->findData(forward);
        if (index >= 0)
            forwardSelect->setCurrentIndex(index);
    }
    ruleLayer->setValue(layer);
    graph->setLayer(linear, info.value(u"model_type"_s).toString() == u"qwen3_5"_s, layer);
    refreshGraph();
    graph->selectOperation(operation, true);
    if (layer < tree->topLevelItemCount()) {
        QSignalBlocker blocker(tree);
        auto *item = tree->topLevelItem(layer);
        if (tree->currentItem() != item) {
            tree->setCurrentItem(item);
            tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            tree->topLevelItem(i)->setExpanded(i == layer);
    }
}

void Workbench::refreshGraph() {
    // 切换层或前向后只恢复同一身份的已采集预览。
    graph->clearRun();
    const auto prefix = branch + u"/"_s + forward + u"/"_s + QString::number(layer) + u"/"_s;
    for (auto it = previews.cbegin(); it != previews.cend(); ++it)
        if (it.key().startsWith(prefix)) {
            const auto op = it.key().mid(prefix.size());
            graph->updateOperation(op, it.value(),
                                   executionLayer == layer &&
                                       execution.value(u"operation_id"_s).toString() == op &&
                                       execution.value(u"forward_id"_s).toString() == forward &&
                                       execution.value(u"branch_id"_s).toString() == branch);
        }
}

void Workbench::query() {
    // 一个查询进行时合并后续选择变化，避免高频事件堆积后台线程。
    if (tracePath.isEmpty())
        return;
    if (!queryId.isEmpty()) {
        dirty = true;
        return;
    }
    queryId = worker.send(u"view.query"_s, selection());
}

void Workbench::receive(const QJsonObject &message) {
    if (anomalies) anomalies->runEvent(message, projectDirectory);
    // 事件驱动界面，不用计时器伪造模型进度。
    const auto name = message.value(u"name"_s).toString();
    const auto payload = message.value(u"payload"_s).toObject();
    const auto context = message.value(u"context"_s).toObject();
    const auto request = message.value(u"request_id"_s).toString();
    if (payload.contains(u"can_undo"_s)) { canUndoModel=payload.value(u"can_undo"_s).toBool(); canRedoModel=payload.value(u"can_redo"_s).toBool(); updateHistoryActions(); }
    if (!historyQueryId.isEmpty() && request==historyQueryId) {
        if(name==u"feature.result"_s||message.value(u"kind"_s).toString()==u"error"_s)historyQueryId.clear();
        return;
    }

    if (closing)
        return;
    if (sessionActivationRequests.contains(request) &&
        (name == u"session.activated"_s || message.value(u"kind"_s).toString() == u"error"_s)) {
        sessionActivationRequests.remove(request);
        // 快速连选后，旧激活请求的成功或忙碌响应均不能影响最新选择。
        if (request != sessionActivationId) return;
    }
    if (!sessionActivationId.isEmpty() && request == sessionActivationId && message.value(u"kind"_s).toString() == u"error"_s) {
        if (payload.value(u"code"_s).toString() == u"BUSY"_s && sessionActivationRetries++ < 20 && !active) {
            // 完成事件早于运行器清理缓存结束，短暂等待后重试最新会话，避免将正常收尾显示为错误。
            QTimer::singleShot(100, this, [this, request] {
                if (sessionActivationId != request || active || closing) return;
                sessionActivationId = worker.send(u"session.activate"_s, {{u"temporary_rules"_s, temporaryRules},
                    {u"weights_revision"_s, weightsRevision}, {u"model_revision"_s, info.value(u"model_revision"_s)}, {u"trace_path"_s, tracePath}});
                sessionActivationRequests.insert(sessionActivationId);
            });
            return;
        }
        sessionActivationPending = false; setActive(active);
    }
    if (!deleteSessionsRequestId.isEmpty() && request == deleteSessionsRequestId && message.value(u"kind"_s).toString() == u"error"_s) {
        if (payload.value(u"code"_s).toString() == u"BUSY"_s && deleteSessionsRetries++ < 20) {
            QTimer::singleShot(100, this, [this, request] {
                if (deleteSessionsRequestId == request && !closing) deleteSessionsRequestId = worker.send(u"trace.close"_s);
            });
            return;
        }
        // 关闭记录失败时取消删除并解锁界面，保留所有会话。
        deleteSessionsRequestId.clear(); deletingSessionIds.clear(); editing = false;
        setActive(false); showError(payload.value(u"message"_s).toString());
        emit received(message); return;
    }
    // 神经元读取独立于生成控制；查询错误不能终止生成或写入聊天正文。
    if(name==u"neurons.frame"_s){
        if(message.value(u"run_id"_s).toString()==runId&&message.value(u"branch_id"_s).toString()==branch)neurons->frame(payload);
        emit received(message);return;
    }
    if(neuronRequests.remove(request)){
        if(request==neuronQueryId){neuronQueryId.clear();if(name==u"neurons.snapshot"_s&&payload.value(u"run_id"_s).toString()==runId&&payload.value(u"branch_id"_s).toString()==branch)neurons->snapshot(payload);else if(message.value(u"kind"_s).toString()==u"error"_s)neurons->failure(payload.value(u"message"_s).toString());}
        emit received(message);return;
    }
    // 分析请求属于独立面板，错误不得改变当前生成状态或追加到聊天正文。
    if (tokenAnalysisRequests.remove(request)) {
        emit received(message);
        return;
    }
    if(name==u"debug.configured"_s){debugPoints=payload.value(u"points"_s).toArray();status(u"断点与监视已更新"_s);emit received(message);return;}
    if(!previewSliceId.isEmpty()&&request==previewSliceId&&(name==u"feature.result"_s||message.value(u"kind"_s).toString()==u"error"_s)){
        previewSliceId.clear();editing=false;setActive(false);emit received(message);return;
    }
    if(!comparisonRequestId.isEmpty()&&request==comparisonRequestId){
        if(name!=u"feature.result"_s&&message.value(u"kind"_s).toString()!=u"error"_s)return;
        comparisonRequestId.clear();editing=false;setActive(false);
        if(message.value(u"kind"_s).toString()==u"error"_s)showError(payload.value(u"message"_s).toString());
        else renderComparison(payload);
        emit received(message);return;
    }
    if (!ruleRequestId.isEmpty() && request == ruleRequestId) {
        // accepted 仅表示排队，最终结果到达前保持互斥。
        if(name!=u"feature.result"_s && message.value(u"kind"_s).toString()!=u"error"_s)return;
        ruleRequestId.clear();editing=false;setActive(false);
        if(message.value(u"kind"_s).toString()==u"error"_s){
            refreshRuleStates(payload.value(u"message"_s).toString());
        } else if(payload.value(u"action"_s).toString()==u"rules.preview"_s){
            pendingRulePreview[u"action"_s]=u"rules.apply"_s;
            for(const auto &key:{u"expected_model_revision"_s,u"expected_vocab_revision"_s})pendingRulePreview[key]=payload.value(key);
            showRulePreview(payload);
        } else {
            weightsRevision=payload.value(u"weights_revision"_s).toString(weightsRevision);
            info[u"model_revision"_s]=payload.value(u"model_revision"_s);
            temporaryRules=payload.value(u"temporary_rules"_s).toArray();
            appliedRuleDraft=payload.value(u"rules"_s).toArray();
            refreshRuleStates();rememberProjectRules();writeProjectState(false);
            status(u"规则已应用"_s);
        }
        emit received(message);return;
    }
    if (name == u"feature.progress"_s) {
        // 新进度带请求身份，过期任务和其他入口的进度不覆盖当前分析。
        if (!request.isEmpty()) {
            if (featureRequestId.isEmpty() || request != featureRequestId) return;
            features->progress(payload);
        }
        status(payload.value(u"message"_s).toString());
        return;
    }
    if (!featureRequestId.isEmpty() && request == featureRequestId) {
        // 只有最终结果或错误会结束任务，避免 accepted 等中间消息提前解锁。
        if (name != u"feature.result"_s && message.value(u"kind"_s).toString() != u"error"_s) return;
        featureRequestId.clear();
        editing = false;
        weightsRevision = payload.value(u"weights_revision"_s).toString(weightsRevision);
        if (payload.contains(u"model_revision"_s)) info[u"model_revision"_s] = payload.value(u"model_revision"_s);
        setActive(false);
        if (message.value(u"kind"_s).toString() == u"error"_s) features->failure(payload.value(u"message"_s).toString());
        else {
            const auto action = payload.value(u"action"_s).toString();
            if(action==u"samples.save"_s||action==u"drafts.save"_s)features->reloadProjectData();
            if(action==u"comparison"_s)renderComparison(payload);
            else if(action==u"rules.draft"_s){loadRuleDraft(payload.value(u"rules"_s).toArray());writeProjectState(false);}
            else if(action==u"rules.apply"_s){
                temporaryRules=payload.value(u"temporary_rules"_s).toArray();appliedRuleDraft=payload.value(u"rules"_s).toArray();
                loadRuleDraft(appliedRuleDraft);rememberProjectRules();
            }else if(action==u"versions.action"_s&&payload.contains(u"rules"_s)){
                appliedRuleDraft=payload.value(u"rules"_s).toArray();temporaryRules=payload.value(u"temporary_rules"_s).toArray();
                refreshRuleStates();rememberProjectRules();features->result(payload);
            }else if (action == u"report.read"_s && payload.contains(u"base_tokens"_s)) {
                changeView(0);
                renderComparison(payload);
            }else features->result(payload);
            // 模型版本提交后同步项目现场，使共享临时规则继续归属于新版本。
            if (action.endsWith(u".apply"_s) || action == u"versions.action"_s || action == u"versions.import"_s)
                writeProjectState(false);
        }
        features->pending(false);
        emit received(message);
        return;
    }
    if (message.value(u"kind"_s).toString() == u"error"_s) {
        closeAfterSave = false;
        resumeRebuilding = false;
        if (request == queryId) {
            queryId.clear();
        } else if (request == sliceId) {
            sliceId.clear();
        } else if (request == scalarId) {
            scalarId.clear();
        } else if (request == actionId) {
            externalEditPending = false;
            const bool wasLoading = loading;
            loading = false;
            finishModelLoading();
            editing = false;
            setActive(false);
            if (wasLoading && !pendingSession.isEmpty()) {
                restoreSession(pendingSession);
                pendingSession = {};
            }
        }
        showError(payload.value(u"message"_s).toString() +
                  u"\n错误代码："_s + payload.value(u"code"_s).toString());
        emit received(message);
        return;
    }
    if (name == u"checkpoint.saved"_s) {
        checkpointPath = payload.value(u"path"_s).toString();
        if (verifying)
            mode = u"step"_s;
        resumeSaved->hide();
        if (writeProjectState(true)) {
            status(u"完整现场已保存，可关闭项目后从同一运算边界恢复"_s);
            if (closeAfterSave) {
                checkpointCommitted = true;
                close();
            } else if (!verifying) {
                // 保存仍在安全边界取快照，随后自动继续本次模式，不暴露调试入口。
                worker.send(u"run.change_mode"_s, {{u"mode"_s, runningMode}});
            }
        } else {
            closeAfterSave = false;
        }
    } else if (name == u"session.activated"_s) {
        if (!request.isEmpty() && !sessionActivationId.isEmpty() && request != sessionActivationId) return;
        sessionActivationPending = false; sessionActivationRetries = 0; setActive(active);
        runId=message.value(u"run_id"_s).toString();
        neuronQueryId.clear();neurons->clearActivity();
        QTimer::singleShot(0,this,[this]{syncNeurons();queryNeurons();});
        temporaryRules = payload.value(u"temporary_rules"_s).toArray();
        weightsRevision = payload.value(u"weights_revision"_s).toString(weightsRevision);
        if (!tracePath.isEmpty() && payload.value(u"trace_available"_s).toBool())
            dirty = true;
    } else if (name == u"trace.closed"_s && !deletingSessionIds.isEmpty()) {
        finishDeleteAllSessions();
    } else if (name == u"trace.closed"_s && pendingSession.contains(u"cleanup_path"_s)) {
        const auto path = pendingSession.value(u"cleanup_path"_s).toString();
        const bool remove = pendingSession.value(u"remove_session"_s).toBool();
        const bool records = pendingSession.value(u"delete_records"_s).toBool();
        pendingSession = {};
        sessionStates.remove(sessionId);
        if (records && !QDir(path + u"/records"_s).removeRecursively())
            showError(u"部分记录删除失败，请检查磁盘权限或文件占用"_s);
        QFile::remove(path + u"/checkpoint.json"_s);
        if (remove) {
            QJsonArray entries;
            for (auto value : project.value(u"sessions"_s).toArray())
                if (value.toObject().value(u"id"_s).toString() != sessionId)
                    entries.append(value);
            project[u"sessions"_s] = entries;
            QFile::remove(path + u"/state.json"_s);
            sessionId.clear();
        }
        restoreSession({});
        // 删除当前会话后仅选择已经存在的会话，最后一条删除后保持空状态。
        if (remove && !project.value(u"sessions"_s).toArray().isEmpty()) {
            const auto next = project.value(u"sessions"_s).toArray().first().toObject().value(u"id"_s).toString();
            activateSession(next);
        }
        refreshSessions();
        writeProjectState(false);
        status(records ? u"会话已清理，关联观测记录已删除"_s : u"会话已清理，观测记录保留在存储管理中"_s);
    } else if (name == u"checkpoint.rebuilding"_s) {
        status(u"正在重建保存现场的计算缓存"_s);
    } else if (name == u"model.ready"_s)
        modelReady(payload);
    else if (name == u"model.progress"_s) {
        status(payload.value(u"message"_s).toString());
        updateModelLoading(payload);
    }
    else if (name == u"model.cancelled"_s) {
        loading = false;
        finishModelLoading();
        if (!pendingSession.isEmpty()) {
            restoreSession(pendingSession);
            pendingSession = {};
        }
        setActive(false);
        runLabel->setText(u"加载已取消"_s);
        status(u"模型加载已取消"_s);
    } else if (name == u"model.unloaded"_s) {
        neuronQueryId.clear();neurons->configure({},false);
        loaded = false;
        snapshot = {};
        previews.clear();
        previewOrder.clear();
        boundary = {};
        queryId.clear();
        tracePath.clear();
        dirty = false;
        graph->clearRun();
        propertyHeat->setMatrix({});
        largeHeat->setMatrix({});
        sliceHeat->setMatrix({});
        setActive(false);
        modelLabel->setText(u"未加载模型"_s);
    } else if (name == u"run.started"_s) {
        clearCaptureNotices();
        if (!resumeRebuilding) {
            responseReplyId = QUuid::createUuid().toString(QUuid::Id128);
            responseTokens = {};
        }
        responseGeneration = payload.value(u"generation"_s).toObject();
        runId = message.value(u"run_id"_s).toString();
        neurons->beginRun(runId,message.value(u"branch_id"_s).toString(u"base"_s));
        tracePath = payload.value(u"path"_s).toString();
        tokenInfo->setText(u"本次输出额度 "_s + QString::number(payload.value(u"max_new_tokens"_s).toInt()) + u" Token"_s);
        maximum->setToolTip(u"允许提前正常结束。本次实际额度："_s + QString::number(payload.value(u"max_new_tokens"_s).toInt()) +
                            u"；输入与历史占用："_s + QString::number(payload.value(u"input_tokens"_s).toInt()));
        branch = message.value(u"branch_id"_s).toString(u"base"_s);
        // 重算替换当前助手回复，基线文本独立保留在分支对比中。
        if (branch != u"base"_s && !messages.isEmpty() &&
            messages.last().toObject().value(u"role"_s).toString() == u"assistant"_s)
            messages.removeLast();
        weightsRevision = payload.value(u"weights_revision"_s).toString(weightsRevision);
        tokens = payload.value(u"tokens"_s).toArray();
        if (!resumeRebuilding)
            assistantText.clear();
        {
            QSignalBlocker blocker(tokenPosition);
            tokenPosition->setValue(std::max(0, int(tokens.size()) - 1));
        }
        forwards = {};
        {
            QSignalBlocker blocker(forwardSelect);
            forwardSelect->clear();
        }
        boundary = {};
        elapsed.restart();
        setActive(true);
        runLabel->setText(u"运行中"_s);
    } else if (name == u"forward.started"_s) {
        const auto id = payload.value(u"forward_id"_s).toString();
        const auto b = message.value(u"branch_id"_s).toString();
        auto positions = payload.value(u"positions"_s).toArray();
        forwards.append(payload);
        while (forwards.size() > 128)
            forwards.removeFirst();
        if (b == branch) {
            QSignalBlocker blocker(forwardSelect);
            if (forwardSelect->findData(id) < 0)
                forwardSelect->addItem(forwardName(id), id);
            if (follow->isChecked()) {
                forward = id;
                forwardSelect->setCurrentIndex(forwardSelect->findData(id));
                QSignalBlocker tokenBlock(tokenPosition);
                if (!positions.isEmpty())
                    tokenPosition->setValue(positions.last().toInt());
            }
        }
        timeline->startForward(id, b, positions.isEmpty() ? 0 : positions.last().toInt());
        if (follow->isChecked())
            graph->clearRun();
    } else if (name == u"operation.completed"_s) {
        const auto id = context.value(u"forward_id"_s).toString();
        const auto b = message.value(u"branch_id"_s).toString();
        const auto op = payload.value(u"operation"_s).toString();
        const int index =
            context.value(u"layer_index"_s).isDouble() ? context.value(u"layer_index"_s).toInt() : -1;
        execution = context;
        execution[u"branch_id"_s] = b;
        executionLayer = index;
        const auto key = b + u"/"_s + id + u"/"_s + QString::number(index) + u"/"_s + op;
        rememberPreview(key, payload);
        timeline->operation(id, b, index, op, payload.value(u"compute_and_capture_ms"_s).toDouble());
        if (index == layer && id == forward && b == branch)
            graph->updateOperation(op, payload, true);
        if (follow->isChecked())
            dirty = true;
    } else if (name == u"step.waiting"_s) {
        resumeRebuilding = false;
        boundary = payload;
        execution = payload;
        execution[u"branch_id"_s] = message.value(u"branch_id"_s);
        executionLayer = payload.value(u"layer_index"_s).toInt(-1);
        if (follow->isChecked()) {
            forward = payload.value(u"forward_id"_s).toString(forward);
            choose(std::max(0, executionLayer), payload.value(u"operation_id"_s).toString(), false);
        }
        setActive(true);
        runLabel->setText(u"已暂停"_s);
        status(u"等待执行："_s + payload.value(u"operation_name"_s).toString());
    } else if (name == u"token.sampled"_s) {
        // 每个片段都绑定原运行和分支；右键不依赖时间轴的当前选择。
        auto sampled = payload;
        sampled[u"run_id"_s] = message.value(u"run_id"_s);
        sampled[u"branch_id"_s] = message.value(u"branch_id"_s);
        sampled[u"trace"_s] = QDir(projectDirectory).relativeFilePath(tracePath);
        sampled.remove(u"display_updates"_s);
        responseTokens.append(sampled);
        for (const auto &value : payload.value(u"display_updates"_s).toArray()) {
            const auto update = value.toObject();
            // 恢复重放可跳过早期事件，使用生成步定位，不把数组下标误当生成位置。
            for (int i = responseTokens.size() - 1; i >= 0; --i) {
                auto token = responseTokens[i].toObject();
                if (token.value(u"generation_step"_s) == update.value(u"index"_s)) {
                    for (auto it = update.begin(); it != update.end(); ++it)
                        if (it.key() != u"index"_s) token[it.key()] = it.value();
                    responseTokens[i] = token;
                    break;
                }
            }
        }
        QString displayedToken =
            payload.value(u"display_text"_s).toString(payload.value(u"text"_s).toString());
        if (payload.value(u"special"_s).toBool())
            displayedToken = u"特殊Token "_s + QString::number(payload.value(u"id"_s).toInt());
        else if (displayedToken.contains(QChar(0xfffd)))
            displayedToken = u"字节片段"_s;
        timeline->token(payload.value(u"forward_id"_s).toString(), message.value(u"branch_id"_s).toString(),
                        displayedToken, payload.value(u"position"_s).toInt());
        tokenInfo->setText(u"已生成Token "_s +
                           QString::number(payload.value(u"position"_s).toInt() - tokens.size() + 1));
        dirty = true;
    } else if (name == u"text.updated"_s) {
        assistantText = payload.value(u"text"_s).toString();
        assistantThought = payload.value(u"reasoning_content"_s).toString();
        responseThinking = payload.value(u"thinking_enabled"_s).toBool();
        responseThinkingComplete = payload.value(u"thinking_complete"_s).toBool(true);
        if (!chatRefresh.isActive())
            chatRefresh.start();
    } else if (name == u"run.finished"_s) {
        assistantText = payload.value(u"text"_s).toString();
        branchTexts[message.value(u"branch_id"_s).toString()] = assistantText;
        assistantThought = payload.value(u"reasoning_content"_s).toString();
        responseThinkingComplete = payload.value(u"thinking_complete"_s).toBool(true);
        if (!assistantText.isEmpty() || !assistantThought.isEmpty())
            messages.append(QJsonObject{{u"role"_s, u"assistant"_s}, {u"content"_s, assistantText},
                {u"reasoning_content"_s, assistantThought}, {u"thinking_complete"_s, responseThinkingComplete}, {u"mode"_s, runningMode},
                {u"reply_id"_s, responseReplyId}, {u"output_tokens"_s, responseTokens}, {u"generation"_s, responseGeneration},
                {u"trace"_s, QDir(projectDirectory).relativeFilePath(tracePath)}, {u"prompt_text"_s, pendingPrompt}});
        renderChat();
        boundary = {};
        setActive(false);
        renderSelection();
        const auto outcome = payload.value(u"outcome"_s).toString();
        const auto reason = payload.value(u"reason"_s).toString();
        const auto stopped = reason == u"length"_s ? u"已达生成上限"_s
                           : reason == u"eos"_s ? u"模型已结束回答"_s
                           : reason == u"stop"_s ? u"已匹配停止条件"_s
                           : outcome == u"cancelled"_s ? u"用户已停止"_s
                           : outcome == u"failed"_s ? u"执行失败"_s : u"运行结束"_s;
        runLabel->setText(stopped);
        status(stopped + u" / "_s + QString::number(payload.value(u"forward_count"_s).toInt()) + u" 次真实前向"_s);
        if (reason == u"length"_s) {
            showCaptureNotice(u"completion"_s, u"已达到本次可用输出上限 "_s + QString::number(payload.value(u"max_new_tokens"_s).toInt()) + u" Token，回答可能尚未结束。"_s);
        } else if (reason == u"eos"_s) {
            showCaptureNotice(u"completion"_s, u"模型已结束回答"_s);
        }
        checkpointPath.clear();
        resumeSaved->hide();
        resumeRebuilding = false;
        if (!verifying)
            writeProjectState(false);
        if(features->currentPage()==0&&centerViews->currentWidget()==features)refreshActivationSources();
        dirty = true;
        QString html = u"<h3>分支输出对比</h3>"_s;
        for (auto it = branchTexts.cbegin(); it != branchTexts.cend(); ++it)
            html += u"<p><b>"_s + branchName(it.key()) + u"</b>　"_s + it.value().toHtmlEscaped() + u"</p>"_s;
        comparison->setHtml(html);
    } else if (name == u"view.snapshot"_s && request == queryId) {
        queryId.clear();
        if (payload.value(u"selection"_s).toObject() == selection())
            renderSnapshot(payload);
        else
            dirty = true;
    } else if (name == u"tensor.slice.result"_s && request == sliceId) {
        sliceId.clear();
        auto values = payload.value(u"values"_s).toArray();
        auto shape = payload.value(u"shape"_s).toArray();
        int cols = shape.isEmpty() ? 1 : std::max(1, shape.last().toInt());
        QJsonArray matrix;
        for (int i = 0; i < values.size(); i += cols) {
            QJsonArray r;
            for (int c = 0; c < cols && i + c < values.size(); ++c)
                r.append(values[i + c]);
            matrix.append(r);
        }
        sliceHeat->setMatrix(matrix, u"真实切片 / "_s + shapeText(shape));
    } else if (name == u"scalar.result"_s && request == scalarId) {
        scalarId.clear();
        QString text = u"原计算值："_s + valueText(payload.value(u"captured"_s), 8) + u"\n单精度重算："_s +
                       valueText(payload.value(u"recomputed_fp32"_s), 8) + u"\n绝对误差："_s +
                       valueText(payload.value(u"absolute_error"_s), 8) + u"\n内积项数："_s +
                       QString::number(payload.value(u"terms"_s).toInt()) + u"\n偏置："_s +
                       valueText(payload.value(u"bias"_s)) + u"\n\n前八项（输入 × 权重 = 贡献）\n"_s;
        for (auto value : payload.value(u"first_terms"_s).toArray()) {
            auto term = value.toObject();
            text += valueText(term.value(u"input"_s)) + u" × "_s + valueText(term.value(u"weight"_s)) +
                    u" = "_s + valueText(term.value(u"product"_s)) + u"\n"_s;
        }
        scalarText->setPlainText(text);
    } else if (name == u"edit.applied"_s || name == u"patch.applied"_s) {
        editing = false;
        if (payload.contains(u"temporary_rules"_s))
            temporaryRules = payload.value(u"temporary_rules"_s).toArray();
        if (payload.contains(u"model_revision"_s))
            info[u"model_revision"_s] = payload.value(u"model_revision"_s);
        weightsRevision = payload.value(u"weights_revision"_s).toString(weightsRevision);
        if (payload.contains(u"backup_path"_s))
            lastBackup = payload.value(u"backup_path"_s).toString();
        if (externalEditPending) {
            externalEditPending = false;
            syncExternalEdit(payload);
        }
        setActive(false);
        status(payload.contains(u"backup_path"_s) ? u"已保存到当前模型。备份："_s + lastBackup
                                                  : u"临时编辑已应用"_s);
        rememberProjectRules();
        const bool stored = writeProjectState(false);
        if (name == u"edit.applied"_s && stored) {
            auto *notice = new QMessageBox(QMessageBox::Information, u"应用成功"_s,
                payload.contains(u"backup_path"_s) ? u"修改已保存到当前模型"_s :
                temporaryRules.isEmpty() ? u"临时编辑已清除"_s : u"临时编辑已应用，将用于后续输入"_s,
                QMessageBox::NoButton, this);
            notice->setAttribute(Qt::WA_DeleteOnClose);
            notice->addButton(u"确定"_s, QMessageBox::AcceptRole);
            notice->open();
        }
        dirty = true;
    } else if (name == u"branch.created"_s) {
        branch = payload.value(u"branch_id"_s).toString();
        neurons->beginRun(runId,branch);
        if (branchSelect->findData(branch) < 0) {
            QSignalBlocker blocker(branchSelect);
            branchSelect->addItem(branchName(branch), branch);
            branchSelect->setCurrentIndex(branchSelect->findData(branch));
            history->addItem(branchName(branch));
            history->item(history->count() - 1)->setData(Qt::UserRole, branch);
        }
        execution = {};
        follow->setChecked(true);
        timeline->setBranch(branch);
        snapshot = {};
        queryId.clear();
    } else if (name == u"trace.opened"_s) {
        auto manifest = payload.value(u"manifest"_s).toObject();
        runId=manifest.value(u"run_id"_s).toString();
        neuronQueryId.clear();neurons->clearActivity();
        auto historicalModel = manifest.value(u"model"_s).toObject();
        historicalModel[u"operation_catalog"_s] = payload.value(u"operation_catalog"_s);
        // 离线记录只更新观测界面，不能激活会话并关闭刚打开的记录。
        modelReady(historicalModel, false);
        weightsRevision=manifest.value(u"weights_revision"_s).toString(u"w0"_s);
        loaded = false;
        offline = true;
        auto neuronModel=info;neuronModel[u"weights_revision"_s]=manifest.value(u"weights_revision"_s);neurons->configure(neuronModel,false);
        tracePath = payload.value(u"path"_s).toString();
        const auto entries = payload.value(u"catalog"_s).toArray();
        forwards = entries;
        {
            QSignalBlocker b(branchSelect);
            QSignalBlocker f(forwardSelect);
            branchSelect->clear();
            forwardSelect->clear();
            for (auto value : entries) {
                const auto entry = value.toObject();
                const auto bId = entry.value(u"branch_id"_s).toString();
                if (branchSelect->findData(bId) < 0)
                    branchSelect->addItem(branchName(bId), bId);
                if (branch.isEmpty() || branchSelect->count() == 1)
                    branch = bId;
                if (bId == branch) {
                    const auto id = entry.value(u"forward_id"_s).toString();
                    forwardSelect->addItem(forwardName(id), id);
                    auto positions = entry.value(u"positions"_s).toArray();
                    timeline->startForward(id, bId, positions.isEmpty() ? 0 : positions.last().toInt());
                }
            }
            forward = forwardSelect->currentData().toString();
        }
        follow->setChecked(false);
        setActive(false);
        dirty = true;
        status(u"已打开离线记录："_s + tracePath);
    } else if (name == u"trace.exported"_s || name == u"model.exported"_s) {
        editing = false;
        setActive(active);
        status(u"导出完成："_s + payload.value(u"path"_s).toString());
    } else if (name == u"capture.degraded"_s) {
        showCaptureNotice(u"capture"_s, payload.value(u"message"_s).toString());
        status(payload.value(u"message"_s).toString());
    }
    emit received(message);
}

void Workbench::rememberPreview(const QString &key, const QJsonObject &value) {
    // 前端只缓存近期缩略图；完整张量单独保存至项目记录，避免现场 JSON 随生成无限增大。
    if (!previews.contains(key))
        previewOrder.append(key);
    previews[key] = value;
    while (previewOrder.size() > 2048)
        previews.remove(previewOrder.takeFirst());
}

void Workbench::renderSnapshot(const QJsonObject &value) {
    // 热图、通道统计和token候选来自同一快照，不补造未采集数据。
    snapshot = value;
    if (value.value(u"status"_s).toString() == u"available"_s) {
        QJsonObject preview{{u"preview"_s, value.value(u"matrix"_s)}, {u"shape"_s, value.value(u"shape"_s)}};
        const auto key = branch + u"/"_s + forward + u"/"_s + QString::number(layer) + u"/"_s + operation;
        if (!previews.contains(key)) {
            rememberPreview(key, preview);
            graph->updateOperation(operation, preview, false);
        }
    }
    const auto attention = value.value(u"attention"_s).toObject();
    const bool attentionView = operation == u"attention"_s || operation == u"linear_delta"_s;
    QJsonArray matrix =
        attentionView ? attention.value(u"matrix"_s).toArray() : value.value(u"matrix"_s).toArray();
    propertyHeat->positiveOnly = operation == u"attention"_s;
    const QString heatCaption = attentionView ? (attention.value(u"type"_s).toString() == u"linear"_s
                                                     ? u"递归状态 / 键通道 × 值通道"_s
                                                     : u"注意力权重 / 查询位置 × 键位置"_s)
                                              : u"真实输出 / 局部切片"_s;
    propertyHeat->setMatrix(matrix, heatCaption);
    if (viewButtons[1]->isChecked()) {
        largeTitle->setText(u"逐层激活空间"_s);
        largeHeat->positiveOnly = false;
        largeHeat->setMatrix(value.value(u"layer_overview"_s).toArray(),
                             u"行：模型层　列：所选通道起始的 32 个隐藏维度 / 未采集位置留空"_s);
    } else {
        largeTitle->setText(attention.value(u"type"_s).toString() == u"linear"_s ? u"线性注意力递归状态"_s
                                                                                 : u"因果注意力矩阵"_s);
        largeHeat->positiveOnly = attention.value(u"type"_s).toString() != u"linear"_s;
        largeHeat->setMatrix(attention.value(u"matrix"_s).toArray(), heatCaption);
    }
    const auto tensors = value.value(u"tensors"_s).toObject();
    QStringList descriptions;
    const QMap<QString, QString> tensorNames{
        {u"input"_s, u"输入"_s},         {u"output"_s, u"输出"_s},
        {u"weights"_s, u"注意力权重"_s}, {u"recurrent_state"_s, u"递归状态"_s},
        {u"key"_s, u"旋转后的键"_s},     {u"baseline"_s, u"基线激活"_s},
        {u"weight_row"_s, u"参数行"_s},  {u"auxiliary"_s, u"辅助张量"_s}};
    for (auto it = tensors.begin(); it != tensors.end(); ++it)
        descriptions << tensorNames.value(it.key(), u"采集张量"_s) + u"　"_s +
                            shapeText(it.value().toObject().value(u"shape"_s).toArray());
    shapeLabel->setText(descriptions.isEmpty() ? u"当前运算尚未采集"_s : descriptions.join(QChar(u'\n')));
    double min = 0, max = 0, sum = 0, squares = 0;
    int count = 0;
    for (auto r : matrix)
        for (auto cell : r.toArray())
            if (cell.isDouble() && std::isfinite(cell.toDouble())) {
                double v = cell.toDouble();
                if (!count)
                    min = max = v;
                min = std::min(min, v);
                max = std::max(max, v);
                sum += v;
                squares += v * v;
                ++count;
            }
    QString text =
        count ? u"预览最小值　"_s + QString::number(min, 'f', 5) + u"\n预览最大值　"_s +
                    QString::number(max, 'f', 5) + u"\n预览均值　"_s + QString::number(sum / count, 'f', 5) +
                    u"\n预览标准差　"_s +
                    QString::number(std::sqrt(std::max(0.0, squares / count - std::pow(sum / count, 2))), 'f',
                                    5)
              : u"未采集"_s;
    const auto n = value.value(u"neuron"_s).toObject();
    if (!n.isEmpty()) {
        text += u"\n\n所选神经元　"_s + valueText(n.value(u"value"_s)) + u"\n基线激活　"_s +
                valueText(n.value(u"baseline"_s)) + u"\n门控投影　"_s + valueText(n.value(u"gate"_s)) +
                u"\n上投影　"_s + valueText(n.value(u"up"_s)) + u"\n门控激活　"_s +
                valueText(n.value(u"silu"_s)) + u"\n输出贡献　"_s + valueText(n.value(u"contribution"_s)) +
                u"\n非数值　"_s + QString::number(n.value(u"nan"_s).toInt()) + u"　无穷大　"_s +
                QString::number(n.value(u"inf"_s).toInt());
    }
    stats->setText(text);
    const auto context = value.value(u"context"_s).toObject();
    contextLabel->setText(branchName(branch) + u"\n"_s + forwardName(forward) + u" / Token位置 "_s +
                          QString::number(tokenPosition->value()) + u"\n参数版本 "_s +
                          context.value(u"weights_revision"_s).toString(weightsRevision) + u"\n"_s +
                          (value.value(u"status"_s).toString() == u"available"_s ? u"来源：真实采集快照"_s
                                                                                 : u"状态：尚未采集"_s));
    auto candidates = value.value(u"probabilities"_s).toArray();
    if (lens->isChecked() && !value.value(u"lens"_s).toObject().isEmpty())
        candidates = value.value(u"lens"_s).toObject().value(u"candidates"_s).toArray();
    probabilities->setRowCount(candidates.size());
    for (int i = 0; i < candidates.size(); ++i) {
        auto entry = candidates[i].toObject();
        probabilities->setItem(i, 0, new QTableWidgetItem(entry.value(u"text"_s).toString()));
        probabilities->setItem(
            i, 1,
            new QTableWidgetItem(QString::number(entry.value(u"probability"_s).toDouble() * 100, 'f', 3) +
                                 u"%"_s));
    }
    const auto old = tensorSelect->currentData().toString();
    {
        QSignalBlocker blocker(tensorSelect);
        tensorSelect->clear();
        for (auto it = tensors.begin(); it != tensors.end(); ++it)
            tensorSelect->addItem(tensorNames.value(it.key(), u"采集张量"_s), it.key());
        tensorSelect->setCurrentIndex(std::max(0, tensorSelect->findData(old.isEmpty() ? u"output"_s : old)));
    }
    setupTensor();
}

void Workbench::setupTensor() {
    // 默认只取末轴 32 列、次末轴 8 行，其余轴固定为单个位置。
    const auto tensor =
        snapshot.value(u"tensors"_s).toObject().value(tensorSelect->currentData().toString()).toObject();
    const auto shape = tensor.value(u"shape"_s).toArray();
    ranges->setRowCount(shape.size());
    QStringList rows;
    for (int i = 0; i < shape.size(); ++i) {
        rows << u"轴 "_s + QString::number(i);
        int extent = shape[i].toInt(), stop = std::min(extent, i == shape.size() - 1   ? 32
                                                               : i == shape.size() - 2 ? 8
                                                                                       : 1);
        for (int c = 0; c < 3; ++c)
            ranges->setItem(i, c, new QTableWidgetItem(QString::number(c == 0 ? 0 : c == 1 ? stop : 1)));
    }
    ranges->setVerticalHeaderLabels(rows);
    tensorHint->setText(shape.isEmpty() ? u"当前运算没有可用张量"_s
                                        : u"形状："_s + shapeText(shape) +
                                              u"\n切片最多 4096 个元素，起始包含，结束不包含。"_s);
}

void Workbench::requestSlice() {
    // 从界面读取全部轴；错误范围交由后端校验，界面不截断为伪结果。
    const auto tensor =
        snapshot.value(u"tensors"_s).toObject().value(tensorSelect->currentData().toString()).toObject();
    if (tensor.isEmpty()) {
        showError(u"当前没有可用张量"_s);
        return;
    }
    QJsonArray axes;
    for (int i = 0; i < ranges->rowCount(); ++i) {
        QJsonObject axis;
        for (int c = 0; c < 3; ++c) {
            bool ok = false;
            int v = ranges->item(i, c) ? ranges->item(i, c)->text().toInt(&ok) : 0;
            if (!ok) {
                showError(u"切片范围必须是整数"_s);
                return;
            }
            axis[c == 0 ? u"start"_s : c == 1 ? u"stop"_s : u"step"_s] = v;
        }
        axes.append(axis);
    }
    sliceId = worker.send(
        u"tensor.slice"_s,
        {{u"tensor_ref"_s, tensor.value(u"ref"_s)}, {u"ranges"_s, axes}, {u"max_elements"_s, 4096}});
}

void Workbench::scalarQuery() {
    // 标量解释只允许真实投影，非线性节点明确提示可直接查看捕获值。
    if (snapshot.isEmpty()) {
        showError(u"请先选择已采集的投影节点"_s);
        return;
    }
    const auto context = snapshot.value(u"context"_s).toObject();
    const auto positions = context.value(u"positions"_s).toArray();
    int rowIndex = 0;
    for (int i = 0; i < positions.size(); ++i)
        if (positions[i].toInt() == tokenPosition->value())
            rowIndex = i;
    auto payload = selection();
    payload[u"row"_s] = rowIndex;
    payload[u"column"_s] =
        (operation == u"gate"_s || operation == u"up"_s) ? neuron->value() : dimension->value();
    scalarId = worker.send(u"scalar.query"_s, payload);
}





void Workbench::changeView(int index) {
    // 三个主视图共用选择，不重复启动计算。
    if(shell) shell->setCurrentIndex(1);
    restoreRulePanels();
    navigator->show(); timelinePanel->show();
    updatePageHeader(u"计算图"_s);
    syncNavigation(index);
    if(workspaceViews)workspaceViews->setCurrentWidget(mainSplit);
    for (int i = 0; i < viewButtons.size(); ++i)
        viewButtons[i]->setChecked(i == index);
    for (int i = 0; i < railButtons.size(); ++i) railButtons[i]->setChecked(i == index);
    for (auto *control : observationControls) control->show();
    if (inspectorIdentity) inspectorIdentity->show();
    if (inspector->currentIndex() == 3) inspector->setCurrentIndex(0);
    if (!observationSplitState.isEmpty()) {
        leftSplit->restoreState(observationSplitState);
        observationSplitState.clear();
    }
    centerViews->setCurrentIndex(index == 0 ? 0 : 1);
    if (index > 0) {
        largeTitle->setText(index == 1 ? u"逐层激活空间"_s : u"注意力与递归状态"_s);
        dirty = true;
        if (!snapshot.isEmpty())
            renderSnapshot(snapshot);
    } else
        QTimer::singleShot(0, graph, &GraphView::fitGraph);
    timelineTabs->setCurrentIndex(0);
    placeConversation();
}

void Workbench::showNeurons(){
    // 完整页面共用模型与运行器，侧栏切换不改变当前生成或观测记录。
    if(!workspaceViews||!neurons)return;
    auto model=loaded||offline?info:QJsonObject{};model[u"weights_revision"_s]=weightsRevision;
    neurons->configure(model,loaded&&!offline);neurons->context(branch,forward,tokenPosition->value());
    if(shell) shell->setCurrentIndex(1);
    restoreRulePanels();
    workspaceViews->setCurrentWidget(neurons);
    syncNavigation(11);
    placeConversation();
    for(int i=0;i<railButtons.size();++i)railButtons[i]->setChecked(i==11);
    for(auto *control:viewButtons)control->setChecked(false);
}

void Workbench::syncNeurons(){
    // 订阅只在页面可见且未暂停时开启，普通对话模式也可只采样这一页。
    if(!processesStarted||closing||!neurons)return;
    auto payload=neurons->selection();payload[u"enabled"_s]=neurons->isVisible()&&loaded&&!offline&&neurons->following();
    const auto id=worker.send(u"neurons.configure"_s,payload);neuronRequests.insert(id);
}

void Workbench::queryNeurons(){
    // 请求编号隔离快速搜索、模型切换和页面隐藏期间的过期读取结果。
    if(!processesStarted||closing||!neurons||!neurons->isVisible()||(!loaded&&!offline))return;
    if(offline)neurons->context(branch,forward,tokenPosition->value());
    auto payload=neurons->selection();payload[u"run_id"_s]=runId;
    neuronQueryId=worker.send(u"neurons.query"_s,payload);neuronRequests.insert(neuronQueryId);
}

void Workbench::openFeature(int index, bool refresh) {
    // 功能页沿用右侧操作区；模型树和时间轴只在观测页面显示。
    if(shell) shell->setCurrentIndex(1);
    restoreRulePanels();
    navigator->hide(); timelinePanel->hide();
    updatePageHeader(QStringList{u"激活分析"_s,u"词表与向量"_s,u"补丁与版本"_s,u"回归验证"_s}.value(index));
    syncNavigation(index+5);
    if(workspaceViews)workspaceViews->setCurrentWidget(mainSplit);
    for (int i = 0; i < railButtons.size(); ++i) railButtons[i]->setChecked(i == index + 5);
    for (auto *control : viewButtons) control->setChecked(false);
    // 新功能保留原分区，只收起无关的图形工具，给表格留出操作空间。
    for (auto *control : observationControls) control->hide();
    if (inspectorIdentity) inspectorIdentity->hide();
    inspector->setCurrentIndex(3);
    if (observationSplitState.isEmpty()) {
        observationSplitState = leftSplit->saveState();
        leftSplit->setSizes({std::max(300, leftSplit->height() - 140), 140});
    }
    centerViews->setCurrentIndex(2);
    features->context(projectDirectory, loaded && !offline, !active && !loading && !editing, branch);
    if (index == 0) refreshActivationSources();
    features->open(index, refresh);
    placeConversation();
}

void Workbench::switchBranch() {
    // 分支切换只浏览历史，前向目录使用该分支自身的记录。
    if (branchSelect->currentIndex() < 0)
        return;
    follow->setChecked(false);
    branch = branchSelect->currentData().toString();
    timeline->setBranch(branch);
    {
        QSignalBlocker blocker(forwardSelect);
        forwardSelect->clear();
        for (const auto &frame : timeline->frames)
            if (frame.branch == branch)
                forwardSelect->addItem(forwardName(frame.id), frame.id);
        if (forwardSelect->count() == 0)
            forwardSelect->addItem(u"输入预填充"_s, u"f0"_s);
        forward = forwardSelect->currentData().toString();
    }
    renderSelection();
    dirty = true;
}

void Workbench::openTrace() {
    // 离线读取不会依赖当前模型，也不会改写历史文件。
    if (active || loading || editing) {
        showError(u"请先停止当前任务"_s);
        return;
    }
    const auto path = QFileDialog::getExistingDirectory(this, u"选择运行记录目录"_s, tracePath);
    if (path.isEmpty())
        return;
    actionId = worker.send(u"trace.open"_s, {{u"path"_s, path}});
}
void Workbench::exportTrace() {
    // 导出目标必须是尚未存在的新目录，由后端检查路径约束。
    if (tracePath.isEmpty()) {
        showError(u"尚无可导出的运行记录"_s);
        return;
    }
    if (active) {
        showError(u"请先停止当前任务"_s);
        return;
    }
    const auto path =
        QFileDialog::getSaveFileName(this, u"输入新记录目录名称"_s, QDir::homePath() + u"/观测记录"_s);
    if (path.isEmpty())
        return;
    actionId = worker.send(u"trace.export"_s, {{u"destination"_s, path}});
}
void Workbench::exportModel() {
    // 另存副本与保存到当前模型是两个明确入口。
    if (!loaded || active || loading || editing) {
        showError(u"请先加载模型并停止当前任务"_s);
        return;
    }
    const auto path =
        QFileDialog::getSaveFileName(this, u"输入模型副本目录名称"_s, QDir::homePath() + u"/模型副本"_s);
    if (path.isEmpty())
        return;
    editing = true;
    setActive(false);
    actionId = worker.send(u"model.export"_s, {{u"destination"_s, path}});
}
void Workbench::restoreBackup() {
    // 只恢复当前模型自己的备份，恢复本身也会生成可撤回备份。
    if (!loaded || active || loading || editing || offline) {
        showError(u"请先加载模型并停止当前任务"_s);
        return;
    }
    const auto path = QFileDialog::getExistingDirectory(
        this, u"选择要恢复的模型备份"_s,
        lastBackup.isEmpty() ? modelPath + u"/observatory-backups"_s : lastBackup);
    if (path.isEmpty())
        return;
    editing = true;
    setActive(false);
    actionId = worker.send(u"model.restore_backup"_s,
                           {{u"backup_path"_s, path}, {u"expected_weights_revision"_s, weightsRevision}});
}

void Workbench::metric(const QJsonObject &message) {
    // 独立监控进程持续采样，暂停计算不会停止资源更新。
    if (message.value(u"name"_s).toString() != u"telemetry.sample"_s)
        return;
    const auto value = message.value(u"payload"_s).toObject();
    if (value.value(u"index"_s).toInt() != 0)
        return;
    if (value.value(u"status"_s).toString() != u"available"_s) {
        gpuMemory->setText(u"显存　不可用"_s);
        gpuUtilization->setText(u"利用率　不可用"_s);
        gpuName->setText(u"无法读取硬件指标"_s);
        const auto reason = value.value(u"reason"_s).toString();
        if (reason != lastTelemetryError) {
            lastTelemetryError = reason;
            showError(u"资源监控无法读取显卡指标，详情："_s + reason);
        }
        return;
    }
    lastTelemetryError.clear();
    const double used = value.value(u"used"_s).toDouble(), total = value.value(u"total"_s).toDouble();
    gpuMemory->setText(u"显存　"_s + QString::number(used / std::pow(1024, 3), 'f', 2) + u" / "_s +
                       QString::number(total / std::pow(1024, 3), 'f', 2) + u" 吉字节"_s);
    memoryBar->setValue(total > 0 ? qRound(used / total * 100) : 0);
    int utilization = value.value(u"utilization"_s).toInt();
    gpuUtilization->setText(u"计算利用率　"_s + QString::number(utilization) + u"%"_s);
    gpuPlot->sample(utilization);
    gpuName->setText(value.value(u"name"_s).toString() + u" / 每秒采样"_s);
}
void Workbench::showError(const QString &text) {
    // 错误始终弹窗；非阻塞展示避免暂停中的模型与主线程互相等待。
    auto *dialog = new QMessageBox(QMessageBox::Critical, u"操作失败"_s, text, QMessageBox::NoButton, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->addButton(u"确定"_s, QMessageBox::AcceptRole);
    dialog->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    dialog->setDetailedText(u"发生时间："_s + QDateTime::currentDateTime().toString(Qt::ISODate) +
        u"\n项目："_s + projectDirectory + u"\n模型："_s + modelPath + u"\n详情："_s + text);
    // Qt 未安装翻译包时，系统自带的详情按钮也必须显示中文。
    for (auto *button : dialog->findChildren<QPushButton *>())
        if (button->text().contains(u"Details"_s)) {
            button->setText(u"展开详情"_s);
            connect(button, &QPushButton::clicked, dialog, [button] {
                button->setText(button->text().startsWith(u"Hide"_s) ? u"收起详情"_s : u"展开详情"_s);
            });
        }
    dialog->open();
    // 错误保留在状态栏并关联对话，避免短暂提示消失后无法追踪。
    status(u"错误："_s + text);
    runLabel->setText(u"操作失败"_s);
    chat->append(u"<p style='color:#e3b28b'>"_s + text.toHtmlEscaped() + u"</p>"_s);
}
void Workbench::status(const QString &text) {
    statusLabel->setText(text);
    statusLabel->setToolTip(text);
}

void Workbench::resetLayout() {
    // 默认尺寸以已确认原型为准，用户拖动的尺寸会在退出时保存。
    QList<int> sizes;
    // 首页和神经元会暂时移走共享控件，按实际成员设置，避免尺寸错配。
    for (int i = 0; i < mainSplit->count(); ++i)
        sizes.append(mainSplit->widget(i) == conversationHost ? conversationWidth :
                     mainSplit->widget(i) == prototypeInspector ? 264 : std::max(400, width() - 164 - conversationWidth - 264));
    mainSplit->setSizes(sizes);
    navSplit->setSizes({132, std::max(250, width() - 164 - 282 - 264 - 132)});
    leftSplit->setSizes({std::max(380, height() - 340), 190});

    QTimer::singleShot(0, graph, &GraphView::fitGraph);
}
void Workbench::saveLayout() {
    // 分区状态按各自名称保存，不依赖窗口当前最大化状态。
    for (auto pair : {qMakePair(mainSplit, u"main_v2"_s), qMakePair(leftSplit, u"left"_s),
                      qMakePair(navSplit, u"nav"_s)})
        settings.setValue(pair.second, pair.first->saveState());
    settings.setValue(u"conversation_geometry"_s, conversationWindow->saveGeometry());
    const auto current=captureSession();QJsonObject preferences;
    for(const auto &key:{u"inspector"_s,u"navigator_tab"_s,u"timeline_tab"_s,u"center_view"_s,u"feature_page"_s,u"view_type"_s,u"search"_s})preferences[key]=current.value(key);
    preferences[u"neuron_page"_s]=workspaceViews->currentWidget()==neurons;preferences[u"neuron_flat"_s]=neurons->flat();
    settings.setValue(u"workspace_preferences"_s,QJsonDocument(preferences).toJson(QJsonDocument::Compact));
    settings.sync();
}
void Workbench::closeEvent(QCloseEvent *event) {
    // 文件写入使用独立运行器，主窗口退出也要等待其安全完成。
    if(!verifying&&fileWindow&&fileWindow->busy()){
        showError(u"权重文件操作尚未完成，请稍后退出"_s);event->ignore();return;
    }
    if (!verifying && loading) {
        showError(u"模型正在加载，请先停止加载或等待完成后关闭"_s);
        event->ignore();
        return;
    }
    if (!verifying && editing) {
        showError(u"模型参数正在写入，请等待完成后关闭，避免中断文件事务"_s);
        event->ignore();
        return;
    }
    if (!verifying && !projectDirectory.isEmpty() && !checkpointCommitted) {
        if (active) {
            closeAfterSave = true;
            saveProject();
            event->ignore();
            return;
        }
        if (!writeProjectState(false)) {
            event->ignore();
            return;
        }
    }
    closing = true;
    conversationWindow->hide();
    if(fileWindow)fileWindow->close();
    finishModelLoading();
    if (!verifying)
        saveLayout();
    worker.stop();
    monitor.stop();
    event->accept();
}
bool Workbench::eventFilter(QObject *object, QEvent *event) {
    if (object == rules && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        updateFrozenRules();
    // 在选中项变化之前拦截右键，保留原列表与聊天内容供菜单准确命中。
    if (sessionList && object == sessionList->viewport() &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick)) {
        if (static_cast<QMouseEvent *>(event)->button() == Qt::RightButton)
            return true;
    }
    // 对话的关闭按钮收起窗口；退出生命周期只由主程序控制。
    if (object == conversationWindow && event->type() == QEvent::Close && !closing) {
        static_cast<QCloseEvent *>(event)->ignore();
        setConversationEmbedded(true);
        return true;
    }
    // 标题栏使用系统原生拖动与双击最大化，保持桌面窗口习惯。
    if (object == titlebar || object->property("titleDrag").toBool()) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && windowHandle())
                windowHandle()->startSystemMove();
        }
    }
    return QMainWindow::eventFilter(object, event);
}
bool Workbench::nativeEvent(const QByteArray &type, void *message, qintptr *result) {
    // 无边框主窗口仍使用系统边缘缩放，内部区域由原生分隔器处理。
#ifdef Q_OS_WIN
    auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_NCHITTEST && !isMaximized() && !isFullScreen()) {
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        RECT rect{};
        GetWindowRect(reinterpret_cast<HWND>(winId()), &rect);
        const int edge = 6;
        bool left = point.x < rect.left + edge, right = point.x >= rect.right - edge,
             top = point.y < rect.top + edge, bottom = point.y >= rect.bottom - edge;
        if (top || bottom || left || right) {
            *result = top      ? (left    ? HTTOPLEFT
                                  : right ? HTTOPRIGHT
                                          : HTTOP)
                      : bottom ? (left    ? HTBOTTOMLEFT
                                  : right ? HTBOTTOMRIGHT
                                          : HTBOTTOM)
                      : left   ? HTLEFT
                               : HTRIGHT;
            return true;
        }
    }
#endif
    return QMainWindow::nativeEvent(type, message, result);
}
