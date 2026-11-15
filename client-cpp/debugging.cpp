#include "workbench.h"
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <memory>

using namespace Qt::StringLiterals;

void Workbench::configureDebug() {
    // 沿用当前检查位置作为默认目标，候选神经元定位后可直接设置监视。
    if(!loaded||offline){showError(u"请先在项目中加载模型"_s);return;}
    auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"断点与监视"_s);dialog->resize(700,520);
    auto *layout=new QVBoxLayout(dialog);
    auto *hint=new QLabel(u"运行到：计算前暂停。条件监视：采集完成后暂停，命中一次。"_s);hint->setWordWrap(true);layout->addWidget(hint);
    auto points=std::make_shared<QJsonArray>(debugPoints);
    auto *table=new QTableWidget(0,4);table->setHorizontalHeaderLabels({u"方式"_s,u"目标"_s,u"生成步"_s,u"条件"_s});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(table,1);
    auto render=[table,points]{
        table->setRowCount(0);
        for(const auto &value:*points){const auto p=value.toObject();const int r=table->rowCount();table->insertRow(r);
            const auto watch=p.value(u"watch"_s).toString();
            table->setItem(r,0,new QTableWidgetItem(watch==u"channel"_s?u"通道监视"_s:watch==u"token"_s?u"Token 概率"_s:u"运行到"_s));
            table->setItem(r,1,new QTableWidgetItem(p.value(u"layer_index"_s).toInt()==-1?u"词表输出"_s:u"第 "_s+QString::number(p.value(u"layer_index"_s).toInt())+u" 层"_s));
            table->setItem(r,2,new QTableWidgetItem(p.contains(u"generation_step"_s)?QString::number(p.value(u"generation_step"_s).toInt()):u"不限"_s));
            const QMap<QString,QString> comparisons{{u"gt"_s,u">"_s},{u"ge"_s,u"≥"_s},{u"lt"_s,u"<"_s},{u"le"_s,u"≤"_s}};
            table->setItem(r,3,new QTableWidgetItem(watch.isEmpty()?u"计算前"_s:QString::number(p.value(u"index"_s).toInt())+u"："_s+comparisons.value(p.value(u"comparison"_s).toString())+QString::number(p.value(u"threshold"_s).toDouble())));
        }
    };render();
    auto *form=new QFormLayout;
    auto *kind=new QComboBox;kind->addItems({u"运行到"_s,u"通道监视"_s,u"Token 概率"_s});
    auto *targetLayer=new QSpinBox;targetLayer->setRange(-1,info.value(u"config"_s).toObject().value(u"num_hidden_layers"_s).toInt(1)-1);targetLayer->setSpecialValueText(u"不限"_s);targetLayer->setValue(layer);
    auto *op=new QComboBox;op->addItem(u"不限算子"_s,QString{});
    for(int i=0;i<operationSelect->count();++i)op->addItem(operationSelect->itemText(i),operationSelect->itemData(i));
    op->setCurrentIndex(std::max(0,op->findData(operation)));
    auto *stepIndex=new QSpinBox;stepIndex->setRange(-1,1000000);stepIndex->setSpecialValueText(u"不限"_s);stepIndex->setValue(-1);
    auto *index=new QSpinBox;index->setRange(0,10000000);index->setValue(neuron->value());
    auto *comparison=new QComboBox;comparison->addItem(u"大于"_s,u"gt"_s);comparison->addItem(u"不小于"_s,u"ge"_s);comparison->addItem(u"小于"_s,u"lt"_s);comparison->addItem(u"不大于"_s,u"le"_s);
    auto *threshold=new QDoubleSpinBox;threshold->setRange(-1e12,1e12);threshold->setDecimals(8);threshold->setValue(.01);
    form->addRow(u"方式"_s,kind);form->addRow(u"层"_s,targetLayer);form->addRow(u"算子"_s,op);form->addRow(u"生成步（从 0 开始）"_s,stepIndex);
    form->addRow(u"通道 / Token 编号"_s,index);form->addRow(u"条件"_s,comparison);form->addRow(u"阈值"_s,threshold);layout->addLayout(form);
    auto update=[=]{const bool watch=kind->currentIndex()>0;op->setEnabled(!watch);targetLayer->setEnabled(kind->currentIndex()!=2);index->setEnabled(watch);comparison->setEnabled(watch);threshold->setEnabled(watch);};
    connect(kind,&QComboBox::currentIndexChanged,dialog,update);
    if(property("candidateWatch").toBool()){kind->setCurrentIndex(1);setProperty("candidateWatch",false);}update();
    auto *error=new QLabel;error->setStyleSheet(u"color:#edb1a5"_s);layout->addWidget(error);
    auto *actions=new QHBoxLayout;
    auto *add=new QPushButton(u"添加"_s);actions->addWidget(add);
    connect(add,&QPushButton::clicked,dialog,[=]{
        if(points->size()>=64){error->setText(u"最多设置 64 项"_s);return;}
        QJsonObject point;
        if(stepIndex->value()>=0)point[u"generation_step"_s]=stepIndex->value();
        if(kind->currentIndex()==0){
            if(targetLayer->value()>=0)point[u"layer_index"_s]=targetLayer->value();
            if(!op->currentData().toString().isEmpty())point[u"operation_id"_s]=op->currentData().toString();
            if(point.isEmpty()){error->setText(u"请指定层、算子或生成步"_s);return;}point[u"boundary"_s]=u"before"_s;
        }else{
            point[u"boundary"_s]=u"after"_s;point[u"watch"_s]=kind->currentIndex()==1?u"channel"_s:u"token"_s;
            point[u"layer_index"_s]=kind->currentIndex()==1?targetLayer->value():-1;
            point[u"operation_id"_s]=kind->currentIndex()==1?u"mlp"_s:u"sample"_s;
            point[u"index"_s]=index->value();point[u"comparison"_s]=comparison->currentData().toString();point[u"threshold"_s]=threshold->value();
        }
        points->append(point);error->clear();render();
    });
    auto *remove=new QPushButton(u"删除"_s);actions->addWidget(remove);
    connect(remove,&QPushButton::clicked,dialog,[=]{if(table->currentRow()>=0){points->removeAt(table->currentRow());render();}});
    auto *clear=new QPushButton(u"清空"_s);actions->addWidget(clear);connect(clear,&QPushButton::clicked,dialog,[=]{*points={};render();});
    actions->addStretch();auto *save=new QPushButton(u"应用"_s);actions->addWidget(save);
    connect(save,&QPushButton::clicked,dialog,[this,dialog,points]{worker.send(u"debug.configure"_s,{{u"points"_s,*points}});dialog->accept();});
    auto *cancel=new QPushButton(u"取消"_s);actions->addWidget(cancel);connect(cancel,&QPushButton::clicked,dialog,&QDialog::reject);
    layout->addLayout(actions);dialog->open();
}
