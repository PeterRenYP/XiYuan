#include "workbench.h"
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QTableWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

QWidget *Workbench::buildComparison() {
    // 在原有分支页内补充真实数值对照，不改变工作区导航方向。
    auto *panel=new QWidget;
    auto *layout=new QVBoxLayout(panel);layout->setContentsMargins(8,8,8,8);
    auto *actions=new QHBoxLayout;
    comparisonMode=new QComboBox;
    comparisonMode->addItem(u"固定前缀"_s,u"fixed"_s);comparisonMode->addItem(u"自由生成"_s,u"free"_s);
    comparisonTarget=new QLineEdit;comparisonTarget->setPlaceholderText(u"目标 Token 文本或编号"_s);
    comparisonTarget->setToolTip(u"留空时比较原始分支本步选择的 Token"_s);
    comparisonSteps=new QSpinBox;comparisonSteps->setRange(1,64);comparisonSteps->setValue(4);
    actions->addWidget(comparisonMode);actions->addWidget(comparisonTarget,1);
    actions->addWidget(new QLabel(u"步数"_s));actions->addWidget(comparisonSteps);
    auto *run=new QPushButton(u"重跑对照"_s);actions->addWidget(run);
    connect(run,&QPushButton::clicked,this,&Workbench::runComparison);
    auto *open=new QPushButton(u"打开对照"_s);actions->addWidget(open);
    connect(open,&QPushButton::clicked,this,[this]{
        const auto path=QFileDialog::getOpenFileName(this,u"打开对照报告"_s,projectDirectory+u"/features"_s,u"对照报告 (*.json)"_s);
        if(path.isEmpty())return;
        QFile file(path);
        if(!file.open(QIODevice::ReadOnly)||file.size()>64*1024*1024){showError(u"报告不可读或过大"_s);return;}
        const auto report=QJsonDocument::fromJson(file.readAll()).object();
        if(!report.contains(u"base_tokens"_s)||!report.contains(u"rows"_s)){showError(u"不是对照报告"_s);return;}
        renderComparison(report);
    });
    layout->addLayout(actions);
    comparison=new QTextBrowser;comparison->setOpenLinks(false);comparison->setOpenExternalLinks(false);
    connect(comparison,&QTextBrowser::anchorClicked,this,[this](const QUrl &url){if(url.scheme()==u"step"_s)showComparisonDetail(url.path().toInt());});
    comparison->setPlainText(u"尚无分支对比结果"_s);
    comparison->setToolTip(u"完成对话并应用临时规则后，使用本轮输入重跑。采集点沿用当前选择。"_s);
    layout->addWidget(comparison,1);
    return panel;
}

void Workbench::runComparison() {
    // 两边读取同一个版本和生成配置，任务期间禁止修改模型与规则。
    if(active||loading||editing){showError(u"请等待当前任务结束"_s);return;}
    if(!loaded||offline||tracePath.isEmpty()){showError(u"请先完成一轮对话"_s);return;}
    editing=true;setActive(false);
    comparisonRequestId=worker.send(u"feature.request"_s,{{u"action"_s,u"comparison"_s},{u"mode"_s,comparisonMode->currentData().toString()},
        {u"target"_s,comparisonTarget->text()},{u"steps"_s,comparisonSteps->value()},{u"layer"_s,layer},
        {u"neuron"_s,neuron->value()},{u"operation"_s,operation},{u"project_root"_s,projectDirectory}});
    comparison->setPlainText(u"正在重跑对照…"_s);timelineTabs->setCurrentIndex(1);
}

void Workbench::renderComparison(const QJsonObject &report) {
    // 用双栏保留回答，数值按生成步和相同前缀对齐；不同上下文明确提示。
    comparisonReport=report;
    QString html=u"<p>"_s+(report.value(u"mode"_s).toString()==u"fixed"_s?u"固定前缀：共同前缀来自原始分支"_s:u"自由生成：两边分别生成"_s)+u"</p>"_s;
    if(!report.value(u"first_divergence"_s).isNull())html+=u"<p>首次 Token 分歧：第 "_s+QString::number(report.value(u"first_divergence"_s).toInt()+1)+u" 步；自由生成后续上下文可能不同。</p>"_s;
    html+=u"<table width='100%' border='1' cellspacing='0' cellpadding='7'><tr><th>原始分支</th><th>编辑分支</th></tr><tr><td width='50%'>"_s+
        report.value(u"base_text"_s).toString().toHtmlEscaped()+u"</td><td>"_s+report.value(u"edited_text"_s).toString().toHtmlEscaped()+u"</td></tr></table>"_s;
    html+=u"<p>第 "_s+QString::number(report.value(u"layer"_s).toInt())+u" 层 · 通道 "_s+QString::number(report.value(u"neuron"_s).toInt())+u" · 原始输出概率</p>"_s;
    html+=u"<table width='100%' border='1' cellspacing='0' cellpadding='5'><tr><th>步</th><th>目标 Token</th><th>原概率 / 排名</th><th>编辑概率 / 排名</th><th>张量最大绝对差</th><th>状态</th></tr>"_s;
    for(const auto &value:report.value(u"rows"_s).toArray()){
        const auto row=value.toObject(),base=row.value(u"base"_s).toObject(),edited=row.value(u"edited"_s).toObject();
        const bool available=row.value(u"status"_s).toString()==u"available"_s;
        html+=u"<tr><td>"_s+QString::number(row.value(u"step"_s).toInt()+1)+u"</td><td>"_s+row.value(u"target_text"_s).toString().toHtmlEscaped()+
            u"</td><td>"_s+QString::number(base.value(u"probability"_s).toDouble(),'g',7)+u" / "_s+QString::number(base.value(u"rank"_s).toInt())+
            u"</td><td>"_s+QString::number(edited.value(u"probability"_s).toDouble(),'g',7)+u" / "_s+QString::number(edited.value(u"rank"_s).toInt())+
            u"</td><td>"_s+(available?QString::number(row.value(u"maximum_absolute"_s).toDouble(),'g',7):u"不可比较"_s)+u"</td><td>"_s+
            (available?u"已对齐 <a href='step:"_s+QString::number(row.value(u"step"_s).toInt())+u"'>查看数值</a>"_s:row.value(u"reason"_s).toString().toHtmlEscaped())+u"</td></tr>"_s;
    }
    html+=u"</table><p>逐位置通道、张量切片、输入及环境记录已保存至："_s+report.value(u"path"_s).toString().toHtmlEscaped()+u"</p>"_s;
    comparison->setHtml(html);timelineTabs->setCurrentIndex(1);status(u"对照完成"_s);
}

void Workbench::showComparisonDetail(int step) {
    // 按生成步展示已保存的真实切片与候选通道，缺失数据不转成零。
    const auto rows=comparisonReport.value(u"rows"_s).toArray();
    if(step<0||step>=rows.size())return;
    const auto record=rows[step].toObject(),slice=record.value(u"slice"_s).toObject();
    auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"第 "_s+QString::number(step+1)+u" 步 · 数值对照"_s);dialog->resize(780,540);
    auto *layout=new QVBoxLayout(dialog);auto *mode=new QComboBox;mode->addItems({u"张量切片"_s,u"候选通道"_s});layout->addWidget(mode);
    auto *description=new QLabel(u"切片 "_s+QString::number(slice.value(u"start"_s).toInt())+u"—"_s+QString::number(slice.value(u"stop"_s).toInt())+u" / "_s+QString::number(slice.value(u"total"_s).toDouble(),'f',0));layout->addWidget(description);
    auto *table=new QTableWidget;table->setColumnCount(5);table->setHorizontalHeaderLabels({u"位置"_s,u"原始分支"_s,u"编辑前"_s,u"编辑后"_s,u"差值"_s});table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table,1);
    const auto fill=[table,record,slice](int index){
        table->setRowCount(0);
        const auto before=slice.value(u"before"_s).toArray(),after=slice.value(u"after"_s).toArray(),channel=record.value(u"channel"_s).toArray();
        const int count=index==0?before.size():channel.size();table->setRowCount(count);
        const auto number=[](const QJsonValue &v){return v.isDouble()?QString::number(v.toDouble(),'g',10):u"未采集"_s;};
        for(int i=0;i<count;++i){
            const auto point=index==1?channel[i].toObject():QJsonObject{};
            const QJsonValue a=index==0?before[i]:point.value(u"before"_s),b=index==0?after[i]:point.value(u"after"_s);
            const QStringList values{QString::number(index==0?slice.value(u"start"_s).toInt()+i:point.value(u"position"_s).toInt()),number(a),index==0?u"不适用"_s:number(point.value(u"edited_pre_intervention"_s)),number(b),a.isDouble()&&b.isDouble()?QString::number(b.toDouble()-a.toDouble(),'g',10):u"不可比较"_s};
            for(int c=0;c<values.size();++c)table->setItem(i,c,new QTableWidgetItem(values[c]));
        }
    };
    connect(mode,&QComboBox::currentIndexChanged,dialog,fill);fill(0);dialog->open();
}
