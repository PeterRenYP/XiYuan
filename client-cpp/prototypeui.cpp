#include "workbench.h"
#include "features.h"
#include "neurons.h"
#include "mcpbridge.h"
#include "uiscale.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QStyle>
#include <functional>
#include <algorithm>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QPainter>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {
// 只移除布局项，控件随后移入原型对应的分区，业务连接保持有效。
void clearLayout(QLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        if (auto *widget = item->widget()) widget->hide();
        delete item;
    }
}
QPushButton *action(const QString &text, QObject *receiver, const std::function<void()> &callback) {
    // 本文件按钮只连接既有业务入口。
    auto *button = new QPushButton(text);
    button->setCursor(Qt::PointingHandCursor);
    QObject::connect(button, &QPushButton::clicked, receiver, callback);
    return button;
}
QVBoxLayout *column(QWidget *widget, int margin = 0, int spacing = 0) {
    // 分区间距直接采用确认原型中的数值。
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(spacing);
    return layout;
}
QLabel *text(const QString &value, const QString &name = {}) {
    auto *label = new QLabel(value);
    label->setObjectName(name);
    return label;
}
QIcon navigationIcon(bool collapsed) {
    // 侧栏轮廓与方向箭头直接表达收起、展开，避免使用工作区图标。
    QPixmap image(24, 24); image.fill(Qt::transparent);
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(u"#a9d8e8"_s), 1.5));
    painter.drawRoundedRect(QRectF(2, 4, 20, 16), 2, 2); painter.drawLine(8, 4, 8, 20);
    const int direction = collapsed ? 1 : -1;
    painter.drawLine(12, 12, 18, 12);
    painter.drawLine(15 + 3 * direction, 12, 15, 9);
    painter.drawLine(15 + 3 * direction, 12, 15, 15);
    return QIcon(image);
}
} // namespace

// 原型的颜色、字号、文字导航与分区边界统一在此定义。
QString prototypeWorkbenchStyle() {
    return uR"(
QWidget{font-size:14px;} QLabel#muted{font-size:12px;color:#97a6bb;} QLabel#heading{font-size:20px;}
QWidget#titlebar{background:#1b1d21;border-bottom:1px solid #39414d;}
QWidget#controlbar{background:#25282d;border-bottom:1px solid #39414d;}
QWidget#prototypePageHeader{background:#1c1e22;} QWidget#prototypePageHeader QWidget{background:transparent;}
QWidget#prototypePageHeader QLabel#heading{font-size:20px;}
QWidget#prototypeInspector QTabWidget{background:#25282d;}
QTabWidget#prototypeNavigator QTabBar::tab{font-size:12px;padding:9px 3px;}
QWidget#rail{background:#22252a;border-right:1px solid #39414d;}
QWidget#rail QPushButton{background:transparent;border:1px solid transparent;text-align:left;padding:9px 11px;min-height:22px;}
QWidget#rail QPushButton:checked{background:#344f60;border-left:2px solid #a9d8e8;color:#e1f2fa;}
QWidget#rail QPushButton:hover{background:#303944;}
QWidget#rail QLabel{padding:12px 10px 4px;color:#97a6bb;font-size:12px;}
QPushButton[quiet=true],QToolButton[quiet=true]{background:transparent;border-color:transparent;}
QPushButton[quiet=true]:hover,QToolButton[quiet=true]:hover{background:#303944;}
QToolButton#prototypeBrand{font-size:18px;color:#a9d8e8;background:transparent;border:0;padding:0;}
QToolButton#prototypeBrand::menu-indicator{image:none;}
QWidget#conversationHost{background:#25282d;border-left:1px solid #39414d;}
QWidget#conversationPanel{background:#25282d;}
QWidget#conversationPanel QTextBrowser{background:#25282d;border:0;font-size:14px;}
QWidget#conversationPanel QWidget#chatComposer{background:#202329;border:1px solid #526170;border-radius:8px;}
QPlainTextEdit#chatPrompt{background:#202329;border:0;padding:4px;selection-background-color:#466275;}
QWidget#conversationPanel QComboBox{padding:6px;font-size:12px;}
QWidget#conversationPanel QPushButton{padding:5px 8px;font-size:12px;}
QWidget#conversationPanel QLabel#chatStatus{font-size:11px;color:#97a6bb;}
QWidget#prototypeInspector{border-left:1px solid #39414d;}
QWidget#prototypeInspector QPushButton#workspace{border:0;border-bottom:2px solid transparent;background:transparent;padding:9px 10px;}
QWidget#prototypeInspector QPushButton#workspace:checked{background:#344f60;border-bottom-color:#a9d8e8;}
QWidget#prototypeInspector QLabel{font-size:12px;}
QWidget#prototypeInspector QLabel#heading{font-size:16px;}
QTreeWidget,QListWidget,QTextBrowser,QTableWidget{background:#22252a;}
QHeaderView::section{background:#22252a;border:0;border-bottom:1px solid #39414d;padding:10px 8px;font-size:12px;}
QTableWidget{gridline-color:#39414d;} QTableWidget::item{padding:6px;}
QLabel#status{font-size:12px;} QStatusBar{background:#1b1d21;border-top:1px solid #39414d;}
QWidget#launcher{background:#1c1e22;} QLabel#launcherBrand{font-size:24px;}
QWidget#homeContent,QWidget#homeContent QLabel{background:transparent;}
QWidget#historyCard{background:#22252a;border-bottom:1px solid #39414d;}
QWidget#historyCard QLabel{background:transparent;border:0;}
QSplitter::handle{background:#39414d;}
)"_s;
}

void Workbench::applyPrototypeLayout() {
    // 解开旧外壳，保留全部模型、会话、观测和干预控件。
    auto *scroll = qobject_cast<QScrollArea *>(shell->widget(1));
    auto *root = scroll->takeWidget();
    auto *layout = qobject_cast<QVBoxLayout *>(root->layout());
    auto *menuRow = layout->itemAt(1)->widget();
    auto *controlbar = layout->itemAt(2)->widget();
    auto *body = layout->itemAt(3)->widget();
    auto *bodyRow = qobject_cast<QHBoxLayout *>(body->layout());
    auto *rail = bodyRow->itemAt(0)->widget();
    auto *menus = menuRow->findChild<QMenuBar *>();
    auto *launcher = shell->widget(0);
    takeCentralWidget();
    shell->removeWidget(scroll);
    bodyRow->removeWidget(workspaceViews);
    shell->addWidget(workspaceViews);
    clearLayout(layout);
    setCentralWidget(root);
    scroll->deleteLater();
    root->setObjectName(u"prototypeRoot"_s);

    // 菜单收进品牌入口；常用设置、帮助和窗口控制沿用原型顶部位置。
    auto *title = qobject_cast<QHBoxLayout *>(titlebar->layout());
    clearLayout(title);
    title->setContentsMargins(20, 10, 10, 10);
    title->setSpacing(16);
    UiScale::fixedHeight(titlebar, 58);
    auto *brand = new QToolButton;
    brand->setObjectName(u"prototypeBrand"_s);
    brand->setText(u"析元"_s);
    // 品牌图标使用现有资源，由 Qt 按屏幕缩放绘制。
    brand->setIcon(QIcon(u":/ui/assets/xiyuan.ico"_s));
    brand->setIconSize(QSize(26, 26));
    brand->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    brand->setAccessibleName(u"析元应用菜单"_s);
    auto *applicationMenu = new QMenu(brand);
    QMenu *helpMenu = nullptr, *editMenu = nullptr;
    for (auto *entry : menus->actions()) {
        if (entry->menu()) {
            applicationMenu->addMenu(entry->menu());
            if (entry->text() == u"帮助"_s) helpMenu = entry->menu();
            if (entry->text() == u"编辑"_s) editMenu = entry->menu();
        }
    }
    brand->setMenu(applicationMenu);
    brand->setPopupMode(QToolButton::InstantPopup);
    title->addWidget(brand);
    // 文件操作统一收进菜单；快捷键由工作台现有 QShortcut 接收，避免重复注册。
    auto *fileButton = new QPushButton(u"文件"_s);
    fileButton->setObjectName(u"fileMenuButton"_s); fileButton->setProperty("quiet", true);
    auto *fileMenu = new QMenu(fileButton);
    fileMenu->addAction(u"新建项目"_s, this, &Workbench::newProject);
    fileMenu->addAction(u"打开项目"_s, this, &Workbench::openProjectDialog);
    fileMenu->addSeparator();
    fileMenu->addAction(u"保存\tCtrl+S"_s, this, [this] { saveProject(); });
    fileButton->setMenu(fileMenu); title->addWidget(fileButton);
    if (editMenu) {
        undoAction = new QAction(u"撤销\tCtrl+Z"_s, this);
        redoAction = new QAction(u"重做\tCtrl+Y / Ctrl+Shift+Z"_s, this);
        editMenu->insertAction(editMenu->actions().value(0), redoAction);
        editMenu->insertAction(redoAction, undoAction);
        connect(undoAction, &QAction::triggered, this, [this] { editHistory(false); });
        connect(redoAction, &QAction::triggered, this, [this] { editHistory(true); });
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            // 打开标题栏菜单时记住原编辑框；点击其他内容才转向模型历史。
            if(!now||now==titlebar||titlebar->isAncestorOf(now)||qobject_cast<QMenu *>(now))return;
            historyFocus=(qobject_cast<QLineEdit *>(now)||qobject_cast<QPlainTextEdit *>(now)||
                (qobject_cast<QTextEdit *>(now)&&!qobject_cast<QTextEdit *>(now)->isReadOnly()))?now:nullptr;
        });
        // 原生文本控件优先接收快捷键，离开编辑框后快捷键复用模型版本历史。
        for (auto *window : {static_cast<QWidget *>(this), conversationWindow}) {
            new QShortcut(QKeySequence::Undo, window, [this] { editHistory(false); });
            new QShortcut(QKeySequence(u"Ctrl+Y"_s), window, [this] { editHistory(true); });
            new QShortcut(QKeySequence(u"Ctrl+Shift+Z"_s), window, [this] { editHistory(true); });
        }
        connect(editMenu, &QMenu::aboutToShow, this, [this] {
            updateHistoryActions();
            if (loaded && !active && !loading && !editing && historyQueryId.isEmpty())
                historyQueryId = worker.send(u"feature.request"_s, {{u"action"_s,u"versions"_s}});
        });
        auto *edit = new QPushButton(u"编辑"_s); edit->setProperty("quiet", true); edit->setMenu(editMenu); title->addWidget(edit);
        auto *timer = new QTimer(this); timer->setInterval(150);
        connect(timer, &QTimer::timeout, this, &Workbench::updateHistoryActions); timer->start();
    }
    auto *control = new QPushButton(u"控制"_s); control->setObjectName(u"controlMenuButton"_s); control->setProperty("quiet", true);
    auto *controlMenu = new QMenu(control);
    controlMenu->addAction(u"神经元干预"_s, this, &Workbench::showIntervention);
    control->setMenu(controlMenu); title->addWidget(control);
    projectLabel->show();
    title->addWidget(projectLabel);
    title->addStretch();
    auto *settingsButton = action(u"设置"_s, this, [this] { configureModel(); });
    settingsButton->setProperty("quiet", true);
    title->addWidget(settingsButton);
    if (helpMenu) {
        helpMenu->addSeparator();
        auto *install = helpMenu->addAction(u"安装 MCP"_s, this, &Workbench::showMcpInstallation);
        install->setObjectName(u"installMcpAction"_s);
        auto *about = helpMenu->addAction(u"关于析元"_s, this, &Workbench::showAbout);
        about->setObjectName(u"aboutAction"_s);
        auto *help = new QPushButton(u"帮助"_s);
        help->setProperty("quiet", true);
        help->setMenu(helpMenu);
        title->addWidget(help);
    }
    for (int i = 0; i < 3; ++i) {
        auto *button = action(i == 0 ? u"—"_s : i == 1 ? u"□"_s : u"×"_s, this, [this, i] {
            if (i == 0) showMinimized();
            else if (i == 1) isMaximized() ? showNormal() : showMaximized();
            else close();
        });
        button->setProperty("quiet", true);
        UiScale::fixedWidth(button, 32);
        title->addWidget(button);
    }
    menuRow->hide();
    layout->addWidget(titlebar);
    titlebar->show();

    // 文字导航与原型顺序一致，隐藏的旧入口仍可从应用菜单访问。
    auto *nav = qobject_cast<QVBoxLayout *>(rail->layout());
    clearLayout(nav);
    nav->setContentsMargins(10, 17, 10, 12);
    nav->setSpacing(4);
    UiScale::fixedWidth(rail, 164);
    auto *collapse = action(u"收起导航"_s, this, [this, rail] {
        navigationCollapsed = !navigationCollapsed;
        UiScale::fixedWidth(rail, navigationCollapsed ? 58 : 164);
        for (auto *button : rail->findChildren<QPushButton *>()) {
            if (!button->property("navigationText").isValid()) continue;
            button->setText(navigationCollapsed ? QString{} : button->property("navigationText").toString());
        }
        for (auto *label : rail->findChildren<QLabel *>()) label->setVisible(!navigationCollapsed);
        auto *toggle = rail->findChild<QPushButton *>(u"toggleNavigation"_s);
        toggle->setIcon(navigationIcon(navigationCollapsed));
        toggle->setToolTip(navigationCollapsed ? u"展开导航"_s : u"收起导航"_s);
        toggle->setAccessibleName(toggle->toolTip());
        if (uiScale) uiScale->setFactor(uiScale->factor());
    });
    collapse->setObjectName(u"toggleNavigation"_s);
    collapse->setAccessibleName(u"收起导航"_s); collapse->setToolTip(u"收起导航"_s);
    collapse->setIcon(navigationIcon(false));
    collapse->setProperty("navigationText", u"收起导航"_s);
    nav->addWidget(collapse);
    const auto addNavigation = [nav](QPushButton *button, const QString &name) {
        button->setText(name);
        button->setProperty("navigationText", name);
        button->setProperty("scaleFixedWidth", QVariant());
        button->setProperty("scaleFixedHeight", QVariant());
        button->setMinimumSize(0, 0);
        button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        button->setIconSize(QSize(17, 17));
        button->setCheckable(true);
        nav->addWidget(button);
        button->show();
    };
    nav->addWidget(text(u"观测"_s));
    addNavigation(railButtons[0], u"计算图"_s);
    addNavigation(railButtons[11], u"神经元"_s);
    addNavigation(railButtons[5], u"激活分析"_s);
    addNavigation(railButtons[6], u"词表与向量"_s);
    nav->addWidget(text(u"干预与验证"_s));
    addNavigation(railButtons[7], u"补丁与版本"_s);
    nav->addStretch();
    addNavigation(railButtons[9], u"权重文件"_s);
    historyNavigation = action(u"运行记录"_s, this, [this] { showHistoryPage(); });
    historyNavigation->setIcon(railButtons[4]->icon());
    addNavigation(historyNavigation, u"运行记录"_s);
    auto *home = action(u"项目首页"_s, this, [this] {
        // 首页仅切换展示，关闭项目仍由应用菜单执行。
        shell->setCurrentIndex(0); refreshRecentProjects(); syncNavigation(-1);
    });
    home->setIcon(railButtons[9]->icon());
    addNavigation(home, u"项目首页"_s);

    // 模型栏位于导航右侧，执行按钮移至计算图页标题右边。
    auto *modelbar = qobject_cast<QHBoxLayout *>(controlbar->layout());
    clearLayout(modelbar);
    modelbar->setContentsMargins(19, 10, 19, 10);
    UiScale::fixedHeight(controlbar, 54);
    modelbar->addWidget(modelLabel);
    modelbar->addWidget(modelMeta);
    modelbar->addWidget(runLabel);
    modelbar->addStretch();
    modelLabel->show(); modelMeta->show(); runLabel->show();
    maximum->hide();
    tokenInfo->hide();
    auto *work = new QWidget;
    auto *workLayout = column(work);
    workLayout->addWidget(controlbar);
    workLayout->addWidget(shell, 1);
    controlbar->show();
    bodyRow->addWidget(work, 1);
    layout->addWidget(body, 1);
    body->show(); rail->show(); shell->show();

    pageHeader = new QWidget;
    pageHeader->setObjectName(u"prototypePageHeader"_s);
    auto *heading = new QHBoxLayout(pageHeader);
    heading->setContentsMargins(16, 17, 16, 15);
    auto *titles = new QWidget;
    auto *titleColumn = column(titles, 0, 3);
    pageTitle = text(u"计算图"_s, u"heading"_s);
    pageSubtitle = text({}, u"muted"_s);
    titleColumn->addWidget(pageTitle);
    titleColumn->addWidget(pageSubtitle);
    heading->addWidget(titles, 1);
    for (auto *button : {play, step, stop}) { heading->addWidget(button); button->show(); }
    auto *centerArea = new QWidget;
    auto *centerLayout = column(centerArea);
    navSplit->setParent(nullptr);
    centerLayout->addWidget(pageHeader);
    centerLayout->addWidget(navSplit, 1);
    leftSplit->insertWidget(0, centerArea);
    navSplit->show();
    center->layout()->itemAt(0)->widget()->hide();
    // 工具栏在窄窗口内横向滚动，保持按钮文字完整且不挤占右侧两区。
    auto *viewbar = observationControls.first();
    auto *centerColumn = qobject_cast<QVBoxLayout *>(center->layout());
    const int toolbarPosition = centerColumn->indexOf(viewbar);
    centerColumn->removeWidget(viewbar);
    auto *toolbarScroll = new QScrollArea;
    toolbarScroll->setWidgetResizable(true);
    toolbarScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    toolbarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    UiScale::minimumWidth(viewbar, 500);
    toolbarScroll->setWidget(viewbar);
    UiScale::fixedHeight(toolbarScroll, 46);
    centerColumn->insertWidget(toolbarPosition, toolbarScroll);
    observationControls[0] = toolbarScroll;
    UiScale::minimumWidth(navigator, 132);
    navigatorTabs->setObjectName(u"prototypeNavigator"_s);
    UiScale::minimumWidth(center, 250);
    auto *inspectorPanel = mainSplit->widget(1);
    prototypeInspector = inspectorPanel;
    inspectorPanel->setObjectName(u"prototypeInspector"_s);
    UiScale::minimumWidth(inspectorPanel, 264);
    conversationHost = new QWidget;
    conversationHost->setObjectName(u"conversationHost"_s);
    column(conversationHost);
    conversationWidth = settings.value(u"conversation_width_v104"_s, 380).toInt();
    UiScale::minimumWidth(conversationHost, 260);
    // 消息、警告和长会话标题不能反向撑大分区，尺寸由分隔线控制。
    conversationHost->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    conversationHost->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    mainSplit->insertWidget(1, conversationHost);
    mainSplit->setStretchFactor(0, 1);
    mainSplit->setStretchFactor(1, 0);
    mainSplit->setStretchFactor(2, 0);

    // 把现有对话控件变为共享面板，嵌入和独立窗口只切换父容器。
    auto *conversationSplit = conversationWindow->findChild<QSplitter *>();
    sessionPanel = conversationSplit->widget(0);
    conversationPanel = conversationSplit->widget(1);
    conversationPanel->setObjectName(u"conversationPanel"_s);
    conversationPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    conversationPanel->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    sessionPanel->hide();
    conversationSplit->hide();
    auto *conversationLayout = qobject_cast<QVBoxLayout *>(conversationPanel->layout());
    conversationLayout->setContentsMargins(10, 10, 10, 10);
    conversationLayout->setSpacing(10);
    conversationTitle = conversationLayout->itemAt(0)->widget();
    auto *chatTitleRow = qobject_cast<QHBoxLayout *>(conversationTitle->layout());
    QPushButton *clear = nullptr;
    for (auto *button : conversationTitle->findChildren<QPushButton *>())
        if (button->text() == u"清空"_s) clear = button;
    clearLayout(chatTitleRow);
    chatTitleRow->addWidget(text(u"对话"_s));
    chatTitleRow->addStretch();
    auto *popout = action(u"弹出窗口"_s, this, [this] { showConversation(); });
    popout->setObjectName(u"popoutConversation"_s);
    chatTitleRow->addWidget(popout);
    auto *sessionRow = new QWidget;
    auto *sessions = new QHBoxLayout(sessionRow);
    sessions->setContentsMargins(0, 0, 0, 0);
    sessions->setSpacing(6);
    sessionPicker = new QComboBox;
    sessionPicker->setObjectName(u"sessionPicker"_s);
    sessionPicker->setAccessibleName(u"切换会话"_s);
    sessionPicker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sessions->addWidget(sessionPicker, 1);
    auto *newButton = action(u"新建"_s, this, [this] { newSession(); });
    newButton->setObjectName(u"newChatSession"_s);
    sessions->addWidget(newButton);
    conversationLayout->insertWidget(1, sessionRow);
    connect(sessionPicker, &QComboBox::activated, this, [this](int index) {
        activateSession(sessionPicker->itemData(index).toString());
    });
    // 会话操作使用明确可见的菜单按钮，内嵌与独立窗口共享此行和原有业务流程。
    auto *sessionActions=new QPushButton(u"会话管理"_s);sessionActions->setObjectName(u"chatSessionActions"_s);
    auto *sessionMenu=new QMenu(sessionActions);sessionActions->setMenu(sessionMenu);sessions->addWidget(sessionActions);
    auto *rename=sessionMenu->addAction(u"修改会话标题"_s,this,[this]{renameSession(sessionPicker->currentData().toString());});
    auto *remove=sessionMenu->addAction(u"删除会话"_s,this,[this]{clearSession(true);});
    sessionMenu->addSeparator();auto *removeAll=sessionMenu->addAction(u"删除所有会话"_s,this,&Workbench::deleteAllSessions);
    sessionMenu->addSeparator();sessionMenu->addAction(u"查看会话列表"_s,this,&Workbench::showSessionManager);
    connect(sessionMenu,&QMenu::aboutToShow,this,[this,rename,remove,removeAll]{
        const bool idle=!active&&!loading&&!editing&&!sessionActivationPending&&!pendingSession.contains(u"cleanup_path"_s);
        const bool selected=!sessionPicker->currentData().toString().isEmpty();
        rename->setEnabled(idle&&selected);remove->setEnabled(idle&&selected);
        removeAll->setEnabled(idle&&!project.value(u"sessions"_s).toArray().isEmpty());
    });
    sessionPicker->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sessionPicker,&QWidget::customContextMenuRequested,this,[this,sessionMenu](const QPoint &position){sessionMenu->popup(sessionPicker->mapToGlobal(position));});
    auto *chatTools = new QWidget;
    auto *tools = new QHBoxLayout(chatTools);
    tools->setContentsMargins(0, 0, 0, 0);
    tokenSplit->setText(u"Token拆分"_s);
    tools->addWidget(tokenSplit);
    tokenSplit->show();
    tools->addStretch();
    if (clear) { clear->setText(u"清空消息"_s); tools->addWidget(clear); clear->show(); }
    conversationLayout->insertWidget(2, chatTools);
    auto *composerRow = prompt->parentWidget();
    auto *composeRow = qobject_cast<QHBoxLayout *>(composerRow->layout());
    composeRow->removeWidget(prompt);
    auto *thinkingOptions = thinkingMode->parentWidget();
    thinkingOptions->layout()->removeWidget(thinkingMode);
    thinkingOptions->hide();
    thinkingHint->hide();
    composeRow->insertWidget(1, thinkingMode);
    thinkingMode->show();
    composeRow->insertStretch(2);
    composeRow->setContentsMargins(0, 0, 0, 0);
    composeRow->setSpacing(5);
    auto *composer = new QWidget;
    composer->setObjectName(u"chatComposer"_s);
    auto *compose = column(composer, 8, 8);
    conversationLayout->removeWidget(composerRow);
    prompt->setObjectName(u"chatPrompt"_s);
    prompt->setPlaceholderText(u"输入消息，支持多行文本"_s);
    UiScale::minimumHeight(prompt, 86);
    UiScale::fixedHeight(prompt, 100);
    compose->addWidget(prompt);
    compose->addWidget(composerRow);
    conversationLayout->addWidget(composer);
    chatSessionStatus = text(u"Enter 发送 · Ctrl+Enter 换行"_s, u"chatStatus"_s);
    conversationLayout->addWidget(chatSessionStatus);
    // 恢复已保存运行属于上下文操作，保留入口并收进会话工具行。
    tools->insertWidget(1, resumeSaved);
    conversationWindow->layout()->removeWidget(conversationSplit);
    deleteAllSessionsButton->hide();
    conversationPanel->setParent(conversationHost);
    conversationHost->layout()->addWidget(conversationPanel);
    conversationPanel->show();
    conversationWindow->resize(750, 650);

    rulesPage = new QWidget;
    rulesPage->setObjectName(u"rulesPage"_s);
    column(rulesPage, 16, 18);
    centerViews->addWidget(rulesPage);
    auto *editingScroll = qobject_cast<QScrollArea *>(inspector->widget(2));
    ruleEditor = editingScroll->widget();
    comparisonPanel = timelineTabs->widget(1);
    qobject_cast<QVBoxLayout *>(comparisonPanel->layout())->insertWidget(0, text(u"分支对比"_s, u"heading"_s));
    connect(inspector, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2 && ruleEditor->parentWidget() == rulesPage) changeView(0);
    });
    historyPage = new QWidget;
    historyPage->setObjectName(u"historyPage"_s);
    auto *records = column(historyPage, 16, 16);
    // 记录页读取项目中真实保存的运行，不把当前会话的分支冒充项目记录。
    auto *recordActions = new QHBoxLayout;
    auto *recordFilter = new QComboBox;
    recordFilter->setObjectName(u"historyFilter"_s);
    recordFilter->setAccessibleName(u"筛选运行记录"_s);
    recordFilter->addItem(u"全部记录"_s, u"all"_s);
    recordFilter->addItem(u"观测模式"_s, u"observe"_s);
    recordFilter->addItem(u"普通对话"_s, u"normal"_s);
    recordActions->addWidget(recordFilter);
    recordActions->addStretch();
    recordActions->addWidget(action(u"导出记录"_s, this, [this] { exportTrace(); }));
    records->addLayout(recordActions);
    auto *recordsList = new QListWidget;
    recordsList->setObjectName(u"prototypeHistory"_s);
    recordsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recordsList->setSelectionMode(QAbstractItemView::NoSelection);
    records->addWidget(recordsList, 1);
    connect(recordFilter, &QComboBox::currentIndexChanged, this, [this] { refreshHistoryRecords(); });
    centerViews->addWidget(historyPage);

    // 首页使用同一外壳；仅保留原型中的主操作、最近项目和次级入口。
    launcher->layout()->itemAt(0)->widget()->hide();
    for (auto *label : launcher->findChildren<QLabel *>()) {
        if (label->objectName() == u"launcherBrand"_s) label->setText(u"打开你的研究现场"_s);
        else if (label->objectName() == u"muted"_s) label->hide();
    }
    auto *content = recentProjects->parentWidget();
    content->setObjectName(u"homeContent"_s);
    content->setProperty("scaleFixedWidth", QVariant());
    content->setMinimumWidth(0); content->setMaximumWidth(720);
    content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    // 内容占满中心可用宽度，两侧留白由内边距负责；窄窗口允许继续收缩。
    auto *homeRow = qobject_cast<QHBoxLayout *>(launcher->layout()->itemAt(2)->layout());
    homeRow->setStretch(0, 0); homeRow->setStretch(1, 1); homeRow->setStretch(2, 0);
    recentProjects->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recentProjects->setTextElideMode(Qt::ElideMiddle);
    auto *homeColumn = qobject_cast<QVBoxLayout *>(content->layout());
    auto *homeActions = new QHBoxLayout;
    for (auto *button : content->findChildren<QPushButton *>()) {
        if (button->text() == u"旧版记录"_s || button->text() == u"权重文件"_s) {
            homeActions->addWidget(button);
            button->setObjectName(u"homeSecondary"_s);
            UiScale::fixedHeight(button, 34);
        }
    }
    homeColumn->addLayout(homeActions);
    // 首页同样保留对话与操作区，切换首页不卸载正在使用的研究现场。
    shell->removeWidget(launcher);
    homeStage = new QSplitter(Qt::Horizontal);
    homeStage->setChildrenCollapsible(false);
    homeStage->setHandleWidth(4);
    homeStage->addWidget(launcher);
    launcher->show();
    shell->insertWidget(0, homeStage);
    for (auto *split : {mainSplit, homeStage})
        connect(split, &QSplitter::splitterMoved, this, [this, split] {
            const int index = split->indexOf(conversationHost);
            if (index < 0 || !conversationHost->isVisible()) return;
            conversationWidth = qRound(split->sizes().value(index) / uiScale->factor());
            settings.setValue(u"conversation_width_v104"_s, conversationWidth);
        });
    if(auto *split=neurons->findChild<QSplitter *>(u"neuronContentSplit"_s))
        connect(split,&QSplitter::splitterMoved,this,[this]{
            if(!neurons->conversationArea()->isVisible())return;
            conversationWidth=qRound(neurons->conversationArea()->width()/uiScale->factor());
            settings.setValue(u"conversation_width_v104"_s,conversationWidth);
        });
    connect(shell, &QStackedWidget::currentChanged, this, [this] { placeConversation(); });
    versionStatus = text(u"版本 "_s + QCoreApplication::applicationVersion(), u"status"_s);
    statusBar()->addPermanentWidget(versionStatus);
    conversationEmbedded = settings.value(u"conversation_embedded"_s, true).toBool();
    placeConversation();
}

void Workbench::updatePageHeader(const QString &title, const QString &subtitle) {
    // 中心标题随功能切换；执行控件只属于计算图页面。
    if (!pageTitle) return;
    pageTitle->setText(title);
    pageSubtitle->setText(subtitle);
    pageSubtitle->setVisible(!subtitle.isEmpty());
    for (auto *button : {play, step, stop}) button->setVisible(title == u"计算图"_s);
}

void Workbench::refreshActivationSources() {
    // 读取每个会话自己的回复引用；不激活会话、不打开主运行器的当前记录。
    QJsonArray sessions;
    for(const auto &entry:project.value(u"sessions"_s).toArray()){
        const auto item=entry.toObject();const auto id=item.value(u"id"_s).toString();
        QJsonObject state;
        if(id==sessionId)state=captureSession();
        else if(sessionStates.contains(id))state=sessionStates.value(id);
        else {QFile file(projectDirectory+u"/sessions/"_s+id+u"/state.json"_s);if(file.open(QIODevice::ReadOnly))state=QJsonDocument::fromJson(file.readAll()).object();}
        QJsonArray replies;
        for(const auto &value:state.value(u"messages"_s).toArray()){
            auto reply=value.toObject();if(reply.value(u"role"_s).toString()!=u"assistant"_s||reply.value(u"mode"_s).toString()==u"normal"_s)continue;
            const auto relative=reply.value(u"trace"_s).toString();
            const auto path=QDir(projectDirectory).absoluteFilePath(relative);
            QJsonObject manifest;QFile file(path+u"/manifest.json"_s);
            const bool safe=!relative.isEmpty()&&!relative.startsWith(u".."_s)&&QDir(path).canonicalPath().startsWith(QDir(projectDirectory+u"/sessions"_s).canonicalPath()+QChar(u'/'),Qt::CaseInsensitive);
            if(safe&&file.open(QIODevice::ReadOnly))manifest=QJsonDocument::fromJson(file.readAll()).object();
            if(manifest.value(u"capture_policy"_s).toString()==u"none"_s)continue;
            if(reply.value(u"mode"_s).toString().isEmpty()&&manifest.isEmpty())continue;
            reply[u"trace_path"_s]=path;reply[u"run_id"_s]=manifest.value(u"run_id"_s);
            const auto tokens=reply.value(u"output_tokens"_s).toArray();
            reply[u"branch_id"_s]=tokens.isEmpty()?u"base"_s:tokens.first().toObject().value(u"branch_id"_s).toString(u"base"_s);
            reply[u"available"_s]=safe&&!manifest.isEmpty()&&QFileInfo::exists(path+u"/index.sqlite"_s);
            replies.append(reply);
        }
        sessions.append(QJsonObject{{u"id"_s,id},{u"title"_s,item.value(u"title"_s)},{u"replies"_s,replies}});
    }
    features->setActivationSessions(sessions);
}

void Workbench::syncNavigation(int index) {
    for (int i = 0; i < railButtons.size(); ++i) railButtons[i]->setChecked(i == index);
    if (rulesNavigation) rulesNavigation->setChecked(index == 12);
    if (historyNavigation) historyNavigation->setChecked(index == 4);
}

void Workbench::placeConversation() {
    // 移动共享容器，不清空聊天、草稿、选中会话、Token分析和生成状态。
    if (!conversationHost || !neurons) return;
    const bool home = shell->currentIndex() == 0;
    const bool neuronPage = !home && workspaceViews->currentWidget() == neurons;
    if (prototypeInspector && homeStage) {
        if (home && homeStage->indexOf(prototypeInspector) < 0) homeStage->addWidget(prototypeInspector);
        else if (!home && mainSplit->indexOf(prototypeInspector) < 0) mainSplit->addWidget(prototypeInspector);
        prototypeInspector->show();
    }
    if (conversationEmbedded) {
        conversationWindow->hide();
        if (conversationPanel->parentWidget() != conversationHost) {
            conversationPanel->setParent(conversationHost);
            conversationHost->layout()->addWidget(conversationPanel);
        }
        if (home) {
            if (homeStage->indexOf(conversationHost) < 0) { homeStage->insertWidget(1, conversationHost); sizeConversation(); }
            homeStage->setStretchFactor(0, 1); homeStage->setStretchFactor(1, 0); homeStage->setStretchFactor(2, 0);
        } else if (neuronPage) {
            if (conversationHost->parentWidget() != neurons->conversationArea()) {
                conversationHost->setParent(neurons->conversationArea());
                neurons->conversationArea()->layout()->addWidget(conversationHost);
                sizeConversation();
            }
        } else if (mainSplit->indexOf(conversationHost) < 0) {
            conversationHost->setParent(nullptr);
            mainSplit->insertWidget(1, conversationHost);
            sizeConversation();
        }
        neurons->conversationArea()->setVisible(neuronPage);
        conversationHost->show();
        conversationPanel->show();
        conversationTitle->show();
    } else {
        conversationHost->hide();
        neurons->conversationArea()->hide();
        if (conversationPanel->parentWidget() != conversationWindow) {
            conversationPanel->setParent(conversationWindow);
            conversationWindow->layout()->addWidget(conversationPanel);
        }
        conversationTitle->hide();
        conversationPanel->show();
    }
}

void Workbench::sizeConversation() {
    // 仅在更换容器时恢复用户宽度；消息刷新和切换会话不重新分配空间。
    const int width = qRound(conversationWidth * (uiScale ? uiScale->factor() : 1.));
    if (auto *split = qobject_cast<QSplitter *>(conversationHost->parentWidget())) {
        auto sizes = split->sizes(); const int index = split->indexOf(conversationHost);
        if (index < 0) return;
        int other = 0; for (int i = 1; i < sizes.size(); ++i) if (i != index) other += sizes[i];
        sizes[index] = width; sizes[0] = std::max(350, split->width() - width - other - 8);
        split->setSizes(sizes);
    } else if (neurons && conversationHost->parentWidget() == neurons->conversationArea()) {
        auto *split = neurons->findChild<QSplitter *>(u"neuronContentSplit"_s);
        if (split) split->setSizes({std::max(350, split->width() - width - 235 - 8), width, 235});
    }
}

void Workbench::showIntervention() {
    // 弹窗临时承载同一个规则编辑器，关闭时归还右侧，所有入口共用草稿与应用结果。
    if (interventionDialog) { interventionDialog->show(); interventionDialog->raise(); return; }
    restoreRulePanels();
    auto *scroll = qobject_cast<QScrollArea *>(inspector->widget(2));
    scroll->takeWidget();
    interventionDialog = new QDialog(this); interventionDialog->setObjectName(u"interventionDialog"_s);
    interventionDialog->setModal(true);
    interventionDialog->setWindowTitle(u"神经元干预"_s); interventionDialog->resize(720, 720);
    auto *layout = column(interventionDialog, 16, 12);
    layout->addWidget(ruleEditor, 1); ruleEditor->show();
    layout->addWidget(action(u"关闭"_s, interventionDialog, [this] { interventionDialog->close(); }), 0, Qt::AlignRight);
    connect(interventionDialog, &QDialog::finished, this, [this, scroll] {
        auto *dialog = interventionDialog; interventionDialog = nullptr;
        scroll->setWidget(ruleEditor); ruleEditor->show(); dialog->deleteLater();
    });
    interventionDialog->show();
}

void Workbench::editHistory(bool redo) {
    // 编辑框的历史独立于模型；不能撤销文字时也不意外写回模型。
    auto *focus = historyFocus ? historyFocus.data() : QApplication::focusWidget();
    if (auto *input = qobject_cast<QLineEdit *>(focus)) { redo ? input->redo() : input->undo(); return; }
    if (auto *input = qobject_cast<QPlainTextEdit *>(focus)) { redo ? input->redo() : input->undo(); return; }
    if (auto *input = qobject_cast<QTextEdit *>(focus); input && !input->isReadOnly()) { redo ? input->redo() : input->undo(); return; }
    if (loaded && !active && !loading && !editing && !offline)
        features->request({{u"action"_s, u"versions.action"_s}, {u"operation"_s, redo ? u"redo"_s : u"undo"_s}});
}

void Workbench::updateHistoryActions() {
    // 菜单按当前焦点与真实版本历史更新，空历史不显示为可执行。
    if (!undoAction || !redoAction) return;
    auto *focus = historyFocus ? historyFocus.data() : QApplication::focusWidget(); bool undo = false, redo = false;
    if (auto *input = qobject_cast<QLineEdit *>(focus)) { undo = !input->isReadOnly() && input->isUndoAvailable(); redo = !input->isReadOnly() && input->isRedoAvailable(); }
    else if (auto *input = qobject_cast<QPlainTextEdit *>(focus)) { undo = !input->isReadOnly() && input->document()->isUndoAvailable(); redo = !input->isReadOnly() && input->document()->isRedoAvailable(); }
    else if (auto *input = qobject_cast<QTextEdit *>(focus); input && !input->isReadOnly()) { undo = input->document()->isUndoAvailable(); redo = input->document()->isRedoAvailable(); }
    else if (loaded && !active && !loading && !editing && !offline) { undo = canUndoModel; redo = canRedoModel; }
    undoAction->setEnabled(undo); redoAction->setEnabled(redo);
}

void Workbench::setConversationEmbedded(bool embedded) {
    // 窗口关闭和嵌入按钮共用入口，重启后恢复最后使用的呈现方式。
    conversationEmbedded = embedded;
    settings.setValue(u"conversation_embedded"_s, embedded);
    placeConversation();
}

void Workbench::showRulesPage() {
    // 规则面板与右侧共用控件，避免两份草稿与应用状态不一致。
    shell->setCurrentIndex(1);
    workspaceViews->setCurrentWidget(mainSplit);
    navigator->hide(); timelinePanel->hide();
    for (auto *control : observationControls) control->hide();
    if (interventionDialog) interventionDialog->close();
    if (ruleEditor->parentWidget() != rulesPage) {
        auto *scroll = qobject_cast<QScrollArea *>(inspector->widget(2));
        scroll->takeWidget();
        rulesPage->layout()->addWidget(ruleEditor);
        timelineTabs->removeTab(timelineTabs->indexOf(comparisonPanel));
        rulesPage->layout()->addWidget(comparisonPanel);
    }
    ruleEditor->show(); comparisonPanel->show();
    centerViews->setCurrentWidget(rulesPage);
    inspector->setCurrentIndex(3);
    updatePageHeader(u"干预规则"_s, u"当前项目 / 规则草稿"_s);
    syncNavigation(12);
    featureDetails->setPlainText(u"干预规则\n\n规则数量："_s + QString::number(rules->rowCount()) + u"\n"_s + editHint->text());
    placeConversation();
}

void Workbench::restoreRulePanels() {
    // 离开规则功能页时归还右侧编辑器与底部分支对比，保持原入口可用。
    if (!ruleEditor || ruleEditor->parentWidget() != rulesPage) return;
    auto *scroll = qobject_cast<QScrollArea *>(inspector->widget(2));
    scroll->setWidget(ruleEditor);
    timelineTabs->addTab(comparisonPanel, u"分支对比"_s);
}

void Workbench::showHistoryPage() {
    // 运行记录独立占据中心区，不显示无关的模型树与执行时间轴。
    shell->setCurrentIndex(1);
    restoreRulePanels();
    workspaceViews->setCurrentWidget(mainSplit);
    navigator->hide(); timelinePanel->hide();
    for (auto *control : observationControls) control->hide();
    refreshHistoryRecords();
    centerViews->setCurrentWidget(historyPage);
    updatePageHeader(u"运行记录"_s);
    syncNavigation(4);
    placeConversation();
}

void Workbench::refreshHistoryRecords() {
    // 仅读取清单元数据，不加载张量；时间取真实文件更新时间，缺失指标不编造。
    auto *records = historyPage->findChild<QListWidget *>(u"prototypeHistory"_s);
    auto *filter = historyPage->findChild<QComboBox *>(u"historyFilter"_s);
    records->clear();
    QList<QFileInfo> manifests;
    if (!projectDirectory.isEmpty()) {
        QDirIterator files(projectDirectory + u"/sessions"_s, {u"manifest.json"_s},
                           QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (files.hasNext()) manifests.append(QFileInfo(files.next()));
    }
    std::sort(manifests.begin(), manifests.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() > b.lastModified();
    });
    for (const auto &file : manifests) {
        QFile input(file.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) continue;
        const auto manifest = QJsonDocument::fromJson(input.readAll()).object();
        if (manifest.value(u"run_id"_s).toString().isEmpty()) continue;
        const bool normal = manifest.value(u"capture_policy"_s).toString() == u"none"_s;
        const auto category = normal ? u"normal"_s : u"observe"_s;
        if (filter->currentData() != u"all"_s && filter->currentData() != category) continue;
        QString title;
        const auto entries = manifest.value(u"messages"_s).toArray();
        for (const auto &entry : entries) {
            const auto message = entry.toObject();
            if (message.value(u"role"_s).toString() == u"user"_s) title = message.value(u"content"_s).toString();
        }
        title = title.simplified();
        if (title.isEmpty()) title = u"运行记录 "_s + manifest.value(u"run_id"_s).toString().left(8);
        auto *card = new QWidget;
        card->setObjectName(u"historyCard"_s);
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(16, 18, 16, 18);
        auto *description = new QWidget;
        auto *labels = column(description, 0, 7);
        auto *name = text(title.left(48));
        name->setToolTip(title); name->setWordWrap(true);
        labels->addWidget(name);
        auto *metadata = text(file.lastModified().toLocalTime().toString(u"yyyy-MM-dd HH:mm"_s) + u" · "_s +
                              (normal ? u"普通对话"_s : u"观测模式"_s), u"muted"_s);
        metadata->setWordWrap(true);
        labels->addWidget(metadata);
        row->addWidget(description, 1);
        const auto path = file.absolutePath();
        row->addWidget(action(u"打开记录"_s, this, [this, path] {
            // 打开离线记录会替换观测现场，先保存当前会话且禁止打断正在执行的任务。
            if (active || loading || editing) { showError(u"请先停止当前任务"_s); return; }
            if (!writeProjectState(false)) return;
            ensureProcesses();
            actionId = worker.send(u"trace.open"_s, {{u"path"_s, path}});
            changeView(0);
        }));
        auto *item = new QListWidgetItem(records);
        item->setData(Qt::UserRole, path);
        item->setSizeHint(QSize(0, qRound(104 * (uiScale ? uiScale->factor() : 1))));
        records->setItemWidget(item, card);
    }
    const auto count = records->count();
    if (count == 0) {
        auto *empty = new QListWidgetItem(u"暂无运行记录"_s, records);
        empty->setFlags(Qt::NoItemFlags);
    }
    featureDetails->setPlainText(u"运行记录\n\n当前筛选："_s + filter->currentText() + u"\n记录数量："_s + QString::number(count));
}

void Workbench::showAbout() {
    // 关于窗口展示作者信息，版本从应用元数据读取。
    auto *dialog = new QDialog(this);
    dialog->setObjectName(u"aboutDialog"_s);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"关于析元"_s);
    dialog->setModal(true);
    UiScale::minimumWidth(dialog, 440);
    auto *layout = column(dialog, 25, 20);
    layout->addWidget(text(u"析元"_s, u"heading"_s));
    layout->addWidget(text(u"模型观测与干预工具"_s, u"muted"_s));
    layout->addWidget(text(u"作者：PeterRYP"_s));
    auto *form = new QFormLayout;
    auto *version = text(QCoreApplication::applicationVersion());
    version->setObjectName(u"applicationVersion"_s);
    form->addRow(u"应用版本"_s, version);
    form->addRow(u"运行平台"_s, text(u"Windows"_s));
    form->addRow(u"界面语言"_s, text(u"简体中文"_s));
    layout->addLayout(form);
    layout->addWidget(action(u"关闭"_s, dialog, [dialog] { dialog->close(); }), 0, Qt::AlignRight);
    dialog->open();
}

void Workbench::showMcpInstallation() {
    // 安装提示词按当前可用入口生成，开发版与安装版均使用标准管道协议。
    auto *dialog = new QDialog(this);
    dialog->setObjectName(u"mcpInstallationDialog"_s);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"安装 MCP"_s);
    dialog->setModal(true);
    dialog->resize(760, 680);
    auto *layout = column(dialog, 24, 15);
    layout->addWidget(text(u"让 Codex 自动安装"_s, u"heading"_s));
    auto *steps = text(u"1. 保持析元运行。\n2. 复制下方提示词，粘贴到 Codex 并发送。\n3. 安装完成后，按 Codex 提示重新连接或重启，再验证连接。"_s);
    steps->setWordWrap(true);
    layout->addWidget(steps);
    const auto *bridge = findChild<McpBridge *>();
    const auto endpoint = bridge ? bridge->localEndpoint() : QString{};
    auto *connection = text(endpoint.isEmpty() ? u"本地接口：未启动"_s : u"本地接口："_s + endpoint);
    connection->setObjectName(u"mcpLocalEndpoint"_s);
    layout->addWidget(connection);
    auto *explanation = text(u"Codex 通过标准管道服务自动发现本机客户端。客户端重启后会自动读取新端口。"_s, u"muted"_s);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    const auto root = Transport::projectRoot();
    QString command = QCoreApplication::applicationDirPath() + u"/Xiyuan-MCP.exe"_s;
    QStringList arguments;
    bool available = QFileInfo::exists(command);
    if (!available) {
        command = root + u"/.venv-mcp/Scripts/python.exe"_s;
        arguments = {u"-X"_s, u"utf8"_s, root + u"/run_mcp.py"_s};
        available = QFileInfo::exists(command) && QFileInfo::exists(arguments.last());
    }
    // PowerShell 单引号和 TOML 字符串分别编码，路径中的空格与引号不会破坏命令。
    const auto quote = [](QString value) { return QChar(u'\'') + value.replace(u"'"_s, u"''"_s) + QChar(u'\''); };
    QString registration = u"codex mcp add model_observatory"_s;
    const auto customDirectory = qEnvironmentVariable("MODEL_OBSERVATORY_MCP_DIR");
    if (!customDirectory.isEmpty()) registration += u" --env "_s + quote(u"MODEL_OBSERVATORY_MCP_DIR="_s + customDirectory);
    registration += u" -- "_s + quote(QDir::toNativeSeparators(command));
    for (const auto &argument : arguments) registration += QChar(u' ') + quote(argument);
    QString promptText = u"请帮我自动安装并验证析元的 MCP 服务，服务名称为 model_observatory。\n\n"_s;
    if (available) {
        promptText += u"客户端程序："_s + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) +
            u"\n已检测到本地 MCP 入口，无需下载或重新安装运行环境。\n\n"_s;
        promptText += u"请先读取 Codex 当前 MCP 配置。相同配置已存在时直接验证；同名配置路径不同时，只更新该服务并保留其他服务。优先使用 Codex 官方命令：\n"_s + registration +
            u"\n\n若命令不在 PATH，请查找本机 Codex 安装目录中的 codex.exe。若 CLI 不可用，备份并只更新实际 CODEX_HOME（未设置时为用户目录下 .codex）内 config.toml 的 [mcp_servers.model_observatory]，不要覆盖其他配置。\n"_s;
        const auto tomlString = [](const QString &value) {
            auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
            return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
        };
        promptText += u"\n配置入口：\ncommand = "_s + tomlString(QDir::toNativeSeparators(command)) + u"\nargs = "_s +
            QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(arguments)).toJson(QJsonDocument::Compact)) +
            u"\nstartup_timeout_sec = 20\ntool_timeout_sec = 90\n"_s;
        if (!customDirectory.isEmpty()) promptText += u"env = { MODEL_OBSERVATORY_MCP_DIR = "_s + tomlString(customDirectory) + u" }\n"_s;
    } else {
        promptText += u"当前客户端目录缺少 MCP 服务入口。请先检查同目录的 Xiyuan-MCP.exe 或源码根目录的 run_mcp.py 与 .venv-mcp/Scripts/python.exe。若均不存在，请明确报告缺少的文件，不要注册无效路径。\n"_s;
    }
    if (bridge && !endpoint.isEmpty()) promptText += u"\n本机实例发现目录："_s + QDir::toNativeSeparators(bridge->connectionDirectory()) + u"。请动态发现端口，不读取或输出连接令牌，也不要把本地 TCP 控制桥配置为 HTTP MCP 地址。\n"_s;
    promptText += u"\n目标客户端进程号："_s + QString::number(QCoreApplication::applicationPid()) +
        u"。安装后执行 codex mcp list --json 核对配置，再调用 observatory_list_instances 发现实例、observatory_select_instance 选择该进程对应的析元窗口、observatory_get_state 验证连接。若工具需要在重启后才加载，请明确告知我重启 Codex；未实际调用成功前不要宣称连接成功。只做安装与只读验证，不切换项目、加载模型、发送消息或修改权重。"_s;
    auto *promptField = new QPlainTextEdit(promptText);
    promptField->setObjectName(u"mcpInstallationPrompt"_s);
    promptField->setReadOnly(true);
    layout->addWidget(promptField, 1);
    auto *buttons = new QHBoxLayout;
    auto *feedback = text({}, u"muted"_s);
    buttons->addWidget(feedback, 1);
    buttons->addWidget(action(u"关闭"_s, dialog, [dialog] { dialog->close(); }));
    auto *copy = action(u"复制安装提示词"_s, dialog, [promptField, feedback] {
        QApplication::clipboard()->setText(promptField->toPlainText());
        feedback->setText(u"已复制，请粘贴到 Codex"_s);
    });
    copy->setObjectName(u"copyMcpInstallationPrompt"_s);
    copy->setProperty("primary", true);
    buttons->addWidget(copy);
    layout->addLayout(buttons);
    dialog->open();
}

void Workbench::showSessionManager() {
    // 原会话列表保留在管理窗口中，批量管理和非当前会话重命名仍使用原业务入口。
    if (auto *existing = findChild<QDialog *>(u"sessionManagerDialog"_s)) {
        existing->show(); existing->raise(); return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(u"sessionManagerDialog"_s);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"管理会话"_s);
    dialog->resize(380, 620);
    auto *layout = column(dialog, 15, 12);
    layout->addWidget(sessionPanel, 1); sessionPanel->show();
    deleteAllSessionsButton->show();
    layout->addWidget(action(u"关闭"_s, dialog, [dialog] { dialog->close(); }));
    connect(dialog, &QDialog::finished, this, [this] {
        sessionPanel->setParent(conversationWindow); sessionPanel->hide();
    });
    dialog->show();
}
