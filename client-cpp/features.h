#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#include <QElapsedTimer>

class QCheckBox;
class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QTimer;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTextEdit;
class Heatmap;

class FeaturePanel final : public QWidget {
    Q_OBJECT
    friend class Workbench;
    friend class McpBridge;
  public:
    explicit FeaturePanel(QWidget *parent = nullptr);
    void open(int index, bool refresh = true);
    int currentPage() const;
    void context(const QString &directory, bool loaded, bool idle, const QString &branch);
    void pending(bool value, const QString &action = {});
    void progress(const QJsonObject &payload);
    void result(const QJsonObject &payload);
    void failure(const QString &message);
    void reloadProjectData();
    void setActivationSessions(const QJsonArray &sessions);
  signals:
    void requested(const QJsonObject &payload);
    void located(const QJsonObject &row, bool draft);
    void cancelled();
    void inspected(const QString &text);
    void monitored(const QJsonObject &row);
  private:
    QWidget *activationPage();
    QWidget *vectorsPage();
    QWidget *versionsPage();
    QWidget *regressionPage();
    void applyPrototypeLayout();
    void request(QJsonObject payload);
    void searchTokens(int offset = 0);
    void queryVector(bool similar = false);
    void previewVector();
    void editWord();
    void preview(const QJsonObject &payload);
    void saveSamples();
    void loadSamples();
    void loadDrafts();
    void saveDrafts();
    void runRegression(bool exportModel = false);
    void addSample(const QJsonObject &sample = {});
    QJsonArray samples() const;
    void renderReport(const QJsonObject &payload);
    void configureSample();
    void editSample();
    void selectActivationSession();
    void selectActivationReply();
    void analyzeActivation();
    void queryActivation(int page = 1);
    void resetActivation();
    void refreshProgressText();
    void finishProgress(bool success, const QString &message);
    QStackedWidget *pages = nullptr;
    QLabel *notice = nullptr, *activationHint = nullptr, *vectorHint = nullptr, *versionHint = nullptr;
    QPushButton *cancelButton = nullptr;
    QWidget *analysisProgressBox = nullptr;
    QProgressBar *analysisProgress = nullptr;
    QLabel *analysisProgressText = nullptr;
    QTimer *analysisProgressTimer = nullptr;
    QElapsedTimer analysisElapsed;
    QString analysisProgressStage;
    bool analysisCancelling = false;
    QTableWidget *activationTable = nullptr, *tokenTable = nullptr, *vectorTable = nullptr,
                 *similarTable = nullptr, *patchTable = nullptr, *sampleTable = nullptr, *reportTable = nullptr,
                 *draftTable = nullptr;
    QComboBox *scope = nullptr, *metric = nullptr, *source = nullptr, *space = nullptr, *editKind = nullptr;
    QSpinBox *analysisLayer = nullptr, *rangeStart = nullptr, *rangeStop = nullptr, *vectorStart = nullptr,
             *vectorCount = nullptr, *editStart = nullptr, *editStop = nullptr;
    QLineEdit *tokenQuery = nullptr, *baselinePath = nullptr;
    QCheckBox *exact = nullptr, *special = nullptr;
    QTextEdit *editValues = nullptr;
    Heatmap *vectorHeat = nullptr;
    QString projectRoot, branchId;
    QJsonArray activationRows, patchRows;
    QJsonArray drafts;
    QJsonObject previewRequest, lastReport;
    QJsonObject activationReport;
    QString activationSourceKey, activationInspection;
    int activationRevision = 0;
    QLineEdit *activationSearch = nullptr;
    QSpinBox *activationLayerMin = nullptr, *activationLayerMax = nullptr, *activationSamples = nullptr,
             *activationPageNumber = nullptr;
    QDoubleSpinBox *activationScore = nullptr;
    QComboBox *activationSort = nullptr, *activationDirection = nullptr, *activationPageSize = nullptr;
    QLabel *activationPagination = nullptr;
    QPushButton *activationPrevious = nullptr, *activationNext = nullptr;
    QJsonArray analysisSessions;
    QComboBox *analysisSession = nullptr, *analysisReply = nullptr, *analysisContentScope = nullptr, *analysisChannel = nullptr;
    QTextEdit *analysisContent = nullptr;
    int tokenId = -1, tokenOffset = 0, tokenTotal = 0;
    bool modelLoaded = false, available = false, busy = false, vocabularyEditable = false;
};
