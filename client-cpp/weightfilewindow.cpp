#include "weightfilewindow.h"
#include "workbench.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QTimer>
#include <QStackedWidget>
#include "neurons.h"
#include <cmath>

using namespace Qt::StringLiterals;

void Workbench::openWeightFile(const QString &input){
    // 工作区使用原型中的权重文件页面，文件关联启动仍保留独立窗口。
    if(!fileWindow){
        fileWindow=new WeightFileWindow(this);
        connect(fileWindow,&WeightFileWindow::received,this,&Workbench::received);
    }
    if(property("fileOnly").toBool()){
        fileWindow->show();fileWindow->raise();
    }else{
        restoreRulePanels();
        shell->setCurrentIndex(1);
        workspaceViews->setCurrentWidget(mainSplit);
        if(!weightPage){weightPage=fileWindow->takeCentralWidget();centerViews->addWidget(weightPage);}
        navigator->hide();timelinePanel->hide();
        for(auto *control:observationControls)control->hide();
        centerViews->setCurrentWidget(weightPage);
        updatePageHeader(u"权重文件"_s);
        syncNavigation(9);placeConversation();
    }
    QString target=input;
    if(target.isEmpty()&&loaded&&!offline){
        // 分片索引包含整个模型的权重映射，优先于任意单个分片。
        QDir model(modelPath);
        const auto indexes=model.entryList({u"*.safetensors.index.json"_s},QDir::Files,QDir::Name);
        const auto weights=model.entryList({u"*.safetensors"_s},QDir::Files,QDir::Name);
        if(!indexes.isEmpty())target=model.filePath(indexes.first());
        else if(!weights.isEmpty())target=model.filePath(weights.first());
        else showError(u"当前模型目录没有可读取的 Safetensors 权重文件"_s);
    }
    if(!target.isEmpty())QTimer::singleShot(0,fileWindow,[this,target]{fileWindow->openFile(target);});
}

namespace {
// 文件浏览使用紧凑表格，与已有工作台保持相同的密度和配色。
QTableWidget *makeTable(const QStringList &headers){
    auto *table=new QTableWidget(0,headers.size());table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);return table;
}
// 缺失值在所有文件结果中保持明确的中文状态。
QString numeric(const QJsonValue &value){return value.isString()?value.toString():value.isDouble()?QString::number(value.toDouble(),'g',16):value.isBool()?value.toBool()?u"是"_s:u"否"_s:u"非有限数"_s;}
void append(QTableWidget *table,const QStringList &columns){
    const int r=table->rowCount();table->insertRow(r);
    for(int i=0;i<columns.size();++i)table->setItem(r,i,new QTableWidgetItem(columns[i]));
}
}

WeightFileWindow::WeightFileWindow(QWidget *parent):QMainWindow(parent),transport(u"file-worker"_s,this){
    // 独立窗口没有项目推理入口，也不会启动模型计算或显卡监控。
    setWindowFlag(Qt::Window);setWindowTitle(u"权重文件 · 析元"_s);setWindowIcon(qApp->windowIcon());resize(1100,730);setMinimumSize(760,520);
    setStyleSheet(u"QWidget{background:#25282d;color:#c3cede;font:12px 'Microsoft YaHei UI';} QTableWidget,QTextEdit,QLineEdit,QSpinBox{background:#22252a;border:1px solid #495463;selection-background-color:#344d5c;} QHeaderView::section{background:#2b313a;padding:7px;border:1px solid #39414d;} QPushButton{background:#303741;border:1px solid #586473;border-radius:2px;padding:6px 12px;} QPushButton:hover{background:#3a4652;}"_s);
    auto *panel=new QWidget;setCentralWidget(panel);auto *layout=new QVBoxLayout(panel);layout->setContentsMargins(16,16,16,16);layout->setSpacing(12);
    auto *toolbar=new QHBoxLayout;fileLabel=new QLabel(u"尚未打开权重文件"_s);toolbar->addWidget(fileLabel,1);
    auto *open=new QPushButton(u"打开权重"_s);toolbar->addWidget(open);
    connect(open,&QPushButton::clicked,this,[this]{const auto p=QFileDialog::getOpenFileName(this,u"打开权重文件"_s,QString{},u"权重文件 (*.safetensors *.index.json)"_s);if(!p.isEmpty())openFile(p);});
    auto *compare=new QPushButton(u"比较权重"_s);toolbar->addWidget(compare);
    connect(compare,&QPushButton::clicked,this,[this]{
        const auto p=QFileDialog::getOpenFileName(this,u"选择对照权重"_s,QString{},u"权重文件 (*.safetensors *.index.json)"_s);
        if(!p.isEmpty())submit(u"file.compare"_s,{{u"other"_s,p},{u"name"_s,selectedName()}});
    });
    auto *restore=new QPushButton(u"恢复备份"_s);toolbar->addWidget(restore);
    connect(restore,&QPushButton::clicked,this,[this]{
        const auto p=QFileDialog::getOpenFileName(this,u"选择析元备份"_s,lastBackup,u"权重备份 (*.bak)"_s);
        if(p.isEmpty())return;
        auto *confirm=new QMessageBox(QMessageBox::Question,u"恢复备份"_s,u"恢复所选备份？当前文件会先备份。"_s,QMessageBox::NoButton,this);
        confirm->setAttribute(Qt::WA_DeleteOnClose);auto *yes=confirm->addButton(u"恢复"_s,QMessageBox::AcceptRole);confirm->addButton(u"取消"_s,QMessageBox::RejectRole);
        connect(confirm,&QMessageBox::finished,this,[this,p,yes,confirm](int){if(confirm->clickedButton()==yes)submit(u"file.restore"_s,{{u"backup"_s,p},{u"expected_revision"_s,revision}});});confirm->open();
    });layout->addLayout(toolbar);
    auto *filters=new QHBoxLayout;search=new QLineEdit;search->setPlaceholderText(u"搜索张量名称"_s);filters->addWidget(search,1);
    auto *find=new QPushButton(u"搜索"_s);filters->addWidget(find);connect(find,&QPushButton::clicked,this,[this]{offset=0;list();});connect(search,&QLineEdit::returnPressed,this,[this]{offset=0;list();});
    for(bool next:{false,true}){auto *page=new QPushButton(next?u"下一页"_s:u"上一页"_s);filters->addWidget(page);connect(page,&QPushButton::clicked,this,[this,next]{offset=std::max(0,offset+(next?100:-100));list();});}
    layout->addLayout(filters);auto *split=new QSplitter(Qt::Vertical);
    tensors=makeTable({u"张量名称"_s,u"形状"_s,u"精度"_s});split->addWidget(tensors);
    connect(tensors,&QTableWidget::cellClicked,this,[this](int,int){slice();});
    auto *detail=new QWidget;auto *detailLayout=new QVBoxLayout(detail);detailLayout->setContentsMargins(8,0,0,0);
    auto *ranges=new QHBoxLayout;start=new QSpinBox;start->setRange(0,2147483647);count=new QSpinBox;count->setRange(1,65536);count->setValue(64);
    ranges->addWidget(new QLabel(u"起点"_s));ranges->addWidget(start);ranges->addWidget(new QLabel(u"数量"_s));ranges->addWidget(count);
    auto *read=new QPushButton(u"读取切片"_s);ranges->addWidget(read);connect(read,&QPushButton::clicked,this,&WeightFileWindow::slice);detailLayout->addLayout(ranges);
    detailLayout->insertWidget(0,new QLabel(u"张量切片"_s));
    sliceHeat=new Heatmap;detailLayout->addWidget(sliceHeat,1);
    values=makeTable({u"展平索引"_s,u"数值"_s});values->setParent(panel);values->hide();
    auto *numbers=new QPushButton(u"查看数值"_s);ranges->addWidget(numbers);
    connect(numbers,&QPushButton::clicked,this,[this]{
        // 数值表复用当前真实切片，窗口关闭后归还给页面继续更新。
        auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"张量数值"_s);dialog->resize(420,500);
        auto *layout=new QVBoxLayout(dialog);layout->addWidget(values);values->show();
        connect(dialog,&QDialog::finished,this,[this]{values->setParent(this);values->hide();});dialog->open();
    });
    editValues=new QTextEdit;editValues->setAcceptRichText(false);editValues->setPlaceholderText(u"修改数值，以空格或逗号分隔，从上方起点写入"_s);editValues->setMaximumHeight(90);detailLayout->addWidget(editValues);editValues->hide();
    auto *edit=new QPushButton(u"预览修改"_s);ranges->addWidget(edit);
    connect(edit,&QPushButton::clicked,this,[this]{if(!editValues->isVisible()){editValues->show();editValues->setFocus();}else preview();});
    split->addWidget(detail);split->setSizes({240,400});layout->addWidget(split,1);
    notice=new QLabel;notice->setWordWrap(true);layout->addWidget(notice);
    connect(&transport,&Transport::message,this,&WeightFileWindow::receive);
    connect(&transport,&Transport::failed,this,[this](const QString &text){
        // 运行器失败也结束外部请求，避免 MCP 永久等待。
        const auto pending=requests;requests.clear();notice->setText(text);
        for(auto it=pending.begin();it!=pending.end();++it)emit received({{u"request_id"_s,it.key()},{u"kind"_s,u"error"_s},{u"name"_s,u"request.failed"_s},{u"payload"_s,QJsonObject{{u"code"_s,u"WORKER_FAILED"_s},{u"message"_s,text}}}});
    });
    transport.start();
}

WeightFileWindow::~WeightFileWindow(){transport.stop();}

QString WeightFileWindow::selectedName() const {
    // 切片和写入只使用明确选中的张量名。
    return tensors->currentRow()<0?QString{}:tensors->item(tensors->currentRow(),0)->text();
}

void WeightFileWindow::openFile(const QString &value){submit(u"file.open"_s,{{u"path"_s,value}});}

QString WeightFileWindow::submit(const QString &name,const QJsonObject &payload,bool external){
    // 同一工作区串行操作；MCP 请求不触发需要用户点击的预览弹窗。
    if(busy()){notice->setText(u"请等待当前文件操作结束"_s);return {};}
    if(name==u"file.apply"_s||name==u"file.restore"_s){
        auto *owner=qobject_cast<Workbench*>(parent());
        if(owner&&owner->loaded&&QDir(owner->modelPath).exists()){
            const auto model=QDir(owner->modelPath).canonicalPath()+QChar(u'/');
            if(QFileInfo(path).canonicalFilePath().startsWith(model,Qt::CaseInsensitive)){
                notice->setText(u"该模型已在项目中加载，请在项目中修改或先关闭模型"_s);return {};
            }
        }
    }
    const auto id=transport.send(name,payload);requests[id]={name,external};notice->setText(u"正在处理…"_s);return id;
}

QJsonObject WeightFileWindow::state() const {
    // 暴露文件身份和版本，外部写入必须使用这个最新版本。
    return {{u"mode"_s,u"file"_s},{u"path"_s,path},{u"revision"_s,revision},{u"busy"_s,busy()},{u"last_backup"_s,lastBackup}};
}

void WeightFileWindow::list(){submit(u"file.list"_s,{{u"query"_s,search->text()},{u"offset"_s,offset},{u"limit"_s,100}});}

void WeightFileWindow::slice(){
    // 尾部切片自动缩短，避免默认数量超过小张量的实际大小。
    const int r=tensors->currentRow();if(r<0)return;
    const auto total=tensors->item(r,0)->data(Qt::UserRole).toDouble();
    const int length=int(std::min(double(count->value()),std::max(0.,total-start->value())));
    submit(u"file.slice"_s,{{u"name"_s,selectedName()},{u"start"_s,start->value()},{u"count"_s,length}});
}

void WeightFileWindow::preview(){
    // 解析数值后交给文件运行器校验实际存储精度，界面不直接写文件。
    if(selectedName().isEmpty()){notice->setText(u"请先选择张量"_s);return;}
    QJsonArray numbers;
    for(const auto &part:editValues->toPlainText().split(QRegularExpression(u"[\\s,，]+"_s),Qt::SkipEmptyParts)){
        bool ok=false;const double value=part.toDouble(&ok);
        if(!ok||!std::isfinite(value)){notice->setText(u"请填写有效的有限数值"_s);return;}numbers.append(value);
    }
    previewRequest={{u"name"_s,selectedName()},{u"start"_s,start->value()},{u"values"_s,numbers},{u"expected_revision"_s,revision}};
    submit(u"file.preview"_s,previewRequest);
}

void WeightFileWindow::renderList(const QJsonObject &payload){
    // 只显示当前页的张量元数据，文件再大也不加载整份参数。
    tensors->setRowCount(0);
    for(const auto &value:payload.value(u"rows"_s).toArray()){
        const auto item=value.toObject();QStringList shape;
        for(const auto &n:item.value(u"shape"_s).toArray())shape<<QString::number(n.toDouble(),'f',0);
        append(tensors,{item.value(u"name"_s).toString(),shape.join(u" × "_s),item.value(u"dtype_name"_s).toString()});
        tensors->item(tensors->rowCount()-1,0)->setData(Qt::UserRole,item.value(u"count"_s).toDouble());
    }
    fileLabel->setText(path);fileLabel->setToolTip(path);notice->setText(u"共 "_s+QString::number(payload.value(u"total"_s).toInt())+u" 个张量"_s);
}

void WeightFileWindow::receive(const QJsonObject &message){
    // 最终结果才能释放文件互斥；错误不会覆盖预览或当前文件身份。
    const auto id=message.value(u"request_id"_s).toString();
    if(!requests.contains(id)){emit received(message);return;}
    const auto pending=requests.take(id);const auto action=pending.first;const auto payload=message.value(u"payload"_s).toObject();
    if(message.value(u"kind"_s).toString()==u"error"_s){notice->setText(payload.value(u"message"_s).toString());emit received(message);return;}
    revision=payload.value(u"revision"_s).toString(revision);path=payload.value(u"path"_s).toString(path);
    lastBackup=payload.value(u"backup"_s).toString(payload.value(u"last_backup"_s).toString(lastBackup));
    if(action==u"file.open"_s||action==u"file.list"_s||action==u"file.restore"_s)renderList(payload);
    else if(action==u"file.slice"_s){
        values->setRowCount(0);const auto data=payload.value(u"values"_s).toArray();
        for(int i=0;i<data.size();++i)append(values,{QString::number(payload.value(u"start"_s).toInt()+i),numeric(data[i])});
        QJsonArray matrix;
        for(int i=0;i<data.size();i+=8){QJsonArray line;for(int j=i;j<std::min(i+8,int(data.size()));++j)line.append(data[j]);matrix.append(line);}
        sliceHeat->setMatrix(matrix,selectedName());
        notice->setText(payload.value(u"dtype_name"_s).toString());
    }
    else if(action==u"file.apply"_s){notice->setText(u"已保存；备份："_s+lastBackup);if(!pending.second)slice();}
    else if(action==u"file.preview"_s&&!pending.second){
        auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"修改预览"_s);dialog->resize(520,420);auto *layout=new QVBoxLayout(dialog);
        layout->addWidget(new QLabel(payload.value(u"name"_s).toString()+u" · "_s+QString::number(payload.value(u"count"_s).toInt())+u" 项"_s));
        auto *table=makeTable({u"索引"_s,u"修改前"_s,u"修改后"_s});const auto a=payload.value(u"before"_s).toArray(),b=payload.value(u"after"_s).toArray();
        for(int i=0;i<a.size()&&i<b.size();++i)append(table,{QString::number(payload.value(u"start"_s).toInt()+i),numeric(a[i]),numeric(b[i])});
        layout->addWidget(table,1);
        layout->addWidget(new QLabel(u"写入前完整备份；预览最多显示 64 项"_s));auto *buttons=new QDialogButtonBox;
        auto *save=buttons->addButton(u"保存"_s,QDialogButtonBox::AcceptRole);buttons->addButton(u"取消"_s,QDialogButtonBox::RejectRole);
        const auto request=previewRequest;connect(save,&QPushButton::clicked,dialog,[this,dialog,request]{submit(u"file.apply"_s,request);dialog->accept();});connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);layout->addWidget(buttons);dialog->open();
    }else if(action==u"file.compare"_s&&!pending.second){
        auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(u"权重比较"_s);dialog->resize(850,470);auto *layout=new QVBoxLayout(dialog);
        auto *table=makeTable({u"张量"_s,u"状态"_s,u"精度"_s,u"最大绝对差"_s,u"平均绝对差"_s,u"超出容差"_s});
        for(const auto &value:payload.value(u"rows"_s).toArray()){const auto row=value.toObject();const bool compared=row.value(u"status"_s).toString()==u"compared"_s;
            append(table,{row.value(u"name"_s).toString(),compared?u"已比较"_s:row.value(u"reason"_s).toString(),row.value(u"dtype_equal"_s).toBool()?u"一致"_s:u"不同"_s,
                compared?numeric(row.value(u"maximum_absolute"_s)):u"不适用"_s,compared?numeric(row.value(u"mean_absolute"_s)):u"不适用"_s,compared?QString::number(row.value(u"outside_tolerance"_s).toInt()):u"不适用"_s});}
        layout->addWidget(table,1);layout->addWidget(new QLabel(u"以 64 位浮点计算；绝对容差 0.00001，相对容差 0.0001"_s));dialog->open();
    }else notice->setText(u"文件操作完成"_s);
    emit received(message);
}
