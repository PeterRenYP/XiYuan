#include "features.h"
#include "uiscale.h"
#include "visuals.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QTextCursor>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace Qt::StringLiterals;

namespace {
class ChannelTrend final : public QWidget {
  public:
    explicit ChannelTrend(const QJsonArray &points,QWidget *parent=nullptr):QWidget(parent),points(points){setMinimumHeight(150);}
  protected:
    void paintEvent(QPaintEvent *) override {
        // 原始采集位置作横轴，正负数值共用纵轴；不对未采集位置补零。
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area=rect().adjusted(60,26,-14,-28);
        double low=0,high=0,minPosition=0,maxPosition=1;
        if(!points.isEmpty()){minPosition=points.first().toObject().value(u"position"_s).toDouble();maxPosition=points.last().toObject().value(u"position"_s).toDouble();}
        for(const auto &value:points)for(const auto &key:{u"before"_s,u"after"_s}){const double n=value.toObject().value(key).toDouble();low=std::min(low,n);high=std::max(high,n);}
        const double span=std::max(1e-9,high-low),positions=std::max(1.,maxPosition-minPosition);
        painter.setPen(QColor(u"#607083"_s));painter.drawRect(area);
        painter.drawText(2,int(area.top()+10),QString::number(high,'g',4));painter.drawText(2,int(area.bottom()),QString::number(low,'g',4));
        painter.drawText(int(area.left()),height()-7,u"Token 位置 "_s+QString::number(minPosition,'f',0));
        painter.drawText(int(area.right()-50),height()-7,QString::number(maxPosition,'f',0));
        for(const auto &key:{u"before"_s,u"after"_s}){
            const QColor color(key==u"before"_s?u"#a9d8e8"_s:u"#e3b28b"_s);painter.setPen(QPen(color,1.5));QPainterPath path;bool first=true;double previous=-1;
            for(const auto &value:points){const auto row=value.toObject();const double position=row.value(u"position"_s).toDouble();
                const QPointF point(area.left()+(position-minPosition)/positions*area.width(),area.bottom()-(row.value(key).toDouble()-low)/span*area.height());
                if(first||position>previous+1)path.moveTo(point);else path.lineTo(point);first=false;previous=position;
            }painter.drawPath(path);painter.drawText(key==u"before"_s?60:150,17,key==u"before"_s?u"干预前"_s:u"干预后"_s);
        }
    }
  private:
    QJsonArray points;
};
// 新页面复用工作台样式，只统一紧凑表格与表单，不引入另一套主题。
QVBoxLayout *column(QWidget *widget) {
    // 创建沿用工作台间距的垂直布局。
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);
    return layout;
}
QLabel *hint(const QString &text) {
    // 次要说明统一使用工作台的弱化文字样式。
    auto *label = new QLabel(text);
    label->setObjectName(u"muted"_s);
    label->setWordWrap(true);
    return label;
}
QTableWidget *table(const QStringList &headers, bool editable = false) {
    // 创建可选择、可滚动的原生数据表格。
    auto *control = new QTableWidget(0, int(headers.size()));
    control->setHorizontalHeaderLabels(headers);
    control->verticalHeader()->hide();
    control->setSelectionBehavior(QAbstractItemView::SelectRows);
    control->setSelectionMode(QAbstractItemView::SingleSelection);
    control->setEditTriggers(editable ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed : QAbstractItemView::NoEditTriggers);
    control->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    control->horizontalHeader()->setStretchLastSection(true);
    control->setAlternatingRowColors(true);
    UiScale::minimumHeight(control, 90);
    return control;
}
void row(QTableWidget *control, const QStringList &values) {
    // 单元格保留完整值供复制，表格不会自动撑开工作区。
    const int index = control->rowCount();
    control->insertRow(index);
    for (int i = 0; i < values.size(); ++i) {
        auto *item = new QTableWidgetItem(values[i]);
        item->setToolTip(values[i]);
        control->setItem(index, i, item);
    }
}
QString number(const QJsonValue &value) {
    // 未评估的指标明确显示状态，不补成零。
    return value.isDouble() ? QString::number(value.toDouble(), 'g', 8) : u"未评估"_s;
}
QSpinBox *spin(int minimum, int maximum, int initial) {
    // 约束整数范围，避免编辑中途触发查询。
    auto *control = new QSpinBox;
    control->setRange(minimum, maximum);
    control->setValue(initial);
    control->setKeyboardTracking(false);
    return control;
}
QComboBox *choices(const QStringList &labels, const QStringList &keys) {
    // 中文选项与协议字段分别保存。
    auto *control = new QComboBox;
    for (int i = 0; i < labels.size(); ++i)
        control->addItem(labels[i], keys[i]);
    return control;
}
QPushButton *button(const QString &text, const std::function<void()> &action, QObject *owner) {
    // 连接原生按钮与当前页面操作。
    auto *control = new QPushButton(text);
    QObject::connect(control, &QPushButton::clicked, owner, action);
    return control;
}

}

FeaturePanel::FeaturePanel(QWidget *parent) : QWidget(parent) {
    // 四项新功能均在中心工作区显示，左侧窄栏负责切换。
    auto *layout = column(this);
    pages = new QStackedWidget;
    for (auto *page : {activationPage(), vectorsPage(), versionsPage(), regressionPage()}) {
        // 小窗口与高倍率时在功能页内滚动，不挤掉控件或改变原生工作区结构。
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        UiScale::minimumWidth(page, 400);
        scroll->setWidget(page);
        pages->addWidget(scroll);
    }
    layout->addWidget(pages, 1);
    // 进度区放在禁用的表单之外，任务运行时仍可读、可取消。
    analysisProgressBox = new QWidget;
    analysisProgressBox->setObjectName(u"activationProgressBox"_s);
    auto *progressLayout = new QVBoxLayout(analysisProgressBox);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(6);
    analysisProgressText = hint(QString{});
    analysisProgressText->setObjectName(u"activationProgressText"_s);
    analysisProgressText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    analysisProgress = new QProgressBar;
    analysisProgress->setObjectName(u"activationProgress"_s);
    analysisProgress->setStyleSheet(u"QProgressBar#activationProgress{min-height:22px;max-height:22px;border:1px solid #687e91;text-align:center;color:#e6f2fa;background:#18212b;} QProgressBar#activationProgress::chunk{background:#466f83;}"_s);
    analysisProgress->setAccessibleName(u"激活分析当前阶段进度"_s);
    analysisProgress->setFormat(u"本阶段 %p%"_s);
    progressLayout->addWidget(analysisProgressText);
    progressLayout->addWidget(analysisProgress);
    layout->addWidget(analysisProgressBox);
    analysisProgressBox->hide();
    analysisProgressTimer = new QTimer(this);
    analysisProgressTimer->setInterval(250);
    connect(analysisProgressTimer, &QTimer::timeout, this, &FeaturePanel::refreshProgressText);
    auto *footer = new QHBoxLayout;
    notice = hint(u"尚未加载模型"_s);
    notice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer->addWidget(notice, 1);
    cancelButton = button(u"停止任务"_s, [this] {
        // 取消为协作式操作，收到最终响应前保留耗时并防止重复提交。
        analysisCancelling = true;
        cancelButton->setEnabled(false);
        notice->setText(u"正在停止，等待当前处理结束"_s);
        refreshProgressText();
        emit cancelled();
    }, this);
    cancelButton->hide();
    footer->addWidget(cancelButton);
    layout->addLayout(footer);
    applyPrototypeLayout();
}

QWidget *FeaturePanel::activationPage() {
    // 主界面只展示排名；范围、统计口径和干预位置可直接辨认。
    auto *page = new QWidget;
    auto *layout = column(page);
    auto *title = new QLabel(u"激活分析"_s);
    title->setObjectName(u"heading"_s);
    layout->addWidget(title);
    auto *filters = new QHBoxLayout;
    scope = choices({u"本轮输入"_s,u"模型回复"_s,u"全部上下文"_s,u"指定位置"_s}, {u"input"_s,u"reply"_s,u"all"_s,u"range"_s});
    scope->setToolTip(u"输入不含历史；回复只统计已实际送入模型的 Token"_s);
    metric = choices({u"层内归一化"_s,u"峰值"_s,u"平均强度"_s,u"激活频率"_s}, {u"normalized"_s,u"peak"_s,u"mean"_s,u"frequency"_s});
    metric->setToolTip(u"归一化：每个 Token 的绝对激活除以该层均方根，再取平均；频率阈值为 0.01"_s);
    source = choices({u"干预前"_s,u"干预后"_s}, {u"baseline"_s,u"output"_s});
    analysisLayer = spin(-1, 100000, -1);
    analysisLayer->setSpecialValueText(u"全部层"_s);
    filters->addWidget(scope);
    filters->addWidget(metric);
    filters->addWidget(source);
    filters->addWidget(new QLabel(u"层"_s));
    filters->addWidget(analysisLayer);
    filters->addStretch();
    filters->addWidget(button(u"分析"_s, [this] { analyzeActivation(); }, this));
    layout->addLayout(filters);
    auto *range = new QHBoxLayout;
    rangeStart = spin(0, 10000000, 0);
    rangeStop = spin(0, 10000000, 0);
    rangeStart->setEnabled(false);
    rangeStop->setEnabled(false);
    rangeStart->setToolTip(u"选择指定位置后可编辑"_s);
    rangeStop->setToolTip(u"选择指定位置后可编辑"_s);
    connect(scope,&QComboBox::currentIndexChanged,this,[this]{
        const bool enabled=scope->currentData().toString()==u"range"_s;
        rangeStart->setEnabled(enabled);rangeStop->setEnabled(enabled);
    });
    range->addWidget(new QLabel(u"Token 位置"_s));
    range->addWidget(rangeStart);
    range->addWidget(new QLabel(u"至"_s));
    range->addWidget(rangeStop);
    baselinePath = new QLineEdit;
    baselinePath->setPlaceholderText(u"基线记录（可选）"_s);
    range->addWidget(baselinePath, 1);
    range->addWidget(button(u"选择基线"_s, [this] {
        const auto path = QFileDialog::getExistingDirectory(this,u"选择基线观测记录"_s,projectRoot);
        if (!path.isEmpty()) baselinePath->setText(path);
    }, this));
    layout->addLayout(range);
    activationHint = hint(u"尚无分析结果"_s);
    activationHint->setToolTip(u"采集点：前馈网络门控乘积、下投影之前。正负值不代表语义方向。摘要不能代替未采集的原始数据。"_s);
    layout->addWidget(activationHint);
    activationTable = table({u"层"_s,u"通道"_s,u"强度"_s,u"峰值（含符号）"_s,u"触发位置"_s,u"触发 Token"_s,u"有效数"_s,u"基线差值"_s});
    layout->addWidget(activationTable, 1);
    connect(activationTable,&QTableWidget::currentCellChanged,this,[this](int r,int,int,int){
        if(r<0||r>=activationRows.size())return;
        const auto item=activationRows[r].toObject();
        activationInspection = u"层内排名："_s+QString::number(item.value(u"layer_rank"_s).toInt())+
            u"\n正向峰值："_s+number(item.value(u"positive"_s))+u"\n负向峰值："_s+number(item.value(u"negative"_s))+
            u"\n平均绝对强度："_s+number(item.value(u"mean"_s))+u"\n激活频率："_s+number(item.value(u"frequency"_s))+
            u"\n阈值：0.01\n有效 Token："_s+QString::number(item.value(u"samples"_s).toInt())+
            u"\n基线有效 Token："_s+(item.contains(u"baseline_samples"_s)?QString::number(item.value(u"baseline_samples"_s).toInt()):u"未选择"_s)+
            u"\n基线强度差："_s+number(item.value(u"difference"_s))+u"\n基线频率差："_s+number(item.value(u"frequency_difference"_s))+
            u"\n\n归一化：每个 Token 的绝对值除以该层均方根，再取平均。\n正负值仅表示数值响应方向。"_s;
        emit inspected(activationInspection);
    });
    connect(activationTable, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
        if (r >= 0 && r < activationRows.size()) emit located(activationRows[r].toObject(), false);
    });
    auto *actions = new QHBoxLayout;
    actions->addWidget(button(u"定位"_s, [this] {
        const int r = activationTable->currentRow();
        if (r >= 0 && r < activationRows.size()) emit located(activationRows[r].toObject(), false);
        else failure(u"请先选择一条排名"_s);
    }, this));
    actions->addWidget(button(u"加入干预"_s, [this] {
        const int r = activationTable->currentRow();
        if (r >= 0 && r < activationRows.size()) emit located(activationRows[r].toObject(), true);
        else failure(u"请先选择一条排名"_s);
    }, this));
    actions->addWidget(button(u"查看数值"_s, [this] {
        const int r=activationTable->currentRow();
        if(r<0||r>=activationRows.size()){failure(u"请先选择一条排名"_s);return;}
        const auto item=activationRows[r].toObject();
        QJsonObject query{{u"action"_s,u"activation.detail"_s},{u"layer"_s,item.value(u"layer"_s)},{u"neuron"_s,item.value(u"neuron"_s)}};
        // 数值详情继续使用产生该排名的记录，不能跟随当前会话跳到其他回复。
        for(const auto &key:{u"trace_path"_s,u"run_id"_s,u"positions"_s,u"branch_id"_s})if(activationReport.contains(key))query[key]=activationReport[key];
        query[u"client_revision"_s]=activationRevision;request(query);
    },this));
    actions->addWidget(button(u"监视"_s,[this]{const int r=activationTable->currentRow();if(r>=0&&r<activationRows.size())emit monitored(activationRows[r].toObject());else failure(u"请先选择一条排名"_s);},this));
    actions->addWidget(button(u"保存分析"_s, [this] {
        // 完整报告已在后端持久化；分块复制，避免只导出当前页或重复占用大量内存。
        const auto sourcePath = activationReport.value(u"path"_s).toString();
        if (sourcePath.isEmpty()) { failure(u"请先分析"_s); return; }
        const auto path = QFileDialog::getSaveFileName(this,u"保存分析"_s,projectRoot+u"/激活分析.json"_s,u"分析文件 (*.json)"_s);
        if (path.isEmpty()) return;
        QFile input(sourcePath); QSaveFile output(path);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) { failure(u"分析文件无法读写"_s); return; }
        while (!input.atEnd()) {
            const auto block=input.read(1024*1024);
            if (block.isEmpty() || output.write(block)!=block.size()) { output.cancelWriting(); failure(u"分析保存失败"_s); return; }
        }
        if (!output.commit()) failure(u"分析保存失败"_s); else notice->setText(u"完整分析已保存"_s);
    }, this));
    actions->addWidget(button(u"打开分析"_s, [this] {
        // 导入与分页均交给运行器，界面不设置报告总条数上限。
        const auto path = QFileDialog::getOpenFileName(this,u"打开分析"_s,projectRoot,u"分析文件 (*.json)"_s);
        if (path.isEmpty()) return;
        request({{u"action"_s,u"activation.open"_s},{u"path"_s,path},{u"page_size"_s,activationPageSize->currentData().toInt()},
            {u"client_revision"_s,++activationRevision}});
    }, this));
    actions->addStretch();
    layout->addLayout(actions);
    return page;
}

QWidget *FeaturePanel::vectorsPage() {
    // 文本搜索、编号定位和向量修改共用同一 token 选择。
    auto *page = new QWidget;
    auto *layout = column(page);
    auto *title = new QLabel(u"词表与向量"_s);
    title->setObjectName(u"heading"_s);
    layout->addWidget(title);
    auto *filters = new QHBoxLayout;
    tokenQuery = new QLineEdit;
    tokenQuery->setPlaceholderText(u"输入文本或 Token 编号"_s);
    tokenQuery->setAccessibleName(u"搜索词表"_s);
    exact = new QCheckBox(u"精确"_s);
    special = new QCheckBox(u"仅特殊 Token"_s);
    filters->addWidget(tokenQuery, 1);
    filters->addWidget(exact);
    filters->addWidget(special);
    filters->addWidget(button(u"搜索"_s,[this]{searchTokens();},this));
    connect(tokenQuery,&QLineEdit::returnPressed,this,[this]{searchTokens();});
    filters->addWidget(button(u"上一页"_s,[this]{searchTokens(std::max(0,tokenOffset-100));},this));
    filters->addWidget(button(u"下一页"_s,[this]{if(tokenOffset+100<tokenTotal) searchTokens(tokenOffset+100);},this));
    layout->addLayout(filters);
    auto *split = new QSplitter(Qt::Vertical);
    tokenTable = table({u"编号"_s,u"词表文本"_s,u"解码内容"_s,u"类型"_s});
    split->addWidget(tokenTable);
    connect(tokenTable,&QTableWidget::cellClicked,this,[this](int r,int){
        if (tokenTable->item(r,0)) { tokenId=tokenTable->item(r,0)->text().toInt(); queryVector(); }
    });
    auto *details = new QWidget;
    auto *detailLayout = column(details);
    auto *controls = new QHBoxLayout;
    space = choices({u"输入向量"_s,u"输出向量"_s},{u"input"_s,u"output"_s});
    vectorStart=spin(0,1000000,0);
    vectorCount=spin(1,512,64);
    controls->addWidget(space);
    controls->addWidget(new QLabel(u"起点"_s));
    controls->addWidget(vectorStart);
    controls->addWidget(new QLabel(u"长度"_s));
    controls->addWidget(vectorCount);
    controls->addWidget(button(u"查看"_s,[this]{queryVector();},this));
    controls->addWidget(button(u"查相近"_s,[this]{queryVector(true);},this));
    controls->addWidget(button(u"自定义查询"_s,[this]{
        auto *dialog=new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(u"自定义向量查询"_s);
        dialog->resize(480,240);
        auto *layout=column(dialog);
        layout->addWidget(hint(u"在当前向量空间搜索；维度须与模型一致"_s));
        auto *text=new QTextEdit;
        text->setPlaceholderText(u"向量数值，以空格或逗号分隔"_s);
        layout->addWidget(text);
        layout->addWidget(button(u"查询"_s,[this,dialog,text]{
            auto content=text->toPlainText();
            content.replace(QChar(u','),QChar(u' ')).replace(QChar(u'，'),QChar(u' ')).replace(QChar(u'\n'),QChar(u' '));
            QJsonArray values;
            for(const auto &part:content.split(QChar(u' '),Qt::SkipEmptyParts)){
                bool ok=false;const auto value=part.toDouble(&ok);
                if(!ok||!std::isfinite(value)){failure(u"查询向量含无效数值"_s);return;}
                values.append(value);
            }
            request({{u"action"_s,u"similar"_s},{u"token_id"_s,tokenId},{u"space"_s,space->currentData().toString()},{u"query_vector"_s,values}});
            dialog->accept();
        },dialog));
        dialog->open();
    },this));
    controls->addWidget(button(u"修改词表"_s,[this]{editWord();},this));
    detailLayout->addLayout(controls);
    vectorHint=hint(u"选择一个 Token 查看真实向量"_s);
    detailLayout->addWidget(vectorHint);
    auto *tables = new QSplitter(Qt::Horizontal);
    vectorTable=table({u"维度"_s,u"数值"_s});
    similarTable=table({u"编号"_s,u"相近 Token"_s,u"余弦相似度"_s});
    connect(similarTable,&QTableWidget::cellDoubleClicked,this,[this](int r,int){
        if (similarTable->item(r,0)) {tokenId=similarTable->item(r,0)->text().toInt();queryVector();}
    });
    tables->addWidget(vectorTable);
    tables->addWidget(similarTable);
    detailLayout->addWidget(tables,1);
    auto *edit = new QHBoxLayout;
    editKind=choices({u"替换"_s,u"缩放"_s,u"增加"_s,u"置零"_s},{u"set"_s,u"scale"_s,u"add"_s,u"zero"_s});
    editStart=spin(0,1000000,0);
    editStop=spin(1,1000000,1);
    edit->addWidget(new QLabel(u"修改区间"_s));
    edit->addWidget(editStart);
    edit->addWidget(new QLabel(u"至（不含）"_s));
    edit->addWidget(editStop);
    edit->addWidget(editKind);
    edit->addWidget(button(u"预览修改"_s,[this]{previewVector();},this));
    detailLayout->addLayout(edit);
    editValues=new QTextEdit;
    editValues->setAcceptRichText(false);
    editValues->setPlaceholderText(u"输入数值，以空格或逗号分隔；一个值可用于整个区间"_s);
    editValues->setAccessibleName(u"向量修改数值"_s);
    UiScale::fixedHeight(editValues,52);
    detailLayout->addWidget(editValues);
    split->addWidget(details);
    split->setStretchFactor(0,1);
    split->setStretchFactor(1,2);
    layout->addWidget(split,1);
    return page;
}

QWidget *FeaturePanel::versionsPage() {
    // 历史按应用顺序展示，停用中间补丁会由服务端从基座重放。
    auto *page=new QWidget;
    auto *layout=column(page);
    auto *title=new QLabel(u"补丁与版本"_s);
    title->setObjectName(u"heading"_s);
    layout->addWidget(title);
    auto *actions=new QHBoxLayout;
    actions->addWidget(button(u"刷新"_s,[this]{request({{u"action"_s,u"versions"_s}});},this));
    for (const auto &key : {u"undo"_s,u"redo"_s})
        actions->addWidget(button(key==u"undo"_s?u"撤销"_s:u"重做"_s,[this,key]{request({{u"action"_s,u"versions.action"_s},{u"operation"_s,key}});},this));
    actions->addWidget(button(u"启用 / 停用"_s,[this]{
        const int r=patchTable->currentRow();
        if (r<0 || r>=patchRows.size()) {failure(u"请选择一个补丁"_s);return;}
        request({{u"action"_s,u"versions.action"_s},{u"operation"_s,u"toggle"_s},{u"patch_id"_s,patchRows[r].toObject().value(u"id"_s)}});
    },this));
    actions->addStretch();
    for (bool importing : {true,false})
        actions->addWidget(button(importing?u"导入"_s:u"导出"_s,[this,importing]{
            const auto path=importing?QFileDialog::getOpenFileName(this,u"导入补丁"_s,projectRoot,u"补丁 (*.json)"_s):
                QFileDialog::getSaveFileName(this,u"导出补丁"_s,projectRoot+u"/补丁.json"_s,u"补丁 (*.json)"_s);
            if(path.isEmpty())return;
            request({{u"action"_s,importing?u"versions.preview"_s:u"versions.export"_s},{u"path"_s,path}});
        },this));
    layout->addLayout(actions);
    versionHint=hint(u"向量、词表和权重修改统一记录；撤销会写回模型"_s);
    layout->addWidget(versionHint);
    patchTable=table({u"顺序"_s,u"修改"_s,u"来源"_s,u"状态"_s,u"范围"_s});
    layout->addWidget(patchTable,1);
    auto *draftActions=new QHBoxLayout;
    draftActions->addWidget(new QLabel(u"项目草稿"_s));
    draftActions->addStretch();
    draftActions->addWidget(button(u"预览草稿"_s,[this]{
        const int r=draftTable->currentRow();
        if(r<0||r>=drafts.size()){failure(u"请选择一个草稿"_s);return;}
        auto payload=drafts[r].toObject();
        payload[u"action"_s]=payload.value(u"action"_s).toString().replace(u".apply"_s,u".preview"_s);
        payload.remove(u"expected_weights_revision"_s);
        request(payload);
    },this));
    draftActions->addWidget(button(u"删除草稿"_s,[this]{
        const int r=draftTable->currentRow();
        if(r>=0&&r<drafts.size()){drafts.removeAt(r);saveDrafts();}
    },this));
    layout->addLayout(draftActions);
    draftTable=table({u"草稿"_s,u"目标 Token"_s,u"范围"_s});
    layout->addWidget(draftTable,1);
    layout->addWidget(hint(u"相同位置按顺序应用；重复导入同一补丁不会叠加"_s));
    return page;
}

QWidget *FeaturePanel::regressionPage() {
    // 样例与报告存放在项目，明确区分通过、失败和未评估。
    auto *page=new QWidget;
    auto *layout=column(page);
    auto *title=new QLabel(u"回归验证"_s);
    title->setObjectName(u"heading"_s);
    layout->addWidget(title);
    auto *actions=new QHBoxLayout;
    actions->addWidget(button(u"添加样例"_s,[this]{
        // 新增后立即打开详情，隐藏的通过条件也能直接填写。
        addSample(); sampleTable->setCurrentCell(sampleTable->rowCount()-1,1); editSample();
    },this));
    actions->addWidget(button(u"删除"_s,[this]{if(sampleTable->currentRow()>=0)sampleTable->removeRow(sampleTable->currentRow());},this));
    actions->addWidget(button(u"保存样例"_s,[this]{saveSamples();},this));
    actions->addWidget(button(u"生成设置"_s,[this]{configureSample();},this));
    actions->addWidget(button(u"上下文"_s,[this]{
        const int r=sampleTable->currentRow();
        if(r<0){failure(u"请选择一条样例"_s);return;}
        auto *dialog=new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(u"样例上下文"_s);
        dialog->resize(640,420);
        auto *layout=column(dialog);
        layout->addWidget(hint(u"按顺序填写历史消息，本条输入会自动追加"_s));
        auto *history=table({u"角色"_s,u"内容"_s},true);
        auto append=[history](const QString &role,const QString &content){
            const int index=history->rowCount();row(history,{QString{},content});
            auto *roles=choices({u"系统"_s,u"用户"_s,u"模型"_s},{u"system"_s,u"user"_s,u"assistant"_s});
            roles->setCurrentIndex(std::max(0,roles->findData(role)));
            history->setCellWidget(index,0,roles);
        };
        for(auto value:sampleTable->item(r,1)->data(Qt::UserRole).toJsonArray()){
            const auto item=value.toObject();append(item.value(u"role"_s).toString(),item.value(u"content"_s).toString());
        }
        layout->addWidget(history,1);
        auto *actions=new QHBoxLayout;
        actions->addWidget(button(u"添加"_s,[append]{append(u"user"_s,QString{});},dialog));
        actions->addWidget(button(u"删除"_s,[history]{if(history->currentRow()>=0)history->removeRow(history->currentRow());},dialog));
        actions->addStretch();
        actions->addWidget(button(u"保存"_s,[dialog,history,this,r]{
            QJsonArray messages;
            for(int i=0;i<history->rowCount();++i){
                auto *role=qobject_cast<QComboBox*>(history->cellWidget(i,0));
                messages.append(QJsonObject{{u"role"_s,role->currentData().toString()},{u"content"_s,history->item(i,1)->text()}});
            }
            if(r<sampleTable->rowCount())sampleTable->item(r,1)->setData(Qt::UserRole,messages);
            dialog->accept();
        },dialog));
        layout->addLayout(actions);dialog->open();
    },this));
    actions->addStretch();
    actions->addWidget(button(u"运行对照"_s,[this]{runRegression();},this));
    actions->addWidget(button(u"导出并验收"_s,[this]{runRegression(true);},this));
    actions->addWidget(button(u"打开报告"_s,[this]{
        const auto path=QFileDialog::getOpenFileName(this,u"打开验证报告"_s,projectRoot+u"/features"_s,u"报告 (*.json)"_s);
        if(path.isEmpty())return;
        QFile file(path);
        if(!file.open(QIODevice::ReadOnly)||file.size()>8*1024*1024){failure(u"报告不可读或过大"_s);return;}
        const auto payload=QJsonDocument::fromJson(file.readAll()).object();
        if(!payload.contains(u"samples"_s)||!payload.contains(u"rows"_s)){failure(u"不是验证报告"_s);return;}
        renderReport(payload);
        notice->setText(u"已打开历史报告"_s);
    },this));
    layout->addLayout(actions);
    layout->addWidget(hint(u"双击编辑样例；可为每条样例设置生成参数"_s));
    auto *split=new QSplitter(Qt::Vertical);
    sampleTable=table({u"类型"_s,u"输入"_s,u"目标 Token"_s,u"通过条件"_s,u"期望文本 / 阈值"_s,u"输出上限"_s,u"最大概率降幅"_s},true);
    sampleTable->setColumnWidth(0,130);
    sampleTable->setColumnWidth(1,160);
    sampleTable->setColumnWidth(3,140);
    sampleTable->setColumnWidth(4,160);
    split->addWidget(sampleTable);
    reportTable=table({u"样例"_s,u"原版结果"_s,u"编辑版结果"_s,u"原版概率"_s,u"编辑版概率"_s,u"判定"_s});
    split->addWidget(reportTable);
    layout->addWidget(split,1);
    connect(reportTable,&QTableWidget::cellDoubleClicked,this,[this](int r,int){
        const auto rows=lastReport.value(u"rows"_s).toArray();
        if(r<0||r>=rows.size())return;
        auto item=rows[r].toObject();
        auto *dialog=new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(u"样例详情"_s);
        dialog->resize(720,480);
        auto *box=column(dialog);
        auto *text=new QTextEdit;
        text->setReadOnly(true);
        const auto before=item.value(u"base"_s).toObject(), after=item.value(u"edited"_s).toObject();
        text->setPlainText(u"原版\n"_s+before.value(u"text"_s).toString()+u"\n概率："_s+number(before.value(u"probability"_s))+u"　排名："_s+number(before.value(u"rank"_s))+
            u"\n\n编辑版\n"_s+after.value(u"text"_s).toString()+u"\n概率："_s+number(after.value(u"probability"_s))+u"　排名："_s+number(after.value(u"rank"_s))+
            u"\n\n判定："_s+after.value(u"reason"_s).toString());
        box->addWidget(text);
        dialog->open();
    });
    return page;
}

void FeaturePanel::applyPrototypeLayout() {
    // 功能页复用原业务控件，只按确认原型整理主操作、详情和次级工具。
    setObjectName(u"prototypeFeatures"_s);
    for (int i = 0; i < pages->count(); ++i) {
        auto *page = qobject_cast<QScrollArea *>(pages->widget(i))->widget();
        auto *layout = qobject_cast<QVBoxLayout *>(page->layout());
        layout->setContentsMargins(15, 0, 15, 18);
        layout->setSpacing(16);
        if (auto *title = page->findChild<QLabel *>(u"heading"_s)) title->hide();
    }
    const auto pageAt = [this](int index) { return qobject_cast<QScrollArea *>(pages->widget(index))->widget(); };
    const auto findButton = [](QWidget *page, const QString &name) -> QPushButton * {
        for (auto *control : page->findChildren<QPushButton *>()) if (control->text() == name) return control;
        return nullptr;
    };
    const auto secondaryMenu = [this](QWidget *page, QBoxLayout *toolbar, const QStringList &names) {
        // 次级能力保留在工具菜单中，主界面减少成排说明与低频操作。
        auto *more = new QPushButton(u"更多"_s);
        auto *menu = new QMenu(more);
        for (const auto &name : names) {
            QPushButton *target = nullptr;
            for (auto *control : page->findChildren<QPushButton *>()) if (control->text() == name) { target = control; break; }
            if (!target) continue;
            target->hide();
            menu->addAction(name, target, &QPushButton::click);
        }
        more->setMenu(menu); toolbar->addWidget(more);
    };
    auto *activation = pageAt(0);
    auto *activationLayout = qobject_cast<QVBoxLayout *>(activation->layout());
    auto *filters = qobject_cast<QHBoxLayout *>(activationLayout->itemAt(1)->layout());
    auto *range = qobject_cast<QHBoxLayout *>(activationLayout->itemAt(2)->layout());
    auto *advanced = new QWidget;
    auto *advancedColumn = new QVBoxLayout(advanced);
    advancedColumn->setContentsMargins(0, 0, 0, 0);
    activationLayout->removeItem(range); advancedColumn->addLayout(range);
    filters->removeWidget(source);
    range->insertWidget(0, source);
    activationLayout->insertWidget(2, advanced);
    advanced->hide();
    auto *toggle = button(u"筛选"_s, [advanced] { advanced->setVisible(!advanced->isVisible()); }, this);
    filters->addWidget(toggle);
    if (auto *analyze = findButton(activation, u"分析"_s)) { analyze->setText(u"重新分析"_s); analyze->setObjectName(u"primary"_s); }
    auto *activationActions = qobject_cast<QHBoxLayout *>(activationLayout->itemAt(activationLayout->count() - 1)->layout());
    secondaryMenu(activation, activationActions, {u"查看数值"_s,u"监视"_s,u"打开分析"_s});
    if (auto *locate = findButton(activation, u"定位"_s)) locate->setText(u"定位到计算图"_s);
    if (auto *add = findButton(activation, u"加入干预"_s)) add->setObjectName(u"primary"_s);
    activationTable->setColumnHidden(7, true);
    connect(toggle, &QPushButton::clicked, this, [this, advanced] { activationTable->setColumnHidden(7, !advanced->isVisible()); });
    activationTable->setHorizontalHeaderLabels({u"层号"_s,u"通道"_s,u"强度"_s,u"峰值"_s,u"触发位置"_s,u"触发Token"_s,u"有效数"_s,u"基线差值"_s});
    // 来源选择位于分析条件之前，直接展示所选回复原文，选区与保存的 UTF-16 Token 边界一致。
    auto *sources=new QWidget;auto *sourceLayout=column(sources);
    auto *pickers=new QHBoxLayout;
    analysisSession=new QComboBox;analysisSession->setObjectName(u"analysisSession"_s);analysisSession->setAccessibleName(u"分析来源会话"_s);
    analysisReply=new QComboBox;analysisReply->setObjectName(u"analysisReply"_s);analysisReply->setAccessibleName(u"观测回复"_s);
    for(auto *picker:{analysisSession,analysisReply}){picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);picker->setMinimumContentsLength(6);}
    pickers->addWidget(new QLabel(u"会话"_s));pickers->addWidget(analysisSession,1);pickers->addWidget(new QLabel(u"观测回复"_s));pickers->addWidget(analysisReply,2);sourceLayout->addLayout(pickers);
    auto *selection=new QHBoxLayout;
    analysisChannel=choices({u"回复正文"_s,u"思考过程"_s},{u"answer"_s,u"reasoning"_s});analysisChannel->setObjectName(u"analysisChannel"_s);
    analysisContentScope=choices({u"整段内容"_s,u"选中的文本"_s},{u"all"_s,u"selection"_s});analysisContentScope->setObjectName(u"analysisContentScope"_s);
    selection->addWidget(analysisChannel);selection->addWidget(analysisContentScope);selection->addStretch();sourceLayout->addLayout(selection);
    analysisContent=new QTextEdit;analysisContent->setObjectName(u"analysisContent"_s);analysisContent->setReadOnly(true);analysisContent->setPlaceholderText(u"选择观测回复后，可选中需要分析的内容"_s);UiScale::fixedHeight(analysisContent,120);sourceLayout->addWidget(analysisContent);
    connect(analysisSession,&QComboBox::currentIndexChanged,this,[this]{selectActivationSession();});
    connect(analysisReply,&QComboBox::currentIndexChanged,this,[this]{selectActivationReply();});
    connect(analysisChannel,&QComboBox::currentIndexChanged,this,[this]{selectActivationReply();});
    auto *note=hint(u"仅支持开启了观测模式的输出使用"_s);note->setObjectName(u"activationModeNote"_s);
    activationLayout->insertWidget(1,note);activationLayout->insertWidget(2,sources);
    scope->setCurrentIndex(1);scope->hide();
    // 文本选区替代手填位置，旧位置控件保留供兼容记录使用，不再作为可操作筛选项。
    for(int i=range->count()-1;i>=0;--i){auto *widget=range->itemAt(i)->widget();if(widget&&widget!=source&&widget!=baselinePath&&qobject_cast<QLabel *>(widget))widget->hide();}
    rangeStart->hide();rangeStop->hide();
    // 结果筛选独立于重新分析条件，所有查询作用于后端的完整结果快照。
    auto *resultFilters=new QWidget;auto *resultColumn=column(resultFilters);auto *searchRow=new QHBoxLayout;
    activationSearch=new QLineEdit;activationSearch->setPlaceholderText(u"搜索层号、神经元编号或触发 Token"_s);searchRow->addWidget(activationSearch,1);
    activationSort=choices({u"按强度"_s,u"按层号"_s,u"按神经元编号"_s,u"按有效次数"_s,u"按触发 Token"_s},{u"score"_s,u"layer"_s,u"neuron"_s,u"samples"_s,u"trigger"_s});
    activationDirection=choices({u"降序"_s,u"升序"_s},{u"desc"_s,u"asc"_s});searchRow->addWidget(activationSort);searchRow->addWidget(activationDirection);resultColumn->addLayout(searchRow);
    auto *bounds=new QGridLayout;
    activationLayerMin=spin(-1,100000,-1);activationLayerMax=spin(-1,100000,-1);
    activationLayerMin->setSpecialValueText(u"不限"_s);activationLayerMax->setSpecialValueText(u"不限"_s);
    activationScore=new QDoubleSpinBox;activationScore->setDecimals(6);activationScore->setRange(0,1e12);activationScore->setSpecialValueText(u"不限"_s);
    activationSamples=spin(0,1000000000,0);activationSamples->setSpecialValueText(u"不限"_s);
    bounds->addWidget(new QLabel(u"层号范围"_s),0,0);bounds->addWidget(activationLayerMin,0,1);bounds->addWidget(new QLabel(u"至"_s),0,2);bounds->addWidget(activationLayerMax,0,3);
    bounds->addWidget(new QLabel(u"最低强度"_s),1,0);bounds->addWidget(activationScore,1,1);bounds->addWidget(new QLabel(u"最少有效次数"_s),1,2);bounds->addWidget(activationSamples,1,3);
    bounds->addWidget(button(u"查询"_s,[this]{queryActivation();},this),0,4);bounds->addWidget(button(u"清空筛选"_s,[this]{
        activationSearch->clear();activationLayerMin->setValue(-1);activationLayerMax->setValue(-1);activationScore->setValue(0);activationSamples->setValue(0);
        activationSort->setCurrentIndex(0);activationDirection->setCurrentIndex(0);queryActivation();},this),1,4);resultColumn->addLayout(bounds);
    activationLayout->insertWidget(activationLayout->indexOf(activationTable),resultFilters);
    auto *pagination=new QHBoxLayout;activationPagination=new QLabel(u"尚无结果"_s);activationPagination->setWordWrap(true);pagination->addStretch();
    activationPrevious=button(u"上一页"_s,[this]{queryActivation(activationPageNumber->value()-1);},this);
    activationNext=button(u"下一页"_s,[this]{queryActivation(activationPageNumber->value()+1);},this);
    activationPrevious->setEnabled(false);activationNext->setEnabled(false);pagination->addWidget(activationPrevious);pagination->addWidget(activationNext);
    activationPageNumber=spin(1,1,1);pagination->addWidget(activationPageNumber);pagination->addWidget(button(u"跳转"_s,[this]{queryActivation(activationPageNumber->value());},this));
    activationPageSize=choices({u"每页 50 条"_s,u"每页 100 条"_s,u"每页 250 条"_s,u"每页 500 条"_s},{u"50"_s,u"100"_s,u"250"_s,u"500"_s});activationPageSize->setCurrentIndex(1);pagination->addWidget(activationPageSize);
    activationLayout->insertWidget(activationLayout->indexOf(activationTable)+1,activationPagination);
    activationLayout->insertLayout(activationLayout->indexOf(activationTable)+2,pagination);
    connect(activationSearch,&QLineEdit::returnPressed,this,[this]{queryActivation();});
    connect(activationPageSize,&QComboBox::currentIndexChanged,this,[this]{if(!activationReport.isEmpty())queryActivation();});
    auto *vectors = pageAt(1);
    auto *details = vectorHint->parentWidget();
    auto *detailLayout = qobject_cast<QVBoxLayout *>(details->layout());
    auto *vectorControls = qobject_cast<QHBoxLayout *>(detailLayout->itemAt(0)->layout());
    secondaryMenu(details, vectorControls, {u"自定义查询"_s,u"修改词表"_s});
    auto *tables = qobject_cast<QSplitter *>(vectorTable->parentWidget());
    vectorTable->setParent(details); vectorTable->hide();
    auto *heatArea = new QWidget;
    auto *heatColumn = column(heatArea);
    heatColumn->addWidget(new QLabel(u"Token向量"_s));
    vectorHeat = new Heatmap;
    UiScale::minimumHeight(vectorHeat, 170);
    heatColumn->addWidget(vectorHeat, 1);
    auto *numbers = button(u"查看数值"_s, [this] {
        // 复用当前切片表，关闭后仍可接收后续查询结果。
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(u"向量数值"_s); dialog->resize(420, 500);
        auto *layout = column(dialog); layout->addWidget(vectorTable); vectorTable->show();
        connect(dialog, &QDialog::finished, this, [this] { vectorTable->setParent(this); vectorTable->hide(); });
        dialog->open();
    }, this);
    heatColumn->addWidget(numbers, 0, Qt::AlignLeft);
    tables->insertWidget(0, heatArea);
    tables->setStretchFactor(0, 2); tables->setStretchFactor(1, 1);
    auto *editLayout = qobject_cast<QHBoxLayout *>(detailLayout->itemAt(3)->layout());
    auto *editArea = new QWidget;
    auto *editColumn = column(editArea);
    detailLayout->removeItem(editLayout); editColumn->addLayout(editLayout);
    detailLayout->removeWidget(editValues); editColumn->addWidget(editValues);
    detailLayout->addWidget(editArea); editArea->hide();
    auto *editButton = button(u"修改向量"_s, [editArea] { editArea->setVisible(!editArea->isVisible()); }, this);
    vectorControls->addWidget(editButton);
    vectorHint->setText(u"请选择Token"_s);
    tokenQuery->setPlaceholderText(u"搜索文本或Token编号"_s);
    special->setText(u"仅特殊Token"_s);
    Q_UNUSED(vectors);
    auto *versions = pageAt(2);
    auto *versionLayout = qobject_cast<QVBoxLayout *>(versions->layout());
    auto *versionActions = qobject_cast<QHBoxLayout *>(versionLayout->itemAt(1)->layout());
    secondaryMenu(versions, versionActions, {u"启用 / 停用"_s,u"导出"_s});
    if (auto *import = findButton(versions, u"导入"_s)) import->setText(u"导入补丁"_s);
    versionHint->setToolTip(versionHint->text()); versionHint->setText(u"当前模型版本"_s);
    auto *versionNote = qobject_cast<QLabel *>(versionLayout->itemAt(versionLayout->count() - 1)->widget());
    if (versionNote) versionNote->hide();
    auto *regression = pageAt(3);
    auto *regressionLayout = qobject_cast<QVBoxLayout *>(regression->layout());
    auto *regressionActions = qobject_cast<QHBoxLayout *>(regressionLayout->itemAt(1)->layout());
    secondaryMenu(regression, regressionActions, {u"删除"_s,u"生成设置"_s,u"上下文"_s,u"打开报告"_s});
    if (auto *run = findButton(regression, u"运行对照"_s)) run->setObjectName(u"primary"_s);
    if (auto *note = qobject_cast<QLabel *>(regressionLayout->itemAt(2)->widget())) { sampleTable->setToolTip(note->text()); note->hide(); }
    // 主表只保留原型中的摘要列，完整条件通过编辑窗口配置。
    sampleTable->setColumnCount(8);
    sampleTable->setHorizontalHeaderLabels({u"样例"_s,u"输入"_s,u"目标Token"_s,u"通过条件"_s,u"期望文本 / 阈值"_s,u"输出上限"_s,u"最大概率降幅"_s,u"结果"_s});
    for (int index : {2,4,6}) sampleTable->hideColumn(index);
    sampleTable->setColumnWidth(0,110); sampleTable->setColumnWidth(1,230); sampleTable->setColumnWidth(3,130); sampleTable->setColumnWidth(5,90);
    auto *reports = qobject_cast<QSplitter *>(reportTable->parentWidget());
    reportTable->setParent(this); reportTable->hide();
    auto *sampleDetails = new QWidget;
    auto *sampleColumn = column(sampleDetails);
    auto *sampleHeader = new QHBoxLayout;
    auto *detailTitle = new QLabel(u"样例详情"_s); detailTitle->setObjectName(u"heading"_s);
    sampleHeader->addWidget(detailTitle); sampleHeader->addStretch();
    sampleHeader->addWidget(button(u"编辑样例"_s,[this]{editSample();},this));
    sampleColumn->addLayout(sampleHeader);
    auto *detailText = new QTextBrowser; detailText->setPlainText(u"尚未选择样例"_s);
    sampleColumn->addWidget(detailText,1); reports->addWidget(sampleDetails);
    const auto refreshDetails = [this,detailText] {
        const int r=sampleTable->currentRow();
        if(r<0||r>=sampleTable->rowCount()){detailText->setPlainText(u"尚未选择样例"_s);return;}
        const auto *condition=qobject_cast<QComboBox*>(sampleTable->cellWidget(r,3));
        if(!condition||!sampleTable->item(r,1))return;
        QString content=u"输入\n"_s+sampleTable->item(r,1)->text()+u"\n\n通过条件："_s+condition->currentText();
        const auto expected=sampleTable->item(r,4)->text();
        if(!expected.isEmpty())content+=u"　"_s+expected;
        const auto rows=lastReport.value(u"rows"_s).toArray();
        if(r<rows.size()){
            const auto result=rows[r].toObject();
            content+=u"\n\n原始版本\n"_s+result.value(u"base"_s).toObject().value(u"text"_s).toString()+
                u"\n\n编辑版本\n"_s+result.value(u"edited"_s).toObject().value(u"text"_s).toString();
            if(reportTable->item(r,5))content+=u"\n\n结果："_s+reportTable->item(r,5)->text();
        }else content+=u"\n\n未评估"_s;
        detailText->setPlainText(content);
    };
    connect(sampleTable,&QTableWidget::itemSelectionChanged,this,refreshDetails);
    connect(sampleTable,&QTableWidget::itemChanged,this,[refreshDetails](QTableWidgetItem*){refreshDetails();});
    connect(sampleTable,&QTableWidget::cellDoubleClicked,this,[this](int,int){editSample();});
    auto *fullReport=button(u"查看完整报告"_s,[this]{
        auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"验证报告"_s);dialog->resize(800,500);
        auto *layout=column(dialog);layout->addWidget(reportTable);reportTable->show();
        connect(dialog,&QDialog::finished,this,[this]{reportTable->setParent(this);reportTable->hide();});dialog->open();
    },this);
    sampleColumn->addWidget(fullReport,0,Qt::AlignLeft);
}

void FeaturePanel::open(int index, bool refresh) {
    // 切换页面只按需刷新，不自动推理或写入模型。
    pages->setCurrentIndex(index);
    const QStringList help{u"激活分析\n\n尚未选择分析结果"_s,
        u"词表与向量\n\n尚未选择Token"_s,
        u"补丁与版本\n\n当前模型的补丁与未应用草稿"_s,
        u"回归验证\n\n尚未选择样例或验证结果"_s};
    emit inspected(index==0 && !activationInspection.isEmpty()?activationInspection:help.value(index));
    if(!refresh||!available||busy)return;
    if(index==1&&modelLoaded)searchTokens();
    else if(index==2&&modelLoaded)request({{u"action"_s,u"versions"_s}});
}

void FeaturePanel::setActivationSessions(const QJsonArray &sessions) {
    // 刷新下拉列表不触发来源切换；即使记录被清理，也保留已完成的分析现场。
    if(!analysisSession)return;
    const auto oldSession=analysisSession->currentData().toString();
    const auto oldTitle=analysisSession->currentText();
    analysisSessions=sessions;
    {QSignalBlocker block(analysisSession);analysisSession->clear();
        for(const auto &value:sessions){const auto item=value.toObject();analysisSession->addItem(item.value(u"title"_s).toString(),item.value(u"id"_s).toString());}
        if(!oldSession.isEmpty()&&analysisSession->findData(oldSession)<0)analysisSession->addItem(oldTitle+u"（来源已移除）"_s,oldSession);
        analysisSession->setCurrentIndex(std::max(0,analysisSession->findData(oldSession)));}
    // selectActivationSession 在调用 selectActivationReply 前恢复原回复，避免短暂选中第一条而清空结果。
    selectActivationSession();
}

void FeaturePanel::selectActivationSession() {
    // 同一会话刷新时按完整运行身份恢复回复，真正换来源时才清空结果。
    const auto oldReply=analysisReply->currentData().toJsonObject();
    const bool sameSession=activationSourceKey.startsWith(analysisSession->currentData().toString()+u"|"_s);
    {QSignalBlocker block(analysisReply);analysisReply->clear();
        for(const auto &value:analysisSessions){const auto item=value.toObject();if(item.value(u"id"_s).toString()!=analysisSession->currentData().toString())continue;
            int number=0;for(const auto &reply:item.value(u"replies"_s).toArray()){
                const auto row=reply.toObject();auto label=u"回复 "_s+QString::number(++number)+u" · "_s+row.value(u"content"_s).toString().simplified().left(28);
                if(!row.value(u"available"_s).toBool())label+=u"（记录不可用）"_s;
                analysisReply->addItem(label,row);
            }
        }
        if(sameSession&&!oldReply.isEmpty()){
            int found=-1;for(int i=0;i<analysisReply->count();++i){const auto row=analysisReply->itemData(i).toJsonObject();
                if(row.value(u"trace_path"_s)==oldReply.value(u"trace_path"_s)&&row.value(u"run_id"_s)==oldReply.value(u"run_id"_s)){found=i;break;}}
            if(found<0){auto missing=oldReply;missing[u"available"_s]=false;analysisReply->addItem(u"原分析回复（记录不可用）"_s,missing);found=analysisReply->count()-1;}
            analysisReply->setCurrentIndex(found);
        }}
    selectActivationReply();
}

void FeaturePanel::selectActivationReply() {
    // 直接显示原始正文，避免 Markdown 渲染改变选区与 Token 位置的对应关系。
    const auto reply=analysisReply->currentData().toJsonObject();
    const auto identity=analysisSession->currentData().toString()+u"|"_s+reply.value(u"trace_path"_s).toString()+u"|"_s+
        reply.value(u"run_id"_s).toString()+u"|"_s+reply.value(u"branch_id"_s).toString()+u"|"_s+analysisChannel->currentData().toString();
    if(identity==activationSourceKey)return;
    activationSourceKey=identity;
    analysisContent->setPlainText(reply.value(analysisChannel->currentData()==u"reasoning"_s?u"reasoning_content"_s:u"content"_s).toString());
    analysisContentScope->setCurrentIndex(0);
    resetActivation();
    activationHint->setText(reply.isEmpty()?u"当前会话没有观测模式的回复，请先使用观测模式完成一轮对话。"_s:
        !reply.value(u"available"_s).toBool()?u"该回复的观测记录未保存或已清理，无法分析。"_s:u"已选择观测回复，可分析整段内容或选中的文本。"_s);
}

void FeaturePanel::analyzeActivation() {
    // 选区采用真实采样 Token 的字符边界，跨字节续片共享区间也全部保留。
    const auto reply=analysisReply->currentData().toJsonObject();
    if(reply.isEmpty()){failure(u"请先选择会话和观测模式下的回复"_s);return;}
    if(!reply.value(u"available"_s).toBool()){failure(u"该回复的观测记录不可用，请重新进行观测"_s);return;}
    const auto cursor=analysisContent->textCursor();const bool selected=analysisContentScope->currentData()==u"selection"_s;
    if(selected&&!cursor.hasSelection()){failure(u"请先在回复内容中选中需要分析的文本"_s);return;}
    QJsonArray positions;
    for(const auto &value:reply.value(u"output_tokens"_s).toArray()){
        const auto token=value.toObject();if(token.value(u"channel"_s).toString(u"answer"_s)!=analysisChannel->currentData().toString()||token.value(u"special"_s).toBool()||token.value(u"hidden"_s).toBool())continue;
        if(selected&&(!token.contains(u"text_start"_s)||!token.contains(u"text_end"_s)))continue;
        if(!selected||(token.value(u"text_start"_s).toInt()<cursor.selectionEnd()&&token.value(u"text_end"_s).toInt()>cursor.selectionStart()))positions.append(token.value(u"position"_s));
    }
    if(positions.isEmpty()){failure(u"所选内容没有保存可映射的 Token 边界，无法准确分析"_s);return;}
    QJsonObject payload{{u"action"_s,u"activation"_s},{u"scope"_s,u"positions"_s},{u"positions"_s,positions},
        {u"trace_path"_s,reply.value(u"trace_path"_s)},{u"run_id"_s,reply.value(u"run_id"_s)},{u"branch_id"_s,reply.value(u"branch_id"_s)},
        {u"source"_s,source->currentData().toString()},{u"metric"_s,metric->currentData().toString()},{u"layer"_s,analysisLayer->value()},
        {u"baseline"_s,baselinePath->text()},{u"paged"_s,true},{u"page_size"_s,activationPageSize->currentData().toInt()},
        {u"client_revision"_s,++activationRevision}};
    payload[u"query"_s]=activationSearch->text();payload[u"sort"_s]=activationSort->currentData().toString();
    payload[u"ascending"_s]=activationDirection->currentIndex()==1;payload[u"score_min"_s]=activationScore->value();payload[u"samples_min"_s]=activationSamples->value();
    if(activationLayerMin->value()>=0)payload[u"layer_min"_s]=activationLayerMin->value();
    if(activationLayerMax->value()>=0)payload[u"layer_max"_s]=activationLayerMax->value();
    request(payload);
}

void FeaturePanel::resetActivation() {
    // 只有真实切换分析来源或项目才失效结果；页面导航不调用此方法。
    ++activationRevision;activationReport={};activationRows={};activationInspection.clear();activationTable->setRowCount(0);
    if(activationPagination)activationPagination->setText(u"尚无结果"_s);
    if(activationPrevious)activationPrevious->setEnabled(false);
    if(activationNext)activationNext->setEnabled(false);
}

void FeaturePanel::queryActivation(int page) {
    // 翻页和搜索只查询同一分析快照；未点击重新分析时不读取张量或执行推理。
    const auto id=activationReport.value(u"analysis_id"_s).toString();
    if(id.isEmpty()){failure(u"请先分析或打开分析文件"_s);return;}
    if(activationLayerMax->value()>=0&&activationLayerMin->value()>activationLayerMax->value()){failure(u"层号范围无效"_s);return;}
    QJsonObject query{{u"action"_s,u"activation.query"_s},{u"analysis_id"_s,id},{u"page"_s,std::max(1,page)},
        {u"page_size"_s,activationPageSize->currentData().toInt()},{u"query"_s,activationSearch->text()},
        {u"sort"_s,activationSort->currentData().toString()},{u"ascending"_s,activationDirection->currentIndex()==1},
        {u"score_min"_s,activationScore->value()},{u"samples_min"_s,activationSamples->value()},
        {u"client_revision"_s,++activationRevision}};
    if(activationLayerMin->value()>=0)query[u"layer_min"_s]=activationLayerMin->value();
    if(activationLayerMax->value()>=0)query[u"layer_max"_s]=activationLayerMax->value();
    request(query);
}

int FeaturePanel::currentPage() const {
    // 保存项目时记录当前功能，重启后恢复同一页面。
    return pages->currentIndex();
}

void FeaturePanel::context(const QString &directory, bool loaded, bool idle, const QString &branch) {
    // 项目切换时加载独立样例，防止将上一项目的数据保存到新项目。
    if(projectRoot!=directory){projectRoot=directory;loadSamples();loadDrafts();tokenId=-1;resetActivation();activationSourceKey.clear();lastReport={};reportTable->setRowCount(0);analysisProgressTimer->stop();analysisProgressBox->hide();}
    branchId=branch;
    if(modelLoaded!=loaded)notice->setText(loaded?u"就绪"_s:u"尚未加载模型"_s);
    modelLoaded=loaded;
    available=idle;
    pages->setEnabled(idle&&!busy);
}

void FeaturePanel::pending(bool value, const QString &action) {
    // 重任务保持状态可见，停止按钮位于禁用区域外。
    busy=value;
    pages->setEnabled(available&&!busy);
    cancelButton->setVisible(busy);
    if(value){
        analysisProgressTimer->stop();
        notice->setText(u"正在处理"_s);
        cancelButton->setEnabled(true);
        analysisCancelling=false;
        analysisProgressBox->setVisible(action.startsWith(u"activation"_s));
        if(!analysisProgressBox->isHidden()){
            analysisElapsed.start();
            analysisProgressStage=u"正在准备分析"_s;
            analysisProgress->setRange(0,0);
            analysisProgress->setFormat(u"本阶段 %p%"_s);
            refreshProgressText();
            analysisProgressTimer->start();
        }
    }else analysisProgressTimer->stop();
}

void FeaturePanel::refreshProgressText() {
    // 耗时由界面计时器更新，大文件读写未产生新事件时仍持续显示。
    if(analysisProgressBox->isHidden()||!analysisElapsed.isValid())return;
    const qint64 seconds=analysisElapsed.elapsed()/1000;
    const auto elapsed=seconds>=3600?QString::number(seconds/3600)+u"小时 "_s+
        QString::number(seconds/60%60)+u"分 "_s+QString::number(seconds%60)+u"秒"_s:
        QString::number(seconds/60)+u"分 "_s+QString::number(seconds%60)+u"秒"_s;
    analysisProgressText->setText((analysisCancelling?u"正在停止，等待当前处理结束"_s:analysisProgressStage)+u" · 已用时 "_s+elapsed);
}

void FeaturePanel::progress(const QJsonObject &payload) {
    // 请求身份在工作台验证；此处只展示真实阶段计数，不估算整体百分比。
    if(!busy||analysisProgressBox->isHidden()||analysisCancelling)return;
    analysisProgressStage=payload.value(u"message"_s).toString();
    const double total=payload.value(u"total"_s).toDouble();
    const double completed=std::clamp(payload.value(u"completed"_s).toDouble(),0.,std::max(0.,total));
    if(total>0){
        analysisProgress->setRange(0,1000);
        analysisProgress->setValue(int(completed/total*1000));
        analysisProgressStage+=u" · 已处理 "_s+QString::number(completed,'f',0)+u" / "_s+
            QString::number(total,'f',0)+u" "_s+payload.value(u"unit"_s).toString(u"条"_s);
    }else analysisProgress->setRange(0,0);
    refreshProgressText();
}

void FeaturePanel::finishProgress(bool success, const QString &message) {
    // 只有收到最终结果才显示完成，取消与失败不显示满进度。
    if(analysisProgressBox->isHidden())return;
    analysisProgressTimer->stop();
    analysisCancelling=false;
    analysisProgressStage=message;
    analysisProgress->setRange(0,1000);
    analysisProgress->setValue(success?1000:0);
    analysisProgress->setFormat(success?u"已完成"_s:message.contains(u"取消"_s)?u"已取消"_s:u"未完成"_s);
    refreshProgressText();
}

void FeaturePanel::request(QJsonObject payload) {
    // 所有功能操作都通过运行器排队，界面线程只组织有界数据。
    if(busy||!available){failure(u"请等待当前任务结束"_s);return;}
    const auto action=payload.value(u"action"_s).toString();
    if(!action.startsWith(u"activation"_s)&&!modelLoaded){failure(u"请先加载模型"_s);return;}
    if(!payload.contains(u"branch_id"_s))payload[u"branch_id"_s]=branchId;
    payload[u"project_root"_s]=projectRoot;
    if(action.endsWith(u".preview"_s))previewRequest=payload;
    emit requested(payload);
}

void FeaturePanel::searchTokens(int offset) {
    // 保留筛选条件并按百条分页查询。
    request({{u"action"_s,u"search"_s},{u"query"_s,tokenQuery->text()},{u"exact"_s,exact->isChecked()},
        {u"special"_s,special->isChecked()},{u"offset"_s,offset}});
}

void FeaturePanel::queryVector(bool similar) {
    // 使用当前 token 和向量空间请求有界切片。
    if(tokenId<0){failure(u"请先选择 Token"_s);return;}
    request({{u"action"_s,similar?u"similar"_s:u"vector"_s},{u"token_id"_s,tokenId},
        {u"space"_s,space->currentData().toString()},{u"start"_s,vectorStart->value()},{u"count"_s,vectorCount->value()}});
}

void FeaturePanel::previewVector() {
    // 数值先在本地检查，服务端再次校验实际形状与存储精度。
    if(tokenId<0){failure(u"请先选择 Token"_s);return;}
    auto text=editValues->toPlainText();
    text.replace(QChar(u','),QChar(u' ')).replace(QChar(u'，'),QChar(u' ')).replace(QChar(u'\n'),QChar(u' '));
    QJsonArray values;
    for(const auto &part:text.split(QChar(u' '),Qt::SkipEmptyParts)){
        bool ok=false;
        const double value=part.toDouble(&ok);
        if(!ok||!std::isfinite(value)){failure(u"请填写有效的有限数值"_s);return;}
        values.append(value);
    }
    if(editKind->currentData().toString()==u"zero"_s)values=QJsonArray{0};
    request({{u"action"_s,u"vector.preview"_s},{u"token_id"_s,tokenId},{u"space"_s,space->currentData().toString()},
        {u"start"_s,editStart->value()},{u"stop"_s,editStop->value()},{u"kind"_s,editKind->currentData().toString()},{u"values"_s,values}});
}

void FeaturePanel::editWord() {
    // 支持的词表格式先收集新文本，再进入统一预览。
    if(tokenId<0){failure(u"请先选择 Token"_s);return;}
    if(!vocabularyEditable){failure(u"当前分词器仅支持查询；词表编辑暂支持普通 WordLevel 条目"_s);return;}
    auto *dialog=new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"修改词表"_s);
    auto *layout=column(dialog);
    layout->addWidget(new QLabel(u"Token "_s+QString::number(tokenId)));
    auto *text=new QLineEdit;
    text->setPlaceholderText(u"新词表文本"_s);
    layout->addWidget(text);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(u"预览"_s);
    buttons->button(QDialogButtonBox::Cancel)->setText(u"取消"_s);
    layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,dialog,[this,dialog,text]{
        request({{u"action"_s,u"vocabulary.preview"_s},{u"token_id"_s,tokenId},{u"text"_s,text->text()}});
        dialog->accept();
    });
    dialog->open();
}

void FeaturePanel::preview(const QJsonObject &payload) {
    // 预览确认是实际写入的最后一步，取消不产生任何修改。
    auto *dialog=new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"修改预览"_s);
    dialog->resize(560,390);
    auto *layout=column(dialog);
    layout->addWidget(hint(u"影响 "_s+QString::number(payload.value(u"count"_s).toInt())+u" 项；重叠补丁 "_s+QString::number(payload.value(u"overlaps"_s).toInt())));
    auto *values=table({u"序号"_s,u"修改前"_s,u"修改后"_s});
    const auto before=payload.value(u"before"_s), after=payload.value(u"after"_s);
    if(before.isArray()){
        const auto old=before.toArray(), edited=after.toArray();
        for(int i=0;i<old.size()&&i<edited.size();++i)row(values,{QString::number(i),number(old[i]),number(edited[i])});
    }else row(values,{u"词表条目"_s,before.toString(),after.toString()});
    layout->addWidget(values,1);
    layout->addWidget(hint(u"写入当前模型并生成备份；共享向量会同时影响输入和输出"_s));
    auto *buttons=new QDialogButtonBox;
    auto *save=buttons->addButton(u"保存到模型"_s,QDialogButtonBox::AcceptRole);
    auto *draft=buttons->addButton(u"保存草稿"_s,QDialogButtonBox::ActionRole);
    auto *cancel=buttons->addButton(u"取消"_s,QDialogButtonBox::RejectRole);
    auto confirmed=previewRequest;
    if(payload.contains(u"weights_revision"_s))confirmed[u"expected_weights_revision"_s]=payload.value(u"weights_revision"_s);
    for(const auto &key:{u"expected_model_revision"_s,u"expected_vocab_revision"_s,u"patch_id"_s})
        if(payload.contains(key))confirmed[key]=payload.value(key);
    confirmed[u"action"_s]=confirmed.value(u"action"_s).toString().replace(u".preview"_s,u".apply"_s);
    connect(save,&QPushButton::clicked,dialog,[this,dialog,confirmed]{request(confirmed);dialog->accept();});
    connect(draft,&QPushButton::clicked,dialog,[this,dialog,confirmed]{
        if(projectRoot.isEmpty()){failure(u"请先打开项目"_s);return;}
        if(drafts.size()>=128){failure(u"最多保存 128 个草稿"_s);return;}
        drafts.append(confirmed);saveDrafts();dialog->accept();
    });
    connect(cancel,&QPushButton::clicked,dialog,&QDialog::reject);
    layout->addWidget(buttons);
    dialog->open();
}

void FeaturePanel::result(const QJsonObject &payload) {
    // 响应由工作台按请求编号匹配，防止旧查询覆盖新选择。
    pending(false);
    finishProgress(true,u"处理完成"_s);
    const auto action=payload.value(u"action"_s).toString();
    notice->setText(u"已完成"_s);
    if(action.endsWith(u".apply"_s)&&previewRequest.contains(u"patch_id"_s)){
        const auto identity=previewRequest.value(u"patch_id"_s);
        bool removed=false;
        for(int i=int(drafts.size())-1;i>=0;--i)
            if(drafts[i].toObject().value(u"patch_id"_s)==identity){drafts.removeAt(i);removed=true;}
        if(removed)saveDrafts();
    }
    if(action==u"search"_s){
        tokenTable->setRowCount(0);
        tokenOffset=payload.value(u"offset"_s).toInt();tokenTotal=payload.value(u"total"_s).toInt();
        vocabularyEditable=payload.value(u"vocabulary_editable"_s).toBool();
        for(auto value:payload.value(u"rows"_s).toArray()){
            auto item=value.toObject();
            auto decoded=item.value(u"decoded"_s).toString();
            if(decoded.trimmed().isEmpty())decoded=u"空白字符"_s;
            decoded.replace(QChar(u'\n'),u"〔换行〕"_s).replace(QChar(u'\t'),u"〔制表〕"_s);
            row(tokenTable,{QString::number(item.value(u"id"_s).toInt()),item.value(u"text"_s).toString(),decoded,item.value(u"special"_s).toBool()?u"特殊"_s:u"普通"_s});
        }
        notice->setText(u"共 "_s+QString::number(tokenTotal)+u" 项；当前 "_s+QString::number(tokenOffset+1)+u"—"_s+QString::number(tokenOffset+tokenTable->rowCount()));
    }else if(action==u"vector"_s){
        vectorTable->setRowCount(0);
        const auto values=payload.value(u"values"_s).toArray();
        const int start=payload.value(u"start"_s).toInt();
        for(int i=0;i<values.size();++i)row(vectorTable,{QString::number(start+i),number(values[i])});
        QJsonArray matrix;
        for (int i = 0; i < values.size(); i += 8) {
            QJsonArray line;
            for (int j = i; j < std::min(i + 8, int(values.size())); ++j) line.append(values[j]);
            matrix.append(line);
        }
        vectorHeat->setMatrix(matrix);
        const QMap<QString, QString> precisions{{u"bfloat16"_s,u"脑浮点十六位"_s},{u"float16"_s,u"半精度"_s},{u"float32"_s,u"单精度"_s},{u"float64"_s,u"双精度"_s}};
        vectorHint->setText(u"Token "_s+QString::number(payload.value(u"token_id"_s).toInt())+u" · "_s+QString::number(payload.value(u"dimension"_s).toInt())+
            u" 维 · "_s+precisions.value(payload.value(u"dtype"_s).toString(),u"其他精度"_s)+(payload.value(u"shared"_s).toBool()?u" · 输入输出共享"_s:u" · 输入输出独立"_s));
        vectorHint->setToolTip(u"范数："_s+number(payload.value(u"norm"_s))+u"　最小："_s+number(payload.value(u"min"_s))+u"　最大："_s+number(payload.value(u"max"_s)));
        emit inspected(vectorHint->text()+u"\n\n词表文本："_s+payload.value(u"text"_s).toString()+u"\n解码："_s+payload.value(u"decoded"_s).toString()+
            u"\n\n"_s+vectorHint->toolTip()+u"\n\n关联文件：\n"_s+[&payload]{QStringList paths;for(const auto &value:payload.value(u"files"_s).toArray())paths<<value.toString();return paths.join(QChar(u'\n'));}()+u"\n\n修改区间的终点不包含在内。\n预览只显示前 64 项；保存草稿不改变模型。"_s);
    }else if(action==u"similar"_s){
        similarTable->setRowCount(0);
        for(auto value:payload.value(u"rows"_s).toArray()){
            const auto item=value.toObject();
            row(similarTable,{QString::number(item.value(u"id"_s).toInt()),item.value(u"text"_s).toString(),number(item.value(u"score"_s))});
        }
    }else if(action.endsWith(u".preview"_s))preview(payload);
    else if(action==u"activation.detail"_s){
        if(payload.contains(u"client_revision"_s)&&payload.value(u"client_revision"_s).toInt()!=activationRevision)return;
        // 每次翻页重建轻量弹窗，保留来源和屏幕位置，不把当前会话身份混入详情。
        auto *previous=findChild<QDialog *>(u"activationDetailDialog"_s);const QPoint previousPosition=previous?previous->pos():QPoint();
        if(previous){previous->setObjectName(QString());previous->close();}
        auto *dialog=new QDialog(this);dialog->setObjectName(u"activationDetailDialog"_s);
        dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"通道数值"_s);dialog->resize(680,500);
        auto *layout=column(dialog);
        layout->addWidget(hint(u"层 "_s+QString::number(payload.value(u"layer"_s).toInt())+u" · 通道 "_s+QString::number(payload.value(u"neuron"_s).toInt())+
            u" · 未采集 "_s+QString::number(payload.value(u"missing"_s).toInt())));
        auto *values=table({u"Token 位置"_s,u"干预前"_s,u"干预后"_s,u"生成步"_s});
        layout->addWidget(new ChannelTrend(payload.value(u"rows"_s).toArray(),dialog));
        for(auto value:payload.value(u"rows"_s).toArray()){
            const auto item=value.toObject();
            row(values,{QString::number(item.value(u"position"_s).toInt()),number(item.value(u"before"_s)),number(item.value(u"after"_s)),item.value(u"forward_id"_s).toString().mid(1)});
        }
        layout->addWidget(values,1);
        const int page=payload.value(u"page"_s).toInt(1),pages=payload.value(u"page_count"_s).toInt(1);
        layout->addWidget(hint(u"共 "_s+QString::number(payload.value(u"total"_s).toInt())+u" 个位置 · 第 "_s+QString::number(page)+u" / "_s+QString::number(pages)+u" 页 · 曲线显示当前页"_s));
        auto *paging=new QHBoxLayout;auto *target=spin(1,pages,page);
        QJsonObject query{{u"action"_s,u"activation.detail"_s},{u"layer"_s,payload.value(u"layer"_s)},{u"neuron"_s,payload.value(u"neuron"_s)}};
        for(const auto &key:{u"trace_path"_s,u"run_id"_s,u"positions"_s,u"branch_id"_s,u"client_revision"_s})if(payload.contains(key))query[key]=payload[key];
        auto load=[this,query](int next)mutable{if(busy||!available)return;query[u"page"_s]=next;request(query);};
        auto *previousPage=button(u"上一页"_s,[load,page]()mutable{load(page-1);},dialog);previousPage->setEnabled(page>1);
        auto *nextPage=button(u"下一页"_s,[load,page]()mutable{load(page+1);},dialog);nextPage->setEnabled(page<pages);
        paging->addWidget(previousPage);paging->addWidget(nextPage);paging->addWidget(target);paging->addWidget(button(u"跳转"_s,[load,target]()mutable{load(target->value());},dialog));layout->addLayout(paging);
        layout->addWidget(hint(u"摘要无法展开未采集的原始数值。"_s));
        dialog->open();if(previous)dialog->move(previousPosition);
    }
    else if(action==u"activation"_s||action==u"activation.query"_s||action==u"activation.open"_s){
        // 来源或项目已变化的旧响应不得覆盖新现场；失败处理也不清空已有结果。
        if(payload.contains(u"client_revision"_s)&&!payload.value(u"client_revision"_s).isNull()&&payload.value(u"client_revision"_s).toInt()!=activationRevision)return;
        const int selected=activationTable->currentRow();
        const auto previous=selected>=0&&selected<activationRows.size()?activationRows[selected].toObject():QJsonObject{};
        activationReport=payload;
        if(!activationReport.contains(u"branch_id"_s))activationReport[u"branch_id"_s]=branchId;
        activationRows=payload.value(u"rows"_s).toArray();activationTable->setRowCount(0);
        for(auto value:activationRows){
            const auto item=value.toObject();
            row(activationTable,{QString::number(item.value(u"layer"_s).toInt()),QString::number(item.value(u"neuron"_s).toInt()),number(item.value(u"score"_s)),
                number(item.value(u"peak"_s)),QString::number(item.value(u"position"_s).toInt()),item.value(u"trigger"_s).toString(),QString::number(item.value(u"samples"_s).toInt()),number(item.value(u"difference"_s))});
            if(!previous.isEmpty()&&item.value(u"layer"_s)==previous.value(u"layer"_s)&&item.value(u"neuron"_s)==previous.value(u"neuron"_s))activationTable->selectRow(activationTable->rowCount()-1);
        }
        const int page=payload.value(u"page"_s).toInt(1),pages=payload.value(u"page_count"_s).toInt(1);
        {QSignalBlocker block(activationPageNumber);activationPageNumber->setRange(1,pages);activationPageNumber->setValue(page);}
        activationPrevious->setEnabled(page>1);activationNext->setEnabled(page<pages);
        activationPagination->setText(u"共 "_s+QString::number(payload.value(u"total"_s).toInt(activationRows.size()))+u" 条 / 全部 "_s+
            QString::number(payload.value(u"unfiltered_total"_s).toInt(activationRows.size()))+u" 条 · 第 "_s+QString::number(page)+u" / "_s+QString::number(pages)+u" 页"_s);
        activationHint->setText(u"已采集 "_s+QString::number(payload.value(u"layers"_s).toInt())+u" 层 · 有效 Token "_s+QString::number(payload.value(u"effective_tokens"_s).toInt())+
            u" · 未采集 "_s+QString::number(payload.value(u"missing_samples"_s).toInt()));
        notice->setText(payload.value(u"note"_s).toString());
    }else if(payload.contains(u"patches"_s)&&action!=u"regression"_s&&action!=u"export.verify"_s){
        patchRows=payload.value(u"patches"_s).toArray();patchTable->setRowCount(0);
        for(int i=0;i<patchRows.size();++i){
            const auto item=patchRows[i].toObject(), target=item.value(u"target"_s).toObject();
            static const QMap<QString,QString> owners{{u"vectors"_s,u"向量"_s},{u"vocabulary"_s,u"词表"_s},{u"rules"_s,u"神经元"_s},{u"import"_s,u"导入"_s}};
            const auto range=target.isEmpty()?u"词表条目"_s:u"索引 "_s+QString::number(target.value(u"index"_s).toInt())+u"，区间 "_s+QString::number(target.value(u"start"_s).toInt())+u"—"_s+QString::number(target.value(u"stop"_s).toInt());
            row(patchTable,{QString::number(i+1),item.value(u"title"_s).toString(),owners.value(item.value(u"owner"_s).toString(),u"导入"_s),item.value(u"enabled"_s).toBool()?u"已启用"_s:u"已停用"_s,range});
        }
        versionHint->setText(payload.value(u"compatible"_s).toBool()?u"当前版本 "_s+payload.value(u"weights_revision"_s).toString():u"基座已被其他入口修改，不能重放旧补丁"_s);
        if(action.endsWith(u".apply"_s))notice->setText(u"已保存到模型，可在补丁与版本中撤销"_s);
    }else if(action==u"regression"_s||action==u"export.verify"_s){
        renderReport(payload);
        notice->setText(action==u"export.verify"_s?(payload.value(u"export"_s).toObject().value(u"passed"_s).toBool()?u"导出副本验收通过"_s:u"导出副本验收失败，请查看报告"_s):u"验证完成，报告已保存到项目"_s);
    }else if(action==u"report.read"_s&&payload.contains(u"samples"_s)){
        // MCP 重开报告与界面的报告选择使用同一张结果表。
        renderReport(payload);
        notice->setText(u"已打开报告"_s);
    }
}

void FeaturePanel::failure(const QString &message) {
    // 失败保留用户输入，并恢复页面操作。
    pending(false);
    finishProgress(false,message);
    notice->setText(message);
}

void FeaturePanel::addSample(const QJsonObject &sample) {
    // 类型和通过条件用中文选项，存储时转换为稳定的英文字段。
    const int r=sampleTable->rowCount();
    auto messages=sample.value(u"messages"_s).toArray();
    const auto input=sample.value(u"input"_s).toString(messages.isEmpty()?QString{}:messages.last().toObject().value(u"content"_s).toString());
    if(!messages.isEmpty())messages.removeLast();
    row(sampleTable,{QString{},input,sample.value(u"target"_s).toString(),QString{},
        sample.value(u"expected"_s).toString(sample.contains(u"threshold"_s)?number(sample.value(u"threshold"_s)):QString{}),
        QString::number(sample.value(u"max_new_tokens"_s).toInt(16)),sample.contains(u"max_probability_drop"_s)?number(sample.value(u"max_probability_drop"_s)):QString{}});
    sampleTable->item(r,1)->setData(Qt::UserRole,messages);
    sampleTable->item(r,1)->setData(Qt::UserRole+1,sample.value(u"generation"_s).toObject());
    sampleTable->item(r,6)->setToolTip(u"仅用于常规样例；留空不评估退化，需填写目标 Token"_s);
    auto *type=choices({u"目标样例"_s,u"常规样例"_s},{u"target"_s,u"regular"_s});
    type->setCurrentIndex(type->findData(sample.value(u"type"_s).toString(u"target"_s)));
    sampleTable->setCellWidget(r,0,type);
    auto *condition=choices({u"只看结果"_s,u"包含文本"_s,u"完全匹配"_s,u"正则匹配"_s,u"最低概率"_s,u"最高排名"_s},
        {u"none"_s,u"contains"_s,u"exact"_s,u"regex"_s,u"probability"_s,u"rank"_s});
    condition->setCurrentIndex(condition->findData(sample.value(u"criterion"_s).toString(u"none"_s)));
    condition->setToolTip(u"概率和排名在首个生成位置计算，目标必须是单个 Token"_s);
    sampleTable->setCellWidget(r,3,condition);
    auto *verdict=new QTableWidgetItem(u"未评估"_s);
    verdict->setFlags(verdict->flags() & ~Qt::ItemIsEditable);
    sampleTable->setItem(r,7,verdict);
}

QJsonArray FeaturePanel::samples() const {
    // 把中文表格选项转换为可持久化样例。
    QJsonArray result;
    for(int r=0;r<sampleTable->rowCount();++r){
        const auto type=qobject_cast<QComboBox*>(sampleTable->cellWidget(r,0));
        const auto condition=qobject_cast<QComboBox*>(sampleTable->cellWidget(r,3));
        auto text=[this,r](int col){return sampleTable->item(r,col)?sampleTable->item(r,col)->text():QString{};};
        const auto criterion=condition->currentData().toString();
        QJsonObject sample{{u"type"_s,type->currentData().toString()},{u"input"_s,text(1)},{u"target"_s,text(2)},
            {u"criterion"_s,criterion},{u"expected"_s,text(4)},{u"max_new_tokens"_s,text(5).toInt()}};
        sample[u"generation"_s]=sampleTable->item(r,1)->data(Qt::UserRole+1).toJsonObject();
        if(criterion==u"probability"_s||criterion==u"rank"_s){
            bool ok=false;const auto threshold=text(4).toDouble(&ok);
            sample[u"threshold"_s]=ok?QJsonValue(threshold):QJsonValue();
        }
        auto messages=sampleTable->item(r,1)->data(Qt::UserRole).toJsonArray();
        if(!messages.isEmpty()){
            messages.append(QJsonObject{{u"role"_s,u"user"_s},{u"content"_s,text(1)}});
            sample[u"messages"_s]=messages;
        }
        if(!text(6).trimmed().isEmpty()){
            bool ok=false;const double drop=text(6).toDouble(&ok);
            sample[u"max_probability_drop"_s]=ok?drop:-1;
        }
        result.append(sample);
    }
    return result;
}

void FeaturePanel::saveSamples() {
    // 以原子文件保存项目样例，避免覆盖半份数据。
    if(projectRoot.isEmpty()){failure(u"请先打开项目"_s);return;}
    QDir().mkpath(projectRoot+u"/features"_s);
    QSaveFile file(projectRoot+u"/features/samples.json"_s);
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(samples()).toJson())<0||!file.commit())failure(u"样例保存失败"_s);
    else notice->setText(u"样例已保存"_s);
}

void FeaturePanel::reloadProjectData() {
    // 外部项目操作完成后刷新同一份样例和草稿，不触发模型修改。
    loadSamples();loadDrafts();
}

void FeaturePanel::loadSamples() {
    // 切换项目时读取对应样例，不混用其他项目内容。
    sampleTable->setRowCount(0);
    if(projectRoot.isEmpty())return;
    QFile file(projectRoot+u"/features/samples.json"_s);
    if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)return;
    for(auto value:QJsonDocument::fromJson(file.readAll()).array())addSample(value.toObject());
}

void FeaturePanel::loadDrafts() {
    // 只读当前项目的有界草稿文件，草稿不代表已经生效。
    drafts={};draftTable->setRowCount(0);
    if(!projectRoot.isEmpty()){
        QFile file(projectRoot+u"/features/drafts.json"_s);
        if(file.open(QIODevice::ReadOnly)&&file.size()<=4*1024*1024)drafts=QJsonDocument::fromJson(file.readAll()).array();
    }
    for(int i=0;i<drafts.size();++i){
        const auto item=drafts[i].toObject();
        row(draftTable,{u"未应用 "_s+QString::number(i+1),QString::number(item.value(u"token_id"_s).toInt()),
            item.value(u"action"_s).toString().startsWith(u"vocabulary"_s)?u"词表条目"_s:QString::number(item.value(u"start"_s).toInt())+u"—"_s+QString::number(item.value(u"stop"_s).toInt())});
    }
}

void FeaturePanel::saveDrafts() {
    // 保存草稿只写项目文件，不改变模型或运行中的临时规则。
    QDir().mkpath(projectRoot+u"/features"_s);
    QSaveFile file(projectRoot+u"/features/drafts.json"_s);
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(drafts).toJson())<0||!file.commit()){failure(u"草稿保存失败"_s);return;}
    loadDrafts();notice->setText(u"草稿已保存，应用后才生效"_s);
}

void FeaturePanel::runRegression(bool exportModel) {
    // 选择当前样例进行基座对照或导出副本验收。
    if(projectRoot.isEmpty()){failure(u"请先打开项目"_s);return;}
    if(sampleTable->rowCount()==0){failure(u"请先添加样例"_s);return;}
    QJsonObject payload{{u"action"_s,exportModel?u"export.verify"_s:u"regression"_s},{u"samples"_s,samples()}};
    if(exportModel){
        const auto destination=QFileDialog::getSaveFileName(this,u"导出到新目录"_s,projectRoot+u"/模型副本"_s);
        if(destination.isEmpty())return;
        payload[u"destination"_s]=destination;
    }
    saveSamples();
    request(payload);
}

void FeaturePanel::renderReport(const QJsonObject &payload) {
    // 按实际判定展示结果，未设置条件不会自动通过。
    lastReport=payload;reportTable->setRowCount(0);
    sampleTable->setRowCount(0);
    for(auto value:payload.value(u"samples"_s).toArray())addSample(value.toObject());
    const auto rows=payload.value(u"rows"_s).toArray();
    for(int i=0;i<rows.size();++i){
        const auto item=rows[i].toObject(), before=item.value(u"base"_s).toObject(), after=item.value(u"edited"_s).toObject();
        const auto pass=after.value(u"passed"_s);
        QString verdict=pass.isBool()?(pass.toBool()?u"通过"_s:u"失败"_s):u"未评估"_s;
        if(after.value(u"status"_s).toString()==u"not_applicable"_s)verdict=u"不适用"_s;
        else if(after.value(u"status"_s).toString()==u"failed"_s)verdict=u"执行失败"_s;
        if(item.contains(u"consistent"_s))verdict=item.value(u"consistent"_s).toBool()?u"副本一致"_s:u"副本差异"_s;
        row(reportTable,{QString::number(i+1),before.value(u"text"_s).toString(),after.value(u"text"_s).toString(),number(before.value(u"probability"_s)),number(after.value(u"probability"_s)),verdict});
        if(i<sampleTable->rowCount())sampleTable->item(i,7)->setText(verdict);
    }
    if(sampleTable->rowCount())sampleTable->selectRow(0);
}

void FeaturePanel::editSample() {
    // 隐藏列仍保留原存储字段，编辑窗口只在保存时回写同一行。
    const int r=sampleTable->currentRow();
    if(r<0){failure(u"请选择一条样例"_s);return;}
    auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"编辑样例"_s);dialog->resize(560,430);
    auto *layout=column(dialog);auto *form=new QFormLayout;
    QMap<int,QLineEdit*> fields;
    for(int c:{1,2,4,5,6}){
        auto *field=new QLineEdit(sampleTable->item(r,c)->text());fields[c]=field;
        form->addRow(sampleTable->horizontalHeaderItem(c)->text(),field);
    }
    QMap<int,QComboBox*> options;
    for(int c:{0,3}){
        const auto *source=qobject_cast<QComboBox*>(sampleTable->cellWidget(r,c));auto *field=new QComboBox;
        for(int i=0;i<source->count();++i)field->addItem(source->itemText(i),source->itemData(i));
        field->setCurrentIndex(source->currentIndex());options[c]=field;form->addRow(sampleTable->horizontalHeaderItem(c)->text(),field);
    }
    layout->addLayout(form);auto *buttons=new QDialogButtonBox;
    auto *save=buttons->addButton(u"保存"_s,QDialogButtonBox::AcceptRole);buttons->addButton(u"取消"_s,QDialogButtonBox::RejectRole);
    connect(save,&QPushButton::clicked,dialog,[this,r,dialog,fields,options]{
        if(r>=sampleTable->rowCount()){dialog->reject();return;}
        for(auto it=fields.cbegin();it!=fields.cend();++it)sampleTable->item(r,it.key())->setText(it.value()->text());
        for(auto it=options.cbegin();it!=options.cend();++it)qobject_cast<QComboBox*>(sampleTable->cellWidget(r,it.key()))->setCurrentIndex(it.value()->currentIndex());
        dialog->accept();
    });connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);layout->addWidget(buttons);dialog->open();
}

void FeaturePanel::configureSample() {
    // 参数保存在样例自身，两边和导出副本读取完全相同的配置。
    const int r=sampleTable->currentRow();if(r<0){failure(u"请选择一条样例"_s);return;}
    const auto saved=sampleTable->item(r,1)->data(Qt::UserRole+1).toJsonObject();
    auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"样例生成设置"_s);
    auto *layout=column(dialog);auto *form=new QFormLayout;
    QMap<QString,QDoubleSpinBox*> fields;
    for(const auto &pair:{qMakePair(u"temperature"_s,u"温度（0 为贪心）"_s),qMakePair(u"top_p"_s,u"累计概率"_s),qMakePair(u"presence_penalty"_s,u"存在惩罚"_s),qMakePair(u"repetition_penalty"_s,u"重复惩罚"_s)}){
        auto *field=new QDoubleSpinBox;field->setDecimals(4);field->setRange(pair.first==u"top_p"_s||pair.first==u"repetition_penalty"_s?.0001:0,pair.first==u"top_p"_s?1:2);
        field->setValue(saved.value(pair.first).toDouble(pair.first==u"top_p"_s?.8:pair.first==u"repetition_penalty"_s?1:0));fields[pair.first]=field;form->addRow(pair.second,field);
    }
    auto *topK=spin(0,1000000,saved.value(u"top_k"_s).toInt(20));form->addRow(u"候选数量（0 为不限）"_s,topK);
    auto *seed=new QLineEdit(QString::number(saved.value(u"seed"_s).toInteger(42)));form->addRow(u"随机种子"_s,seed);layout->addLayout(form);
    auto *thinking=new QCheckBox(u"深度思考"_s);thinking->setChecked(saved.value(u"template_options"_s).toObject().value(u"enable_thinking"_s).toBool());layout->addWidget(thinking);
    auto *error=hint(QString{});layout->addWidget(error);auto *buttons=new QDialogButtonBox;
    auto *save=buttons->addButton(u"保存"_s,QDialogButtonBox::AcceptRole);buttons->addButton(u"取消"_s,QDialogButtonBox::RejectRole);
    connect(save,&QPushButton::clicked,dialog,[=]{
        bool ok=false;const auto number=seed->text().toLongLong(&ok);if(!ok||number<0){error->setText(u"随机种子须为非负整数"_s);return;}
        QJsonObject settings{{u"top_k"_s,topK->value()},{u"seed"_s,number}};
        settings[u"template_options"_s]=QJsonObject{{u"enable_thinking"_s,thinking->isChecked()}};
        for(auto it=fields.cbegin();it!=fields.cend();++it)settings[it.key()]=it.value()->value();
        if(r<sampleTable->rowCount())sampleTable->item(r,1)->setData(Qt::UserRole+1,settings);
        dialog->accept();
    });connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);layout->addWidget(buttons);dialog->open();
}
