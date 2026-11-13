#include "TactileSceneAdapter.h"
#include "TactileSurfaceModel.h"
#include "TactileHistory.h"
#include <algorithm>
#include <cmath>
namespace {
QVariantMap pressureArrow(const tactile::Force &force,double scale,double maximum,bool reverseZ=false) {
    auto value=TactileSceneAdapter::arrow({force.x,force.y,reverseZ?force.z:-force.z},scale,maximum);
    value["fz"]=std::isfinite(force.z)?force.z:0;
    value["offset"]=QVariant::fromValue(QVector3D());
    return value;
}
}
TactileSceneAdapter::TactileSceneAdapter(QObject *parent):QObject(parent),
    m_interpolatedPoints(new TactileInstanceBuffer(this)),
    m_interpolatedPointShafts(new TactileInstanceBuffer(this)),
    m_interpolatedPointHeads(new TactileInstanceBuffer(this)),
    m_interpolatedComponentShafts(new TactileInstanceBuffer(this)),
    m_interpolatedComponentHeads(new TactileInstanceBuffer(this)) { invalidate(); }
QVector3D TactileSceneAdapter::pointPosition(int index,int type) {
    const auto &surface=tactileSurfaceModel(type);
    return surface.valid&&index>=0&&index<surface.positions.size()?surface.positions[index]:QVector3D();
}
QVariantMap TactileSceneAdapter::surface() const {
    const auto &s=tactileSurfaceModel(m_type);
    return {{"valid",s.valid},{"id",tactile::sensorLayout().fingerIds[m_type]},
        {"mesh",s.mesh},{"position",QVariant::fromValue(s.meshPosition)},
        {"rotation",QVariant::fromValue(s.meshRotation)},
        {"center",QVariant::fromValue(s.boundsCenter)},{"size",QVariant::fromValue(s.boundsSize)}};
}

QColor TactileSceneAdapter::forceColor(double z) {
    const double pressure=std::isfinite(z)?std::clamp(z/PressureColorMaximum,0.0,1.0):0;
    return QColor::fromHsvF((210-210*pressure)/360,.65,.8);
}
QVariantMap TactileSceneAdapter::componentArrow(double input,int axis,double gain,bool reverseZ) {
    // Counts, not calibrated N. Optional deadband is applied before glyphs.
    const double value=std::isfinite(input)?input:0;
    const double magnitude=std::abs(value),scale=45.0/32767.0*gain;
    const bool finite=std::isfinite(scale)&&scale>0&&axis>=0&&axis<3;
    const bool visible=finite&&magnitude>1e-6;
    // Axis directions have only six possible rotations, independent of force.
    static const QQuaternion rotations[3][2]={
        {QQuaternion::rotationTo({0,1,0},{1,0,0}),QQuaternion::rotationTo({0,1,0},{-1,0,0})},
        {QQuaternion::rotationTo({0,1,0},{0,1,0}),QQuaternion::rotationTo({0,1,0},{0,-1,0})},
        {QQuaternion::rotationTo({0,1,0},{0,0,-1}),QQuaternion::rotationTo({0,1,0},{0,0,1})}};
    const bool reversed=reverseZ&&axis==2;
    QVariantMap result{{"arrowVisible",visible},{"rotation",QVariant::fromValue(visible?rotations[axis][(value<0)!=reversed?1:0]:QQuaternion())},
        {"length",visible?std::min(45.0,magnitude*scale):0},{"magnitude",finite?magnitude:0},
        {"fx",finite&&axis==0?value:0},{"fy",finite&&axis==1?value:0},{"fz",finite&&axis==2?(reversed?value:-value):0}};
    result["value"]=value;
    // Tail stays at the contact; pressure extends into the translucent model.
    result["offset"]=QVariant::fromValue(QVector3D());
    return result;
}
QVariantMap TactileSceneAdapter::arrow(const tactile::Force &force,double scale,double maximum) {
    const double norm=std::hypot(force.x,force.y,force.z);
    const bool finite=std::isfinite(norm)&&std::isfinite(scale)&&std::isfinite(maximum)&&scale>0&&maximum>0;
    const bool visible=finite&&norm>1e-6;
    const QVector3D direction=visible?QVector3D(float(force.x/norm),float(force.y/norm),float(force.z/norm)):QVector3D(0,1,0);
    return {{"arrowVisible",visible},{"rotation",QVariant::fromValue(visible?QQuaternion::rotationTo(QVector3D(0,1,0),direction):QQuaternion())},
        {"length",visible?std::min(maximum,norm*scale):0},{"magnitude",finite?norm:0},
        {"fx",finite?force.x:0},{"fy",finite?force.y:0},{"fz",finite?force.z:0}};
}
void TactileSceneAdapter::setGridSize(int size) {
    if(size>=2&&size<=4&&size!=m_gridSize){m_gridSize=size;emit optionsChanged();}
}
void TactileSceneAdapter::rebuildPoints(const tactile::Finger *finger,int selectedModule) {
    QVariantList points;
    Q_UNUSED(selectedModule);
    const auto &surface=tactileSurfaceModel(m_type);
    const int rawCount=tactile::sensorLayout(m_type).points.size();
    points.reserve(rawCount+(surface.valid&&m_gridSize>2?surface.interpolation[m_gridSize-3].size():0));
    bool firstDot=true;
    const auto appendPoint=[&](QVariantMap &point) {
        // Complete the map before sharing it with QVariantList: modifying a
        // toMap() copy afterwards detaches and copies every property per point.
        // Positive compression remains a display displacement in metres.
        const double z=point["fz"].toDouble();
        const float lift=float((std::isfinite(z)?std::max(0.0,z):0.0)*45.0/32767.0*m_arrowGain*.0001);
        const auto dot=point["position"].value<QVector3D>()+QVector3D(0,0,lift);
        point["dotPosition"]=QVariant::fromValue(dot);
        if(firstDot){m_dotMinimum=m_dotMaximum=dot;firstDot=false;}
        else for(int axis=0;axis<3;++axis){m_dotMinimum[axis]=std::min(m_dotMinimum[axis],dot[axis]);m_dotMaximum[axis]=std::max(m_dotMaximum[axis],dot[axis]);}
        points.append(point);
    };
    for(int index=0;index<rawCount;++index) {
        const auto force=finger?finger->modules[0].points[index].force:tactile::Force{};
        auto p=pressureArrow(force,40.0/32767.0*m_arrowGain,60,m_reverseZArrows);
        p["components"]=QVariantList{componentArrow(force.x,0,m_arrowGain),componentArrow(force.y,1,m_arrowGain),componentArrow(force.z,2,m_arrowGain,m_reverseZArrows)};
        const auto label=QString("P%1").arg(index+1);
        p["position"]=QVariant::fromValue(pointPosition(index,m_type));p["radius"]=1.75;p["color"]=forceColor(force.z);
        p["label"]=label;p["id"]=tactile::sensorLayout().fingerIds[m_type]+"/"+label;p["interpolated"]=false;
        p["selected"]=finger!=nullptr;appendPoint(p);
    }
    if(surface.valid&&m_gridSize>2) {
        int index=0;
        for(const auto &sample:surface.interpolation[m_gridSize-3]) {
            tactile::Force force;
            if(finger)for(int j=0;j<sample.sources.size();++j) {
                const auto &raw=finger->modules[0].points[sample.sources[j]].force;
                force.x+=raw.x*sample.weights[j];force.y+=raw.y*sample.weights[j];force.z+=raw.z*sample.weights[j];
            }
            auto p=pressureArrow(force,40.0/32767.0*m_arrowGain,60,m_reverseZArrows);
            p["components"]=QVariantList{componentArrow(force.x,0,m_arrowGain),componentArrow(force.y,1,m_arrowGain),componentArrow(force.z,2,m_arrowGain,m_reverseZArrows)};
            const auto label=QString("I%1").arg(++index);
            p["position"]=QVariant::fromValue(sample.position);p["radius"]=1.25;p["color"]=forceColor(force.z);
            p["label"]=label;p["id"]=tactile::sensorLayout().fingerIds[m_type]+"/"+label;p["interpolated"]=true;
            p["selected"]=finger!=nullptr;appendPoint(p);
        }
    }
    m_points=std::move(points);
}
void TactileSceneAdapter::invalidate(bool typeChanged) {
    m_valid=false;rebuildPoints(nullptr,-1);
    rebuildInstances();
    m_resultant=pressureArrow({},12,90);m_resultant["position"]=QVariant::fromValue(QVector3D());
    // Update the raw repeater count before its delegates read a shorter list.
    if(typeChanged)emit surfaceChanged();
    emit frameChanged();
}
void TactileSceneAdapter::publish(const tactile::Frame &frame,int finger,bool moduleMode,int module,const tactile::Frame *baseline,double noiseThreshold) {
    const bool typeChanged=finger>=0&&finger<3&&m_type!=finger;
    if(typeChanged)m_type=finger;
    if(!tactile::validateFrame(frame)||finger<0||finger>=3||!frame.fingers[finger].valid||module!=0||!tactileSurfaceModel(m_type).valid){invalidate(typeChanged);return;}
    auto f=frame.fingers[finger];
    const bool relative=baseline&&tactile::validateFrame(*baseline)&&tactile::matchingBaseline(frame,*baseline)&&baseline->fingers[finger].valid;
    if(relative||noiseThreshold>0) {
        f.resultant={};f.modules[0].resultant={};
        for(int p=0;p<f.modules[0].points.size();++p) {
            auto &force=f.modules[0].points[p].force;
            if(relative)force=tactile::relativeForce(force,baseline->fingers[finger].modules[0].points[p].force);
            force=tactile::zeroSmallComponents(force,noiseThreshold);
            f.modules[0].resultant+=force;
        }
        f.resultant=f.modules[0].resultant;
    }
    rebuildPoints(&f,moduleMode?module:-1);
    m_resultant=pressureArrow(moduleMode?f.modules[module].resultant:f.resultant,12.0/32767.0*m_arrowGain,90,m_reverseZArrows);
    m_resultant["position"]=QVariant::fromValue(tactileSurfaceModel(m_type).resultantPosition+m_resultant["offset"].value<QVector3D>()*.0001f);m_valid=true;
    rebuildInstances();
    if(typeChanged)emit surfaceChanged();
    emit frameChanged();
}
void TactileSceneAdapter::setPalette(const QPalette &palette){m_palette=palette;rebuildInstances();emit themeChanged();}
void TactileSceneAdapter::setPointArrows(bool value){if(value!=m_pointArrows){m_pointArrows=value;rebuildInstances();emit optionsChanged();}}
void TactileSceneAdapter::setResultantArrow(bool value){if(value!=m_resultantArrow){m_resultantArrow=value;emit optionsChanged();}}
void TactileSceneAdapter::setComponentArrows(bool value){if(value!=m_componentArrows){m_componentArrows=value;rebuildInstances();emit optionsChanged();}}
void TactileSceneAdapter::setDotMatrix(bool value){if(value!=m_dotMatrix){m_dotMatrix=value;rebuildInstances();emit optionsChanged();}}
void TactileSceneAdapter::setArrowWidthGain(double gain){
    if(!std::isfinite(gain)||gain<0.1||gain>10||gain==m_arrowWidthGain)return;
    m_arrowWidthGain=gain;rebuildInstances();emit optionsChanged();
}
void TactileSceneAdapter::setResultantSizeGain(double gain){
    if(!std::isfinite(gain)||gain<0.1||gain>10||gain==m_resultantSizeGain)return;
    m_resultantSizeGain=gain;emit optionsChanged();
}

void TactileSceneAdapter::setArrowMeshBounds(bool head,double bottom,double height) {
    if(!std::isfinite(bottom)||!std::isfinite(height)||height<=0||height>1000||std::abs(bottom)>1000)return;
    auto &oldBottom=head?m_headBottom:m_shaftBottom;
    auto &oldHeight=head?m_headHeight:m_shaftHeight;
    if(oldBottom==bottom&&oldHeight==height)return;
    oldBottom=bottom;oldHeight=height;rebuildInstances();
}
void TactileSceneAdapter::rebuildInstances() {
    using Entry=QQuick3DInstancing::InstanceTableEntry;
    auto &spheres=m_spheres,&pointShafts=m_pointShafts,&pointHeads=m_pointHeads,
        &componentShafts=m_componentShafts,&componentHeads=m_componentHeads;
    spheres.clear();pointShafts.clear();pointHeads.clear();componentShafts.clear();componentHeads.clear();
    const int extras=m_valid?qMax(0,int(m_points.size())-rawPointCount()):0;
    spheres.reserve(extras);pointShafts.reserve(extras);pointHeads.reserve(extras);
    componentShafts.reserve(extras*3);componentHeads.reserve(extras*3);
    // The same 0.1 mm glyph units used by TactileArrow, expressed in the sensor's
    // metre grid; sensorRoot remains the only metres-to-millimetres conversion.
    constexpr double glyphScale=.0001;
    const auto appendArrow=[&](const QVariantMap &arrow,const QVector3D &anchor,double radius,
        double thickness,const QColor &color,QVector<Entry> &shafts,QVector<Entry> &heads) {
        if(!arrow["arrowVisible"].toBool())return;
        const auto rotation=arrow["rotation"].value<QQuaternion>();
        const double length=arrow["length"].toDouble();
        const auto shaftPosition=anchor+rotation.rotatedVector(QVector3D(0,float(-m_shaftBottom*length/m_shaftHeight*glyphScale),0));
        const auto headPosition=anchor+rotation.rotatedVector(QVector3D(0,float((length-m_headBottom*3/m_headHeight)*glyphScale),0));
        shafts.append(TactileInstanceBuffer::entry(shaftPosition,
            QVector3D(float(radius/50*glyphScale),float(length/m_shaftHeight*glyphScale),float(radius/50*glyphScale)),rotation,color));
        const float headRadius=float(thickness*.81/50*glyphScale);
        heads.append(TactileInstanceBuffer::entry(headPosition,QVector3D(headRadius,float(3/m_headHeight*glyphScale),headRadius),rotation,color));
    };
    static const QColor axisColors[]={QColor("#ff525d"),QColor("#48e58b"),QColor("#40baff")};
    for(int i=rawPointCount();i<rawPointCount()+extras;++i) {
        const auto point=m_points[i].toMap();const auto position=point[m_dotMatrix?"dotPosition":"position"].value<QVector3D>();
        const double radius=m_dotMatrix?3.5:point["radius"].toDouble();
        const float sphereRadius=float(radius/50*glyphScale);
        spheres.append(TactileInstanceBuffer::entry(position,QVector3D(sphereRadius,sphereRadius,sphereRadius),{},point["color"].value<QColor>()));
        if(m_dotMatrix)continue;
        if(m_pointArrows)appendArrow(point,position,radius/2*m_arrowWidthGain,.75*m_arrowWidthGain,textColor(),pointShafts,pointHeads);
        if(m_componentArrows){
            const auto components=point["components"].toList();
            for(int axis=0;axis<3;++axis)appendArrow(components[axis].toMap(),position,.875*m_arrowWidthGain,1.4*m_arrowWidthGain,axisColors[axis],componentShafts,componentHeads);
        }
    }
    m_interpolatedPoints->setEntries(spheres);m_interpolatedPointShafts->setEntries(pointShafts);m_interpolatedPointHeads->setEntries(pointHeads);
    m_interpolatedComponentShafts->setEntries(componentShafts);m_interpolatedComponentHeads->setEntries(componentHeads);
}
void TactileSceneAdapter::setModelOpacity(double value){
    if(!std::isfinite(value))return;
    value=std::clamp(value,0.0,1.0);
    if(value!=m_modelOpacity){m_modelOpacity=value;emit optionsChanged();}
}
void TactileSceneAdapter::setLowCostRendering(bool value){if(value!=m_lowCostRendering){m_lowCostRendering=value;emit optionsChanged();}}
