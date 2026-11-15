#include "workbench.h"
#include "uiscale.h"
#include <QCheckBox>
#include <QTableView>
#include <QScrollBar>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

void Workbench::showRulePreview(const QJsonObject &payload) {
    // 摘要列出实际影响范围，切片可查询整个目标张量，确认后才整体提交。
    auto *dialog=new QDialog(this);dialog->setObjectName(u"rulePreviewDialog"_s);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"应用预览"_s);dialog->resize(840,700);
    auto *layout=new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(u"临时规则 "_s+QString::number(payload.value(u"temporary_count"_s).toInt())+u" 条 · 权重规则 "_s+QString::number(payload.value(u"model_count"_s).toInt())+u" 条"_s));
    // 临时干预没有可提前计算的真实激活值，先完整展示操作，不把空差异表当预览。
    auto *summary=new QTableWidget(0,7);summary->setObjectName(u"rulePreviewSummary"_s);
    summary->setHorizontalHeaderLabels({u"模型层"_s,u"通道"_s,u"类型"_s,u"修改方式"_s,u"数值"_s,u"作用范围"_s,u"状态"_s});
    summary->setEditTriggers(QAbstractItemView::NoEditTriggers);summary->verticalHeader()->hide();
    summary->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);summary->horizontalHeader()->setStretchLastSection(true);
    const QMap<QString,QString> kinds{{u"zero"_s,u"置零"_s},{u"scale"_s,u"缩放"_s},{u"set"_s,u"指定值"_s},{u"add"_s,u"增量"_s},{u"steer"_s,u"归一化引导"_s}};
    const QMap<QString,QString> scopes{{u"all"_s,u"全部 Token"_s},{u"generated"_s,u"生成阶段"_s},{u"position"_s,u"指定 Token"_s},{u"down"_s,u"下投影"_s},{u"up"_s,u"上投影"_s},{u"gate"_s,u"门控投影"_s}};
    const auto append=[summary,kinds,scopes](const QJsonObject &rule,const QString &state){
        const bool model=rule.value(u"target"_s).toString()==u"model"_s;const int row=summary->rowCount();summary->insertRow(row);
        auto range=scopes.value(rule.value(model?u"projection"_s:u"scope"_s).toString(),u"全部 Token"_s);
        if(!model&&rule.value(u"scope"_s).toString()==u"position"_s)range+=u" "_s+QString::number(rule.value(u"token_position"_s).toInt());
        if(model&&rule.value(u"kind"_s).toString()==u"set"_s)range+=u" / 维度 "_s+QString::number(rule.value(u"dimension"_s).toInt());
        const QStringList values{QString::number(rule.value(u"layer_index"_s).toInt()),QString::number(rule.value(u"neuron_index"_s).toInt()),model?u"修改模型"_s:u"临时干预"_s,kinds.value(rule.value(u"kind"_s).toString(),u"未知方式"_s),QString::number(rule.value(u"value"_s).toDouble(),'g',10),range,state};
        for(int c=0;c<values.size();++c)summary->setItem(row,c,new QTableWidgetItem(values[c]));
    };
    const auto draft=payload.value(u"rules"_s).toArray();
    for(const auto &value:draft){const auto rule=value.toObject();append(rule,rule.value(u"enabled"_s).toBool(true)?u"将应用"_s:u"未启用"_s);}
    const auto identity=[](const QJsonObject &r){return r.value(u"target"_s).toString()+u"/"_s+QString::number(r.value(u"layer_index"_s).toInt())+u"/"_s+QString::number(r.value(u"neuron_index"_s).toInt())+u"/"_s+r.value(u"projection"_s).toString();};
    for(const auto &value:payload.value(u"previous_rules"_s).toArray()){
        bool kept=false;for(const auto &next:draft)kept|=identity(next.toObject())==identity(value.toObject());
        if(!kept)append(value.toObject(),u"将移除"_s);
    }
    layout->addWidget(summary,1);
    auto *explanation=new QLabel;explanation->setObjectName(u"rulePreviewExplanation"_s);explanation->setWordWrap(true);
    explanation->setText(payload.value(u"temporary_count"_s).toInt()>0?u"临时干预应用后作用于后续符合条件的计算；实际激活变化需运行后查看。下方仅显示权重数值差异。"_s:
        draft.isEmpty()?u"将清空当前干预规则；已有规则将在上方标记为移除。"_s:
        payload.value(u"model_count"_s).toInt()==0?u"所有草稿规则均未启用；应用后将停用原配置中的对应规则。"_s:u"下方展示权重修改前后的真实数值。"_s);
    layout->addWidget(explanation);
    auto *targets=new QComboBox;const auto items=payload.value(u"targets"_s).toArray();
    for(const auto &v:items){const auto target=v.toObject();targets->addItem(target.value(u"target"_s).toString()+u" · 变更 "_s+QString::number(target.value(u"count"_s).toInt())+u" 项"_s,target);}
    layout->addWidget(targets);
    auto *range=new QHBoxLayout;auto *start=new QLineEdit(u"0"_s);auto *count=new QSpinBox;count->setRange(1,512);count->setValue(64);auto *query=new QPushButton(u"查询切片"_s);
    range->addWidget(new QLabel(u"展平起点"_s));range->addWidget(start,1);range->addWidget(new QLabel(u"数量"_s));range->addWidget(count);range->addWidget(query);layout->addLayout(range);
    auto *table=new QTableWidget(0,3);table->setHorizontalHeaderLabels({u"坐标 / 展平索引"_s,u"修改前"_s,u"修改后"_s});table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(table,1);
    auto *hint=new QLabel(items.isEmpty()?u"本次没有权重数值变化。临时干预和规则状态以上方摘要为准。"_s:u"每个目标最多展示 64 项差异；查询切片可核对其他位置"_s);hint->setWordWrap(true);layout->addWidget(hint);
    if(items.isEmpty()){targets->hide();start->setEnabled(false);count->setEnabled(false);table->hide();}
    auto *buttons=new QDialogButtonBox;auto *commit=buttons->addButton(u"应用"_s,QDialogButtonBox::AcceptRole);buttons->addButton(u"取消"_s,QDialogButtonBox::RejectRole);layout->addWidget(buttons);
    const auto fill=[table](const QJsonArray &rows){table->setRowCount(rows.size());for(int i=0;i<rows.size();++i){const auto r=rows[i].toObject();const QStringList text{r.value(u"coordinate"_s).isArray()?QString::fromUtf8(QJsonDocument(r.value(u"coordinate"_s).toArray()).toJson(QJsonDocument::Compact)):QString::number(r.value(u"index"_s).toDouble(),'f',0),QString::number(r.value(u"before"_s).toDouble(),'g',10),QString::number(r.value(u"after"_s).toDouble(),'g',10)};for(int c=0;c<3;++c)table->setItem(i,c,new QTableWidgetItem(text[c]));}};
    connect(targets,&QComboBox::currentIndexChanged,dialog,[targets,fill](int){fill(targets->currentData().toJsonObject().value(u"samples"_s).toArray());});
    fill(targets->currentData().toJsonObject().value(u"samples"_s).toArray());query->setEnabled(!items.isEmpty());
    const auto preview=pendingRulePreview;auto request=std::make_shared<QString>();
    connect(query,&QPushButton::clicked,dialog,[this,preview,targets,start,count,query,commit,hint,request]{
        if(active||loading||editing){hint->setText(u"请等待当前任务结束"_s);return;}
        bool ok=false;const qint64 position=start->text().toLongLong(&ok);if(!ok||position<0){hint->setText(u"请填写有效起点"_s);return;}
        auto p=preview;p[u"action"_s]=u"rules.slice"_s;p[u"name"_s]=targets->currentData().toJsonObject().value(u"target"_s);p[u"start"_s]=double(position);p[u"count"_s]=count->value();
        editing=true;setActive(false);query->setEnabled(false);commit->setEnabled(false);*request=worker.send(u"feature.request"_s,p);previewSliceId=*request;
    });
    connect(&worker,&Transport::message,dialog,[this,request,query,commit,hint,fill](const QJsonObject &m){
        if(request->isEmpty()||m.value(u"request_id"_s).toString()!=*request)return;
        if(m.value(u"name"_s).toString()!=u"feature.result"_s&&m.value(u"kind"_s).toString()!=u"error"_s)return;
        request->clear();editing=false;setActive(false);query->setEnabled(true);commit->setEnabled(true);const auto p=m.value(u"payload"_s).toObject();
        if(m.value(u"kind"_s).toString()==u"error"_s){hint->setText(p.value(u"message"_s).toString());return;}
        QJsonArray rows;const auto before=p.value(u"before"_s).toArray(),after=p.value(u"after"_s).toArray();for(int i=0;i<before.size();++i)rows.append(QJsonObject{{u"index"_s,p.value(u"start"_s).toDouble()+i},{u"before"_s,before[i]},{u"after"_s,after[i]}});fill(rows);hint->setText(u"目标共 "_s+QString::number(p.value(u"total"_s).toDouble(),'f',0)+u" 项；当前切片保留未变化值"_s);
    });
    connect(commit,&QPushButton::clicked,dialog,[this,dialog,preview]{if(active||loading||editing)return;editing=true;setActive(false);ruleRequestId=worker.send(u"feature.request"_s,preview);dialog->accept();});
    connect(buttons,&QDialogButtonBox::rejected,dialog,[dialog,request]{if(request->isEmpty())dialog->reject();});dialog->open();
}

QWidget *Workbench::buildEditing() {
    // 主面板只展示规则和状态，全部配置集中在同一个新增、修改弹窗。
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(13,13,13,13);
    layout->setSpacing(9);
    editHint = new QLabel(u"草稿未应用"_s);
    editHint->setWordWrap(true);
    layout->addWidget(editHint);
    rules = new QTableWidget(0,5);
    rules->setHorizontalHeaderLabels({u"启用 / 目标"_s,u"方式 / 范围"_s,u"数值"_s,u"类型"_s,u"状态"_s});
    rules->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    rules->horizontalHeader()->setStretchLastSection(true);
    rules->verticalHeader()->hide();
    rules->setSelectionBehavior(QAbstractItemView::SelectRows);
    rules->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rules->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(rules,1);
    // 冻结启用与目标列，复用同一数据模型和选择模型，横向滚动只移动右侧内容。
    frozenRules = new QTableView(rules);
    frozenRules->setObjectName(u"frozenRuleChecks"_s);
    frozenRules->setModel(rules->model());
    frozenRules->setSelectionModel(rules->selectionModel());
    frozenRules->setSelectionBehavior(QAbstractItemView::SelectRows);
    frozenRules->setEditTriggers(QAbstractItemView::NoEditTriggers);
    frozenRules->setFocusPolicy(Qt::NoFocus);
    frozenRules->verticalHeader()->hide();
    frozenRules->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    frozenRules->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    frozenRules->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    frozenRules->setContextMenuPolicy(Qt::CustomContextMenu);
    for (int column = 1; column < rules->columnCount(); ++column) frozenRules->hideColumn(column);
    rules->installEventFilter(this);
    connect(rules->horizontalHeader(), &QHeaderView::sectionResized, this, [this] { updateFrozenRules(); });
    connect(rules->verticalHeader(), &QHeaderView::sectionResized, this, [this](int row, int, int height) { frozenRules->setRowHeight(row, height); });
    connect(rules->verticalScrollBar(), &QScrollBar::valueChanged, frozenRules->verticalScrollBar(), &QScrollBar::setValue);
    connect(frozenRules->verticalScrollBar(), &QScrollBar::valueChanged, rules->verticalScrollBar(), &QScrollBar::setValue);
    connect(frozenRules, &QTableView::doubleClicked, this, [this](const QModelIndex &index) { editRule(index.row()); });
    connect(frozenRules, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto index = frozenRules->indexAt(point);
        if (index.isValid()) emit rules->customContextMenuRequested(rules->viewport()->mapFromGlobal(frozenRules->viewport()->mapToGlobal(point)));
    });
    updateFrozenRules();
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(u"添加规则"_s);
    apply = new QPushButton(u"预览并应用"_s);
    apply->setObjectName(u"primary"_s);
    clearEdit = new QPushButton(u"清空草稿"_s);
    layout->removeWidget(editHint);
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(editHint); toolbar->addStretch(); toolbar->addWidget(add);
    layout->insertLayout(0, toolbar);
    actions->addStretch(); actions->addWidget(clearEdit); actions->addWidget(apply);
    layout->addLayout(actions);
    connect(add,&QPushButton::clicked,this,[this]{editRule();});
    connect(apply,&QPushButton::clicked,this,&Workbench::applyRules);
    connect(clearEdit,&QPushButton::clicked,this,[this]{
        if(active||editing||loading)return;
        rules->setRowCount(0); refreshRuleStates(); writeProjectState(false);
    });
    connect(rules,&QTableWidget::itemChanged,this,[this](QTableWidgetItem *item){
        if(item->column()!=0)return;
        auto rule=item->data(Qt::UserRole).toJsonObject();
        rule[u"enabled"_s]=item->checkState()==Qt::Checked;
        {QSignalBlocker block(rules);item->setData(Qt::UserRole,rule);}
        refreshRuleStates();
        if(!restoringSession)writeProjectState(false);
    });
    connect(rules,&QTableWidget::customContextMenuRequested,this,[this](const QPoint &point){
        const int row=rules->indexAt(point).row();
        if(row<0||active||editing||loading)return;
        auto *menu=new QMenu(rules);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(u"修改"_s,this,[this,row]{editRule(row);});
        menu->addAction(u"删除"_s,this,[this,row]{rules->removeRow(row);refreshRuleStates();writeProjectState(false);});
        menu->popup(rules->viewport()->mapToGlobal(point));
    });
    connect(rules,&QTableWidget::cellDoubleClicked,this,[this](int row,int){editRule(row);});

    ruleDialog = new QDialog(this);
    ruleDialog->setModal(true);
    UiScale::minimumWidth(ruleDialog,440);
    auto *dialogLayout=new QVBoxLayout(ruleDialog);
    auto *form=new QFormLayout;
    editTarget=new QComboBox;
    editTarget->addItem(u"临时编辑"_s,u"temporary"_s);
    editTarget->addItem(u"修改模型"_s,u"model"_s);
    ruleLayer=new QSpinBox;ruleLayer->setRange(0,100000);
    ruleNeuron=new QSpinBox;ruleNeuron->setRange(0,1000000);
    rulePosition=new QSpinBox;rulePosition->setRange(0,10000000);
    ruleDimension=new QSpinBox;ruleDimension->setRange(0,1000000);
    ruleKind=new QComboBox;
    ruleScope=new QComboBox;
    ruleScope->addItem(u"指定 Token"_s,u"position"_s);
    ruleScope->addItem(u"全部 Token"_s,u"all"_s);
    ruleScope->addItem(u"生成阶段"_s,u"generated"_s);
    projection=new QComboBox;
    projection->addItem(u"下投影"_s,u"down"_s);
    projection->addItem(u"门控投影"_s,u"gate"_s);
    projection->addItem(u"上投影"_s,u"up"_s);
    ruleValue=new QDoubleSpinBox;ruleValue->setRange(-1e12,1e12);ruleValue->setDecimals(8);
    form->addRow(u"编辑类型"_s,editTarget);form->addRow(u"模型层（从 0 开始）"_s,ruleLayer);
    form->addRow(u"神经元通道"_s,ruleNeuron);form->addRow(u"修改方式"_s,ruleKind);
    form->addRow(u"作用范围"_s,ruleScope);form->addRow(u"Token 位置"_s,rulePosition);
    form->addRow(u"参数投影"_s,projection);form->addRow(u"系数维度"_s,ruleDimension);
    form->addRow(u"修改值 / 强度"_s,ruleValue);dialogLayout->addLayout(form);
    ruleError=new QLabel;ruleError->setWordWrap(true);ruleError->setStyleSheet(u"color:#edb1a5"_s);
    dialogLayout->addWidget(ruleError);
    auto *buttons=new QDialogButtonBox;
    auto *save=buttons->addButton(u"确定"_s,QDialogButtonBox::AcceptRole);
    buttons->addButton(u"取消"_s,QDialogButtonBox::RejectRole);
    connect(buttons,&QDialogButtonBox::rejected,ruleDialog,&QDialog::reject);
    connect(save,&QPushButton::clicked,this,[this]{addRule();});
    dialogLayout->addWidget(buttons);
    connect(editTarget,&QComboBox::currentIndexChanged,this,&Workbench::updateEditType);
    connect(ruleKind,&QComboBox::currentIndexChanged,this,[this]{
        ruleDimension->setEnabled(editTarget->currentIndex()==1&&ruleKind->currentData().toString()==u"set"_s);
    });
    connect(ruleScope,&QComboBox::currentIndexChanged,this,[this]{rulePosition->setEnabled(editTarget->currentIndex()==0&&ruleScope->currentIndex()==0);});
    updateEditType();
    return panel;
}

void Workbench::updateFrozenRules() {
    // 表格缩放、表头变化时保持固定列覆盖首列，纵向位置与原表同步。
    if (!frozenRules) return;
    frozenRules->setColumnWidth(0, rules->columnWidth(0));
    frozenRules->horizontalHeader()->setFixedHeight(rules->horizontalHeader()->height());
    frozenRules->setGeometry(rules->frameWidth(), rules->frameWidth(),
        std::min(rules->columnWidth(0) + 2 * frozenRules->frameWidth(), rules->viewport()->width()),
        rules->viewport()->height() + rules->horizontalHeader()->height());
    frozenRules->raise();
}

void Workbench::updateEditType() {
    // 类型切换只修改弹窗控件，不删除列表中的另一类规则。
    const bool model=editTarget->currentIndex()==1;
    ruleKind->clear();
    ruleKind->addItem(model?u"通道权重置零"_s:u"激活置零"_s,u"zero"_s);
    ruleKind->addItem(model?u"通道权重缩放"_s:u"激活缩放"_s,u"scale"_s);
    ruleKind->addItem(model?u"指定系数值"_s:u"指定激活值"_s,u"set"_s);
    if(!model){ruleKind->addItem(u"激活增量"_s,u"add"_s);ruleKind->addItem(u"残差归一化引导"_s,u"steer"_s);}
    ruleScope->setEnabled(!model);rulePosition->setEnabled(!model&&ruleScope->currentIndex()==0);
    projection->setEnabled(model);ruleDimension->setEnabled(false);
}

void Workbench::editRule(int row) {
    // 修改使用原参数，取消不会改动草稿。
    if(active||loading||editing)return;
    editingRuleRow=row;
    ruleError->clear();
    ruleDialog->setWindowTitle(row<0?u"添加规则"_s:u"修改规则"_s);
    if(row>=0){
        const auto rule=rules->item(row,0)->data(Qt::UserRole).toJsonObject();
        editTarget->setCurrentIndex(rule.value(u"target"_s).toString()==u"model"_s?1:0);
        ruleLayer->setValue(rule.value(u"layer_index"_s).toInt());ruleNeuron->setValue(rule.value(u"neuron_index"_s).toInt());
        ruleKind->setCurrentIndex(ruleKind->findData(rule.value(u"kind"_s).toString()));
        ruleScope->setCurrentIndex(ruleScope->findData(rule.value(u"scope"_s).toString(u"all"_s)));
        projection->setCurrentIndex(projection->findData(rule.value(u"projection"_s).toString(u"down"_s)));
        rulePosition->setValue(rule.value(u"token_position"_s).toInt());ruleDimension->setValue(rule.value(u"dimension"_s).toInt());
        ruleValue->setValue(rule.value(u"value"_s).toDouble());
    }
    ruleDialog->open();
}

QJsonArray Workbench::ruleDraft() const {
    // 界面草稿包含停用条目，以便重启后继续编辑和校验重复目标。
    QJsonArray draft;
    for(int i=0;i<rules->rowCount();++i)draft.append(rules->item(i,0)->data(Qt::UserRole).toJsonObject());
    return draft;
}

void Workbench::appendRule(const QJsonObject &input) {
    // 数据保存在首列，兼容旧的会话及 MCP 数据读取入口。
    QSignalBlocker block(rules);
    auto rule=input;
    if(!rule.contains(u"target"_s))rule[u"target"_s]=u"temporary"_s;
    if(!rule.contains(u"enabled"_s))rule[u"enabled"_s]=true;
    const int r=rules->rowCount();rules->insertRow(r);
    auto *target=new QTableWidgetItem(u"层 "_s+QString::number(rule.value(u"layer_index"_s).toInt())+u" / 通道 "_s+QString::number(rule.value(u"neuron_index"_s).toInt()));
    target->setFlags(target->flags()|Qt::ItemIsUserCheckable);
    target->setCheckState(rule.value(u"enabled"_s).toBool()?Qt::Checked:Qt::Unchecked);
    target->setData(Qt::UserRole,rule);rules->setItem(r,0,target);
    const QMap<QString,QString> kinds{{u"zero"_s,u"置零"_s},{u"scale"_s,u"缩放"_s},{u"set"_s,u"指定值"_s},{u"add"_s,u"增量"_s},{u"steer"_s,u"归一化引导"_s}};
    const bool model=rule.value(u"target"_s).toString()==u"model"_s;
    const QMap<QString,QString> scopes{{u"all"_s,u"全部 Token"_s},{u"generated"_s,u"生成阶段"_s},{u"position"_s,u"指定 Token"_s},{u"down"_s,u"下投影"_s},{u"up"_s,u"上投影"_s},{u"gate"_s,u"门控投影"_s}};
    rules->setItem(r,1,new QTableWidgetItem(kinds.value(rule.value(u"kind"_s).toString(),u"未知方式"_s)+u" / "_s+scopes.value(rule.value(model?u"projection"_s:u"scope"_s).toString())));
    rules->setItem(r,2,new QTableWidgetItem(QString::number(rule.value(u"value"_s).toDouble(),'g',8)));
    rules->setItem(r,3,new QTableWidgetItem(model?u"修改模型"_s:u"临时编辑"_s));
    rules->setItem(r,4,new QTableWidgetItem);
}

void Workbench::loadRuleDraft(const QJsonArray &draft) {
    // 恢复项目草稿不触发应用，旧生效配置仍由后端确认。
    QSignalBlocker block(rules);
    rules->setRowCount(0);
    for(const auto &value:draft)appendRule(value.toObject());
    refreshRuleStates();
}

void Workbench::refreshRuleStates(const QString &failure) {
    // 全列表原子提交，任一草稿变化都需重新应用；状态始终带中文说明。
    QSignalBlocker block(rules);
    const bool changed=ruleDraft()!=appliedRuleDraft;
    for(int i=0;i<rules->rowCount();++i){
        const auto rule=rules->item(i,0)->data(Qt::UserRole).toJsonObject();
        rules->item(i,4)->setText(!failure.isEmpty()?u"应用失败"_s:changed?u"待应用"_s:rule.value(u"enabled"_s).toBool()?u"已应用"_s:u"未启用"_s);
        rules->item(i,4)->setToolTip(failure);
    }
    editHint->setText(!failure.isEmpty()?u"应用失败；上次配置仍生效："_s+failure:changed?u"草稿待应用；上次配置仍生效"_s:u"当前配置已应用"_s);
}

void Workbench::addRule() {
    // 保存前排除自身检查唯一目标，启用和停用条目均参与。
    QJsonObject rule{{u"target"_s,editTarget->currentData().toString()},{u"enabled"_s,true},
        {u"layer_index"_s,ruleLayer->value()},{u"neuron_index"_s,ruleNeuron->value()},
        {u"kind"_s,ruleKind->currentData().toString()},{u"scope"_s,ruleScope->currentData().toString()},
        {u"token_position"_s,rulePosition->value()},{u"projection"_s,projection->currentData().toString()},
        {u"dimension"_s,ruleDimension->value()},{u"value"_s,ruleValue->value()}};
    const bool dialog=ruleDialog->isVisible();
    const int own=dialog?editingRuleRow:-1;
    const auto fail=[this,dialog](const QString &text){if(dialog)ruleError->setText(text);else showError(text);};
    if(own<0&&rules->rowCount()>=128){fail(u"最多添加 128 条规则"_s);return;}
    for(int i=0;i<rules->rowCount();++i){
        if(i==own)continue;
        const auto other=rules->item(i,0)->data(Qt::UserRole).toJsonObject();
        if(other.value(u"target"_s)==rule.value(u"target"_s)&&other.value(u"layer_index"_s)==rule.value(u"layer_index"_s)&&
            other.value(u"neuron_index"_s)==rule.value(u"neuron_index"_s)&&(editTarget->currentIndex()==0||other.value(u"projection"_s)==rule.value(u"projection"_s))){
            fail(u"目标重复：第 "_s+QString::number(ruleLayer->value())+u" 层，通道 "_s+QString::number(ruleNeuron->value()));return;
        }
    }
    if(own>=0){rule[u"enabled"_s]=rules->item(own,0)->checkState()==Qt::Checked;auto draft=ruleDraft();draft[own]=rule;loadRuleDraft(draft);}
    else appendRule(rule);
    refreshRuleStates();
    if(dialog)ruleDialog->accept();
    if(!restoringSession)writeProjectState(false);
}

void Workbench::applyRules() {
    // 先获取真实预览，用户核对后提交同一草稿及来源版本。
    if(active||loading||editing||offline||!loaded){showError(u"请先加载模型并停止当前任务"_s);return;}
    editing=true;setActive(false);
    pendingRulePreview={{u"action"_s,u"rules.preview"_s},{u"rules"_s,ruleDraft()},{u"expected_weights_revision"_s,weightsRevision}};
    ruleRequestId=worker.send(u"feature.request"_s,pendingRulePreview);
}

void Workbench::syncExternalEdit(const QJsonObject &payload) {
    // 外部已应用配置只替换相应类型，保留另一类型的草稿。
    const auto target=payload.value(u"target"_s).toString(u"temporary"_s);
    QJsonArray draft;
    for(const auto &value:ruleDraft())if(value.toObject().value(u"target"_s).toString()!=target)draft.append(value);
    const auto incoming=target==u"model"_s?payload.value(u"rules"_s).toArray():temporaryRules;
    for(const auto &value:incoming){auto rule=value.toObject();rule[u"target"_s]=target;rule[u"enabled"_s]=true;draft.append(rule);}
    appliedRuleDraft=draft;loadRuleDraft(draft);
}
