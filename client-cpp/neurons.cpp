#include "neurons.h"
#include "uiscale.h"
#include <QApplication>
#include <QGridLayout>
#include <QHelpEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QShowEvent>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numeric>

using namespace Qt::StringLiterals;
namespace {
constexpr double pi=3.14159265358979323846;
// 小数和缺失值使用同一种中文格式，绝不将空值显示为零。
QString number(const QJsonValue &value){
    if(!value.isDouble()||!std::isfinite(value.toDouble()))return u"未采集"_s;
    const double n=value.toDouble();return (n>=0?u"+"_s:QString{})+QString::number(n,'f',4);
}
// 页面控件沿用原生工作区的紧凑边距与语义名称。
QLabel *text(const QString &value,const QString &name={}){
    auto *label=new QLabel(value);label->setObjectName(name);label->setWordWrap(true);return label;
}
QPushButton *action(const QString &value,const QString &name={}){
    auto *button=new QPushButton(value);button->setObjectName(name);button->setCursor(Qt::PointingHandCursor);return button;
}
QVBoxLayout *column(QWidget *widget,int margin=0,int spacing=0){
    auto *layout=new QVBoxLayout(widget);layout->setContentsMargins(margin,margin,margin,margin);layout->setSpacing(spacing);return layout;
}
QHBoxLayout *row(QWidget *widget,int margin=0,int spacing=7){
    auto *layout=new QHBoxLayout(widget);layout->setContentsMargins(margin,margin,margin,margin);layout->setSpacing(spacing);return layout;
}
// 有正负方向的激活条与原型一致，中心刻度表示零。
class ActivationMeter final : public ScaledWidget {
  public:
    explicit ActivationMeter(QWidget *parent=nullptr):ScaledWidget(parent){UiScale::fixedHeight(this,18);}
  protected:
    void paintEvent(QPaintEvent *) override{
        QPainter p(this);preparePainter(p);const double w=logicalWidth(),middle=w/2;
        p.fillRect(QRectF(0,7,w,4),QColor(u"#39414b"_s));
        if(property("available").toBool()){
            const double value=property("value").toDouble(),maximum=std::max(.000001,property("maximum").toDouble());
            const double size=std::min(1.,std::abs(value)/maximum)*middle;
            p.fillRect(QRectF(value>=0?middle:middle-size,7,size,4),QColor(value>=0?u"#9acddd"_s:u"#d49b69"_s));
        }
        p.setPen(QColor(u"#8196aa"_s));p.drawLine(QPointF(middle,3),QPointF(middle,15));
    }
};
}

NeuronCanvas::NeuronCanvas(QWidget *parent):ScaledWidget(parent){
    // 相机使用真实三维坐标做透视投影，自绘方案无需引入额外浏览器或三维运行库。
    setObjectName(u"neuronCanvas"_s);setAccessibleName(u"神经元交互画布"_s);
    setAccessibleDescription(u"拖动旋转，平铺时拖动平移；滚轮缩放，方向键选择节点"_s);
    setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);setCursor(Qt::OpenHandCursor);
    UiScale::minimumHeight(this,360);UiScale::minimumWidth(this,300);
    // 光晕纹理只生成一次，动画中直接缩放贴图，避免每束光每帧创建径向渐变。
    for(int i=0;i<4;++i){QPixmap image(64,64);image.fill(Qt::transparent);QPainter painter(&image);
        QColor color(i%2?u"#d9ae86"_s:u"#b4e3f1"_s);color.setAlpha(i<2?190:255);
        QRadialGradient glow(QPointF(32,32),30);glow.setColorAt(0,color);color.setAlpha(i<2?80:150);glow.setColorAt(.2,color);color.setAlpha(0);glow.setColorAt(1,color);
        painter.setPen(Qt::NoPen);painter.setBrush(glow);painter.drawEllipse(QPointF(32,32),30,30);painter.end();beamGlows[i]=image;}
    clock.start();timer.setInterval(33);timer.setTimerType(Qt::PreciseTimer);connect(&timer,&QTimer::timeout,this,&NeuronCanvas::animate);
}

quint64 NeuronCanvas::key(int layer,int neuron) const{
    // 稳定复合身份不依赖当前采样顺序。
    return (quint64(quint32(layer))<<32)|quint32(neuron);
}

int NeuronCanvas::ensureNode(int layer,int neuron){
    // 搜索全量编号时只补上该真实节点，避免一次渲染整个模型。
    const auto identity=key(layer,neuron);if(indices.contains(identity))return indices.value(identity);
    if(layer<0||layer>=layers.size())return -1;
    const auto info=layers[layer].toObject();const int count=info.value(u"count"_s).toInt();
    if(neuron<0||neuron>=count)return -1;
    const auto samples=info.value(u"indices"_s).toArray();
    int slot=0;bool sampled=false;
    // 搜索结果落在对应编号区间旁的独立格位，避免堆积到层卡片外部。
    for(int i=0;i<samples.size();++i){if(samples[i].toInt()==neuron)sampled=true;if(std::abs(samples[i].toInt()-neuron)<std::abs(samples[slot].toInt()-neuron))slot=i;}
    const quint32 hash=quint32(neuron)*2654435761u+quint32(layer)*2246822519u;
    const double angle=double(hash%100003)/100003*2*pi,z=layers.size()>1?(double(layer)/(layers.size()-1)-.5)*1.8:0;
    const double radius=.83+double((hash>>8)%1000)/1000*.17,cross=std::sqrt(std::max(.08,1-(z/.99)*(z/.99)));
    const double fold=1+.045*std::sin(angle*11+layer*.8)+.025*std::cos(angle*17-layer);
    const double cosine=std::cos(angle);
    Node node;node.layer=layer;node.neuron=neuron;node.slot=slot;node.sampled=sampled;
    node.position=QVector3D(float((cosine<0?-1:1)*(.025+std::abs(cosine)*.97*radius*cross*fold)),float(std::sin(angle)*.78*radius*cross*fold),float(z));
    const int index=nodes.size();indices[identity]=index;nodes.append(node);return index;
}

void NeuronCanvas::configure(const QJsonArray &value){
    // 重建只发生在模型维度变化时，普通快照不能重置相机。
    if(layers==value)return;
    timer.stop();layers=value;nodes.clear();paths.clear();clearNetworkPaths();indices.clear();projected.clear();selectedIndex=-1;
    for(const auto &item:layers){const auto info=item.toObject();for(const auto &neuron:info.value(u"indices"_s).toArray())ensureNode(info.value(u"layer_index"_s).toInt(),neuron.toInt());}
    // 平铺静态底图逐层保存全部真实编号坐标，动态激活仍复用原采样节点与路径。
    flatLayers.clear();flatLayers.resize(layers.size());
    for(int layer=0;layer<layers.size();++layer){const int count=layers[layer].toObject().value(u"count"_s).toInt();
        auto &points=flatLayers[layer];points.reserve(count);for(int neuron=0;neuron<count;++neuron)points.append(flatPoint(layer,neuron));}
    resetView();update();
}

void NeuronCanvas::clearActivity(){
    // 保留节点身份和视角，只清除上一轮的活动。
    timer.stop();for(auto &node:nodes){node.available=false;node.value=node.target=0;node.forward.clear();node.token=-1;}
    for(auto &path:paths)path.started=-1;
    for(auto &path:networkPaths)path.started=-1;
    propagationCursor.clear();
    update();
}

void NeuronCanvas::applyFrame(const QJsonObject &frame,bool animation){
    // 一帧只包含一个层的一行实际数值，未返回的节点继续保持未采集。
    const int layer=frame.value(u"layer_index"_s).toInt(-1);const auto ids=frame.value(u"indices"_s).toArray(),values=frame.value(u"values"_s).toArray();
    const double maximum=std::max(.000001,frame.value(u"maximum"_s).toDouble(1));
    const auto forward=frame.value(u"forward_id"_s).toString();const int token=frame.value(u"token_position"_s).toInt(-1);
    if(frame.contains(u"network_paths"_s))setNetworkPaths(layer,frame.value(u"network_paths"_s).toArray());
    bool sourceUpdated=false;
    for(int i=0;i<ids.size()&&i<values.size();++i){
        const int index=ensureNode(layer,ids[i].toInt());if(index<0)continue;auto &node=nodes[index];
        // 单节点模式冻结其他节点；最新全层帧由页面缓存，右键后统一恢复。
        if(selectedIndex>=0&&index!=selectedIndex)continue;
        const bool available=values[i].isDouble()&&std::isfinite(values[i].toDouble());
        if(index==selectedIndex&&available&&(node.forward!=forward||node.token!=token))sourceUpdated=true;
        const bool wasAvailable=node.available;node.available=available;node.target=values[i].toDouble();node.maximum=maximum;node.forward=forward;node.token=token;
        if(!animation||!wasAvailable)node.value=node.target;
    }
    // 高频Token不会反复重置在途光束，确保光线可以真正抵达目标。
    if(animation&&sourceUpdated)for(auto &path:paths)
        if(nodes[path.source].layer==layer&&nodes[path.source].available&&nodes[path.source].target!=0&&
           !inFlight(path,activityTime()))startPath(path,activityTime());
    if(animation&&pausedAt<0&&selectedIndex<0)launchNetwork(layer);
    // 高频输入只更新目标值；固定节奏的计时器合并绘制，且不能被每条消息重新推迟。
    if(animation&&isVisible()&&pausedAt<0){if(!timer.isActive())timer.start();}
    else update();
}

void NeuronCanvas::setPaths(const QJsonArray &value){
    // 只绘制后端返回的真实投影耦合路径，树突装饰不会连接到其他节点。
    const auto previousPaths=paths;paths.clear();for(const auto &item:value){const auto info=item.toObject();const int source=ensureNode(info.value(u"source_layer"_s).toInt(),info.value(u"source_neuron"_s).toInt());
        const int target=ensureNode(info.value(u"target_layer"_s).toInt(),info.value(u"target_neuron"_s).toInt());
        if(source>=0&&target>=0){Path path{source,target,info.value(u"coefficient"_s).toDouble(),-1};
            // 同一节点的重复详情返回不能使正在前进的光束倒退到起点。
            for(const auto &previous:previousPaths)if(previous.source==source&&previous.target==target){path.started=previous.started;path.duration=previous.duration;path.negative=previous.negative;break;}
            paths.append(path);}}
    update();
}

void NeuronCanvas::setNetworkPaths(int layer,const QJsonArray &value){
    // 保留同一真实路径的在途进度，详情查询与点击不能重置全局播放。
    if(networkTopology.contains(layer)&&networkTopology.value(layer)==value)return;
    networkTopology[layer]=value;
    for(const auto &item:value){const auto edge=item.toArray();if(edge.size()!=3||!edge[2].isDouble())continue;
        const int source=ensureNode(layer,edge[0].toInt()),target=ensureNode(layer+1,edge[1].toInt());if(source<0||target<0)continue;
        const auto identity=key(source,target);const auto found=networkIndices.constFind(identity);
        if(found==networkIndices.cend()){const int index=networkPaths.size();networkIndices[identity]=index;layerPaths[layer].append(index);networkPaths.append({source,target,edge[2].toDouble(),-1});}
        else networkPaths[*found].coefficient=edge[2].toDouble();
    }
}

void NeuronCanvas::clearNetworkPaths(){
    // 相同模型尺寸下修改权重也必须丢弃旧拓扑，不能只清掉节点颜色。
    networkPaths.clear();networkIndices.clear();networkTopology.clear();layerPaths.clear();propagationCursor.clear();update();
}

qint64 NeuronCanvas::activityTime() const{
    // 暂停锁住显示时钟，恢复后光束从原位置继续。
    return pausedAt>=0?pausedAt:clock.elapsed();
}

int NeuronCanvas::activeBeamCount() const{
    // 暴露实际在途光束数量，供无点击联调验证画布确实开始播放。
    const auto now=activityTime();int count=0;for(const auto &path:selectedIndex<0?networkPaths:paths)if(inFlight(path,now))++count;return count;
}

void NeuronCanvas::clearSelection(){
    // 清空检查路径后回到全局模式，下一批真实帧重新调度各层光束。
    selectedIndex=-1;paths.clear();for(auto &path:networkPaths)path.started=-1;propagationCursor.clear();update();
}

void NeuronCanvas::setActivityPaused(bool value){
    // 暂停同时冻结颜色插值与全局、选中路径，避免只停数据而动画仍在走。
    if(value&&pausedAt<0){pausedAt=clock.elapsed();timer.stop();}
    else if(!value&&pausedAt>=0){const auto duration=clock.elapsed()-pausedAt;for(auto &path:paths)if(path.started>=0)path.started+=duration;for(auto &path:networkPaths)if(path.started>=0)path.started+=duration;pausedAt=-1;if(isVisible())timer.start();}
    update();
}

void NeuronCanvas::launchNetwork(int layer){
    // 直接读取本层索引，按总绘制预算公平分配名额，所有层仍持续轮换真实源节点。
    const auto &candidates=layerPaths[layer];int active=0;const auto now=activityTime();
    for(int index:candidates)if(inFlight(networkPaths[index],now))++active;
    if(candidates.isEmpty())return;
    const int start=propagationCursor.value(layer)%candidates.size();int scanned=0;
    const int limit=std::clamp(beamBudget/std::max(1,int(layers.size())-1),1,6);
    while(scanned<candidates.size()&&active<limit){const int index=candidates[(start+scanned)%candidates.size()];++scanned;auto &path=networkPaths[index];const auto &source=nodes[path.source];
        if(!source.available||source.target==0||inFlight(path,now))continue;
        startPath(path,now);++active;
    }
    propagationCursor[layer]=(start+scanned)%candidates.size();
}

bool NeuronCanvas::inFlight(const Path &path,qint64 now) const{
    // 尚未开始的显示延迟也占用预算，防止高频输入不断堆积待播放光束。
    return path.started>=0&&now<path.started+path.duration;
}

void NeuronCanvas::startPath(Path &path,qint64 now){
    // 延迟和时长只影响呈现：真实非零激活才会触发，每束光保存自己的时钟和发射颜色。
    const auto &source=nodes[path.source];const auto &target=nodes[path.target];
    const quint32 phase=quint32(source.neuron)*2654435761u+quint32(source.layer)*2246822519u+quint32(target.neuron)*3266489917u;
    path.started=now+phase%320;
    const auto delta=project(source).point-project(target).point;
    path.duration=850+int(std::min(500.,std::hypot(delta.x(),delta.y())*.45))+int((phase>>8)%240);
    path.negative=source.target*path.coefficient<0;
    emit propagated(source.layer,source.neuron,target.layer,target.neuron);
}

void NeuronCanvas::ensureProjection() const{
    // 激活只改变颜色；相机、尺寸和节点身份不变时复用投影及深度排序。
    const QSize size(logicalWidth(),logicalHeight());
    if(projected.size()==nodes.size()&&projectionSize==size&&projectionOffset==offset&&projectionYaw==yaw&&projectionPitch==pitch&&projectionZoom==zoom&&projectionFlat==flatView)return;
    projected.clear();projected.reserve(nodes.size());for(const auto &node:nodes)projected.append(project(node));
    depthOrder.resize(nodes.size());std::iota(depthOrder.begin(),depthOrder.end(),0);
    if(!flatView)std::sort(depthOrder.begin(),depthOrder.end(),[this](int a,int b){return projected[a].depth<projected[b].depth;});
    projectionSize=size;projectionOffset=offset;projectionYaw=yaw;projectionPitch=pitch;projectionZoom=zoom;projectionFlat=flatView;
}

NeuronCanvas::Projection NeuronCanvas::project(const Node &node) const{
    // 绘制和命中检测共用同一个投影，支持系统 DPI 与软件界面缩放。
    const double w=logicalWidth(),h=logicalHeight();
    if(flatView)return {flatScreenPoint(node.layer,node.neuron),0,std::min(zoom,2.4)};
    const double x=node.position.x()*std::cos(yaw)+node.position.z()*std::sin(yaw),z0=node.position.z()*std::cos(yaw)-node.position.x()*std::sin(yaw);
    const double y=node.position.y()*std::cos(pitch)-z0*std::sin(pitch),z=node.position.y()*std::sin(pitch)+z0*std::cos(pitch),perspective=3.8/(3.8-z);
    const double scale=std::min(w*.4,h*.42)*zoom;
    return {QPointF(w/2+x*scale*perspective+offset.x(),h/2-y*scale*perspective+offset.y()),z,perspective};
}

QPointF NeuronCanvas::flatPoint(int layer,int neuron) const {
    // 每个编号占据唯一的小格，确定性错落不会在刷新时抖动，也不会冒充已采集活动。
    const int count=std::max(1,layers[layer].toObject().value(u"count"_s).toInt());
    const int columns=std::max(1,int(std::ceil(std::sqrt(count*128./340.))));
    const int rows=(count+columns-1)/columns;
    const quint32 hash=quint32(neuron)*2654435761u+quint32(layer)*2246822519u;
    const double jitterX=(double(hash&1023)/1023.-.5)*.5,jitterY=(double((hash>>10)&1023)/1023.-.5)*.5;
    return QPointF(-64+(neuron%columns+.5+jitterX)*128/columns,-170+(neuron/columns+.5+jitterY)*340/rows);
}

QPointF NeuronCanvas::flatScreenPoint(int layer,int neuron) const {
    // 全量底图、已采集覆盖层和命中检测使用相同坐标与缩放。
    const auto point=flatPoint(layer,neuron);const double fit=std::clamp((logicalHeight()-140)/368.,.1,1.);
    return QPointF(logicalWidth()/2+((layer-(layers.size()-1)/2.)*176+point.x())*zoom+offset.x(),
        logicalHeight()/2+point.y()*zoom*fit+offset.y());
}

int NeuronCanvas::nodeCount() const {
    // 平铺展示全量真实数量；三维继续返回原有可交互采样节点数量。
    if(!flatView)return nodes.size();
    int count=0;for(const auto &layer:layers)count+=layer.toObject().value(u"count"_s).toInt();return count;
}

void NeuronCanvas::paintEvent(QPaintEvent *){
    // 画面按深度排序；颜色和亮度只由已采集数值决定。
    QElapsedTimer paintClock;paintClock.start();
    QPainter p(this);preparePainter(p);p.setRenderHint(QPainter::Antialiasing);p.fillRect(logicalRect(),QColor(u"#1c1f23"_s));
    QFont font=qApp->font();font.setPixelSize(11);p.setFont(font);const double w=logicalWidth(),h=logicalHeight();
    if(nodes.isEmpty()){p.setPen(QColor(u"#9baec1"_s));p.drawText(logicalRect(),Qt::AlignCenter,u"请先加载模型，或打开观测记录"_s);return;}
    ensureProjection();const auto &positions=projected;
    p.save();p.setClipRect(QRectF(0,49,w,h-94));
    if(flatView){
        const double fit=std::clamp((h-140)/368.,.1,1.),middle=(layers.size()-1)/2.;
        for(int i=0;i<layers.size();++i){const double x=w/2+(i-middle)*176*zoom+offset.x(),half=67*zoom,top=h/2-184*zoom*fit+offset.y();
            if(x+half<0||x-half>w)continue;
            p.fillRect(QRectF(x-half,top,half*2,368*zoom*fit),QColor(i==(selectedIndex<0?-1:nodes[selectedIndex].layer)?u"#27343d"_s:u"#20262d"_s));
            // 底图逐点对应实际神经元；只绘制可见层，并批量提交点集以减少绘制开销。
            const int count=layers[i].toObject().value(u"count"_s).toInt();
            p.save();p.translate(x,h/2+offset.y());p.scale(zoom,zoom*fit);
            QPen dots(QColor(u"#64798b"_s));dots.setWidthF(std::clamp(std::sqrt(128.*340./std::max(1,count))*.32,.25,1.3));dots.setCapStyle(Qt::RoundCap);
            p.setPen(dots);p.drawPoints(flatLayers[i]);p.restore();
            p.setPen(QColor(u"#a7bdd0"_s));if(zoom>.3)p.drawText(QRectF(x-45,std::max(52.,top-29),90,20),Qt::AlignCenter,u"第 "_s+QString::number(i)+u" 层 · "_s+QString::number(count));
            if(zoom>.6)p.drawText(QPointF(x+84*zoom,std::max(65.,top-12)),u"→"_s);
        }
    }else{
        const double scale=std::min(w*.4,h*.42)*zoom;p.setPen(QPen(QColor(u"#2b3742"_s),.5));p.setBrush(Qt::NoBrush);
        for(int i=0;i<3;++i)p.drawEllipse(QPointF(w/2+offset.x(),h*.87+offset.y()),scale*(.85+i*.18),scale*.12);
    }
    for(const auto &path:paths){const auto a=positions[path.source].point,b=positions[path.target].point;QPainterPath curve(a);curve.quadTo((a+b)/2+QPointF(0,-18),b);p.setPen(QPen(QColor(u"#698fa4"_s),.8));p.drawPath(curve);}
    const auto now=activityTime();
    QVector<double> flashes(nodes.size(),0.);QVector<const Path*> activePaths;
    if(selectedIndex<0)for(const auto &path:networkPaths)if(inFlight(path,now)&&now>=path.started)activePaths.append(&path);
    const int globalBeamCount=activePaths.size();
    for(const auto &path:paths)if(inFlight(path,now)&&now>=path.started)activePaths.append(&path);
    // 先合并各节点的亮起强度，避免每绘制一个节点都扫描全部连接。
    for(const auto *path:activePaths){const double phase=double(now-path->started)/path->duration;if(phase<.2)flashes[path->source]=std::max(flashes[path->source],1-phase/.2);if(phase>.8)flashes[path->target]=std::max(flashes[path->target],std::sin((phase-.8)/.2*pi));}
    for(int index:depthOrder){const auto &node=nodes[index];const auto &projection=positions[index];const auto point=projection.point;
        if(point.x()<-15||point.x()>w+15||point.y()<76||point.y()>h-61)continue;
        if(flatView&&!node.available&&index!=selectedIndex)continue;
        const double strength=node.available?std::min(1.,std::abs(node.value)/node.maximum*3):0;
        QColor color(!node.available||strength<.025?u"#64798b"_s:node.value<0?u"#d49b69"_s:u"#a9d5e5"_s);
        const double opacity=node.available?.36+strength*.64:.37,size=(flatView?1.6:.85+(node.neuron%7)*.12)*projection.scale*(1+strength*.4);
        const double flash=flashes[index];
        if(strength>.3||flash>.01){p.setPen(Qt::NoPen);auto glow=color;glow.setAlphaF(std::min(.5,strength*.12+flash*.25));p.setBrush(glow);p.drawEllipse(point,size*(3+flash*2),size*(3+flash*2));}
        color.setAlphaF(opacity);p.setPen(QPen(color,.55));
        if(!flatView&&index%5==0){for(int k=0;k<4;++k){const double angle=k*pi/2+index;const QPointF end=point+QPointF(std::cos(angle),std::sin(angle))*size*4.5;p.drawLine(point,end);p.drawLine(end,end+QPointF(std::cos(angle+.7),std::sin(angle+.7))*size*1.8);}}
        p.setBrush(color);p.setPen(Qt::NoPen);p.drawEllipse(point,std::max(.7,size+flash),std::max(.7,size+flash));
        if(flatView&&zoom>=6&&index==selectedIndex){p.setPen(QColor(u"#b3c9dc"_s));p.drawText(QRectF(point.x()-25,point.y()+7,50,18),Qt::AlignCenter,u"#"_s+QString::number(node.neuron));}
    }
    // 光束沿同一路径向前，短拖尾和箭头保证方向可辨，动画不伪造激活值。
    const QRectF viewport(0,49,w,h-94);
    for(int beamIndex=0;beamIndex<activePaths.size();++beamIndex){const auto &path=*activePaths[beamIndex];const bool global=beamIndex<globalBeamCount;const double progress=(double(now-path.started)/path.duration-.08)/.78;if(progress<0||progress>1)continue;
        const auto a=positions[path.source].point,b=positions[path.target].point;
        // 包围框包含曲线最高处和光晕；完全离屏的光束无需绘制。
        if(!QRectF(a,b).normalized().adjusted(-16,-34,16,16).intersects(viewport))continue;
        const auto pointAt=[&](double t){return a+(b-a)*t+QPointF(0,-36*t*(1-t));};
        QColor color(path.negative?u"#d9ae86"_s:u"#b4e3f1"_s);
        // 全局光束使用较细、较淡的短拖尾，远看仍能分辨星点与传播方向。
        const int segments=global?5:10;
        for(int k=0;k<segments;++k){color.setAlphaF((k+1)/double(segments)*(global?.48:1.));p.setPen(QPen(color,global?.85:1.7,Qt::SolidLine,Qt::RoundCap));const double t=std::max(0.,progress-.2+k*.2/segments);p.drawLine(pointAt(t),pointAt(std::min(progress,t+.2/segments)));}
        const auto head=pointAt(progress),back=pointAt(std::max(0.,progress-.02));
        const double radius=global?7:13;color.setAlpha(global?190:255);p.drawPixmap(QRectF(head.x()-radius,head.y()-radius,radius*2,radius*2),beamGlows[(global?0:2)+(path.negative?1:0)],QRectF(2,2,60,60));
        const double angle=std::atan2(head.y()-back.y(),head.x()-back.x());p.setPen(QPen(color,1));p.drawLine(head,head-QPointF(std::cos(angle-.55),std::sin(angle-.55))*6);p.drawLine(head,head-QPointF(std::cos(angle+.55),std::sin(angle+.55))*6);
    }
    if(selectedIndex>=0){const auto point=positions[selectedIndex].point;p.setPen(QPen(QColor(u"#c3eafa"_s),1));p.setBrush(Qt::NoBrush);p.drawEllipse(point,9,9);
        if(!paths.isEmpty()){const auto &target=nodes[paths.first().target];const auto targetPoint=positions[paths.first().target].point;p.setPen(QColor(u"#b9d9e9"_s));p.drawEllipse(targetPoint,6,6);p.drawText(QRectF(targetPoint.x()-75,targetPoint.y()+12,150,20),Qt::AlignCenter,u"目标 "_s+QString::number(target.layer)+u"层 #"_s+QString::number(target.neuron));}
    }
    p.restore();
    // 浮标放在传播反方向，顶部与底部信息始终保持可读。
    if(selectedIndex>=0){const auto point=positions[selectedIndex].point;const bool right=!paths.isEmpty()&&positions[paths.first().target].point.x()>point.x();
        if(point.x()>0&&point.x()<w&&point.y()>74&&point.y()<h-60){const QRectF box(std::clamp(point.x()+(right?-161.:22.),9.,std::max(9.,w-148)),std::clamp(point.y()-67,52.,std::max(52.,h-112)),139,49);
            p.setPen(QPen(QColor(u"#6c9aad"_s),.8));p.setBrush(QColor(u"#253743"_s));p.drawRoundedRect(box,3,3);p.drawLine(point+QPointF(right?-8:8,-5),QPointF(right?box.right():box.left(),box.center().y()));
            p.setPen(QColor(u"#9eb4c7"_s));p.drawText(box.adjusted(9,5,-5,-23),u"已选神经元"_s);p.setPen(QColor(u"#e1f2fa"_s));p.drawText(box.adjusted(9,23,-5,-5),u"第 "_s+QString::number(nodes[selectedIndex].layer)+u" 层 · #"_s+QString::number(nodes[selectedIndex].neuron));}
    }
    p.setPen(QColor(u"#9bafc1"_s));p.drawText(QRectF(14,12,w-28,20),Qt::AlignLeft,u"真实采集 · "_s+QString::number(nodeCount())+(flatView?u" 个真实神经元 · 亮度仅表示已采集值"_s:u" 个显示节点"_s));
    const QStringList labels{u"正激活"_s,u"负激活"_s,u"未采集 / 接近零"_s};const QList<QColor> colors{QColor(u"#9acddd"_s),QColor(u"#d49b69"_s),QColor(u"#71808e"_s)};
    double x=14;for(int i=0;i<labels.size();++i){p.setBrush(colors[i]);p.setPen(Qt::NoPen);p.drawEllipse(QPointF(x+2,h-18),2.5,2.5);p.setPen(QColor(u"#9bafc1"_s));p.drawText(QPointF(x+10,h-14),labels[i]);x+=i==2?110:70;}
    if(w>400)p.drawText(QRectF(w-80,h-36,68,24),Qt::AlignRight,flatView?u"层号 →"_s:u"上 / 前"_s);
    // 预算只限制后续发射数量，已在途的每束光继续按自身进度走完。
    const double cost=paintClock.nsecsElapsed()/1.e6;paintCostMs=paintCostMs==0?cost:paintCostMs*.9+cost*.1;
    beamBudget=paintCostMs>20?72:paintCostMs>12?96:144;
}

void NeuronCanvas::animate(){
    // 插值只平滑显示真实采样值；最后一条光束结束后定时器立即停止。
    if(!isVisible()||pausedAt>=0){timer.stop();return;}bool needed=false;
    for(int i=0;i<nodes.size();++i){auto &node=nodes[i];if((selectedIndex<0||i==selectedIndex)&&node.available&&std::abs(node.target-node.value)>.0001){node.value+=(node.target-node.value)*.35;needed=true;}}
    if(activeBeamCount()>0)needed=true;
    update();if(!needed)timer.stop();
}

void NeuronCanvas::select(int layer,int neuron,bool center){
    // 精确选择和采样选择具有相同身份，搜索不会生成虚构编号。
    bool removed=false;for(int i=int(nodes.size())-1;i>=0;--i)if(!nodes[i].sampled&&(nodes[i].layer!=layer||nodes[i].neuron!=neuron)){nodes.removeAt(i);removed=true;}
    if(removed){paths.clear();projected.clear();indices.clear();for(int i=0;i<nodes.size();++i)indices[key(nodes[i].layer,nodes[i].neuron)]=i;}
    selectedIndex=ensureNode(layer,neuron);if(center)centerSelection();update();
}
void NeuronCanvas::centerSelection(){
    // 搜索定位将节点转到前方，平铺保留下一层的阅读空间。
    if(selectedIndex<0)return;
    const auto &node=nodes[selectedIndex];offset={};
    if(flatView){zoom=1;const double middle=(layers.size()-1)/2.;offset.setX(-(std::min(node.layer,std::max(0,int(layers.size())-2))+.5-middle)*176);}
    else{yaw=-std::atan2(node.position.x(),node.position.z());pitch=std::atan2(node.position.y(),std::hypot(node.position.x(),node.position.z()));zoom=1.18;}
    emit zoomChanged(qRound(zoom*100));
}
void NeuronCanvas::setFlat(bool value){
    // 只切换投影，不重建数据与选中状态。
    flatView=value;resetView();update();
}
void NeuronCanvas::setPan(bool value){pan=value;setCursor(value||flatView?Qt::OpenHandCursor:Qt::SizeAllCursor);}
void NeuronCanvas::changeZoom(double factor){
    // 平铺允许缩小到全层总览，三维限制近裁切范围。
    zoom=std::clamp(zoom*factor,flatView?.12:.5,flatView?16.:2.8);emit zoomChanged(qRound(zoom*100));update();
}
void NeuronCanvas::resetView(){
    // 重置仅影响相机，当前真实数据与路径保持不变。
    yaw=-.24;pitch=.16;zoom=1;offset={};if(flatView&&selectedIndex>=0)centerSelection();emit zoomChanged(qRound(zoom*100));update();
}
void NeuronCanvas::showAll(){
    // 平铺总览按实际层数适配宽度，保持全部层可进入视野。
    resetView();offset={};if(flatView)zoom=std::clamp((logicalWidth()-70)/std::max(176.,layers.size()*176.),.12,1.);
    emit zoomChanged(qRound(zoom*100));update();
}
bool NeuronCanvas::replay(){
    // 单次回放只允许已有真实非零源激活和真实权重路径。
    if(paths.isEmpty()||!nodes[paths.first().source].available||nodes[paths.first().source].target==0)return false;
    const auto now=activityTime();for(auto &path:paths)if(!inFlight(path,now))startPath(path,now);
    if(isVisible()&&pausedAt<0&&!timer.isActive())timer.start();
    update();return true;
}
QPointF NeuronCanvas::nodePosition(int layer,int neuron) const{
    // 平铺中的任意真实编号均可定位，不要求事先创建采样节点。
    if(flatView&&layer>=0&&layer<layers.size()&&neuron>=0&&neuron<layers[layer].toObject().value(u"count"_s).toInt())return flatScreenPoint(layer,neuron)*interfaceScale();
    const auto found=indices.constFind(key(layer,neuron));return found==indices.cend()?QPointF(-1,-1):project(nodes[*found]).point*interfaceScale();
}
int NeuronCanvas::hit(const QPointF &position){
    // 密集平铺先反算层和小格，再比较邻近编号，避免每次点击扫描模型全部神经元。
    if(flatView){
        const double fit=std::clamp((logicalHeight()-140)/368.,.1,1.);
        const double localX=(position.x()-logicalWidth()/2-offset.x())/zoom;
        const int layer=qRound(localX/176+(layers.size()-1)/2.);
        if(layer<0||layer>=layers.size())return -1;
        const int count=layers[layer].toObject().value(u"count"_s).toInt();
        const int columns=std::max(1,int(std::ceil(std::sqrt(std::max(1,count)*128./340.)))),rows=(count+columns-1)/columns;
        if(rows<=0)return -1;
        const double x=localX-(layer-(layers.size()-1)/2.)*176,y=(position.y()-logicalHeight()/2-offset.y())/(zoom*fit);
        const int column=int(std::floor((x+64)*columns/128)),row=int(std::floor((y+170)*rows/340));
        int best=-1;double distance=8;
        for(int r=std::max(0,row-1);r<=std::min(rows-1,row+1);++r)for(int c=std::max(0,column-1);c<=std::min(columns-1,column+1);++c){
            const int neuron=r*columns+c;if(neuron>=count)continue;const auto delta=flatScreenPoint(layer,neuron)-position;const double d=std::hypot(delta.x(),delta.y());
            if(d<distance){distance=d;best=neuron;}}
        return best<0?-1:ensureNode(layer,best);
    }
    int best=-1;double distance=12;for(int i=0;i<nodes.size();++i){const auto projected=project(nodes[i]);const auto delta=projected.point-position;const double d=std::hypot(delta.x(),delta.y());if(d<distance){distance=d;best=i;}}return best;
}

void NeuronCanvas::mousePressEvent(QMouseEvent *event){
    // 同时支持左键旋转与中键平移，按住 Shift 可以临时平移。
    if(event->button()==Qt::RightButton){emit selectionCleared();event->accept();return;}
    if(event->button()!=Qt::LeftButton&&event->button()!=Qt::MiddleButton)return;
    dragging=true;moved=false;pressed=previous=event->position()/interfaceScale();setCursor(Qt::ClosedHandCursor);setFocus();
}
void NeuronCanvas::mouseMoveEvent(QMouseEvent *event){
    // 屏幕坐标先转换为基准坐标，再更新相机。
    if(!dragging)return;
    const auto position=event->position()/interfaceScale(),delta=position-previous;previous=position;
    if(std::hypot(position.x()-pressed.x(),position.y()-pressed.y())>4)moved=true;
    if(flatView||pan||event->buttons().testFlag(Qt::MiddleButton)||event->modifiers().testFlag(Qt::ShiftModifier))offset+=delta;
    else{yaw+=delta.x()*.008;pitch=std::clamp(pitch+delta.y()*.008,-1.4,1.4);}update();
}
void NeuronCanvas::mouseReleaseEvent(QMouseEvent *event){
    // 点击只改变选择，拖动不会误触发节点详情请求。
    if(!dragging)return;
    dragging=false;setCursor(Qt::OpenHandCursor);
    if(!moved&&event->button()==Qt::LeftButton){const int index=hit(event->position()/interfaceScale());if(index>=0){selectedIndex=index;emit selected(nodes[index].layer,nodes[index].neuron);update();}}
}
void NeuronCanvas::wheelEvent(QWheelEvent *event){changeZoom(std::exp(event->angleDelta().y()*.001));event->accept();}
void NeuronCanvas::keyPressEvent(QKeyEvent *event){
    // 键盘可以选择所有显示节点并查看详情，不依赖鼠标命中微小星点。
    if(nodes.isEmpty())return;
    if(event->key()==Qt::Key_Left||event->key()==Qt::Key_Right){selectedIndex=std::clamp(selectedIndex+(event->key()==Qt::Key_Right?1:-1),0,int(nodes.size())-1);centerSelection();emit selected(nodes[selectedIndex].layer,nodes[selectedIndex].neuron);update();}
    else if(event->key()==Qt::Key_Home)resetView();else ScaledWidget::keyPressEvent(event);
}
void NeuronCanvas::hideEvent(QHideEvent *event){timer.stop();ScaledWidget::hideEvent(event);}
bool NeuronCanvas::event(QEvent *event){
    // 原生提示显示精确身份与数值，缺失数据有明确中文说明。
    if(event->type()==QEvent::ToolTip){auto *help=static_cast<QHelpEvent*>(event);const int index=hit(QPointF(help->pos())/interfaceScale());
        if(index>=0){const auto &node=nodes[index];QToolTip::showText(help->globalPos(),u"第 "_s+QString::number(node.layer)+u" 层 · 神经元 #"_s+QString::number(node.neuron)+u"\n激活："_s+(node.available?number(node.target):u"未采集"_s),this);return true;}}
    return ScaledWidget::event(event);
}

NeuronPanel::NeuronPanel(QWidget *parent):QWidget(parent){
    // 独立完整页面复刻原型，不挤占计算图原有检查器和时间轴。
    setObjectName(u"neuronPage"_s);auto *body=row(this,0,0);
    auto *navigator=new QWidget;UiScale::fixedWidth(navigator,150);auto *nav=column(navigator,11,8);
    nav->addWidget(text(u"神经元　模型结构"_s));nav->addWidget(text(u"前馈网络神经元\n按模型层分区"_s,u"muted"_s));
    auto *all=action(u"全部层"_s);nav->addWidget(all);connect(all,&QPushButton::clicked,this,[this]{canvas->showAll();});
    auto *layerArea=new QScrollArea;layerArea->setWidgetResizable(true);layerArea->setFrameShape(QFrame::NoFrame);layerArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *layerContent=new QWidget;layerGrid=new QGridLayout(layerContent);layerGrid->setContentsMargins(0,0,0,0);layerGrid->setSpacing(3);layerGrid->setAlignment(Qt::AlignTop);layerArea->setWidget(layerContent);nav->addWidget(layerArea,1);
    nav->addWidget(text(u"模型神经元总数"_s,u"muted"_s));total=text(u"未加载"_s,u"heading"_s);nav->addWidget(total);nav->addWidget(text(u"按层查看 · 按编号定位"_s,u"muted"_s));body->addWidget(navigator);
    auto *center=new QWidget;auto *main=column(center);
    auto *header=new QWidget;auto *heading=row(header,15,8);auto *titles=new QWidget;auto *titleColumn=column(titles,0,4);titleColumn->addWidget(text(u"神经元"_s,u"heading"_s));titleColumn->addWidget(text(u"探索神经元活动与传播方向"_s,u"muted"_s));heading->addWidget(titles,1);
    view3d=action(u"三维视图"_s,u"neuronView3d"_s);viewFlat=action(u"平铺视图"_s,u"neuronViewFlat"_s);for(auto *button:{view3d,viewFlat}){button->setCheckable(true);heading->addWidget(button);}main->addWidget(header);
    auto *search=new QWidget;auto *fields=row(search,13,8);
    auto *layerBox=new QWidget;UiScale::fixedWidth(layerBox,100);auto *layerColumn=column(layerBox,0,5);layerColumn->addWidget(text(u"层号"_s));layerInput=new QLineEdit;layerInput->setObjectName(u"neuronLayerInput"_s);layerInput->setAccessibleName(u"层号，从零开始"_s);layerColumn->addWidget(layerInput);fields->addWidget(layerBox);
    auto *neuronBox=new QWidget;auto *neuronColumn=column(neuronBox,0,5);neuronColumn->addWidget(text(u"神经元编号"_s));neuronInput=new QLineEdit;neuronInput->setObjectName(u"neuronIndexInput"_s);neuronInput->setAccessibleName(u"神经元编号，从零开始"_s);neuronColumn->addWidget(neuronInput);fields->addWidget(neuronBox,1);
    for(auto *input:{layerInput,neuronInput}){input->setInputMethodHints(Qt::ImhDigitsOnly);connect(input,&QLineEdit::returnPressed,this,&NeuronPanel::locate);}
    locateButton=action(u"定位"_s,u"neuronLocate"_s);fields->addWidget(locateButton,0,Qt::AlignBottom);connect(locateButton,&QPushButton::clicked,this,&NeuronPanel::locate);main->addWidget(search);
    auto *activityBar=new QWidget;auto *activities=row(activityBar,13);play=action(u"暂停活动"_s,u"neuronPlay"_s);play->setCheckable(true);emitOnce=action(u"发射一次"_s,u"neuronEmit"_s);emitOnce->setToolTip(u"回放最近一次真实激活对应的投影耦合方向"_s);activities->addWidget(play);activities->addWidget(emitOnce);activities->addStretch();activity=text(u"等待模型生成"_s,u"muted"_s);activities->addWidget(activity);main->addWidget(activityBar);
    auto *toolbar=new QWidget;toolbar->setObjectName(u"panelHeader"_s);auto *tools=row(toolbar,5,4);rotate=action(u"旋转"_s);panButton=action(u"平移"_s);rotate->setCheckable(true);panButton->setCheckable(true);tools->addWidget(rotate);tools->addWidget(panButton);
    auto *minus=action(u"−"_s);auto *plus=action(u"＋"_s);zoomLabel=action(u"100%"_s);tools->addWidget(minus);tools->addWidget(zoomLabel);tools->addWidget(plus);tools->addStretch();auto *reset=action(u"重置视角"_s);tools->addWidget(reset);main->addWidget(toolbar);
    canvas=new NeuronCanvas;main->addWidget(canvas,1);
    auto *footer=new QWidget;auto *foot=column(footer,11,5);message=text(u"加载模型后可查看神经元；编号从零开始"_s,u"muted"_s);message->setObjectName(u"neuronMessage"_s);foot->addWidget(message);count=text(u"空间排布为示意 · 光束表示投影耦合方向"_s,u"muted"_s);foot->addWidget(count);contextLabel=text(u"当前上下文：未采集"_s,u"muted"_s);foot->addWidget(contextLabel);main->addWidget(footer);body->addWidget(center,1);
    // 只为嵌入会话增加可拖动边界，神经元画布和详情控件保持原对象。
    body->removeWidget(center);
    auto *contentSplit=new QSplitter(Qt::Horizontal);contentSplit->setObjectName(u"neuronContentSplit"_s);contentSplit->setChildrenCollapsible(false);contentSplit->setHandleWidth(4);
    contentSplit->addWidget(center);body->addWidget(contentSplit,1);
    chatArea=new QWidget;chatArea->setObjectName(u"neuronConversationArea"_s);column(chatArea,0,0);chatArea->layout()->setSizeConstraint(QLayout::SetNoConstraint);chatArea->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);UiScale::minimumWidth(chatArea,260);chatArea->hide();contentSplit->addWidget(chatArea);
    auto *inspectorArea=new QScrollArea;inspectorArea->setFrameShape(QFrame::NoFrame);inspectorArea->setWidgetResizable(true);UiScale::fixedWidth(inspectorArea,235);auto *inspector=new QWidget;auto *details=column(inspector,14,10);details->addWidget(text(u"神经元详情"_s));details->addWidget(text(u"前馈网络 / 中间通道"_s,u"muted"_s));identity=text(u"未选择神经元"_s,u"heading"_s);identity->setObjectName(u"neuronIdentity"_s);subtitle=text(u"请先加载模型"_s,u"muted"_s);details->addWidget(identity);details->addWidget(subtitle);
    details->addWidget(text(u"当前激活"_s));activation=text(u"未采集"_s,u"neuronActivation"_s);details->addWidget(activation);meter=new ActivationMeter;details->addWidget(meter);rank=text(u"激活绝对值排名：未采集"_s,u"muted"_s);details->addWidget(rank);details->addWidget(text(u"计算明细"_s));
    for(const auto &entry:QList<QPair<QString,QLabel**>>{{u"门控投影"_s,&gate},{u"门控激活"_s,&silu},{u"上投影"_s,&up},{u"中间激活"_s,&middle}}){auto *line=new QWidget;auto *content=row(line,0);content->addWidget(text(entry.first,u"muted"_s));content->addStretch();*entry.second=text(u"未采集"_s);content->addWidget(*entry.second);details->addWidget(line);}
    details->addWidget(text(u"Token激活轨迹"_s));historyText=text(u"未采集"_s,u"muted"_s);historyText->setObjectName(u"neuronHistory"_s);UiScale::minimumHeight(historyText,68);details->addWidget(historyText);
    details->addWidget(text(u"传播路径"_s));pathSource=action(u"源神经元\n未选择"_s,u"neuronPath"_s);pathTarget=action(u"目标神经元\n未选择"_s,u"neuronPath"_s);details->addWidget(pathSource);details->addWidget(text(u"↓ 投影耦合方向"_s,u"muted"_s));details->addWidget(pathTarget);pathNote=text(u"加载匹配模型后读取权重"_s,u"muted"_s);pathNote->setObjectName(u"neuronPathNote"_s);details->addWidget(pathNote);location=text(u"编号起点：0"_s,u"muted"_s);details->addWidget(location);details->addStretch();inspectorArea->setWidget(inspector);contentSplit->addWidget(inspectorArea);contentSplit->setStretchFactor(0,1);contentSplit->setStretchFactor(1,0);contentSplit->setStretchFactor(2,0);
    setStyleSheet(u"QLabel#heading{font-size:16px;color:#d3dfed;} QLabel#neuronActivation{font-family:'Consolas';font-size:28px;color:#b5dfeb;} QPushButton#neuronPath{text-align:left;background:#202832;border:1px solid #43596a;padding:8px;} QPushButton[neuronLayer=true]{background:transparent;border:1px solid transparent;padding:3px 0;font-size:11px;} QPushButton[neuronLayer=true]:hover{background:#303e49;border-color:#708b9c;} QPushButton[neuronLayer=true]:checked{background:#344f60;border-color:#8ab4ca;} QLineEdit[invalid=true]{border-color:#d49b69;}"_s);
    connect(canvas,&NeuronCanvas::selected,this,[this](int layer,int neuron){choose(layer,neuron,false);});
    connect(canvas,&NeuronCanvas::selectionCleared,this,&NeuronPanel::clearSelection);
    connect(canvas,&NeuronCanvas::zoomChanged,this,[this](int percent){zoomLabel->setText(QString::number(percent)+u"%"_s);});
    connect(view3d,&QPushButton::clicked,this,[this]{restoreView(false);});connect(viewFlat,&QPushButton::clicked,this,[this]{restoreView(true);});
    connect(rotate,&QPushButton::clicked,this,[this]{canvas->setPan(false);rotate->setChecked(true);panButton->setChecked(false);});connect(panButton,&QPushButton::clicked,this,[this]{canvas->setPan(true);rotate->setChecked(false);panButton->setChecked(true);});
    connect(minus,&QPushButton::clicked,this,[this]{canvas->changeZoom(1/1.18);});connect(plus,&QPushButton::clicked,this,[this]{canvas->changeZoom(1.18);});connect(reset,&QPushButton::clicked,canvas,&NeuronCanvas::resetView);connect(zoomLabel,&QPushButton::clicked,canvas,&NeuronCanvas::resetView);
    connect(play,&QPushButton::clicked,this,[this]{setLive(!live);});
    connect(emitOnce,&QPushButton::clicked,this,[this]{message->setText(canvas->replay()?u"回放真实采集对应的投影耦合方向；光束速度为显示效果"_s:u"请先采集非零源激活并读取有效的权重路径"_s);});
    connect(pathSource,&QPushButton::clicked,this,[this]{if(!pathInfo.isEmpty()){const auto path=pathInfo.first().toObject();choose(path.value(u"source_layer"_s).toInt(),path.value(u"source_neuron"_s).toInt(),true);}});
    connect(pathTarget,&QPushButton::clicked,this,[this]{if(!pathInfo.isEmpty()){const auto path=pathInfo.first().toObject();choose(path.value(u"target_layer"_s).toInt(),path.value(u"target_neuron"_s).toInt(),true);}});
    updateButtons();
}

void NeuronPanel::configure(const QJsonObject &value,bool connected){
    // 模型或权重版本变化即清空旧数据；仅运行状态变化不重置用户视角。
    const auto revision=value.value(u"model_revision"_s).toString(),weights=value.value(u"weights_revision"_s).toString();
    const auto config=value.value(u"config"_s).toObject();const int layers=config.value(u"num_hidden_layers"_s).toInt(),channels=config.value(u"intermediate_size"_s).toInt();
    const bool changed=revision!=modelRevision||weights!=weightsRevision||config!=model.value(u"config"_s).toObject();model=value;online=connected;modelRevision=revision;weightsRevision=weights;
    if(changed){
        layerInfo={};const int samples=std::max(8,std::min(64,2560/std::max(1,layers)));
        for(int layer=0;layer<layers;++layer){QJsonArray indices;const int n=std::min(channels,samples);for(int i=0;i<n;++i)indices.append(i*(channels-1)/std::max(1,n-1));layerInfo.append(QJsonObject{{u"layer_index"_s,layer},{u"count"_s,channels},{u"indices"_s,indices}});}
        canvas->configure(layerInfo);canvas->clearNetworkPaths();clearActivity();
        while(auto *item=layerGrid->takeAt(0)){delete item->widget();delete item;}
        for(int layer=0;layer<layers;++layer){auto *button=action(u"第 "_s+QString::number(layer).rightJustified(2,QChar(u'0'))+u" 层"_s);button->setObjectName(u"neuronLayer-"_s+QString::number(layer));button->setProperty("neuronLayer",true);button->setCheckable(true);button->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);layerGrid->addWidget(button,layer/2,layer%2);connect(button,&QPushButton::clicked,this,[this,layer]{choose(layer,selectedNeuron,true);});}
        total->setText(layers?QString::number(qint64(layers)*channels):u"未加载"_s);
        if(layers&&channels){selectedLayer=std::clamp(selectedLayer,0,layers-1);selectedNeuron=std::clamp(selectedNeuron,0,channels-1);if(hasSelection)canvas->select(selectedLayer,selectedNeuron);identity->setText(hasSelection?u"神经元 #"_s+QString::number(selectedNeuron):u"未选择神经元"_s);subtitle->setText(hasSelection?u"第 "_s+QString::number(selectedLayer)+u" 层 / 共 "_s+QString::number(channels)+u" 个通道"_s:u"全局播放 · 点击查看详情"_s);if(hasSelection){layerInput->setText(QString::number(selectedLayer));neuronInput->setText(QString::number(selectedNeuron));}}
        else{hasSelection=false;identity->setText(u"未选择神经元"_s);subtitle->setText(u"请先加载模型"_s);layerInput->clear();neuronInput->clear();}
        layerInput->setPlaceholderText(u"0–"_s+QString::number(std::max(0,layers-1)));neuronInput->setPlaceholderText(u"0–"_s+QString::number(std::max(0,channels-1)));
    }
    updateButtons();if(changed){emit subscriptionChanged();emit queryRequested();}
}

void NeuronPanel::beginRun(const QString &run,const QString &branch){
    // 新生成、重算与会话切换不能继续显示上一轮的星点活动。
    runId=run;branchId=branch;forwardId=u"f0"_s;tokenPosition=0;clearActivity();emit subscriptionChanged();emit queryRequested();
}
void NeuronPanel::context(const QString &branch,const QString &forward,int position){
    // 离线或暂停时使用明确的记录位置，在线活动期间保留实时位置。
    branchId=branch;if(!following()){forwardId=forward;tokenPosition=position;}
}
void NeuronPanel::clearActivity(){
    // 缺失状态与数值零分开，保留相机和当前选择。
    canvas->clearActivity();recentFrames.clear();history={};pathInfo={};detail={};canvas->setPaths({});renderDetails({});renderHistory();pathSource->setText(u"源神经元\n未采集"_s);pathTarget->setText(u"目标神经元\n未读取"_s);pathNote->setText(u"等待真实采集及权重路径"_s);contextLabel->setText(u"当前上下文：未采集"_s);updateButtons();
}
QJsonObject NeuronPanel::selection() const{
    // 请求保留模型、分支与位置，工作台另以请求编号过滤过期结果。
    return {{u"has_selection"_s,hasSelection},{u"layer_index"_s,selectedLayer},{u"neuron_index"_s,selectedNeuron},{u"live"_s,following()},{u"historical"_s,!online},{u"model_revision"_s,modelRevision},{u"weights_revision"_s,weightsRevision},{u"branch_id"_s,branchId},{u"forward_id"_s,forwardId},{u"token_position"_s,tokenPosition}};
}

void NeuronPanel::clearSelection(){
    // 右键清除所有选中状态，立即用最近真实帧恢复全局颜色与播放。
    hasSelection=false;canvas->clearSelection();history={};pathInfo={};renderDetails({});renderHistory();
    identity->setText(u"未选择神经元"_s);subtitle->setText(u"全局播放 · 点击查看详情"_s);layerInput->clear();neuronInput->clear();
    pathSource->setText(u"源神经元\n全局活动"_s);pathTarget->setText(u"目标神经元\n各层真实路径"_s);message->setText(u"已清空选中，播放全部活动；左键查看单节点，右键恢复全局"_s);
    pathNote->setText(u"真实投影耦合 · 全局播放"_s);location->setText(u"编号起点：0"_s);
    for(const auto &frame:recentFrames)canvas->applyFrame(frame,following());
    updateButtons();emit subscriptionChanged();emit queryRequested();
}
void NeuronPanel::locate(){
    // 对两个输入框分别验证；错误保留在页面内，不阻止正在进行的生成。
    const QRegularExpression integer(u"^[0-9]+$"_s);bool layerOk=false,neuronOk=false;const auto a=layerInput->text().trimmed(),b=neuronInput->text().trimmed();
    const int layer=a.toInt(&layerOk),neuron=b.toInt(&neuronOk);
    layerOk=layerOk&&integer.match(a).hasMatch()&&layer>=0&&layer<layerInfo.size();
    neuronOk=neuronOk&&integer.match(b).hasMatch()&&neuron>=0&&layerOk&&neuron<layerInfo[layer].toObject().value(u"count"_s).toInt();
    layerInput->setProperty("invalid",!layerOk);neuronInput->setProperty("invalid",!neuronOk);layerInput->setStyleSheet(!layerOk?u"border-color:#d49b69;"_s:QString{});neuronInput->setStyleSheet(!neuronOk?u"border-color:#d49b69;"_s:QString{});
    if(!layerOk||!neuronOk){message->setText(!layerOk?u"层号必须是当前模型范围内的整数，编号从零开始"_s:u"神经元编号必须是当前层范围内的整数，编号从零开始"_s);return;}
    choose(layer,neuron,true);
}
void NeuronPanel::choose(int layer,int neuron,bool center){
    // 所有入口统一选择、重订阅和查询，防止详情与高亮节点不一致。
    if(layer<0||layer>=layerInfo.size())return;
    const int channels=layerInfo[layer].toObject().value(u"count"_s).toInt();if(channels<=0)return;
    hasSelection=true;selectedLayer=layer;selectedNeuron=std::clamp(neuron,0,channels-1);layerInput->setText(QString::number(layer));neuronInput->setText(QString::number(selectedNeuron));
    for(auto *input:{layerInput,neuronInput}){input->setProperty("invalid",false);input->setStyleSheet({});}
    canvas->select(layer,selectedNeuron,center);canvas->setPaths({});pathInfo={};history={};renderHistory();renderDetails({});identity->setText(u"神经元 #"_s+QString::number(selectedNeuron));subtitle->setText(u"第 "_s+QString::number(layer)+u" 层 / 共 "_s+QString::number(channels)+u" 个通道"_s);location->setText(u"层号 / 神经元编号："_s+QString::number(layer)+u" / "_s+QString::number(selectedNeuron)+u"\n编号起点：0"_s);
    pathSource->setText(u"源神经元\n当前选中节点"_s);pathTarget->setText(u"目标神经元\n正在读取"_s);updateButtons();
    message->setText(u"已定位第 "_s+QString::number(layer)+u" 层 · 神经元 #"_s+QString::number(selectedNeuron));emit subscriptionChanged();emit queryRequested();
}
void NeuronPanel::renderDetails(const QJsonObject &value){
    // 所有数值来自同一实际采集点，格式层只负责中文和小数精度。
    detail=value;activation->setText(number(value.value(u"value"_s)));middle->setText(number(value.value(u"value"_s)));gate->setText(number(value.value(u"gate"_s)));silu->setText(number(value.value(u"silu"_s)));up->setText(number(value.value(u"up"_s)));
    const auto numberValue=value.value(u"value"_s);meter->setProperty("available",numberValue.isDouble());meter->setProperty("value",numberValue.toDouble());meter->setProperty("maximum",value.value(u"maximum"_s).toDouble(std::max(1.,std::abs(numberValue.toDouble()))));meter->update();
    rank->setText(value.value(u"rank"_s).isDouble()?u"激活绝对值排名：第 "_s+QString::number(value.value(u"rank"_s).toInt())+u" 名"_s:u"激活绝对值排名：未采集"_s);
    if(value.contains(u"token_position"_s)){tokenPosition=value.value(u"token_position"_s).toInt();forwardId=value.value(u"forward_id"_s).toString(forwardId);contextLabel->setText(u"当前上下文：Token位置 "_s+QString::number(tokenPosition)+u" / "_s+(branchId==u"base"_s?u"原始分支"_s:u"编辑分支"_s));}
}
void NeuronPanel::renderHistory(){
    // 轨迹只绘制已采集位置；没有数据时保留明确的未采集状态。
    if(history.isEmpty()){historyText->setPixmap({});historyText->setText(u"未采集"_s);return;}
    QPixmap image(194,68);image.fill(QColor(u"#25282d"_s));QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(QColor(u"#3a4450"_s),.7));for(int y:{16,34,52})p.drawLine(0,y,194,y);
    double maximum=.000001;for(const auto &item:history)maximum=std::max(maximum,std::abs(item.toObject().value(u"value"_s).toDouble()));
    QPainterPath path;bool started=false;for(int i=0;i<history.size();++i){const auto value=history[i].toObject().value(u"value"_s);if(!value.isDouble()){started=false;continue;}const QPointF point(history.size()>1?i*191./(history.size()-1):96.,34-value.toDouble()/maximum*26);if(started)path.lineTo(point);else path.moveTo(point);started=true;}
    p.setPen(QPen(QColor(u"#98becf"_s),1.2));p.drawPath(path);historyText->setText({});historyText->setPixmap(image);
}
void NeuronPanel::frame(const QJsonObject &payload){
    // 暂停、离线与其他模型版本的帧不能改变当前画面。
    if(!isVisible()||!following()||payload.value(u"model_revision"_s).toString()!=modelRevision||payload.value(u"weights_revision"_s).toString()!=weightsRevision)return;
    recentFrames[payload.value(u"layer_index"_s).toInt()]=payload;canvas->applyFrame(payload);activity->setText(hasSelection?u"正在观测选中节点"_s:u"正在观测全局活动"_s);
    if(!hasSelection)return;
    if(payload.value(u"layer_index"_s).toInt()!=selectedLayer)return;
    auto selected=payload.value(u"selected"_s).toObject();
    if(selected.value(u"neuron_index"_s).toInt(-1)!=selectedNeuron){selected={};const auto ids=payload.value(u"indices"_s).toArray(),values=payload.value(u"values"_s).toArray();for(int i=0;i<ids.size()&&i<values.size();++i)if(ids[i].toInt()==selectedNeuron)selected[u"value"_s]=values[i];}
    for(const auto &key:{u"token_position"_s,u"forward_id"_s,u"maximum"_s})selected[key]=payload.value(key);
    renderDetails(selected);
    if(selected.value(u"value"_s).isDouble()){QJsonObject point{{u"position"_s,tokenPosition},{u"forward_id"_s,forwardId},{u"value"_s,selected.value(u"value"_s)}};if(!history.isEmpty()&&history.last().toObject().value(u"position"_s).toInt()==tokenPosition)history.removeLast();history.append(point);while(history.size()>128)history.removeFirst();renderHistory();}
}
void NeuronPanel::snapshot(const QJsonObject &payload){
    // 结果必须仍属于当前选中节点；过期请求由工作台首先过滤。
    const auto selected=payload.value(u"selected"_s).toObject();
    if(payload.value(u"model_revision"_s).toString()!=modelRevision||payload.value(u"weights_revision"_s).toString()!=weightsRevision||selected.value(u"layer_index"_s).toInt()!=selectedLayer||selected.value(u"neuron_index"_s).toInt()!=selectedNeuron)return;
    const auto actualLayers=payload.value(u"layers"_s).toArray();if(!actualLayers.isEmpty()){layerInfo=actualLayers;canvas->configure(layerInfo);if(hasSelection)canvas->select(selectedLayer,selectedNeuron);}
    for(const auto &item:payload.value(u"network_paths"_s).toArray()){const auto entry=item.toObject();canvas->setNetworkPaths(entry.value(u"layer_index"_s).toInt(),entry.value(u"paths"_s).toArray());}
    for(const auto &item:payload.value(u"frames"_s).toArray()){const auto frame=item.toObject();recentFrames[frame.value(u"layer_index"_s).toInt()]=frame;canvas->applyFrame(frame,false);}
    if(!hasSelection){updateButtons();return;}
    pathInfo=payload.value(u"paths"_s).toArray();canvas->setPaths(pathInfo);renderDetails(selected);history=payload.value(u"history"_s).toArray();renderHistory();
    if(!pathInfo.isEmpty()){const auto path=pathInfo.first().toObject();pathSource->setText(u"源神经元\n第 "_s+QString::number(path.value(u"source_layer"_s).toInt())+u" 层 · #"_s+QString::number(path.value(u"source_neuron"_s).toInt()));pathTarget->setText(u"目标神经元\n第 "_s+QString::number(path.value(u"target_layer"_s).toInt())+u" 层 · #"_s+QString::number(path.value(u"target_neuron"_s).toInt()));pathNote->setText(u"投影耦合 · 系数 "_s+number(path.value(u"coefficient"_s)));}
    else{pathSource->setText(u"源神经元\n当前选中节点"_s);pathTarget->setText(u"目标神经元\n无可用路径"_s);pathNote->setText(online?u"最后一层或无非零耦合路径"_s:u"离线记录不推测权重路径"_s);}
    pathNote->setToolTip(payload.value(u"path_note"_s).toString());if(following())canvas->replay();updateButtons();
}
void NeuronPanel::failure(const QString &value){message->setText(value);}
void NeuronPanel::setLive(bool value){
    // 面板按钮和 MCP 共用播放状态，恢复后重新订阅实际生成数据。
    if(live==value)return;
    live=value;canvas->setActivityPaused(!live);updateButtons();
    message->setText(live?u"跟随生成采集真实活动"_s:u"活动已暂停，画面保留最近采集值"_s);
    emit subscriptionChanged();
    if(live)emit queryRequested();
}
void NeuronPanel::updateButtons(){
    // 离线可浏览记录，但不能开启实时订阅；没有模型时禁用定位。
    const bool available=!layerInfo.isEmpty();for(auto *widget:QList<QWidget*>{layerInput,neuronInput,locateButton})widget->setEnabled(available);
    play->setEnabled(online);play->setChecked(following());play->setText(following()?u"暂停活动"_s:u"播放活动"_s);emitOnce->setEnabled(available&&!pathInfo.isEmpty());
    view3d->setChecked(!canvas->flat());viewFlat->setChecked(canvas->flat());rotate->setEnabled(!canvas->flat());rotate->setChecked(!canvas->flat());panButton->setChecked(canvas->flat());
    for(int i=0;i<layerGrid->count();++i)if(auto *button=qobject_cast<QPushButton*>(layerGrid->itemAt(i)->widget()))button->setChecked(hasSelection&&i==selectedLayer);
    if(!online)activity->setText(available?u"离线记录"_s:u"未加载模型"_s);else if(!live)activity->setText(u"活动已暂停"_s);else if(detail.isEmpty())activity->setText(u"等待模型生成"_s);
}
void NeuronPanel::restoreView(bool flat){canvas->setFlat(flat);updateButtons();}
void NeuronPanel::showEvent(QShowEvent *event){QWidget::showEvent(event);emit subscriptionChanged();emit queryRequested();}
void NeuronPanel::hideEvent(QHideEvent *event){QWidget::hideEvent(event);emit subscriptionChanged();}
