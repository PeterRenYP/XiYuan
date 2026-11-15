#pragma once
#include "transport.h"
#include "visuals.h"
#include "chatprompt.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QPointer>

class UiScale;
class QAction;
class AnomalyController;
class FeaturePanel;
class WeightFileWindow;
class NeuronPanel;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTableView;
class QTextBrowser;
class QTreeWidget;
class QLockFile;
QString prototypeWorkbenchStyle();

class Workbench final : public QMainWindow {
    Q_OBJECT
    // 本地 MCP 桥复用工作台状态与入口，避免维护另一套会话和模型。
    friend class McpBridge;
    friend class WeightFileWindow;
  public:
    // 所有界面由原生 C++ 控件组成，模型计算只经独立进程执行。
    explicit Workbench(bool startProcesses = true);
    ~Workbench() override;
    void loadModel(const QString &path, const QString &device = QStringLiteral("cuda:0"));
    void runPrompt(const QString &text, int outputOverride = 0);
    void setMode(const QString &value);
    void choose(int layer, const QString &operation, bool manual = true);
    void releaseStep();
    void cancel();
    void query();
    void applyRules();
    void resetLayout();
    void configureModel();
    void startVerification(const QString &directory);
    void startScaleVerification(const QString &directory);
    void startProjectVerification(const QString &directory);
    void startMaintenanceVerification(const QString &directory);
    void startProjectRuntimeVerification(const QString &directory);
    void startFeatureVerification(const QString &directory);
    void startPlanUiVerification(const QString &directory);
    void startNeuronVerification(const QString &directory);
    void startSessionVerification(const QString &directory);
    void startSessionRuntimeVerification(const QString &directory);
    void startPrototypeVerification(const QString &directory);
    QJsonObject selection() const;
    void receive(const QJsonObject &message);
    void openWeightFile(const QString &path = {});
  signals:
    void received(const QJsonObject &message);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;

  private:
    void build();
    void applyPrototypeLayout();
    void setConversationEmbedded(bool embedded);
    void placeConversation();
    void showAbout();
    void showMcpInstallation();
    void showSessionManager();
    void showIntervention();
    void editHistory(bool redo);
    void updateHistoryActions();
    void refreshActivationSources();
    void sizeConversation();
    void showRulesPage();
    void restoreRulePanels();
    void showHistoryPage();
    void refreshHistoryRecords();
    void updatePageHeader(const QString &title, const QString &subtitle = {});
    void syncNavigation(int index);
    QWidget *buildNavigator();
    QWidget *buildCenter();
    QWidget *buildInspector();
    QWidget *buildTimeline();
    QWidget *buildComparison();
    void runComparison();
    void renderComparison(const QJsonObject &report);
    void configureDebug();
    QWidget *buildProperties();
    QWidget *buildTensor();
    QWidget *buildEditing();
    void modelReady(const QJsonObject &payload, bool restoreProject = true);
    void renderSelection();
    void renderSnapshot(const QJsonObject &snapshot);
    void rememberPreview(const QString &key, const QJsonObject &value);
    void refreshGraph();
    void setActive(bool value);
    void changeView(int index);
    void openFeature(int index, bool refresh = true);
    void showNeurons();
    void syncNeurons();
    void queryNeurons();
    void showError(const QString &text);
    void status(const QString &text);
    void metric(const QJsonObject &message);
    void setupTensor();
    void requestSlice();
    void scalarQuery();
    void addRule();
    void editRule(int row = -1);
    void appendRule(const QJsonObject &rule);
    QJsonArray ruleDraft() const;
    void loadRuleDraft(const QJsonArray &draft);
    void refreshRuleStates(const QString &failure = {});
    void updateEditType();
    void openTrace();
    void exportTrace();
    void exportModel();
    void restoreBackup();
    void switchBranch();
    void saveLayout();
    QString verifyInterfaceScale(const QString &directory, QJsonObject &checks);
    QWidget *buildLauncher();
    void refreshRecentProjects();
    void newProject();
    void openProjectDialog();
    bool createProjectAt(const QString &directory, const QString &name, const QString &model, int limit,
                         const QString &device = QStringLiteral("cuda:0"));
    bool openProjectAt(const QString &directory, bool load = true);
    void showLauncher();
    void ensureProcesses();
    void saveProject(bool manual = true);
    bool writeProjectState(bool pin);
    QJsonObject captureSession() const;
    void restoreSession(const QJsonObject &state, bool activateRuntime = true);
    void activateSession(const QString &id);
    void newSession(bool activateRuntime = true);
    void clearSession(bool remove = false);
    void deleteAllSessions();
    void finishDeleteAllSessions();
    void updateFrozenRules();
    void renderChat(bool streamingOnly = false);
    void setChatHtml(const QString &html, bool forceBottom = false);
    void showTokenMenu(const QString &anchor, const QPoint &position);
    void analyzeToken(const QJsonObject &target);
    QString tokenAnalysisHtml(const QJsonObject &report) const;
    void showCaptureNotice(const QString &key, const QString &text);
    void clearCaptureNotices();
    void showConversation();
    void updateThinkingSupport();
    void beginModelLoading();
    void updateModelLoading(const QJsonObject &payload);
    void finishModelLoading();
    void rememberProjectRules();
    void restoreProjectRules(const QJsonObject &legacy = {});
    void syncExternalEdit(const QJsonObject &payload);
    void refreshSessions();
    void renameSession(const QString &id);
    void updateOutputLimit();
    void showStorage();
    void resumeSavedRun();
    QString sessionDirectory() const;

    QStackedWidget *shell = nullptr;
    QListWidget *recentProjects = nullptr;
    QListWidget *sessionList = nullptr;
    QComboBox *conversationMode = nullptr;
    QString runningMode = QStringLiteral("observe");
    QLabel *projectLabel = nullptr, *captureWarning = nullptr;
    QWidget *captureNoticeFrame = nullptr;
    QMap<QString, QString> captureNotices;
    QPushButton *resumeSaved = nullptr;
    QJsonObject project, pendingSession;
    // 浏览历史会话只替换聊天展示，生成状态始终保存在原会话中。
    QString viewedSessionId, runningInputDraft;
    QJsonObject viewedSession;
    QSet<int> viewedCollapsedThoughts;
    QMap<QString, QJsonObject> sessionStates;
    QStringList deletingSessionIds;
    QString deleteSessionsRequestId;
    int sessionActivationRetries = 0, deleteSessionsRetries = 0;
    bool sessionActivationPending = false;
    QSet<QString> sessionActivationRequests;
    int runningChatScroll = 0;
    QPushButton *deleteAllSessionsButton = nullptr;
    QJsonArray temporaryRules;
    QString rulesModelKey;
    QString projectDirectory, sessionId, checkpointPath, pendingSessionId;
    QLockFile *projectLock = nullptr;
    bool runtimeEnabled = true, processesStarted = false, restoringSession = false,
         resumeRebuilding = false, closeAfterSave = false, checkpointCommitted = false;
    bool verifyProjectRuntime = false;

    UiScale *uiScale = nullptr;
    QPushButton *interfaceZoom = nullptr;
    Transport worker, monitor;
    AnomalyController *anomalies = nullptr;
    QSettings settings;
    QJsonObject info, snapshot, boundary, execution;
    QJsonArray messages, tokens, forwards, catalog;
    QMap<QString, QJsonObject> previews;
    QStringList previewOrder;
    QMap<QString, QString> branchTexts;
    QString modelPath, mode = QStringLiteral("normal"), branch = QStringLiteral("base"),
                       forward = QStringLiteral("f0");
    QString operation = QStringLiteral("norm"), weightsRevision = QStringLiteral("w0"), queryId, sliceId,
            scalarId;
    QString tracePath, runId, lastBackup, assistantText, pendingPrompt;
    QString actionId;
    QString lastTelemetryError;
    int layer = 0, executionLayer = -1;
    bool loaded = false, active = false, loading = false, editing = false, closing = false, offline = false,
         dirty = false, verifying = false;
    QElapsedTimer elapsed;

    QSplitter *mainSplit = nullptr, *leftSplit = nullptr, *navSplit = nullptr;
    QWidget *conversationWindow = nullptr;
    QWidget *conversationPanel = nullptr, *conversationHost = nullptr;
    QWidget *conversationTitle = nullptr, *sessionPanel = nullptr;
    QWidget *pageHeader = nullptr, *rulesPage = nullptr, *historyPage = nullptr;
    QWidget *ruleEditor = nullptr, *comparisonPanel = nullptr;
    QWidget *weightPage = nullptr;
    QSplitter *homeStage = nullptr;
    QWidget *prototypeInspector = nullptr;
    QComboBox *sessionPicker = nullptr;
    QLabel *pageTitle = nullptr, *pageSubtitle = nullptr, *chatSessionStatus = nullptr;
    QLabel *versionStatus = nullptr;
    QPushButton *rulesNavigation = nullptr, *historyNavigation = nullptr;
    bool conversationEmbedded = true, navigationCollapsed = false;
    int conversationWidth = 380;
    QDialog *interventionDialog = nullptr;
    QAction *undoAction = nullptr, *redoAction = nullptr;
    bool canUndoModel = false, canRedoModel = false;
    QString historyQueryId;
    QPointer<QWidget> historyFocus;
    QDialog *loadingDialog = nullptr;
    QProgressBar *loadingProgress = nullptr;
    QLabel *loadingStep = nullptr, *thinkingHint = nullptr;
    QCheckBox *thinkingMode = nullptr;
    QCheckBox *tokenSplit = nullptr;
    QJsonArray responseTokens;
    QJsonObject responseGeneration;
    QString responseReplyId;
    QMap<QString, QJsonObject> chatTokenTargets;
    QTimer chatRefresh;
    QString sessionActivationId;
    QSet<QString> tokenAnalysisRequests;
    QString assistantThought;
    QSet<int> collapsedThoughts;
    bool responseThinking = false, responseThinkingComplete = true;
    bool externalEditPending = false;
    bool stopping = false;
    QDialog *ruleDialog = nullptr;
    QLabel *ruleError = nullptr;
    int editingRuleRow = -1;
    QJsonArray appliedRuleDraft;
    QJsonObject pendingRulePreview;
    void showRulePreview(const QJsonObject &payload);
    QString ruleRequestId;
    QString previewSliceId;
    QString comparisonRequestId;
    QJsonObject comparisonReport;
    void showComparisonDetail(int step);
    QComboBox *comparisonMode = nullptr;
    QLineEdit *comparisonTarget = nullptr;
    QSpinBox *comparisonSteps = nullptr;
    QJsonArray debugPoints;
    WeightFileWindow *fileWindow = nullptr;
    QMap<QString,QJsonObject> featureReports;
    QStringList featureReportOrder;
    QWidget *navigator = nullptr, *center = nullptr, *timelinePanel = nullptr, *titlebar = nullptr;
    GraphView *graph = nullptr;
    LayerStrip *strip = nullptr;
    Heatmap *propertyHeat = nullptr, *largeHeat = nullptr, *sliceHeat = nullptr;
    Timeline *timeline = nullptr;
    ResourcePlot *gpuPlot = nullptr;
    QStackedWidget *centerViews = nullptr;
    QStackedWidget *workspaceViews = nullptr;
    NeuronPanel *neurons = nullptr;
    QString neuronQueryId;
    QSet<QString> neuronRequests;
    FeaturePanel *features = nullptr;
    QTextBrowser *featureDetails = nullptr;
    QWidget *inspectorIdentity = nullptr;
    QString featureRequestId;
    QList<QPushButton *> railButtons;
    QList<QWidget *> observationControls;
    QByteArray observationSplitState;
    QTabWidget *inspector = nullptr, *navigatorTabs = nullptr, *timelineTabs = nullptr;
    QTreeWidget *tree = nullptr;
    QListWidget *history = nullptr;
    QTextBrowser *chat = nullptr, *scalarText = nullptr, *comparison = nullptr;
    QLabel *runLabel = nullptr, *modelLabel = nullptr, *modelMeta = nullptr, *positionLabel = nullptr,
           *statusLabel = nullptr, *selectionLabel = nullptr;
    QLabel *identity = nullptr, *eyebrow = nullptr, *formula = nullptr, *shapeLabel = nullptr,
           *stats = nullptr, *contextLabel = nullptr, *cellValue = nullptr, *tensorHint = nullptr;
    QLabel *gpuMemory = nullptr, *gpuUtilization = nullptr, *gpuName = nullptr, *timeLabel = nullptr,
           *largeTitle = nullptr, *editHint = nullptr, *tokenInfo = nullptr;
    QProgressBar *memoryBar = nullptr;
    ChatPrompt *prompt = nullptr;
    QLineEdit *search = nullptr;
    QComboBox *branchSelect = nullptr, *forwardSelect = nullptr, *operationSelect = nullptr,
              *headSelect = nullptr, *tensorSelect = nullptr, *editTarget = nullptr, *ruleKind = nullptr,
              *ruleScope = nullptr, *projection = nullptr;
    QSpinBox *tokenPosition = nullptr, *neuron = nullptr, *dimension = nullptr, *maximum = nullptr,
             *ruleLayer = nullptr, *ruleNeuron = nullptr, *rulePosition = nullptr, *ruleDimension = nullptr;
    QDoubleSpinBox *temperature = nullptr, *thinkingTemperature = nullptr, *ruleValue = nullptr;
    QCheckBox *follow = nullptr, *lens = nullptr;
    QPushButton *play = nullptr, *step = nullptr, *stop = nullptr, *send = nullptr, *apply = nullptr,
                *clearEdit = nullptr, *zoomLabel = nullptr;
    QTableWidget *ranges = nullptr, *rules = nullptr, *probabilities = nullptr;
    QTableView *frozenRules = nullptr;
    QList<QPushButton *> workspaceButtons, viewButtons;
};
