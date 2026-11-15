#include "workbench.h"
#include "features.h"
#include "neurons.h"
#include "chattokenview.h"
#include <algorithm>
#include "uiscale.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include "anomalycontroller.h"

using namespace Qt::StringLiterals;

namespace {
// 项目文件全部使用原子替换，不能用一次失败的保存覆盖上一个有效现场。
bool writeJson(const QString &path, const QJsonObject &value, QString *error = nullptr) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
QJsonObject readJson(const QString &path, QString *error = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        if (error)
            *error = u"无法读取文件或文件过大："_s + path + u"\n"_s + file.errorString();
        return {};
    }
    QJsonParseError parse;
    auto value = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!value.isObject() || parse.error != QJsonParseError::NoError) {
        if (error)
            *error = u"项目文件格式错误："_s + path;
        return {};
    }
    return value.object();
}
QVBoxLayout *vertical(QWidget *widget, int margin = 0, int spacing = 8) {
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(spacing);
    return layout;
}
QPushButton *action(const QString &text, QWidget *parent, std::function<void()> callback) {
    auto *button = new QPushButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    QObject::connect(button, &QPushButton::clicked, parent, callback);
    return button;
}
bool validId(const QString &id) {
    return QRegularExpression(u"^[a-f0-9]{32}$"_s).match(id).hasMatch();
}
quint64 diskBytes(const QString &path) {
    // 不跟随目录链接，项目统计不跨入外部模型或其他目录。
    quint64 total = 0;
    QDirIterator files(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        total += files.fileInfo().size();
    }
    return total;
}
QString storageText(quint64 bytes) {
    return QString::number(bytes / 1073741824.0, 'f', 2) + u" GiB"_s;
}
}

QWidget *Workbench::buildLauncher() {
    // 与参考图一致：大面积留白、居中操作区和最近项目；不堆叠工作区面板。
    auto *page = new QWidget;
    page->setObjectName(u"launcher"_s);
    auto *layout = vertical(page, 0, 0);
    auto *header = new QWidget;
    header->setProperty("titleDrag", true);
    header->installEventFilter(this);
    auto *head = new QHBoxLayout(header);
    head->setContentsMargins(18, 6, 8, 6);
    head->addWidget(new QLabel(u"析元"_s));
    head->addStretch();
    head->addWidget(action(u"最小化"_s, this, [this] { showMinimized(); }));
    head->addWidget(action(u"最大化"_s, this, [this] { isMaximized() ? showNormal() : showMaximized(); }));
    head->addWidget(action(u"关闭"_s, this, [this] { close(); }));
    layout->addWidget(header);
    layout->addStretch(3);
    auto *horizontal = new QHBoxLayout;
    horizontal->addStretch();
    auto *content = new QWidget;
    UiScale::fixedWidth(content, 640);
    auto *center = vertical(content, 24, 18);
    auto *brand = new QLabel(u"析元"_s);
    brand->setObjectName(u"launcherBrand"_s);
    center->addWidget(brand);
    auto *subtitle = new QLabel(u"从一个项目开始，保存每次探索的完整现场。"_s);
    subtitle->setObjectName(u"muted"_s);
    center->addWidget(subtitle);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(12);
    auto *create = action(u"新建项目"_s, this, [this] { newProject(); });
    create->setObjectName(u"launcherAction"_s);
    UiScale::fixedHeight(create, 64);
    auto *open = action(u"打开项目"_s, this, [this] { openProjectDialog(); });
    open->setObjectName(u"launcherAction"_s);
    UiScale::fixedHeight(open, 64);
    auto *archive = action(u"旧版记录"_s, this, [this] {
        const auto archiveRoot = QStorageInfo(Transport::projectRoot()).rootPath() + u"ModelObservatoryData/旧版运行记录"_s;
        auto path = QFileDialog::getExistingDirectory(this, u"选择旧版运行记录"_s,
                                                     settings.value(u"archive_directory"_s, archiveRoot).toString());
        if (!path.isEmpty()) {
            settings.setValue(u"archive_directory"_s, path);
            shell->setCurrentIndex(1);
            ensureProcesses();
            worker.send(u"trace.open"_s, {{u"path"_s, path}});
        }
    });
    archive->setObjectName(u"launcherAction"_s);
    UiScale::fixedHeight(archive, 64);
    actions->addWidget(create);
    actions->addWidget(open);
    actions->addWidget(archive);
    auto *weights=action(u"权重文件"_s,this,[this]{openWeightFile();});
    weights->setObjectName(u"launcherAction"_s);UiScale::fixedHeight(weights,64);actions->addWidget(weights);
    center->addLayout(actions);
    center->addWidget(new QLabel(u"最近项目"_s));
    recentProjects = new QListWidget;
    recentProjects->setObjectName(u"recentProjects"_s);
    UiScale::fixedHeight(recentProjects, 230);
    center->addWidget(recentProjects);
    connect(recentProjects, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) { openProjectAt(item->data(Qt::UserRole).toString()); });
    connect(recentProjects, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { openProjectAt(item->data(Qt::UserRole).toString()); });
    auto *note = new QLabel(u"模型与记录保存在你选择的位置。项目不复制模型权重。"_s);
    note->setObjectName(u"muted"_s);
    center->addWidget(note);
    horizontal->addWidget(content);
    horizontal->addStretch();
    layout->addLayout(horizontal);
    layout->addStretch(4);
    refreshRecentProjects();
    return page;
}

void Workbench::refreshRecentProjects() {
    // 注册表仅记录最近路径，项目数据全部位于项目目录。
    if (!recentProjects)
        return;
    recentProjects->clear();
    for (const auto &path : settings.value(u"recent_projects"_s).toStringList()) {
        if (!QFile::exists(path + u"/project.json"_s))
            continue;
        const auto data = readJson(path + u"/project.json"_s);
        auto *item = new QListWidgetItem(data.value(u"name"_s).toString(QDir(path).dirName()) + u"\n"_s + path,
                                         recentProjects);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    if (recentProjects->count() == 0) {
        auto *empty = new QListWidgetItem(u"尚无最近项目。新建项目或打开已有项目目录。"_s, recentProjects);
        empty->setFlags(Qt::NoItemFlags);
    }
}

void Workbench::ensureProcesses() {
    // 启动页不加载推理依赖，进入项目后才启动独立进程。
    if (runtimeEnabled && anomalies) anomalies->start();
    if (processesStarted || !runtimeEnabled)
        return;
    const auto path = projectDirectory.isEmpty() ? Transport::projectRoot() + u"/data"_s : projectDirectory;
    worker.setStorageRoot(path);
    monitor.setStorageRoot(path);
    worker.start();
    monitor.start();
    processesStarted = true;
}

void Workbench::newProject() {
    // 项目目录和模型目录分别选择，避免把观测文件写进模型权重目录。
    if (active || loading || editing) {
        showError(u"请先停止当前任务，再创建项目"_s);
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"新建项目"_s);
    UiScale::minimumWidth(dialog, 640);
    auto *layout = vertical(dialog, 24, 14);
    auto *heading = new QLabel(u"新建观测项目"_s);
    heading->setObjectName(u"heading"_s);
    layout->addWidget(heading);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(u"未命名项目"_s);
    auto *directory = new QLineEdit(Transport::projectRoot() + u"/projects/未命名项目"_s);
    auto *model = new QLineEdit(modelPath);
    auto *models = new QComboBox;
    auto recent = settings.value(u"recent_models"_s, QStringList{}).toStringList();
    for (const auto &entry : recent)
        models->addItem(QDir(entry).dirName(), entry);
    auto *limit = new QSpinBox;
    limit->setRange(1, 262143);
    // 在加载模型前读取本地配置；用户未调低时始终跟随所选模型的满上限。
    auto updateLimit = [model, limit] {
        const bool full = limit->value() == limit->maximum();
        const auto raw = readJson(model->text() + u"/config.json"_s);
        const auto config = raw.contains(u"text_config"_s) ? raw.value(u"text_config"_s).toObject() : raw;
        limit->setMaximum(std::max(1, config.value(u"max_position_embeddings"_s).toInt(262144) - 1));
        if (full)
            limit->setValue(limit->maximum());
    };
    limit->setValue(limit->maximum());
    updateLimit();
    connect(model, &QLineEdit::textChanged, dialog, updateLimit);
    auto *device = new QComboBox;
    device->addItem(u"显卡计算"_s, u"cuda:0"_s);
    device->addItem(u"处理器计算"_s, u"cpu"_s);
    form->addRow(u"项目名称"_s, name);
    auto pathRow = [this, dialog](QLineEdit *field, const QString &title) {
        auto *widget = new QWidget;
        auto *row = new QHBoxLayout(widget);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(field, 1);
        row->addWidget(action(u"选择目录"_s, dialog, [this, field, title] {
            auto path = QFileDialog::getExistingDirectory(this, title, field->text());
            if (!path.isEmpty())
                field->setText(path);
        }));
        return widget;
    };
    form->addRow(u"项目目录"_s, pathRow(directory, u"选择独立的项目文件夹"_s));
    form->addRow(u"最近使用的模型"_s, models);
    form->addRow(u"模型目录"_s, pathRow(model, u"选择模型权重目录"_s));
    form->addRow(u"每次最多生成Token"_s, limit);
    form->addRow(u"计算设备"_s, device);
    layout->addLayout(form);
    auto *hint = new QLabel(u"记录写入项目目录。张量采集不设额度限制；默认保留 1 GiB 磁盘空间，可在存储管理中调整。"_s);
    hint->setWordWrap(true);
    hint->setObjectName(u"muted"_s);
    layout->addWidget(hint);
    connect(models, &QComboBox::activated, dialog,
            [models, model](int i) { model->setText(models->itemData(i).toString()); });
    connect(name, &QLineEdit::textEdited, dialog, [directory](const QString &value) {
        directory->setText(QFileInfo(directory->text()).absolutePath() + u"/"_s + value);
    });
    auto *buttons = new QDialogButtonBox;
    auto *create = buttons->addButton(u"创建并打开"_s, QDialogButtonBox::AcceptRole);
    auto *cancel = buttons->addButton(u"取消"_s, QDialogButtonBox::RejectRole);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(create, &QPushButton::clicked, dialog, [=] {
        if (createProjectAt(directory->text(), name->text(), model->text(), limit->value(), device->currentData().toString()))
            dialog->accept();
    });
    layout->addWidget(buttons);
    dialog->open();
}

bool Workbench::createProjectAt(const QString &directory, const QString &name, const QString &model,
                                int limit, const QString &device) {
    const auto path = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    const auto modelDirectory = QDir::cleanPath(QFileInfo(model).absoluteFilePath());
    const auto raw = readJson(modelDirectory + u"/config.json"_s);
    const auto config = raw.contains(u"text_config"_s) ? raw.value(u"text_config"_s).toObject() : raw;
    if (limit == 0)
        limit = std::max(1, config.value(u"max_position_embeddings"_s).toInt(262144) - 1);
    if (name.trimmed().isEmpty() || limit < 1 || QDir(path).isRoot() ||
        QFile::exists(path + u"/project.json"_s) || QFile::exists(path + u"/config.json"_s) ||
        path.compare(modelDirectory, Qt::CaseInsensitive) == 0 ||
        modelDirectory.startsWith(path + u"/"_s, Qt::CaseInsensitive) ||
        path.startsWith(modelDirectory + u"/"_s, Qt::CaseInsensitive)) {
        showError(u"项目名称、目录或Token上限无效。项目与模型目录必须分开，已有项目请使用打开项目。"_s);
        return false;
    }
    if (!QFile::exists(modelDirectory + u"/config.json"_s)) {
        showError(u"模型目录缺少 config.json，请选择完整模型目录"_s);
        return false;
    }
    if (!QDir().mkpath(path)) {
        showError(u"无法创建项目目录，请检查权限和磁盘空间"_s);
        return false;
    }
    QJsonObject data{{u"schema_version"_s, 1}, {u"name"_s, name.trimmed()}, {u"model_path"_s, modelDirectory},
        {u"device"_s, device}, {u"max_tokens"_s, limit}, {u"temperature"_s, .7},
        {u"storage"_s, QJsonObject{{u"total_mib"_s, 0}, {u"run_mib"_s, 0},
                                 {u"reserve_mib"_s, 1024}, {u"auto_clean"_s, true}, {u"policy"_s, u"standard"_s}}},
        {u"sessions"_s, QJsonArray{}}};
    QString error;
    if (!writeJson(path + u"/project.json"_s, data, &error)) {
        showError(u"创建项目失败："_s + error);
        return false;
    }
    return openProjectAt(path, runtimeEnabled);
}

void Workbench::openProjectDialog() {
    const auto path = QFileDialog::getExistingDirectory(this, u"打开观测项目目录"_s,
                                                       Transport::projectRoot() + u"/projects"_s);
    if (!path.isEmpty())
        openProjectAt(path);
}

bool Workbench::openProjectAt(const QString &directory, bool load) {
    // 项目锁防止两个客户端同时覆盖会话和存储索引。
    if (active || loading || editing) {
        showError(u"请先停止当前任务，再切换项目"_s);
        return false;
    }
    const auto path = QDir(directory).canonicalPath();
    QString error;
    const auto data = readJson(path + u"/project.json"_s, &error);
    if (path.isEmpty() || data.value(u"schema_version"_s).toInt() != 1) {
        showError(error.isEmpty() ? u"该目录不是有效的观测项目"_s : error);
        return false;
    }
    QJsonObject savedState;
    const auto savedId = data.value(u"current_session"_s).toString();
    if (validId(savedId)) {
        // 先读完并校验现场，再切换项目；损坏文件不能被空现场自动覆盖。
        const auto statePath = path + u"/sessions/"_s + savedId + u"/state.json"_s;
        if (!QFileInfo(statePath).canonicalFilePath().startsWith(path + u"/sessions/"_s, Qt::CaseInsensitive)) {
            showError(u"会话现场文件缺失或路径越界，未覆盖已有项目"_s);
            return false;
        }
        savedState = readJson(statePath, &error);
        if (!error.isEmpty()) {
            showError(error);
            return false;
        }
    }
    if (!projectDirectory.isEmpty() && path != projectDirectory && !writeProjectState(false))
        return false;
    if (path != projectDirectory) {
        auto *lock = new QLockFile(path + u"/.project.lock"_s);
        if (!lock->tryLock(0)) {
            delete lock;
            showError(u"项目正在另一个客户端中打开，请先关闭另一个窗口"_s);
            return false;
        }
        delete projectLock;
        projectLock = lock;
    }
    projectDirectory = path;
    sessionStates.clear(); viewedSessionId.clear(); viewedSession = {};
    project = data;
    temporaryRules = {};
    rulesModelKey.clear();
    if (!load)
        loaded = false;
    projectLabel->setText(u"│　"_s + project.value(u"name"_s).toString());
    setWindowTitle(project.value(u"name"_s).toString() + u" — 析元"_s);
    modelPath = project.value(u"model_path"_s).toString();
    const auto rawConfig = readJson(modelPath + u"/config.json"_s);
    const auto modelConfig = rawConfig.contains(u"text_config"_s) ? rawConfig.value(u"text_config"_s).toObject() : rawConfig;
    maximum->setMaximum(std::max(1, modelConfig.value(u"max_position_embeddings"_s).toInt(262144) - 1));
    maximum->setValue(project.value(u"max_tokens"_s).toInt(maximum->maximum()));
    temperature->setValue(project.value(u"temperature"_s).toDouble(.7));
    thinkingTemperature->setValue(project.value(u"thinking_temperature"_s).toDouble(1.0));
    worker.setStorageRoot(path);
    monitor.setStorageRoot(path);
    auto recent = settings.value(u"recent_projects"_s).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 12)
        recent.removeLast();
    if (!verifying)
        settings.setValue(u"recent_projects"_s, recent);
    shell->setCurrentIndex(1);
    sessionId = project.value(u"current_session"_s).toString();
    if (!validId(sessionId)) {
        sessionId.clear();
        project[u"current_session"_s] = QString{};
        pendingSession = {};
    } else {
        pendingSession = savedState;
    }
    refreshSessions();
    if (load) {
        ensureProcesses();
        loadModel(modelPath, project.value(u"device"_s).toString(u"cuda:0"_s));
    } else {
        restoreSession(pendingSession);
        pendingSession = {};
        writeProjectState(false);
    }
    // 成功进入项目后再显示对话，避免启动页被无项目的窗口遮挡。
    if(conversationEmbedded) placeConversation(); else showConversation();
    return true;
}

QString Workbench::sessionDirectory() const {
    return projectDirectory + u"/sessions/"_s + sessionId;
}

void Workbench::rememberProjectRules() {
    // 按真实模型指纹保存项目共享规则；空规则也落盘，防止旧会话将其复活。
    if (projectDirectory.isEmpty())
        return;
    const auto key = loaded ? info.value(u"model_revision"_s).toString() : u"path:"_s + QDir(modelPath).absolutePath();
    if (key.isEmpty())
        return;
    auto edits = project.value(u"temporary_edits"_s).toObject();
    edits[key] = QJsonObject{{u"rules"_s, temporaryRules}, {u"weights_revision"_s, weightsRevision},
                            {u"model_path"_s, modelPath}};
    project[u"temporary_edits"_s] = edits;
    rulesModelKey = key;
}

void Workbench::restoreProjectRules(const QJsonObject &legacy) {
    // 会话不能覆盖项目已应用的规则；旧格式只在模型身份一致时迁移一次。
    const auto key = loaded ? info.value(u"model_revision"_s).toString() : u"path:"_s + QDir(modelPath).absolutePath();
    const auto edits = project.value(u"temporary_edits"_s).toObject();
    const auto saved = edits.value(key).toObject();
    temporaryRules = saved.value(u"rules"_s).toArray();
    weightsRevision = saved.value(u"weights_revision"_s).toString(u"w0"_s);
    if (!edits.contains(key) && !legacy.isEmpty() &&
        ((!runtimeEnabled && legacy.value(u"model_path"_s).toString() == modelPath) ||
         (loaded && legacy.value(u"model_revision"_s).toString() == key))) {
        temporaryRules = legacy.value(u"temporary_rules"_s).toArray();
        weightsRevision = legacy.value(u"weights_revision"_s).toString(u"w0"_s);
    }
    rulesModelKey = key;
    rememberProjectRules();
}

QJsonObject Workbench::captureSession() const {
    // 保存小型现场元数据和已有记录引用，不把模型或完整张量再次复制一份。
    QJsonArray draftRules, frames;
    for (int i = 0; i < rules->rowCount(); ++i)
        draftRules.append(rules->item(i, 0)->data(Qt::UserRole).toJsonObject());
    for (const auto &frame : timeline->frames) {
        QJsonArray done;
        QJsonObject durations;
        for (const auto &key : frame.done)
            done.append(key);
        for (auto it = frame.durations.cbegin(); it != frame.durations.cend(); ++it)
            durations[it.key()] = it.value();
        frames.append(QJsonObject{{u"id"_s, frame.id}, {u"branch"_s, frame.branch}, {u"token"_s, frame.token},
            {u"position"_s, frame.position}, {u"sampled_position"_s, frame.sampledPosition},
            {u"done"_s, done}, {u"durations"_s, durations}});
    }
    QJsonObject p, texts, layout;
    for (auto it = previews.cbegin(); it != previews.cend(); ++it)
        p[it.key()] = it.value();
    for (auto it = branchTexts.cbegin(); it != branchTexts.cend(); ++it)
        texts[it.key()] = it.value();
    for (auto pair : {qMakePair(mainSplit, u"main_v2"_s), qMakePair(leftSplit, u"left"_s),
                      qMakePair(navSplit, u"nav"_s)})
        layout[pair.second] = QString::fromLatin1(pair.first->saveState().toBase64());
    if (!observationSplitState.isEmpty()) layout[u"left"_s] = QString::fromLatin1(observationSplitState.toBase64());
    auto relativeTrace = tracePath.isEmpty() ? QString{} : QDir(projectDirectory).relativeFilePath(tracePath);
    if (relativeTrace.startsWith(u".."_s))
        relativeTrace.clear();
    auto relativeCheckpoint = checkpointPath.isEmpty() ? QString{} : QDir(projectDirectory).relativeFilePath(checkpointPath);
    QJsonArray collapsed;
    for (int index : collapsedThoughts)
        collapsed.append(index);
    QJsonObject notices;
    for (auto it = captureNotices.cbegin(); it != captureNotices.cend(); ++it)
        notices[it.key()] = it.value();
    return {{u"assistant_thought"_s, assistantThought}, {u"thinking_mode"_s, thinkingMode->isChecked()},
        {u"response_thinking"_s, responseThinking}, {u"thinking_complete"_s, responseThinkingComplete},
        {u"collapsed_thoughts"_s, collapsed},
        {u"response_tokens"_s, responseTokens}, {u"response_generation"_s, responseGeneration},
        {u"response_reply_id"_s, responseReplyId}, {u"capture_notices"_s, notices},
        {u"conversation_geometry"_s, QString::fromLatin1(conversationWindow->saveGeometry().toBase64())},
        {u"messages"_s, messages}, {u"assistant_text"_s, assistantText}, {u"input_draft"_s, viewedSessionId.isEmpty() ? prompt->text() : runningInputDraft},
        {u"model_path"_s, modelPath}, {u"device"_s, project.value(u"device"_s)},
        {u"temporary_rules"_s, temporaryRules}, {u"draft_rules"_s, draftRules}, {u"edit_target"_s, editTarget->currentIndex()},
        {u"editor"_s, QJsonObject{{u"layer_index_base"_s, 0}, {u"layer"_s, ruleLayer->value()}, {u"neuron"_s, ruleNeuron->value()},
            {u"kind"_s, ruleKind->currentData().toString()}, {u"scope"_s, ruleScope->currentData().toString()},
            {u"projection"_s, projection->currentData().toString()}, {u"position"_s, rulePosition->value()},
            {u"dimension"_s, ruleDimension->value()}, {u"value"_s, ruleValue->value()}}},
        {u"weights_revision"_s, weightsRevision}, {u"model_revision"_s, info.value(u"model_revision"_s)},
        {u"selection"_s, selection()}, {u"tokens"_s, tokens}, {u"forwards"_s, forwards}, {u"frames"_s, frames},
        {u"snapshot"_s, snapshot}, {u"previews"_s, p}, {u"branch_texts"_s, texts},
        {u"trace"_s, relativeTrace}, {u"checkpoint"_s, relativeCheckpoint}, {u"mode"_s, mode},
        {u"inspector"_s, inspector->currentIndex()}, {u"center_view"_s, centerViews->currentIndex()},
        {u"feature_page"_s, features->currentPage()},
        {u"navigator_tab"_s, navigatorTabs->currentIndex()}, {u"timeline_tab"_s, timelineTabs->currentIndex()},
        {u"view_type"_s, viewButtons[2]->isChecked() ? 2 : viewButtons[1]->isChecked() ? 1 : 0},
        {u"search"_s, search->text()}, {u"tensor_type"_s, tensorSelect->currentData().toString()}, {u"lens"_s, lens->isChecked()},
        {u"follow"_s, follow->isChecked()}, {u"layout"_s, layout}, {u"ui_scale"_s, uiScale->factor()},
        {u"graph_scale"_s, graph->transform().m11() / uiScale->factor()},
        {u"graph_center_x"_s, graph->mapToScene(graph->viewport()->rect().center()).x()},
        {u"graph_center_y"_s, graph->mapToScene(graph->viewport()->rect().center()).y()},
        {u"chat_scroll"_s, viewedSessionId.isEmpty() ? chat->verticalScrollBar()->value() : runningChatScroll}, {u"last_backup"_s, lastBackup}};
}

bool Workbench::writeProjectState(bool pin) {
    if (projectDirectory.isEmpty())
        return true;
    project[u"max_tokens"_s] = maximum->value();
    project[u"temperature"_s] = temperature->value();
    project[u"thinking_temperature"_s] = thinkingTemperature->value();
    project[u"model_path"_s] = modelPath;
    project[u"current_session"_s] = sessionId;
    project[u"updated_at"_s] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    rememberProjectRules();
    const auto state = captureSession();
    auto workspaces = project.value(u"rule_workspaces"_s).toObject();
    workspaces[QDir(modelPath).absolutePath()] = QJsonObject{{u"draft"_s, ruleDraft()}, {u"applied"_s, appliedRuleDraft}};
    project[u"rule_workspaces"_s] = workspaces;
    const qint64 extra = QJsonDocument(state).toJson(QJsonDocument::Compact).size() +
                         QJsonDocument(project).toJson(QJsonDocument::Compact).size();
    const auto storage = project.value(u"storage"_s).toObject();
    const qint64 reserve = qint64(storage.value(u"reserve_mib"_s).toInt(1024)) * 1024 * 1024;
    if (QStorageInfo(projectDirectory).bytesAvailable() < reserve + extra) {
        showError(u"磁盘保留空间不足，无法安全保存现场。请清理观测记录或释放磁盘空间。"_s);
        return false;
    }
    QString error;
    if ((!sessionId.isEmpty() && !writeJson(sessionDirectory() + u"/state.json"_s, state, &error)) ||
        !writeJson(projectDirectory + u"/project.json"_s, project, &error)) {
        showError(u"保存项目失败，上次有效现场仍保留："_s + error);
        return false;
    }
    if (!sessionId.isEmpty()) {
        sessionStates[sessionId] = state;
        // 仅保留少量最近现场，避免历史会话数量增加时占满内存。
        while (sessionStates.size() > 8) sessionStates.erase(sessionStates.begin());
    }
    if (pin && !tracePath.isEmpty() && QFile::exists(tracePath + u"/manifest.json"_s)) {
        auto manifest = readJson(tracePath + u"/manifest.json"_s);
        manifest[u"pinned"_s] = true;
        if (!writeJson(tracePath + u"/manifest.json"_s, manifest, &error)) {
            showError(u"保留观测记录失败："_s + error);
            return false;
        }
    }
    return true;
}

void Workbench::saveProject(bool manual) {
    // 活跃计算先暂停到稳定边界，收到恢复点确认后再提交前端现场。
    if (projectDirectory.isEmpty()) {
        if (manual)
            showError(u"请先创建或打开项目"_s);
        return;
    }
    if (loading || editing) {
        if (manual)
            showError(u"模型加载或参数保存尚未结束，请完成后保存项目"_s);
        return;
    }
    if (active) {
        if (!manual)
            return;
        checkpointPath = sessionDirectory() + u"/checkpoint.json"_s;
        worker.send(u"run.checkpoint"_s, {{u"project_root"_s, projectDirectory}, {u"path"_s, checkpointPath}});
        status(u"正在暂停到安全边界并保存现场"_s);
        return;
    }
    if (writeProjectState(manual))
        status(manual ? u"项目现场已保存，当前观测记录已保留"_s : u"项目已自动保存"_s);
}

void Workbench::restoreSession(const QJsonObject &savedState, bool activateRuntime) {
    // 切换会话立即清除活动，并拒收切换前尚未返回的神经元查询。
    sessionActivationId.clear();
    sessionActivationPending = false;
    sessionActivationRetries = 0;
    neuronQueryId.clear();
    // 切换前发出的视图查询不能阻塞或覆盖目标会话的首次查询。
    queryId.clear(); sliceId.clear(); scalarId.clear();
    if(neurons)neurons->clearActivity();
    const bool wasNeuronPage=workspaceViews&&workspaceViews->currentWidget()==neurons;
    // 界面属于应用：旧会话的布局和窗口坐标不得覆盖用户当前工作位置。
    QJsonObject state = savedState;
    const auto currentUi = captureSession();
    for (const auto &key : {u"inspector"_s, u"center_view"_s, u"feature_page"_s,
            u"navigator_tab"_s, u"timeline_tab"_s, u"view_type"_s, u"search"_s,
            u"tensor_type"_s, u"lens"_s, u"follow"_s, u"layout"_s, u"ui_scale"_s,
            u"graph_scale"_s, u"graph_center_x"_s, u"graph_center_y"_s})
        state[key] = currentUi.value(key);
    restoringSession = true;
    messages = state.value(u"messages"_s).toArray();
    assistantText = state.value(u"assistant_text"_s).toString();
    responseTokens = state.value(u"response_tokens"_s).toArray();
    responseGeneration = state.value(u"response_generation"_s).toObject();
    responseReplyId = state.value(u"response_reply_id"_s).toString();
    clearCaptureNotices();
    const auto notices = state.value(u"capture_notices"_s).toObject();
    for (auto it = notices.begin(); it != notices.end(); ++it)
        showCaptureNotice(it.key(), it.value().toString());
    restoreProjectRules(state);
    assistantThought = state.value(u"assistant_thought"_s).toString();
    responseThinking = state.value(u"response_thinking"_s).toBool();
    responseThinkingComplete = state.value(u"thinking_complete"_s).toBool(true);
    collapsedThoughts.clear();
    for (const auto &index : state.value(u"collapsed_thoughts"_s).toArray())
        collapsedThoughts.insert(index.toInt());
    thinkingMode->setChecked(thinkingMode->isEnabled() && state.value(u"thinking_mode"_s).toBool());
    prompt->setText(state.value(u"input_draft"_s).toString());
    tracePath = state.value(u"trace"_s).toString();
    if (!tracePath.isEmpty())
        tracePath = QDir(projectDirectory).absoluteFilePath(tracePath);
    checkpointPath = state.value(u"checkpoint"_s).toString();
    if (!checkpointPath.isEmpty())
        checkpointPath = QDir(projectDirectory).absoluteFilePath(checkpointPath);
    // 只接受本项目内的相对引用，移动项目后自动按新位置恢复。
    if (!tracePath.isEmpty() && (!QDir(tracePath).canonicalPath().startsWith(projectDirectory + u"/sessions/"_s, Qt::CaseInsensitive) || !QFile::exists(tracePath + u"/manifest.json"_s))) {
        tracePath.clear();
        showCaptureNotice(u"record"_s, u"该观测缓存已清理，会话和临时规则仍保留"_s);
    }
    if (!checkpointPath.isEmpty() && (!QFileInfo(checkpointPath).canonicalFilePath().startsWith(projectDirectory + u"/sessions/"_s, Qt::CaseInsensitive) || !QFile::exists(checkpointPath)))
        checkpointPath.clear();
    tokens = state.value(u"tokens"_s).toArray();
    forwards = state.value(u"forwards"_s).toArray();
    previews.clear();
    previewOrder.clear();
    branchTexts.clear();
    const auto savedPreviews = state.value(u"previews"_s).toObject();
    for (const auto &key : savedPreviews.keys())
        rememberPreview(key, savedPreviews.value(key).toObject());
    const auto savedTexts = state.value(u"branch_texts"_s).toObject();
    for (const auto &key : savedTexts.keys())
        branchTexts[key] = savedTexts.value(key).toString();
    const auto chosen = state.value(u"selection"_s).toObject();
    branch = chosen.value(u"branch_id"_s).toString(u"base"_s);
    forward = chosen.value(u"forward_id"_s).toString(u"f0"_s);
    layer = chosen.value(u"layer_index"_s).toInt();
    operation = chosen.value(u"operation"_s).toString(u"norm"_s);
    timeline->clear();
    for (auto value : state.value(u"frames"_s).toArray()) {
        const auto entry = value.toObject();
        TimelineFrame frame;
        frame.id = entry.value(u"id"_s).toString();
        frame.branch = entry.value(u"branch"_s).toString();
        frame.token = entry.value(u"token"_s).toString();
        frame.position = entry.value(u"position"_s).toInt();
        frame.sampledPosition = entry.value(u"sampled_position"_s).toInt(-1);
        for (auto key : entry.value(u"done"_s).toArray())
            frame.done.insert(key.toString());
        const auto durations = entry.value(u"durations"_s).toObject();
        for (auto it = durations.begin(); it != durations.end(); ++it)
            frame.durations[it.key()] = it.value().toDouble();
        timeline->frames.append(frame);
    }
    {
        QSignalBlocker b(branchSelect), f(forwardSelect);
        branchSelect->clear();
        history->clear();
        auto branches = branchTexts.keys();
        if (!branches.contains(branch))
            branches.prepend(branch);
        for (const auto &id : branches) {
            const auto name = id == u"base"_s ? u"原始分支"_s : u"编辑分支 "_s + id.right(8);
            branchSelect->addItem(name, id);
            history->addItem(name);
            history->item(history->count() - 1)->setData(Qt::UserRole, id);
        }
        branchSelect->setCurrentIndex(branchSelect->findData(branch));
        forwardSelect->clear();
        for (const auto &frame : timeline->frames)
            if (frame.branch == branch)
                forwardSelect->addItem(frame.id == u"f0"_s ? u"输入预填充"_s : u"缓存解码 "_s + frame.id.mid(1), frame.id);
        if (forwardSelect->findData(forward) < 0)
            forwardSelect->addItem(u"输入预填充"_s, forward);
        forwardSelect->setCurrentIndex(forwardSelect->findData(forward));
    }
    timeline->setBranch(branch);
    timeline->selectFrame(forward);
    follow->setChecked(false);
    choose(layer, operation, false);
    tokenPosition->setValue(chosen.value(u"token_position"_s).toInt());
    neuron->setValue(chosen.value(u"neuron_index"_s).toInt());
    dimension->setValue(chosen.value(u"dimension"_s).toInt());
    headSelect->setCurrentIndex(std::max(0, headSelect->findData(chosen.value(u"head"_s).toInt())));
    // 草稿和生效快照属于项目与模型，不随会话切换。
    const auto workspace = project.value(u"rule_workspaces"_s).toObject().value(QDir(modelPath).absolutePath()).toObject();
    if (!workspace.isEmpty()) {
        appliedRuleDraft = workspace.value(u"applied"_s).toArray();
        loadRuleDraft(workspace.value(u"draft"_s).toArray());
    } else if (rules->rowCount() == 0) {
        QJsonArray legacy;
        for (const auto &value : state.value(u"draft_rules"_s).toArray()) {
            auto rule = value.toObject();
            rule[u"target"_s] = state.value(u"edit_target"_s).toInt() == 1 ? u"model"_s : u"temporary"_s;
            rule[u"enabled"_s] = true;
            legacy.append(rule);
        }
        loadRuleDraft(legacy);
    }
    snapshot = state.value(u"snapshot"_s).toObject();
    if (!snapshot.isEmpty())
        renderSnapshot(snapshot);
    setMode(state.value(u"mode"_s).toString(u"normal"_s));
    centerViews->setCurrentIndex(state.value(u"center_view"_s).toInt());
    navigatorTabs->setCurrentIndex(state.value(u"navigator_tab"_s).toInt());
    timelineTabs->setCurrentIndex(state.value(u"timeline_tab"_s).toInt());
    const int viewType = state.value(u"view_type"_s).toInt();
    for (int i = 0; i < viewButtons.size(); ++i)
        viewButtons[i]->setChecked(i == viewType);
    search->setText(state.value(u"search"_s).toString());
    tensorSelect->setCurrentIndex(std::max(0, tensorSelect->findData(state.value(u"tensor_type"_s).toString())));
    lens->setChecked(state.value(u"lens"_s).toBool());
    inspector->setCurrentIndex(state.value(u"inspector"_s).toInt());
    lastBackup = state.value(u"last_backup"_s).toString();
    // 倍率属于应用且已保留；避免相同倍率触发整棵控件树重排和设置落盘。
    const auto layout = state.value(u"layout"_s).toObject();
    for (auto pair : {qMakePair(mainSplit, u"main_v2"_s), qMakePair(leftSplit, u"left"_s),
                      qMakePair(navSplit, u"nav"_s)})
        if (layout.contains(pair.second))
            pair.first->restoreState(QByteArray::fromBase64(layout.value(pair.second).toString().toLatin1()));
    const auto factor = state.value(u"graph_scale"_s).toDouble(1) * uiScale->factor();
    graph->setTransform(QTransform::fromScale(factor, factor));
    graph->centerOn(state.value(u"graph_center_x"_s).toDouble(540), state.value(u"graph_center_y"_s).toDouble(260));
    follow->setChecked(state.value(u"follow"_s).toBool(false));
    resumeSaved->setVisible(!checkpointPath.isEmpty());
    renderChat();
    static_cast<ChatTokenView *>(chat)->setReadingPosition(state.value(u"chat_scroll"_s).toInt());
    boundary = {};
    setActive(false);
    restoringSession = false;
    const auto splitState = observationSplitState;
    const int savedPage = state.value(u"center_view"_s).toInt();
    if (savedPage == 2)
        openFeature(std::clamp(state.value(u"feature_page"_s).toInt(), 0, 3), false);
    else if (savedPage == 3) showRulesPage();
    else if (savedPage == 4) showHistoryPage();
    else if (savedPage == 5) openWeightFile();
    else
        changeView(viewType);
    timelineTabs->setCurrentIndex(state.value(u"timeline_tab"_s).toInt());
    inspector->setCurrentIndex(state.value(u"inspector"_s).toInt());
    for (auto pair : {qMakePair(mainSplit, u"main_v2"_s), qMakePair(leftSplit, u"left"_s), qMakePair(navSplit, u"nav"_s)})
        pair.first->restoreState(QByteArray::fromBase64(currentUi.value(u"layout"_s).toObject().value(pair.second).toString().toLatin1()));
    observationSplitState = splitState;
    if(wasNeuronPage)showNeurons();
    if (loaded && processesStarted && activateRuntime) {
        sessionActivationPending = true;
        sessionActivationId = worker.send(u"session.activate"_s, {{u"temporary_rules"_s, temporaryRules},
            {u"weights_revision"_s, weightsRevision}, {u"model_revision"_s, info.value(u"model_revision"_s)},
            {u"trace_path"_s, tracePath}});
        sessionActivationRequests.insert(sessionActivationId);
        setActive(active);
    }
}

void Workbench::refreshSessions() {
    // 原位更新条目，鼠标按下到释放期间不销毁条目，也不重置列表滚动位置。
    QSignalBlocker block(sessionList);
    const int scroll = sessionList->verticalScrollBar()->value();
    const auto entries = project.value(u"sessions"_s).toArray();
    QListWidgetItem *selected = nullptr;
    const auto selectedId = viewedSessionId.isEmpty() ? sessionId : viewedSessionId;
    for (int row = 0; row < entries.size(); ++row) {
        const auto value = entries[row];
        auto entry = value.toObject();
        const auto id = entry.value(u"id"_s).toString();
        auto *item = sessionList->item(row);
        if (!item || item->data(Qt::UserRole).toString() != id) {
            int found = row + 1;
            while (found < sessionList->count() && sessionList->item(found)->data(Qt::UserRole).toString() != id) ++found;
            item = found < sessionList->count() ? sessionList->takeItem(found) : new QListWidgetItem;
            sessionList->insertItem(row, item);
        }
        item->setText(entry.value(u"title"_s).toString());
        item->setData(Qt::UserRole, entry.value(u"id"_s).toString());
        item->setToolTip(entry.value(u"title"_s).toString());
        if (id == selectedId) selected = item;
    }
    while (sessionList->count() > entries.size()) delete sessionList->takeItem(sessionList->count() - 1);
    sessionList->setCurrentItem(selected);
    sessionList->verticalScrollBar()->setValue(scroll);
    if(sessionPicker){
        QSignalBlocker pickerBlock(sessionPicker);
        sessionPicker->clear();
        for(const auto &entry:entries){const auto item=entry.toObject();sessionPicker->addItem(item.value(u"title"_s).toString(),item.value(u"id"_s).toString());}
        sessionPicker->setCurrentIndex(sessionPicker->findData(selectedId));
        if(chatSessionStatus)chatSessionStatus->setText(sessionPicker->currentIndex() < 0 ? u"尚未选择会话"_s :
            u"当前会话："_s+sessionPicker->currentText()+u" · Enter 发送 · Ctrl+Enter 换行"_s);
    }
    setActive(active);
}

void Workbench::renameSession(const QString &id) {
    // 以右键命中的编号重命名，不切换当前会话，也不重建工作区。
    const auto entries = project.value(u"sessions"_s).toArray();
    QString title;
    for (const auto &value : entries)
        if (value.toObject().value(u"id"_s).toString() == id) title = value.toObject().value(u"title"_s).toString();
    if (title.isEmpty()) return;
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"修改会话标题"_s);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(u"会话标题"_s));
    auto *name = new QLineEdit(title);
    layout->addWidget(name);
    auto *error = new QLabel;
    error->setStyleSheet(u"color:#edb1a5"_s);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox;
    auto *save = buttons->addButton(u"保存"_s, QDialogButtonBox::AcceptRole);
    buttons->addButton(u"取消"_s, QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, dialog, [this, id, name, error, dialog] {
        if (name->text().trimmed().isEmpty()) { error->setText(u"会话标题不能为空"_s); name->setFocus(); return; }
        const auto previous = project;
        auto changed = project.value(u"sessions"_s).toArray();
        for (int i = 0; i < changed.size(); ++i) {
            auto entry = changed[i].toObject();
            if (entry.value(u"id"_s).toString() == id) { entry[u"title"_s] = name->text().trimmed(); changed[i] = entry; }
        }
        project[u"sessions"_s] = changed;
        if (!writeProjectState(false)) { project = previous; error->setText(u"保存失败，请重试"_s); return; }
        refreshSessions();
        dialog->accept();
    });
    layout->addWidget(buttons);
    name->selectAll();
    dialog->open();
}

void Workbench::activateSession(const QString &id) {
    if (!validId(id))
        return;
    if (loading || editing) {
        showError(u"请先停止当前任务，再切换会话"_s);
        refreshSessions();
        return;
    }
    if (id == sessionId) {
        if (!viewedSessionId.isEmpty()) {
            viewedSessionId.clear(); viewedSession = {};
            prompt->setText(runningInputDraft);
            renderChat();
            static_cast<ChatTokenView *>(chat)->setReadingPosition(runningChatScroll);
            refreshSessions();
        }
        return;
    }
    QString error;
    const auto state = sessionStates.contains(id) ? sessionStates.value(id) :
        readJson(projectDirectory + u"/sessions/"_s + id + u"/state.json"_s, &error);
    if (!error.isEmpty()) {
        showError(error);
        refreshSessions();
        return;
    }
    if (active) {
        // 推理运行器和主工作区继续追踪原任务；历史 Token 分析使用各自保存的记录路径。
        if (viewedSessionId.isEmpty()) {
            runningInputDraft = prompt->text();
            runningChatScroll = chat->verticalScrollBar()->value();
        }
        viewedSessionId = id; viewedSession = state; viewedCollapsedThoughts.clear();
        for (const auto &index : state.value(u"collapsed_thoughts"_s).toArray()) viewedCollapsedThoughts.insert(index.toInt());
        prompt->setText(state.value(u"input_draft"_s).toString());
        renderChat();
        static_cast<ChatTokenView *>(chat)->setReadingPosition(state.value(u"chat_scroll"_s).toInt());
        refreshSessions();
        return;
    }
    if (!writeProjectState(false)) { refreshSessions(); return; }
    if (!viewedSessionId.isEmpty()) prompt->setText(runningInputDraft);
    viewedSessionId.clear(); viewedSession = {};
    sessionId = id;
    const auto savedModel = state.value(u"model_path"_s).toString(modelPath);
    if (runtimeEnabled && QDir(savedModel).canonicalPath() != QDir(modelPath).canonicalPath()) {
        pendingSession = state;
        loadModel(savedModel, state.value(u"device"_s).toString(u"cuda:0"_s));
        sessionActivationId = actionId;
    } else {
        restoreSession(state);
    }
    // 目标会话没有变化，仅原子更新项目选择，避免再次遍历磁盘并重写整份现场。
    project[u"current_session"_s] = sessionId;
    if (!writeJson(projectDirectory + u"/project.json"_s, project, &error)) showError(u"保存会话选择失败："_s + error);
    refreshSessions();
}

void Workbench::newSession(bool activateRuntime) {
    // 新会话始终以对话模式创建，不继承上一会话的观测或调试模式。
    if (projectDirectory.isEmpty() || active || loading || editing) {
        showError(u"请先打开项目并停止当前任务，再新建会话"_s);
        return;
    }
    if (!writeProjectState(false))
        return;
    sessionId = QUuid::createUuid().toString(QUuid::Id128);
    auto entries = project.value(u"sessions"_s).toArray();
    entries.append(QJsonObject{{u"id"_s, sessionId}, {u"title"_s, u"会话 "_s + QString::number(entries.size() + 1)}});
    project[u"sessions"_s] = entries;
    restoreSession({{u"mode"_s, u"normal"_s}}, activateRuntime);
    refreshSessions();
    writeProjectState(false);
}

void Workbench::clearSession(bool remove) {
    // 删除范围固定为当前会话目录；模型、其他会话和旧版归档不在范围内。
    if (projectDirectory.isEmpty() || sessionId.isEmpty() || active || loading || editing) {
        showError(u"请先打开项目并停止当前任务，再清理会话"_s);
        return;
    }
    const auto confirmedRoot=projectDirectory, confirmedId=sessionId;
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(remove ? u"删除会话"_s : u"清空会话"_s);
    auto *layout = vertical(dialog, 22, 12);
    auto *description = new QLabel(remove ? u"删除当前会话及其上下文？项目已应用的临时神经元规则将保留。"_s : u"清空当前会话的消息和恢复点？项目已应用的临时神经元规则将保留。"_s);
    layout->addWidget(description);
    auto *data = new QCheckBox(u"同时删除此会话的观测记录，释放磁盘空间"_s);
    data->setChecked(true);
    layout->addWidget(data);
    auto *buttons = new QDialogButtonBox;
    auto *yes = buttons->addButton(u"确认"_s, QDialogButtonBox::AcceptRole);
    auto *no = buttons->addButton(u"取消"_s, QDialogButtonBox::RejectRole);
    connect(no, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(yes, &QPushButton::clicked, dialog, [=] {
        // 非模态确认期间若会话或运行状态变化，必须重新确认，不能删除后来切换的会话。
        if(projectDirectory!=confirmedRoot||sessionId!=confirmedId||active||loading||editing){
            showError(u"项目、会话或任务状态已变化，请重新确认删除"_s);dialog->reject();return;
        }
        const auto oldId = confirmedId;
        const auto path = sessionDirectory();
        if (!validId(oldId) || !QDir(path).canonicalPath().startsWith(QDir(projectDirectory).canonicalPath() + u"/sessions/"_s)) {
            showError(u"会话路径校验失败，未执行删除"_s);
            return;
        }
        // 先切离旧索引，Windows 才能安全删除已关闭的数据库文件。
        pendingSessionId = remove ? oldId : QString{};
        const auto deleteRecords = data->isChecked();
        if (processesStarted) {
            pendingSession = {{u"cleanup_path"_s, path}, {u"remove_session"_s, remove}, {u"delete_records"_s, deleteRecords}};
            worker.send(u"trace.close"_s);
        } else {
            // 与真实运行器使用同一收尾路径，确保删除会话时更新项目目录索引。
            pendingSession = {{u"cleanup_path"_s, path}, {u"remove_session"_s, remove}, {u"delete_records"_s, deleteRecords}};
            receive({{u"name"_s, u"trace.closed"_s}});
        }
        dialog->accept();
    });
    layout->addWidget(buttons);
    dialog->open();
}

void Workbench::deleteAllSessions() {
    // 确认范围只包含当前项目登记的会话；运行期间不允许删除生成现场。
    if (projectDirectory.isEmpty() || active || loading || editing || project.value(u"sessions"_s).toArray().isEmpty()) return;
    const auto root = projectDirectory;
    const auto entries = project.value(u"sessions"_s).toArray();
    auto *dialog = new QDialog(conversationWindow);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"删除所有会话"_s);
    auto *layout = vertical(dialog, 22, 12);
    auto *description = new QLabel(u"确认删除当前项目的全部 "_s + QString::number(entries.size()) +
        u" 个会话及其聊天、恢复点和观测记录？此操作无法撤销。模型、项目配置和已应用的干预规则将保留。"_s);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *buttons = new QDialogButtonBox;
    auto *confirm = buttons->addButton(u"确认删除"_s, QDialogButtonBox::DestructiveRole);
    auto *cancel = buttons->addButton(u"取消"_s, QDialogButtonBox::RejectRole);
    cancel->setDefault(true);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(confirm, &QPushButton::clicked, dialog, [this, root, entries, dialog] {
        if (projectDirectory != root || project.value(u"sessions"_s).toArray() != entries || active || loading || editing) {
            showError(u"项目、会话列表或任务状态已变化，请重新确认删除"_s); dialog->reject(); return;
        }
        const auto canonical = QDir(root).canonicalPath();
        // 删除前检查全部路径。链接目录及越界路径均不能进入批量删除流程。
        for (const auto &value : entries) {
            const auto id = value.toObject().value(u"id"_s).toString();
            const auto path = root + u"/sessions/"_s + id;
            if (!validId(id) || (QFileInfo::exists(path) &&
                QDir(path).canonicalPath().compare(canonical + u"/sessions/"_s + id, Qt::CaseInsensitive) != 0)) {
                showError(u"会话路径校验失败，未执行删除"_s); return;
            }
        }
        if (!writeProjectState(false)) return;
        deletingSessionIds.clear();
        for (const auto &value : entries) deletingSessionIds.append(value.toObject().value(u"id"_s).toString());
        deleteSessionsRetries = 0;
        editing = true; setActive(false);
        dialog->accept();
        if (processesStarted) deleteSessionsRequestId = worker.send(u"trace.close"_s);
        else finishDeleteAllSessions();
    });
    layout->addWidget(buttons);
    dialog->open();
}

void Workbench::finishDeleteAllSessions() {
    // 运行器关闭记录后再删目录，避免 Windows 数据库文件占用；失败项保留在列表。
    const auto root = QDir(projectDirectory).canonicalPath();
    QSet<QString> removed;
    QStringList failures;
    for (const auto &id : deletingSessionIds) {
        const auto path = projectDirectory + u"/sessions/"_s + id;
        if (!validId(id) || (QFileInfo::exists(path) &&
            QDir(path).canonicalPath().compare(root + u"/sessions/"_s + id, Qt::CaseInsensitive) != 0)) {
            failures.append(id); continue;
        }
        if (!QFileInfo::exists(path) || QDir(path).removeRecursively()) { removed.insert(id); sessionStates.remove(id); }
        else failures.append(id);
    }
    deletingSessionIds.clear();
    deleteSessionsRequestId.clear();
    QJsonArray remaining;
    for (const auto &value : project.value(u"sessions"_s).toArray())
        if (!removed.contains(value.toObject().value(u"id"_s).toString())) remaining.append(value);
    project[u"sessions"_s] = remaining;
    if (removed.contains(sessionId)) sessionId.clear();
    viewedSessionId.clear(); viewedSession = {};
    editing = false;
    restoreSession({});
    if (!remaining.isEmpty()) {
        // 先清空旧现场身份，不能将空白现场覆盖到未删成功的会话中。
        sessionId.clear();
        activateSession(remaining.first().toObject().value(u"id"_s).toString());
    }
    refreshSessions();
    writeProjectState(false);
    if (!failures.isEmpty()) showError(u"部分会话删除失败，已保留失败条目，请检查文件占用或磁盘权限"_s);
    else status(u"当前项目的全部会话及其观测记录已删除"_s);
}

void Workbench::showLauncher() {
    if (active || loading || editing) {
        showError(u"请先停止当前任务，再关闭项目"_s);
        return;
    }
    if (!writeProjectState(false))
        return;
    if (processesStarted)
        worker.send(u"model.unload"_s);
    loaded = false;
    projectDirectory.clear();
    project = {};
    delete projectLock;
    projectLock = nullptr;
    shell->setCurrentIndex(0);
    conversationWindow->hide();
    refreshRecentProjects();
    setWindowTitle(u"析元"_s);
}

void Workbench::resumeSavedRun() {
    if (!loaded || active || checkpointPath.isEmpty()) {
        showError(u"请先加载保存时的模型，才能恢复运行"_s);
        return;
    }
    resumeRebuilding = true;
    setActive(true);
    setMode(u"step"_s);
    actionId = worker.send(u"run.restore"_s, {{u"path"_s, checkpointPath}, {u"temporary_rules"_s, temporaryRules}});
    status(u"正在重建保存位置的计算缓存，完成后停在原运算边界"_s);
}

void Workbench::showStorage() {
    // 展示实际项目占用和可清理缓存，手动保留的现场不会被自动清理。
    if (projectDirectory.isEmpty()) {
        showError(u"请先打开项目"_s);
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"项目存储管理"_s);
    UiScale::minimumWidth(dialog, 740);
    auto *layout = vertical(dialog, 22, 12);
    auto *summary = new QLabel(u"项目目录："_s + projectDirectory + u"\n项目占用："_s + storageText(diskBytes(projectDirectory)) +
        u"　磁盘剩余："_s + storageText(QStorageInfo(projectDirectory).bytesAvailable()));
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *form = new QFormLayout;
    auto setting = [this](const QString &key, int fallback, int min, int max) {
        auto *field = new QSpinBox;
        field->setRange(min, max);
        field->setValue(project.value(u"storage"_s).toObject().value(key).toInt(fallback));
        field->setSuffix(u" MiB"_s);
        return field;
    };
    auto *reserve = setting(u"reserve_mib"_s, 1024, 256, 65536);
    auto *automatic = new QCheckBox(u"自动清理已结束且未手动保留的观测缓存"_s);
    automatic->setChecked(project.value(u"storage"_s).toObject().value(u"auto_clean"_s).toBool(true));
    auto *policy = new QComboBox;
    policy->addItem(u"按需采集：当前检查层与所有层输出"_s, u"standard"_s);
    policy->addItem(u"完整采集：所有层的运算输入与输出"_s, u"full"_s);
    policy->setCurrentIndex(std::max(0, policy->findData(project.value(u"storage"_s).toObject().value(u"policy"_s).toString(u"standard"_s))));
    form->addRow(u"磁盘至少保留"_s, reserve);
    form->addRow(u"观测方式"_s, policy);
    layout->addLayout(form);
    auto *capacityHint = new QLabel(u"张量采集不设单次、项目总量或单块大小限制。磁盘预留始终生效。"_s);
    capacityHint->setWordWrap(true);
    layout->addWidget(capacityHint);
    layout->addWidget(automatic);
    auto *list = new QTableWidget(0, 4);
    list->setHorizontalHeaderLabels({u"运行记录"_s, u"占用"_s, u"保留状态"_s, u"操作"_s});
    list->setMinimumHeight(200);
    QDirIterator manifests(projectDirectory + u"/sessions"_s, {u"manifest.json"_s}, QDir::Files | QDir::NoSymLinks,
                           QDirIterator::Subdirectories);
    while (manifests.hasNext()) {
        const auto file = manifests.next();
        const auto path = QFileInfo(file).absolutePath();
        const auto manifest = readJson(file);
        if (manifest.value(u"managed_by"_s).toString() != u"observatory-project"_s)
            continue;
        const int row = list->rowCount();
        list->insertRow(row);
        list->setItem(row, 0, new QTableWidgetItem(QFileInfo(path).fileName().left(12)));
        list->setItem(row, 1, new QTableWidgetItem(storageText(diskBytes(path))));
        list->setItem(row, 2, new QTableWidgetItem(manifest.value(u"pinned"_s).toBool() ? u"手动保留"_s : u"可清理缓存"_s));
        auto *remove = action(u"删除记录"_s, dialog, [this, dialog, path] {
            if (active || loading || editing || path == tracePath) {
                showError(u"请先切换到其他会话，再删除当前正在查看或使用的记录"_s);
                return;
            }
            const auto canonical = QDir(path).canonicalPath();
            if (!canonical.startsWith(QDir(projectDirectory).canonicalPath() + u"/sessions/"_s) || !validId(QFileInfo(path).fileName())) {
                showError(u"记录目录校验失败"_s);
                return;
            }
            auto *ask = new QMessageBox(QMessageBox::Question, u"删除观测记录"_s,
                u"删除此记录的张量和执行索引？会话消息与神经元规则保留。"_s, QMessageBox::NoButton, this);
            auto *yes = ask->addButton(u"删除"_s, QMessageBox::AcceptRole);
            ask->addButton(u"取消"_s, QMessageBox::RejectRole);
            ask->setAttribute(Qt::WA_DeleteOnClose);
            connect(ask, &QMessageBox::finished, this, [this, ask, yes, path, dialog](int) {
                if (ask->clickedButton() == yes) {
                    if (!QDir(path).removeRecursively())
                        showError(u"删除记录失败，请检查文件占用和权限"_s);
                    dialog->close();
                    showStorage();
                }
            });
            ask->open();
        });
        list->setCellWidget(row, 3, remove);
    }
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->resizeColumnsToContents();
    layout->addWidget(list);
    auto *hint = new QLabel(u"普通对话不保存中间张量。手动保存项目会保留当前记录；删除会话时可一并清理记录。旧版记录是独立归档，从启动页打开。"_s);
    hint->setWordWrap(true);
    hint->setObjectName(u"muted"_s);
    layout->addWidget(hint);
    layout->addWidget(action(u"保存存储设置"_s, dialog, [=] {
        reserve->interpretText();
        // 写回零额度，避免旧版本字段在项目恢复或其他入口中继续产生限制。
        project[u"storage"_s] = QJsonObject{{u"total_mib"_s, 0}, {u"run_mib"_s, 0},
            {u"reserve_mib"_s, reserve->value()}, {u"auto_clean"_s, automatic->isChecked()}, {u"policy"_s, policy->currentData().toString()}};
        if (writeProjectState(false)) {
            status(u"存储设置已保存，下次运行生效"_s);
            dialog->accept();
        }
    }));
    dialog->open();
}
