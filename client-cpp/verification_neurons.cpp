// 原生验收读取真实模型产生的样本；不把演示激活植入产品路径。
#include "workbench.h"
#include "neurons.h"
#include "uiscale.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTest>
#include <algorithm>
#include <memory>
#include <limits>

using namespace Qt::StringLiterals;

// 用真实样本驱动受控时钟，验收光束互不重置、结束独立以及高频输入下的刷新节奏。
struct NeuronAnimationVerification {
    static QJsonObject run(const QJsonObject &sample){
        QJsonObject checks;NeuronCanvas canvas;canvas.resize(1200,800);canvas.configure(sample.value(u"layers"_s).toArray());canvas.show();
        const auto frames=sample.value(u"frames"_s).toArray();for(const auto &frame:frames)canvas.applyFrame(frame.toObject());
        const auto original=canvas.networkPaths;QSet<qint64> starts;QSet<int> durations;int flights=0;
        for(const auto &path:original)if(canvas.inFlight(path,canvas.activityTime())){starts.insert(path.started);durations.insert(path.duration);++flights;}
        checks[u"光束错开发射并使用独立行进时长"_s]=starts.size()>10&&durations.size()>10;
        checks[u"全局光束受总预算约束"_s]=flights>20&&flights<=144;
        int ticks=0;const auto connection=QObject::connect(&canvas.timer,&QTimer::timeout,&canvas,[&]{++ticks;});
        // 每 5 毫秒重复送达数据，计时器仍须持续触发，不能不断被输入重新启动。
        for(int i=0;i<30;++i){for(const auto &frame:frames)canvas.applyFrame(frame.toObject());QTest::qWait(5);}
        checks[u"高频输入期间动画计时器持续推进"_s]=ticks>=3;
        QObject::disconnect(connection);bool unchanged=original.size()==canvas.networkPaths.size();
        for(int i=0;i<original.size()&&unchanged;++i)if(original[i].started>=0)unchanged=original[i].started==canvas.networkPaths[i].started&&original[i].duration==canvas.networkPaths[i].duration;
        checks[u"新数据不重置任何在途光束"_s]=unchanged;
        canvas.setFlat(true);
        int actual=0;bool exact=true,stable=true;
        for(int layer=0;layer<canvas.layers.size();++layer){
            const int count=canvas.layers[layer].toObject().value(u"count"_s).toInt();actual+=count;
            exact=exact&&canvas.flatLayers[layer].size()==count;
            for(int neuron=0;neuron<count;++neuron)stable=stable&&canvas.flatLayers[layer][neuron]==canvas.flatPoint(layer,neuron);
        }
        checks[u"平铺每层点数与实际神经元数量一致"_s]=exact&&canvas.nodeCount()==actual;
        checks[u"平铺点位稳定且与真实编号一一对应"_s]=stable;
        bool same=true;
        for(int i=0;i<original.size();++i)same=same&&original[i].started==canvas.networkPaths[i].started&&original[i].duration==canvas.networkPaths[i].duration;
        checks[u"切换视图保留每束光的独立进度"_s]=same;
        canvas.setActivityPaused(true);const auto paused=canvas.activityTime();QTest::qWait(50);
        checks[u"暂停冻结每束光的独立时钟"_s]=canvas.activityTime()==paused;
        const auto oldStarts=canvas.networkPaths;canvas.setActivityPaused(false);const auto resumed=canvas.activityTime();
        bool continuous=true;for(int i=0;i<oldStarts.size();++i)if(oldStarts[i].started>=0)continuous=continuous&&std::abs((paused-oldStarts[i].started)-(resumed-canvas.networkPaths[i].started))<=2;
        checks[u"恢复后各光束从原进度继续"_s]=continuous;
        // 改变输入的符号只影响下一次发射，已有光束保存触发时的正负颜色。
        bool colorStable=false;for(int i=0;i<canvas.networkPaths.size();++i){const auto before=canvas.networkPaths[i];if(before.started<0)continue;
            const auto &source=canvas.nodes[before.source];for(const auto &entry:frames){auto frame=entry.toObject();if(frame.value(u"layer_index"_s).toInt()!=source.layer)continue;
                const auto ids=frame.value(u"indices"_s).toArray();auto values=frame.value(u"values"_s).toArray();
                for(int j=0;j<ids.size();++j)if(ids[j].toInt()==source.neuron)values[j]=-values[j].toDouble();
                frame[u"values"_s]=values;canvas.applyFrame(frame);colorStable=canvas.networkPaths[i].negative==before.negative;break;}break;}
        checks[u"在途光束保持发射时颜色"_s]=colorStable;
        qint64 firstEnd=std::numeric_limits<qint64>::max(),lastEnd=0;
        for(const auto &path:canvas.networkPaths)if(path.started>=0){firstEnd=std::min(firstEnd,path.started+path.duration);lastEnd=std::max(lastEnd,path.started+path.duration);}
        canvas.pausedAt=firstEnd+1;const int remaining=canvas.activeBeamCount();
        checks[u"部分光束结束时其余光束仍在行进"_s]=remaining>0&&remaining<flights;
        canvas.pausedAt=lastEnd+1;checks[u"无新激活时光束全部自然结束"_s]=canvas.activeBeamCount()==0;
        canvas.timer.stop();return checks;
    }
};

void Workbench::startNeuronVerification(const QString &directory){
    // 使用独立设置、不启动运行器，采集样本由 Python 真实前向测试生成。
    verifying=true;const auto root=QDir(directory).absolutePath();QDir().mkpath(root);
    if(QCoreApplication::arguments().contains(u"--benchmark-neurons"_s)){
        // 同一真实样本、窗口尺寸和软件绘制平台用于优化前后对照，不加载用户模型。
        QFile source(root+u"/实时样本.json"_s);source.open(QIODevice::ReadOnly);
        const auto sample=QJsonDocument::fromJson(source.readAll()).object().value(u"snapshot"_s).toObject();
        const auto frames=sample.value(u"frames"_s).toArray();QJsonObject metrics;
        NeuronCanvas canvas;canvas.resize(1200,800);canvas.configure(sample.value(u"layers"_s).toArray());canvas.show();QTest::qWait(30);
        for(bool flat:{false,true}){
            canvas.setActivityPaused(false);canvas.clearActivity();canvas.setFlat(flat);if(flat)canvas.showAll();
            for(const auto &frame:frames)canvas.applyFrame(frame.toObject());
            QTest::qWait(400);canvas.setActivityPaused(true);
            canvas.grab().save(root+(flat?u"/全局平铺.png"_s:u"/全局三维.png"_s));
            QImage image(canvas.size(),QImage::Format_ARGB32_Premultiplied);QVector<double> times;QElapsedTimer elapsed;
            for(int i=0;i<80;++i){elapsed.start();canvas.render(&image);times.append(elapsed.nsecsElapsed()/1.e6);}
            std::sort(times.begin(),times.end());double sum=0;for(double time:times)sum+=time;
            QJsonObject view{{u"节点数"_s,canvas.nodeCount()},{u"在途光束"_s,canvas.activeBeamCount()},{u"平均绘制毫秒"_s,sum/times.size()},{u"绘制P95毫秒"_s,times[int(times.size()*.95)]}};
            canvas.setActivityPaused(false);elapsed.start();
            for(int i=0;i<30;++i)for(const auto &frame:frames)canvas.applyFrame(frame.toObject());
            view[u"一轮全层更新毫秒"_s]=elapsed.nsecsElapsed()/1.e6/30;
            // 相机连续变化时也测量，防止缓存仅改善静止画面。
            times.clear();for(int i=0;i<40;++i){elapsed.start();canvas.changeZoom(i%2?1/1.005:1.005);canvas.render(&image);times.append(elapsed.nsecsElapsed()/1.e6);}
            sum=0;for(double time:times)sum+=time;view[u"缩放重绘平均毫秒"_s]=sum/times.size();
            metrics[flat?u"平铺"_s:u"三维"_s]=view;
        }
        const bool pass=!frames.isEmpty()&&canvas.nodeCount()>1000;
        QFile report(root+u"/性能对比.json"_s);if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{u"pass"_s,pass},{u"metrics"_s,metrics}}).toJson());
        closing=true;close();QApplication::exit(pass?0:1);return;
    }
    if(QCoreApplication::arguments().contains(u"--verify-neurons-runtime"_s)){
        // 独立客户端与真实 Python 运行器走完整管道，验证生成中实时收帧。
        struct State{int phase=0,frames=0;bool selectedFrame=false,finished=false,done=false,beams3d=false,beamsFlat=false;QSet<int> sourceLayers;QSet<QString> sources;QJsonObject checks;};
        auto state=std::make_shared<State>();
        connect(neurons->canvas,&NeuronCanvas::propagated,this,[state](int layer,int neuron,int,int){state->sourceLayers.insert(layer);state->sources.insert(QString::number(layer)+u":"_s+QString::number(neuron));});
        const auto finish=[this,root,state](bool pass,const QString &error){
            if(state->done)return;
            state->done=true;QFile report(root+u"/真实管道验收.json"_s);
            if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{u"pass"_s,pass},{u"checks"_s,state->checks},{u"error"_s,error},{u"frames"_s,state->frames}}).toJson());
            closing=true;worker.send(u"shutdown"_s);close();QApplication::exit(pass?0:1);
        };
        connect(this,&Workbench::received,this,[this,state,root,finish](const QJsonObject &message){
            const auto name=message.value(u"name"_s).toString();const auto data=message.value(u"payload"_s).toObject();
            if(message.value(u"kind"_s).toString()==u"error"_s){finish(false,data.value(u"message"_s).toString());return;}
            if(name==u"model.ready"_s){
                // 只打开页面，不搜索、不点击节点，也不手动发射。
                shell->setCurrentIndex(1);showNeurons();neurons->restoreView(false);
            }
            if(name==u"neurons.frame"_s){++state->frames;if(data.value(u"layer_index"_s).toInt()==0&&data.value(u"selected"_s).toObject().value(u"value"_s).isDouble())state->selectedFrame=true;
                if(neurons->canvas->activeBeamCount()>0){if(neurons->flat())state->beamsFlat=true;else state->beams3d=true;}
                if(state->frames==12)neurons->restoreView(true);
            }
            if(name==u"run.finished"_s){state->finished=true;queryNeurons();}
            if(name==u"neurons.snapshot"_s&&data.value(u"selected"_s).toObject().value(u"neuron_index"_s).toInt()==0){
                if(state->phase==0){state->phase=1;state->checks[u"生成前读取真实权重路径"_s]=!data.value(u"paths"_s).toArray().isEmpty();QTimer::singleShot(100,this,[this]{setMode(u"normal"_s);runPrompt(u"你好"_s,8);});}
                else if(state->phase==1&&state->finished){
                    state->phase=2;
                    state->checks[u"生成期间收到选中神经元激活"_s]=state->selectedFrame;
                    state->checks[u"完成后查询真实缓存"_s]=!data.value(u"frames"_s).toArray().isEmpty();
                    state->checks[u"全局模式不自动选中节点"_s]=!neurons->selection().value(u"has_selection"_s).toBool();
                    state->checks[u"不点击节点时三维自动播放光束"_s]=state->beams3d;
                    state->checks[u"不点击节点时平铺自动播放光束"_s]=state->beamsFlat;
                    state->checks[u"所有非末层自动参与传播"_s]=state->sourceLayers.size()==info.value(u"config"_s).toObject().value(u"num_hidden_layers"_s).toInt()-1;
                    state->checks[u"多个未选中神经元自动发射"_s]=state->sources.size()>20;
                    state->checks[u"全程保持默认节点无需定位"_s]=neurons->selection().value(u"layer_index"_s).toInt()==0&&neurons->selection().value(u"neuron_index"_s).toInt()==0;
                    neurons->restoreView(false);QTest::qWait(300);grab().save(root+u"/真实生成-三维.png"_s);neurons->restoreView(true);grab().save(root+u"/真实生成-平铺.png"_s);
                    bool pass=true;for(auto it=state->checks.begin();it!=state->checks.end();++it)pass=pass&&it.value().toBool();finish(pass,pass?QString{}:u"真实生成与界面数据不一致"_s);
                }
            }
        });
        QTimer::singleShot(60000,this,[finish]{finish(false,u"真实运行器验收超时"_s);});
        resize(1440,1000);uiScale->setFactor(1);
        const bool opened=QFile::exists(root+u"/隔离项目/project.json"_s)?openProjectAt(root+u"/隔离项目"_s,true):createProjectAt(root+u"/隔离项目"_s,u"神经元运行验收"_s,root+u"/验收模型"_s,128,u"cpu"_s);
        if(!opened)finish(false,u"无法创建隔离验收项目"_s);
        return;
    }
    QJsonObject checks;QString error;
    const auto check=[&](bool pass,const QString &name){checks[name]=pass;if(!pass&&error.isEmpty())error=name;};
    QFile file(root+u"/实时样本.json"_s);QJsonObject fixture;
    if(file.open(QIODevice::ReadOnly))fixture=QJsonDocument::fromJson(file.readAll()).object();
    check(!fixture.isEmpty(),u"读取真实模型样本"_s);
    shell->setCurrentIndex(1);uiScale->setFactor(1);resize(1440,1000);showNeurons();QTest::qWait(40);
    auto *layerField=neurons->findChild<QLineEdit*>(u"neuronLayerInput"_s);
    auto *indexField=neurons->findChild<QLineEdit*>(u"neuronIndexInput"_s);
    auto *locate=neurons->findChild<QPushButton*>(u"neuronLocate"_s);
    check(!locate->isEnabled()&&neurons->canvas->nodeCount()==0,u"未加载状态无虚构节点"_s);
    grab().save(root+u"/未加载.png"_s);
    if(!fixture.isEmpty()){
        info=fixture.value(u"model"_s).toObject();weightsRevision=u"w0"_s;loaded=true;
        const auto sample=fixture.value(u"snapshot"_s).toObject();
        const auto animationChecks=NeuronAnimationVerification::run(sample);
        for(auto it=animationChecks.begin();it!=animationChecks.end();++it)check(it.value().toBool(),it.key());
        const auto chosen=sample.value(u"selected"_s).toObject();
        const int layerId=chosen.value(u"layer_index"_s).toInt(),neuronId=chosen.value(u"neuron_index"_s).toInt();
        showNeurons();QSet<int> globalLayers;QSet<QString> globalSources;
        const auto propagation=connect(neurons->canvas,&NeuronCanvas::propagated,this,[&](int a,int b,int,int){globalLayers.insert(a);globalSources.insert(QString::number(a)+u":"_s+QString::number(b));});
        for(const auto &frame:sample.value(u"frames"_s).toArray())neurons->frame(frame.toObject());
        check(globalLayers.size()==info.value(u"config"_s).toObject().value(u"num_hidden_layers"_s).toInt()-1&&globalSources.size()>20&&neurons->canvas->activeBeamCount()>0,u"无点击时全局多层节点自动传播"_s);
        neurons->restoreView(true);check(neurons->canvas->activeBeamCount()>0,u"切换平铺保留全局在途光束"_s);
        auto *pause=neurons->findChild<QPushButton*>(u"neuronPlay"_s);pause->click();QTest::qWait(40);const auto frozenCanvas=neurons->canvas->grab().toImage();QTest::qWait(80);check(frozenCanvas==neurons->canvas->grab().toImage(),u"暂停冻结全局光束及颜色"_s);pause->click();
        disconnect(propagation);neurons->restoreView(false);neurons->clearActivity();
        QSet<QString> soloSources;const auto soloConnection=connect(neurons->canvas,&NeuronCanvas::propagated,this,[&](int a,int b,int,int){soloSources.insert(QString::number(a)+u":"_s+QString::number(b));});
        showNeurons();layerField->setText(QString::number(layerId));indexField->setText(QString::number(neuronId));locate->click();neurons->snapshot(sample);QTest::qWait(40);
        neurons->canvas->replay();
        for(const auto &frame:sample.value(u"frames"_s).toArray())neurons->frame(frame.toObject());
        check(neurons->selection().value(u"has_selection"_s).toBool()&&soloSources==QSet<QString>{QString::number(layerId)+u":"_s+QString::number(neuronId)},u"搜索定位后只播放选中神经元"_s);
        neurons->restoreView(true);neurons->canvas->replay();check(soloSources.size()==1&&neurons->canvas->activeBeamCount()>0,u"平铺视图保留单节点播放模式"_s);
        disconnect(soloConnection);QTest::mouseClick(neurons->canvas,Qt::RightButton,Qt::NoModifier,QPoint(100,100));
        check(!neurons->selection().value(u"has_selection"_s).toBool()&&layerField->text().isEmpty()&&indexField->text().isEmpty()&&neurons->canvas->activeBeamCount()>4,u"右键清空选中并恢复全局播放"_s);
        neurons->restoreView(false);QTest::mouseClick(neurons->canvas,Qt::RightButton,Qt::NoModifier,QPoint(100,100));check(!neurons->selection().value(u"has_selection"_s).toBool()&&neurons->canvas->activeBeamCount()>4,u"三维右键同样恢复全局播放"_s);
        layerField->setText(QString::number(layerId));indexField->setText(QString::number(neuronId));locate->click();neurons->snapshot(sample);
        check(neurons->selection().value(u"neuron_index"_s).toInt()==neuronId&&neurons->canvas->nodeCount()>1000,u"全量编号定位与有界显示"_s);
        auto *activation=neurons->findChild<QLabel*>(u"neuronActivation"_s);
        check(activation->text()!=u"未采集"_s,u"显示实际激活"_s);
        const auto selection=neurons->selection();indexField->setText(u"-1"_s);locate->click();
        check(neurons->selection()==selection&&indexField->property("invalid").toBool(),u"非法编号不改变选择"_s);
        indexField->setText(QString::number(neuronId));locate->click();neurons->snapshot(sample);
        const auto originalPosition=neurons->canvas->nodePosition(layerId,neuronId);
        QTest::mousePress(neurons->canvas,Qt::LeftButton,Qt::NoModifier,QPoint(350,240));
        QTest::mouseMove(neurons->canvas,QPoint(400,270),20);QTest::mouseRelease(neurons->canvas,Qt::LeftButton,Qt::NoModifier,QPoint(400,270));
        check(neurons->canvas->nodePosition(layerId,neuronId)!=originalPosition,u"拖动实际旋转三维坐标"_s);
        neurons->canvas->select(layerId,neuronId,true);neurons->canvas->replay();QTest::qWait(500);grab().save(root+u"/神经元-三维.png"_s);
        neurons->findChild<QPushButton*>(u"neuronViewFlat"_s)->click();
        check(neurons->flat()&&neurons->selection().value(u"neuron_index"_s)==selection.value(u"neuron_index"_s),u"平铺与三维共用选择"_s);
        neurons->canvas->replay();QTest::qWait(500);grab().save(root+u"/神经元-平铺.png"_s);
        const auto paths=sample.value(u"paths"_s).toArray();
        if(!paths.isEmpty()){
            const auto target=paths.first().toObject();const int a=target.value(u"target_layer"_s).toInt(),b=target.value(u"target_neuron"_s).toInt();
            const auto point=neurons->canvas->nodePosition(a,b).toPoint();
            check(neurons->canvas->rect().contains(point),u"真实路径目标在画面中"_s);
            QTest::mouseClick(neurons->canvas,Qt::LeftButton,Qt::NoModifier,point);
            check(neurons->selection().value(u"layer_index"_s).toInt()==a&&neurons->selection().value(u"neuron_index"_s).toInt()==b,u"点击真实目标读取身份"_s);
            check(neurons->selection().value(u"has_selection"_s).toBool(),u"左键点击节点进入单节点模式"_s);
        }
        layerField->setText(QString::number(layerId));indexField->setText(QString::number(neuronId));locate->click();neurons->snapshot(sample);
        neurons->canvas->changeZoom(2.2);QTest::qWait(30);grab().save(root+u"/神经元-平铺放大.png"_s);
        auto *play=neurons->findChild<QPushButton*>(u"neuronPlay"_s);play->click();const auto frozen=activation->text();
        auto newer=sample.value(u"frames"_s).toArray().first().toObject();newer[u"layer_index"_s]=layerId;newer[u"selected"_s]=QJsonObject{{u"neuron_index"_s,neuronId},{u"value"_s,99.}};neurons->frame(newer);
        check(!neurons->following()&&activation->text()==frozen,u"暂停活动冻结数值"_s);play->click();
        neurons->beginRun(u"新运行"_s,u"base"_s);check(activation->text()==u"未采集"_s,u"新运行清除旧活动"_s);
        neurons->snapshot(sample);auto changed=info;changed[u"weights_revision"_s]=u"w1"_s;neurons->configure(changed,true);
        check(activation->text()==u"未采集"_s,u"权重版本变化清理活动"_s);
        neurons->snapshot(sample);check(activation->text()==u"未采集"_s,u"拒绝过期权重快照"_s);
        changeView(0);check(workspaceViews->currentWidget()!=neurons,u"返回原工作区"_s);showNeurons();
        neurons->configure({},false);check(!locate->isEnabled()&&neurons->canvas->nodeCount()==0,u"卸载清除节点并禁用搜索"_s);
    }
    QFile report(root+u"/神经元界面验收.json"_s);if(report.open(QIODevice::WriteOnly))report.write(QJsonDocument(QJsonObject{{u"pass"_s,error.isEmpty()},{u"checks"_s,checks},{u"error"_s,error}}).toJson());
    closing=true;close();QApplication::exit(error.isEmpty()?0:1);
}
