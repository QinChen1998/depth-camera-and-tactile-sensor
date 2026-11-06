#include "TactileTestFixtures.h"
#include "../sensors/TactileSceneAdapter.h"
#include "../sensors/TactileSurfaceModel.h"
#include "../sensors/TactileVisualizationWidget.h"
#include "../RenderingSettings.h"
#include "../TactileDockWidget.h"
#include <QMainWindow>
#include <QApplication>
#include <QMap>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QSlider>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QPushButton>
#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlExpression>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <limits>
namespace {
void pump(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}

bool interpolatedInstancesMatch(TactileSceneAdapter *adapter,QObject *root,QTextStream *diagnostics=nullptr)
{
    if(!adapter||!root)return false;
    const auto points=adapter->points();
    const auto extras=qMax(0,int(points.size())-adapter->rawPointCount());
    auto *spheres=adapter->interpolatedPoints();
    auto *pointShafts=adapter->interpolatedPointShafts(),*pointHeads=adapter->interpolatedPointHeads();
    auto *componentShafts=adapter->interpolatedComponentShafts(),*componentHeads=adapter->interpolatedComponentHeads();
    if(spheres->count()!=(adapter->valid()?extras:0))return false;
    if(!adapter->valid())return !pointShafts->count()&&!pointHeads->count()&&!componentShafts->count()&&!componentHeads->count();
    const auto binding=[&](const char *name,TactileInstanceBuffer *buffer) {
        auto *model=root->findChild<QObject *>(name);
        return model&&model->property("instancing").value<QQuick3DInstancing *>()==buffer;
    };
    bool okay=binding("tactileInterpolatedPoints",spheres)
        &&binding("tactileInterpolatedPointShafts",pointShafts)&&binding("tactileInterpolatedPointHeads",pointHeads)
        &&binding("tactileInterpolatedComponentShafts",componentShafts)&&binding("tactileInterpolatedComponentHeads",componentHeads);
    if(!okay&&diagnostics)*diagnostics<<"Instance model binding mismatch\n";
    const double gain=adapter->arrowWidthGain();
    auto *rawArrow=root->findChild<QObject *>("pointArrow0");
    auto *shaft=rawArrow?rawArrow->findChild<QObject *>("arrowShaft"):nullptr;
    auto *head=rawArrow?rawArrow->findChild<QObject *>("arrowHead"):nullptr;
    if(!shaft||!head)return false;
    const double shaftHeight=shaft->property("nativeHeight").toDouble(),shaftBottom=shaft->property("nativeBottom").toDouble();
    const double headHeight=head->property("nativeHeight").toDouble(),headBottom=head->property("nativeBottom").toDouble();
    int pointIndex=0,componentIndex=0;
    const auto near=[](const QVector3D &a,const QVector3D &b){return (a-b).length()<1e-8;};
    // Verify the actual GPU matrix columns. Qt's rotation inspection helper
    // decomposes the matrix, and instanceColor exposes linear RGB, not sRGB.
    const auto matches=[&](TactileInstanceBuffer *table,int index,const QVector3D &position,
        const QVector3D &scale,const QQuaternion &rotation,const QColor &color) {
        int count=0;const auto bytes=table->instanceBuffer(&count);
        using Entry=QQuick3DInstancing::InstanceTableEntry;
        if(count!=table->count()||index>=count||bytes.size()!=count*qsizetype(sizeof(Entry)))return false;
        Entry entry;std::memcpy(&entry,bytes.constData()+index*sizeof(Entry),sizeof(Entry));
        const auto linear=[](double channel){return float(channel<=.04045?channel/12.92:std::pow((channel+.055)/1.055,2.4));};
        const QVector4D expectedColor(linear(color.redF()),linear(color.greenF()),linear(color.blueF()),float(color.alphaF()));
        const bool result=near(table->instancePosition(index),position)
            &&near(QVector3D(entry.row0.x(),entry.row1.x(),entry.row2.x()),rotation.rotatedVector(QVector3D(scale.x(),0,0)))
            &&near(QVector3D(entry.row0.y(),entry.row1.y(),entry.row2.y()),rotation.rotatedVector(QVector3D(0,scale.y(),0)))
            &&near(QVector3D(entry.row0.z(),entry.row1.z(),entry.row2.z()),rotation.rotatedVector(QVector3D(0,0,scale.z())))
            // Qt uses an approximation for linear RGB; allow one 8-bit step.
            &&std::abs(entry.color.x()-expectedColor.x())<1.0/255
            &&std::abs(entry.color.y()-expectedColor.y())<1.0/255
            &&std::abs(entry.color.z()-expectedColor.z())<1.0/255
            &&std::abs(entry.color.w()-expectedColor.w())<1e-6;
        if(!result&&index==0&&diagnostics)*diagnostics<<"Instance transform/color mismatch: X/Y/Z error "
            <<(QVector3D(entry.row0.x(),entry.row1.x(),entry.row2.x())-rotation.rotatedVector(QVector3D(scale.x(),0,0))).length()<<" / "
            <<(QVector3D(entry.row0.y(),entry.row1.y(),entry.row2.y())-rotation.rotatedVector(QVector3D(0,scale.y(),0))).length()<<" / "
            <<(QVector3D(entry.row0.z(),entry.row1.z(),entry.row2.z())-rotation.rotatedVector(QVector3D(0,0,scale.z()))).length()
            <<", color "<<entry.color.x()<<","<<entry.color.y()<<","<<entry.color.z()
            <<" expected "<<expectedColor.x()<<","<<expectedColor.y()<<","<<expectedColor.z()<<"\n";
        return result;
    };
    const auto checkArrow=[&](const QVariantMap &arrow,const QVector3D &position,double radius,double thickness,
        const QColor &color,TactileInstanceBuffer *shafts,TactileInstanceBuffer *heads,int &index) {
        if(!arrow["arrowVisible"].toBool())return true;
        const auto rotation=arrow["rotation"].value<QQuaternion>();const double length=arrow["length"].toDouble();
        const bool result=matches(shafts,index,position+rotation.rotatedVector(QVector3D(0,float(-shaftBottom*length/shaftHeight*.0001),0)),
                QVector3D(float(radius/50*.0001),float(length/shaftHeight*.0001),float(radius/50*.0001)),rotation,color)
            &&matches(heads,index,position+rotation.rotatedVector(QVector3D(0,float((length-headBottom*3/headHeight)*.0001),0)),
                QVector3D(float(thickness*.81/50*.0001),float(3/headHeight*.0001),float(thickness*.81/50*.0001)),rotation,color);
        ++index;return result;
    };
    const QColor colors[]={QColor("#ff525d"),QColor("#48e58b"),QColor("#40baff")};
    for(int i=0;i<extras;++i) {
        const auto point=points[adapter->rawPointCount()+i].toMap();const auto position=point[adapter->dotMatrix()?"dotPosition":"position"].value<QVector3D>();
        const auto radius=adapter->dotMatrix()?3.5:point["radius"].toDouble();
        okay &= matches(spheres,i,position,QVector3D(float(radius/50*.0001),float(radius/50*.0001),float(radius/50*.0001)),{},point["color"].value<QColor>());
        if(adapter->dotMatrix())continue;
        if(adapter->pointArrows())okay &= checkArrow(point,position,radius/2*gain,.75*gain,adapter->textColor(),pointShafts,pointHeads,pointIndex);
        const auto components=point["components"].toList();
        if(adapter->componentArrows())for(int axis=0;axis<3;++axis)okay &= checkArrow(components[axis].toMap(),position,.875*gain,1.4*gain,colors[axis],componentShafts,componentHeads,componentIndex);
    }
    return okay&&pointShafts->count()==pointIndex&&pointHeads->count()==pointIndex
        &&componentShafts->count()==componentIndex&&componentHeads->count()==componentIndex;
}

bool interpolatedPicking(QQuickWidget *view,TactileSceneAdapter *adapter)
{
    auto *root=view->rootObject();
    int selected=0;
    for(int index=adapter->rawPointCount();index<adapter->points().size()&&selected<3;++index) {
        QQmlExpression project(qmlContext(root),root,QString("project(displayPosition(display.points[%1]))").arg(index));
        const auto position=project.evaluate().value<QVector3D>();
        if(project.hasError())return false;
        if(position.z()<=0||!view->rect().contains(QPoint(qRound(position.x()),qRound(position.y()))))continue;
        const QPointF local(position.x(),position.y()),global(view->mapToGlobal(local.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,local,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(view,&press);QApplication::sendEvent(view,&release);pump(10);
        if(root->property("inspectedPoint").toInt()==index)++selected;
    }
    root->setProperty("inspectedPoint",-1);
    return selected==3;
}

bool rawPointPicking(TactileVisualizationWidget &widget,QTextStream &diagnostics,bool expanded=false)
{
    auto *view=widget.findChild<QQuickWidget *>("tactileQuickView");
    auto *adapter=widget.findChild<TactileSceneAdapter *>();
    auto *table=widget.findChild<QTableWidget *>("tactilePointValues");
    if(!view||!view->rootObject()||!adapter||!table)return false;
    auto *root=view->rootObject();
    const auto frame=widget.frame();const auto resultant=widget.selectedForce();
    const auto history=widget.history().samples().size();
    QVector<QVector3D> positions;
    for(int index=0;index<adapter->rawPointCount();++index) {
        QQmlExpression project(qmlContext(root),root,QString("project(displayPosition(pointSnapshot[%1]))").arg(index));
        const auto position=project.evaluate().value<QVector3D>();
        if(project.hasError()||position.z()<=0||!view->rect().contains(QPoint(qRound(position.x()),qRound(position.y()))))return false;
        positions.append(position);
    }
    auto *spatial=root->findChild<QObject *>("tactileView3d");
    for(int index=0;index<positions.size();++index) {
        const auto position=positions[index];
        QPointF local(position.x(),position.y());
        if(expanded) {
            bool found=false;
            for(const auto &offset:QList<QPointF>{{9,0},{-9,0},{0,9},{0,-9},{6.4,6.4},{-6.4,6.4},{6.4,-6.4},{-6.4,-6.4}}) {
                const auto probe=local+offset;
                if(!view->rect().contains(probe.toPoint()))continue;
                bool nearest=true;
                for(int other=0;other<positions.size();++other)if(other!=index)
                    nearest &= std::hypot(probe.x()-positions[other].x(),probe.y()-positions[other].y())>std::hypot(offset.x(),offset.y());
                if(!nearest)continue;
                QQmlExpression ray(qmlContext(spatial),spatial,QString("pick(%1, %2).objectHit").arg(probe.x(),0,'g',12).arg(probe.y(),0,'g',12));
                const auto hit=ray.evaluate();
                if(ray.hasError())return false;
                if(hit.value<QObject *>())continue; // The expanded target must work outside every visible sphere.
                local=probe;found=true;break;
            }
            if(!found){diagnostics<<"no clear expanded probe for type="<<widget.selectedFinger()<<" point="<<index<<'\n';return false;}
        }
        const QPointF global(view->mapToGlobal(local.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,local,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(view,&press);QApplication::sendEvent(view,&release);pump(10);
        const auto raw=frame.fingers[widget.selectedFinger()].modules[0].points[index].force;
        const auto expected=widget.curveForce(raw,index);
        const double values[]={expected.x,expected.y,expected.z};
        bool okay=root->property("inspectedPoint").toInt()==index&&widget.selectedPoint()==index&&table->currentRow()==index
            &&table->selectionModel()->isRowSelected(index,QModelIndex())&&table->visualItemRect(table->item(index,0)).intersects(table->viewport()->rect())
            &&tactile::sameForce(widget.chartForce(),expected)&&tactile::sameForce(widget.selectedForce(),resultant)
            &&widget.frame().sequence==frame.sequence&&widget.history().samples().size()==history;
        for(int axis=0;axis<3;++axis)okay &= widget.findChild<QLabel *>(QString("force%1").arg(axis))->text().endsWith(QString::number(values[axis],'f',0));
        if(!okay) {
            diagnostics<<"raw pick type="<<widget.selectedFinger()<<" dots="<<adapter->dotMatrix()<<" grid="<<adapter->gridSize()
                <<" expanded="<<expanded<<" expected="<<index<<" inspected="<<root->property("inspectedPoint").toInt()<<" selected="<<widget.selectedPoint()<<'\n';
            return false;
        }
    }
    const int selected=widget.selectedPoint();
    adapter->selectRawPoint(-1);adapter->selectRawPoint(adapter->rawPointCount());
    return widget.selectedPoint()==selected;
}

bool overlappingPointPicking(TactileVisualizationWidget &widget)
{
    auto *view=widget.findChild<QQuickWidget *>("tactileQuickView");auto *root=view->rootObject();
    auto *adapter=widget.findChild<TactileSceneAdapter *>();
    const auto project=[&](int index) {
        QQmlExpression expression(qmlContext(root),root,QString("project(displayPosition(pointSnapshot[%1]))").arg(index));
        const auto p=expression.evaluate().value<QVector3D>();return QPointF(p.x(),p.y());
    };
    int first=0,second=1;double separation=std::numeric_limits<double>::infinity();
    for(int i=0;i<adapter->rawPointCount();++i)for(int j=i+1;j<adapter->rawPointCount();++j) {
        const auto delta=project(i)-project(j);const auto distance=std::hypot(delta.x(),delta.y());
        if(distance<separation){separation=distance;first=i;second=j;}
    }
    root->setProperty("cameraDistance",root->property("cameraDistance").toDouble()*separation/16);pump(60);
    const auto a=project(first),b=project(second);const auto before=widget.frame();
    const auto history=widget.history().samples().size();
    const auto click=[&](const QPointF &local) {
        const QPointF global(view->mapToGlobal(local.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,local,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(view,&press);QApplication::sendEvent(view,&release);pump(10);
    };
    click(a*.6+b*.4);const bool firstSelected=widget.selectedPoint()==first&&root->property("inspectedPoint").toInt()==first;
    click(a*.4+b*.6);const bool secondSelected=widget.selectedPoint()==second&&root->property("inspectedPoint").toInt()==second;
    const auto force=widget.chartForce();click(QPointF(2,2));
    return firstSelected&&secondSelected&&root->property("inspectedPoint").toInt()==-1&&widget.selectedPoint()==second
        &&tactile::sameForce(widget.chartForce(),force)&&widget.frame().sequence==before.sequence&&widget.history().samples().size()==history;
}

// Exercise the real control, adapter notifications and loaded QML mesh transforms
// while paused, including interpolated points when the caller enables them.
bool independentArrowWidths(TactileVisualizationWidget &widget,QObject *root)
{
    auto *width=widget.findChild<QDoubleSpinBox *>("tactileArrowWidthGain");
    auto *length=widget.findChild<QDoubleSpinBox *>("tactileArrowGain");
    auto *adapter=widget.findChild<TactileSceneAdapter *>();
    if(!width||!length||!adapter||!widget.frame().valid)return false;
    const double previousWidth=width->value(),previousLength=length->value();
    width->setValue(1);length->setValue(2);pump(30);
    struct Transform { QObject *model;QVector3D scale;double y; };
    QList<Transform> models;
    QList<QPair<QObject *,QVariant>> arrowLengths,sphereScales;
    QStringList names{"resultantArrow"};
    for(int i=0;i<adapter->rawPointCount();++i) {
        names.append(QString("pointArrow%1").arg(i));
        for(int axis=0;axis<3;++axis)names.append(QString("componentArrow%1_%2").arg(i).arg(axis));
        auto *point=root->findChild<QObject *>(QString("tactilePoint%1").arg(i));
        if(!point)return false;
        sphereScales.append({point,point->property("scale")});
    }
    for(const auto &name:names) {
        auto *arrow=root->findChild<QObject *>(name);
        if(!arrow)return false;
        arrowLengths.append({arrow,arrow->property("forceLength")});
        for(const char *part:{"arrowShaft","arrowHead"}) {
            auto *model=arrow->findChild<QObject *>(part);
            if(!model)return false;
            models.append({model,model->property("scale").value<QVector3D>(),model->property("y").toDouble()});
        }
    }
    const auto points=adapter->points();const auto resultant=adapter->resultant();
    const auto frame=widget.frame();const auto force=widget.selectedForce();
    const auto history=widget.history().samples().size();
    bool okay=true;
    for(double gain:{0.1,2.5,10.0,1.0}) {
        width->setValue(gain);pump(30);
        okay &= adapter->arrowWidthGain()==gain&&adapter->arrowGain()==2;
        okay &= interpolatedInstancesMatch(adapter,root);
        for(const auto &model:models) {
            const auto actual=model.model->property("scale").value<QVector3D>();
            const auto expected=QVector3D(model.scale.x()*gain,model.scale.y(),model.scale.z()*gain);
            okay &= (actual-expected).length()<1e-6&&model.model->property("y").toDouble()==model.y;
        }
        for(const auto &arrow:arrowLengths)okay &= arrow.first->property("forceLength")==arrow.second;
        for(const auto &sphere:sphereScales)okay &= sphere.first->property("scale")==sphere.second;
        okay &= adapter->points()==points&&adapter->resultant()==resultant
            &&widget.frame().sequence==frame.sequence&&widget.history().samples().size()==history
            &&tactile::sameForce(widget.selectedForce(),force);
    }
    for(double invalid:{0.0,10.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        adapter->setArrowWidthGain(invalid);okay &= adapter->arrowWidthGain()==1;
    }
    width->setValue(previousWidth);length->setValue(previousLength);pump(30);
    return okay;
}

bool componentContract()
{
    bool okay=true;
    for(int axis=0;axis<3;++axis) {
        for(double value:{0.,1.,-1.,199.,-199.,200.,-200.,32767.,-32767.,std::numeric_limits<double>::quiet_NaN()}) {
            const auto a=TactileSceneAdapter::componentArrow(value,axis);
            const bool visible=std::isfinite(value)&&value!=0;
            okay &= a["arrowVisible"].toBool()==visible;
            if(!visible){okay &= a["length"].toDouble()==0;continue;}
            const float sign=value<0?-1.f:1.f;
            const QVector3D expected=axis==0?QVector3D(sign,0,0):axis==1?QVector3D(0,sign,0):QVector3D(0,0,-sign);
            const auto direction=a["rotation"].value<QQuaternion>().rotatedVector(QVector3D(0,1,0));
            okay &= (direction-expected).length()<1e-5;
            okay &= std::abs(a["length"].toDouble()-45*std::abs(value)/32767)<1e-6;
            if(axis==2)okay &= a["offset"].value<QVector3D>().isNull()&&(direction*(a["length"].toFloat()+3)).z()*sign<0;
        }
    }
    return okay;
}

bool fixedPointSizes()
{
    TactileSceneAdapter adapter;
    QColor unloadedColor, loadedColor;
    bool okay=true;
    for(bool loaded:{false,true}) {
        for(int finger=0;finger<3;++finger) {
            const auto frame=tactileTestFrame(finger,loaded?tactile::Force{600,-400,1000}:tactile::Force{});
            adapter.publish(frame,finger,false,0);
            okay &= adapter.valid() && adapter.points().size()==(finger==2?16:20);
            for(const auto &point:adapter.points())okay &= point.toMap()["radius"].toDouble()==1.75;
            const auto color=adapter.points().front().toMap()["color"].value<QColor>();
            if(!loaded)unloadedColor=color;else loadedColor=color;
        }
    }
    return okay && unloadedColor!=loadedColor;
}

tactile::Frame pressureColorFixture(int type=0)
{
    auto frame=tactileTestFrame(type,{});
    const double pressures[]={0,500,1250,2000,2500,4000};
    for(auto &finger:frame.fingers) {
        if(!finger.valid)continue;
        auto &module=finger.modules[0];module.resultant={};
        for(int i=0;i<module.points.size();++i) {
            module.points[i].force={1000,-500,pressures[i%6]};
            module.resultant+=module.points[i].force;
        }
        finger.resultant=module.resultant;
    }
    return frame;
}

bool pressureColorsContract()
{
    const auto nearColor=[](const QColor &a,const QColor &b) {
        return std::abs(a.redF()-b.redF())<.0001&&std::abs(a.greenF()-b.greenF())<.0001&&std::abs(a.blueF()-b.blueF())<.0001;
    };
    const auto blue=TactileSceneAdapter::forceColor(0),red=TactileSceneAdapter::forceColor(2500);
    bool okay=TactileSceneAdapter::PressureColorMaximum==2500
        &&nearColor(blue,QColor::fromRgbF(.28,.54,.8))
        &&nearColor(TactileSceneAdapter::forceColor(1250),QColor::fromRgbF(.41,.8,.28))
        &&nearColor(red,QColor::fromRgbF(.8,.28,.28));
    for(double z:{-32767.,-1.,std::numeric_limits<double>::quiet_NaN()})okay &= TactileSceneAdapter::forceColor(z)==blue;
    for(double z:{2500.,2501.,32767.})okay &= TactileSceneAdapter::forceColor(z)==red;
    TactileSceneAdapter adapter;
    for(int type=0;type<3;++type)for(int grid=2;grid<=4;++grid)for(bool dots:{false,true})for(bool reversed:{false,true}) {
        const auto frame=pressureColorFixture(type);okay &= tactile::validateFrame(frame);
        adapter.setGridSize(grid);adapter.setDotMatrix(dots);adapter.setReverseZArrows(reversed);adapter.setArrowGain(reversed?100:.1);
        adapter.publish(frame,type,false,0);
        okay &= adapter.valid();const auto points=adapter.points();
        for(int i=0;i<points.size();++i) {
            double z=0;
            if(i<adapter.rawPointCount())z=frame.fingers[type].modules[0].points[i].force.z;
            else {
                const auto &sample=tactileSurfaceModel(type).interpolation[grid-3][i-adapter.rawPointCount()];
                for(int j=0;j<sample.sources.size();++j)z+=frame.fingers[type].modules[0].points[sample.sources[j]].force.z*sample.weights[j];
            }
            // Interpolate pressure before mapping color; clipping raw colors first
            // would give a different answer around the 2500-count boundary.
            const auto expected=QColor::fromHsvF((210-210*qBound(0.,z/2500.,1.))/360,.65,.8);
            const auto point=points[i].toMap();
            okay &= std::abs(point["fz"].toDouble()-z)<1e-6&&point["color"].value<QColor>()==expected;
        }
    }
    return okay;
}

// Densification preserves raw measurements and resultants.
bool interpolatedGrids()
{
    TactileSceneAdapter adapter;bool okay=true;
    for(int type=0;type<3;++type) {
        const auto frame=tactileTestFrame(type);
        adapter.setGridSize(2);adapter.publish(frame,type,false,0);
        const auto points=adapter.points();const int count=type==2?16:20;
        okay &= adapter.valid()&&points.size()==count&&adapter.gridSize()==2;
        tactile::Force sum;QMap<float,int> rows;
        float minX=1000,maxX=-1000,minY=1000,maxY=-1000;
        for(int i=0;i<points.size();++i) {
            const auto p=points[i].toMap();const auto position=p["position"].value<QVector3D>();
            rows[position.y()]++;minX=qMin(minX,position.x());maxX=qMax(maxX,position.x());minY=qMin(minY,position.y());maxY=qMax(maxY,position.y());
            const tactile::Force force{p["fx"].toDouble(),p["fy"].toDouble(),p["fz"].toDouble()};sum+=force;
            okay &= !p["interpolated"].toBool()&&tactile::sameForce(force,frame.fingers[type].modules[0].points[i].force);
        }
        okay &= rows.values()==(type==2?QList<int>{4,4,4,4}:QList<int>{2,4,4,4,4,2});
        okay &= std::abs((maxX-minX)/(maxY-minY)-(type==2?1.0:2.0/3.0))<1e-5;
        okay &= tactile::sameForce(sum,frame.fingers[type].resultant);
        int surfaceChanges=0;QObject observer;
        QObject::connect(&adapter,&TactileSceneAdapter::surfaceChanged,&observer,[&]{++surfaceChanges;});
        adapter.publish(frame,type,false,0);
        okay &= surfaceChanges==0;
        const auto resultant=adapter.resultant();
        const auto raw=frame.fingers[type].resultant;
        const auto expectedDirection=QVector3D(raw.x,raw.y,-raw.z).normalized();
        okay &= (resultant["rotation"].value<QQuaternion>().rotatedVector(QVector3D(0,1,0))-expectedDirection).length()<1e-5;
        for(int i=0;i<points.size();++i) {
            const auto p=points[i].toMap();const auto f=frame.fingers[type].modules[0].points[i].force;
            okay &= (p["rotation"].value<QQuaternion>().rotatedVector(QVector3D(0,1,0))-QVector3D(f.x,f.y,-f.z).normalized()).length()<1e-5;
        }
        for(int level=3;level<=4;++level) {
            adapter.setGridSize(level);adapter.publish(frame,type,false,0);
            const auto dense=adapter.points();
            okay &= dense.size()==(type==2?(level==3?49:100):(level==3?65:136));
            for(int i=0;i<count;++i)okay &= dense[i]==points[i];
            okay &= adapter.resultant()==resultant;
            const auto &extra=tactileSurfaceModel(type).interpolation[level-3];
            for(int i=0;i<extra.size();++i) {
                tactile::Force expected;
                for(int j=0;j<extra[i].sources.size();++j) {
                    const auto f=frame.fingers[type].modules[0].points[extra[i].sources[j]].force;
                    expected.x+=f.x*extra[i].weights[j];expected.y+=f.y*extra[i].weights[j];expected.z+=f.z*extra[i].weights[j];
                }
                const auto p=dense[count+i].toMap();
                okay &= p["interpolated"].toBool()&&p["position"].value<QVector3D>()==extra[i].position;
                okay &= tactile::sameForce(expected,{p["fx"].toDouble(),p["fy"].toDouble(),p["fz"].toDouble()});
            }
        }
        adapter.invalidate();okay &= !adapter.valid();
        for(const auto &p:adapter.points()) {
            okay &= !p.toMap()["arrowVisible"].toBool();
            for(const auto &component:p.toMap()["components"].toList())okay &= !component.toMap()["arrowVisible"].toBool();
        }
    }
    return okay;
}

bool pressureDotsContract()
{
    const auto fixture=[](int type,double xy,double z,bool varying) {
        auto frame=tactileTestFrame(type,{});
        for(auto &finger:frame.fingers) {
            if(!finger.valid)continue;
            auto &module=finger.modules[0];module.resultant={};
            for(int i=0;i<module.points.size();++i) {
                module.points[i].force={xy,-xy,z+(varying?i*50:0)};
                module.resultant+=module.points[i].force;
            }
            finger.resultant=module.resultant;
        }
        return frame;
    };
    TactileSceneAdapter adapter;adapter.setDotMatrix(true);bool okay=true;
    for(int type=0;type<3;++type)for(int grid=2;grid<=4;++grid) {
        adapter.setGridSize(grid);
        const auto frame=fixture(type,3000,100,true),otherXY=fixture(type,-21000,100,true);
        for(double gain:{0.1,1.0,25.0,100.0}) {
            adapter.setArrowGain(gain);adapter.publish(frame,type,false,0);
            const auto points=adapter.points();
            okay &= adapter.valid()&&points.size()==(type==2?(grid==2?16:grid==3?49:100):(grid==2?20:grid==3?65:136));
            for(int i=0;i<points.size();++i) {
                double expectedZ=100+i*50;
                if(i>=adapter.rawPointCount()) {
                    expectedZ=0;const auto &sample=tactileSurfaceModel(type).interpolation[grid-3][i-adapter.rawPointCount()];
                    for(int j=0;j<sample.sources.size();++j)expectedZ+=(100+sample.sources[j]*50)*sample.weights[j];
                }
                const auto p=points[i].toMap();
                const auto delta=p["dotPosition"].value<QVector3D>()-p["position"].value<QVector3D>();
                okay &= delta.x()==0&&delta.y()==0&&std::abs(delta.z()-expectedZ/32767*0.0045*gain)<1e-8;
            }
            adapter.publish(otherXY,type,false,0);
            for(int i=0;i<points.size();++i)okay &= adapter.points()[i].toMap()["dotPosition"]==points[i].toMap()["dotPosition"];
        }
        for(double pressure:{0.0,500.0,1500.0}) {
            const auto frame=fixture(type,20000,pressure,false),baseline=fixture(type,0,1000,false);
            adapter.publish(frame,type,false,0,&baseline);
            for(const auto &p:adapter.points()) {
                const auto point=p.toMap();
                const auto delta=point["dotPosition"].value<QVector3D>()-point["position"].value<QVector3D>();
                okay &= std::abs(delta.z()-std::max(0.0,pressure-1000)/32767*0.0045*100)<1e-8
                    &&std::abs(point["fz"].toDouble()-(pressure-1000))<1e-8;
            }
        }
        adapter.publish(fixture(type,20000,0,false),type,false,0);
        for(const auto &p:adapter.points())okay &= p.toMap()["dotPosition"]==p.toMap()["position"];
        adapter.invalidate();
        okay &= !adapter.valid()&&adapter.interpolatedPoints()->count()==0;
        for(const auto &p:adapter.points())okay &= p.toMap()["dotPosition"]==p.toMap()["position"];
    }
    return okay;
}

// Inspect loaded native mesh bounds, not just the intended geometry formulas.
// This catches the Cone base-at-origin / Cylinder centered-at-origin mismatch.
bool connectedRigidHeads(QObject *root,QTextStream &out)
{
    bool okay=true;
    for(const char *name:{"pointArrow0","resultantArrow"}) {
        auto *arrow=root->findChild<QObject *>(name);
        auto *shaft=arrow?arrow->findChild<QObject *>("arrowShaft"):nullptr;
        auto *head=arrow?arrow->findChild<QObject *>("arrowHead"):nullptr;
        if(!shaft||!head){out<<"Missing arrow models: "<<name<<'\n';return false;}
        auto bound=[&](QObject *model,const QString &expression){
            QQmlExpression query(qmlContext(model),model,expression);
            const auto value=query.evaluate();okay &= !query.hasError() && value.isValid();
            return value.toDouble();
        };
        const double shaftMin=bound(shaft,"bounds.minimum.y"),shaftMax=bound(shaft,"bounds.maximum.y");
        const double headMin=bound(head,"bounds.minimum.y"),headMax=bound(head,"bounds.maximum.y");
        out<<name<<" native Y bounds: shaft="<<shaftMin<<","<<shaftMax<<" head="<<headMin<<","<<headMax<<'\n';
        okay &= shaftMin<0 && shaftMax>0 && headMin>=0 && headMax>headMin;
        const auto headScale=head->property("scale").value<QVector3D>();
        const double originalLength=arrow->property("forceLength").toDouble();
        for(double length:{0.04,3.0,30.0,90.0}) {
            arrow->setProperty("forceLength",length);
            const auto shaftScale=shaft->property("scale").value<QVector3D>();
            const double shaftY=shaft->property("y").toDouble(),headY=head->property("y").toDouble();
            out<<"shaft length="<<length<<" tail="<<shaftY+shaftMin*shaftScale.y()
               <<" joint gap="<<(headY+headMin*headScale.y())-(shaftY+shaftMax*shaftScale.y())
               <<" head height="<<(headMax-headMin)*headScale.y()<<" shaft radius="<<shaftScale.x()*50<<'\n';
            okay &= head->property("scale").value<QVector3D>()==headScale;
            okay &= std::abs(shaftY+shaftMin*shaftScale.y())<1e-4;
            okay &= std::abs(shaftY+shaftMax*shaftScale.y()-(headY+headMin*headScale.y()))<1e-4;
            okay &= std::abs((shaftMax-shaftMin)*shaftScale.y()-length)<1e-4;
            okay &= std::abs((headMax-headMin)*headScale.y()-3.0)<1e-4;
            okay &= std::abs(shaftScale.x()*50-0.875)<1e-4;
        }
        arrow->setProperty("forceLength",originalLength);
    }
    return okay;
}
}
void tactileMouseDragFromPoint(QQuickWidget *view,Qt::MouseButton button,const QPointF &start)
{
    const QPointF finish=start+QPointF(70,25);
    QMouseEvent press(QEvent::MouseButtonPress,start,view->mapToGlobal(start.toPoint()),button,button,Qt::NoModifier);QApplication::sendEvent(view,&press);
    QMouseEvent move(QEvent::MouseMove,finish,view->mapToGlobal(finish.toPoint()),Qt::NoButton,button,Qt::NoModifier);QApplication::sendEvent(view,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,finish,view->mapToGlobal(finish.toPoint()),button,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(view,&release);pump(80);
}
void tactileMouseDrag(QQuickWidget *view,Qt::MouseButton button)
{
    tactileMouseDragFromPoint(view,button,QPointF(view->width()*.4,view->height()*.5));
}
int runTactile3DTests(const QString &path)
{
    QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 3;QTextStream out(&file);bool all=true;
    auto check=[&](const char *label,bool result){out<<(result?"PASS ":"FAIL ")<<label<<'\n';out.flush();all&=result;};
    const QVector<tactile::Force> forces={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{.3,-.7,1.2}};
    bool directions=true;for(const auto &force:forces){auto p=TactileSceneAdapter::arrow(force,40,60);const auto actual=p["rotation"].value<QQuaternion>().rotatedVector(QVector3D(0,1,0));const auto expected=QVector3D(force.x,force.y,force.z).normalized();directions&=p["arrowVisible"].toBool()&&(actual-expected).length()<1e-5;}
    check("six axes mixed and parallel antiparallel arrow rotations",directions);
    check("zero and near zero hidden",!TactileSceneAdapter::arrow({},40,60)["arrowVisible"].toBool()&&!TactileSceneAdapter::arrow({1e-10,0,0},40,60)["arrowVisible"].toBool());
    check("display saturation preserves force",TactileSceneAdapter::arrow({100,0,0},40,60)["length"].toDouble()==60&&TactileSceneAdapter::arrow({100,0,0},40,60)["fx"].toDouble()==100);
    check("surface projection retains top row and positive contact depth",TactileSceneAdapter::pointPosition(0).y()>TactileSceneAdapter::pointPosition(2).y()&&TactileSceneAdapter::pointPosition(0).z()>0);
    check("Z pressure colors span blue to red at 2500 counts and follow interpolated pressure independently of arrow reversal and gain",pressureColorsContract());
    TactileSceneAdapter adapter;auto fixture=tactileTestFrame();bool mapping=true;
    for(int f=0;f<3;++f)for(int m=-1;m<1;++m){fixture=tactileTestFrame(f);adapter.publish(fixture,f,m>=0,qMax(0,m));const auto expected=m<0?fixture.fingers[f].resultant:fixture.fingers[f].modules[m].resultant;const auto value=adapter.resultant();mapping&=adapter.valid()&&adapter.points().size()==(f==2?16:20)&&value["fx"].toDouble()==expected.x&&value["fy"].toDouble()==expected.y&&value["fz"].toDouble()==expected.z;}
    check("all finger module resultant mappings",mapping);fixture.valid=false;adapter.publish(fixture,0,false,0);check("invalid snapshot clears validity and old arrows",!adapter.valid()&&!adapter.resultant()["arrowVisible"].toBool());
    TactileVisualizationWidget widget;widget.resize(900,620);widget.show();widget.start();pump(100);
    widget.showMinimized();pump(100);widget.showNormal();pump(100);
    check("standalone minimize and restore never manufacture data",widget.running()&&!widget.frame().valid&&widget.history().samples().empty());
    TactileTestAccess::showSnapshot(widget);pump(100);
    auto *view=widget.findChild<QQuickWidget *>("tactileQuickView");auto *bridge=widget.findChild<TactileSceneAdapter *>();
    check("independent Quick 3D scene ready",view&&view->status()==QQuickWidget::Ready&&view->rootObject()&&bridge&&bridge->valid());
    if(view&&view->rootObject()){
        auto *root=view->rootObject();const auto force=widget.selectedForce();const auto frame=widget.frame();const auto historySize=widget.history().samples().size();const auto yaw=root->property("orbitYaw").toDouble();
        bool pointLabels=false;
        for(auto *object:root->findChildren<QObject *>()){
            const auto text=object->property("text").toString();bool number=false;
            if(text.startsWith('P')&&text.mid(1).toInt(&number)>0&&number)pointLabels=true;
        }
        check("permanent P1-P20/P16 labels are removed from the model",!pointLabels);
        check("fixed-snapshot thickness control scales all shafts and heads without changing length, sphere size or measurements",independentArrowWidths(widget,root));
        tactileMouseDrag(view,Qt::LeftButton);
        check("snapshot viewport receives orbit drag without selecting point",root->property("orbitYaw").toDouble()!=yaw&&root->property("inspectedPoint").toInt()==-1);
        check("camera does not alter force sequence or history",tactile::sameForce(force,widget.selectedForce())&&frame.sequence==widget.frame().sequence&&widget.history().samples().size()==historySize);
        const auto pan=root->property("cameraPan").value<QVector3D>();tactileMouseDrag(view,Qt::MiddleButton);check("middle drag changes only local camera pan",root->property("cameraPan").value<QVector3D>()!=pan);
        const double distance=root->property("cameraDistance").toDouble();const QPointF center(view->rect().center());
        QWheelEvent wheel(center,view->mapToGlobal(center.toPoint()),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(view,&wheel);pump(50);
        check("wheel zoom preserves frozen data",root->property("cameraDistance").toDouble()<distance&&tactile::sameForce(force,widget.selectedForce()));
        auto *mode=widget.findChild<QComboBox *>("tactileDisplayMode");
        auto *density=widget.findChild<QComboBox *>("tactileDensity");
        const int previousMode=mode->currentIndex(),previousDensity=density->currentIndex();
        for(int type:{0,1,2})for(const auto &displayMode:{QStringLiteral("xyz"),QStringLiteral("dots")}) {
            TactileTestAccess::showSnapshot(widget,type);
            mode->setCurrentIndex(mode->findData(displayMode));
            bool picked=true,expandedPicked=true;
            for(int level:{0,1,2}) {
                density->setCurrentIndex(level);QMetaObject::invokeMethod(root,"frontView");pump(60);
                picked &= rawPointPicking(widget,out);
                expandedPicked &= rawPointPicking(widget,out,true);
            }
            check("every raw sensor point click selects its XYZ curve and visible table row at all interpolation levels",picked);
            check("clicks nine pixels outside raw spheres select the matching curve and row in XYZ and dot modes at all densities",expandedPicked);
        }
        TactileTestAccess::showSnapshot(widget,2);mode->setCurrentIndex(mode->findData("xyz"));density->setCurrentIndex(0);
        QMetaObject::invokeMethod(root,"frontView");pump(60);
        check("overlapping enlarged targets select the nearest point after zoom and empty space preserves chart selection",overlappingPointPicking(widget));
        QQmlExpression dragStart(qmlContext(root),root,"project(displayPosition(pointSnapshot[0]))");
        const auto dragPoint=dragStart.evaluate().value<QVector3D>();
        const int beforeDragSelection=widget.selectedPoint();const auto beforeDragYaw=root->property("orbitYaw").toDouble();
        tactileMouseDragFromPoint(view,Qt::LeftButton,QPointF(dragPoint.x()+9,dragPoint.y()));
        check("dragging from an enlarged point target orbits without changing sensor selection",widget.selectedPoint()==beforeDragSelection
            &&root->property("orbitYaw").toDouble()!=beforeDragYaw&&root->property("inspectedPoint").toInt()==-1);
        mode->setCurrentIndex(previousMode);density->setCurrentIndex(previousDensity);
        TactileTestAccess::showSnapshot(widget,0);root->setProperty("inspectedPoint",-1);
        auto *legend=root->findChild<QObject *>("tactileLegend");
        check("model bottom-left legend is absent regardless of communication status",!legend);
        const auto directory=QFileInfo(path).absolutePath();
        widget.findChild<QPushButton *>("frontTactileView")->click();pump(100);check("front screenshot",widget.grab().save(directory+"/front.png"));
        widget.findChild<QPushButton *>("resetTactileView")->click();pump(100);check("oblique screenshot",widget.grab().save(directory+"/oblique.png"));
        root->setProperty("orbitYaw",82.0);root->setProperty("orbitPitch",0.0);pump(100);check("side screenshot",widget.grab().save(directory+"/side.png"));
        const auto keptYaw=root->property("orbitYaw");TactileTestAccess::showSnapshot(widget,2);
        check("selection retains camera and matches resultant",root->property("orbitYaw")==keptYaw&&bridge->resultant()["fz"].toDouble()==widget.selectedForce().z&&bridge->resultant()["fx"].toDouble()==widget.history().samples().back().forces[2][0].x);
        auto *resultant=root->findChild<QObject *>("resultantArrow");
        check("QML resultant uses adapter direction length and anchor",resultant&&(resultant->property("rotation").value<QQuaternion>().rotatedVector(QVector3D(0,1,0))-bridge->resultant()["rotation"].value<QQuaternion>().rotatedVector(QVector3D(0,1,0))).length()<1e-5&&std::abs(resultant->property("forceLength").toDouble()-bridge->resultant()["length"].toDouble())<1e-4&&resultant->property("position").value<QVector3D>()==bridge->resultant()["position"].value<QVector3D>());
        widget.findChild<QPushButton *>("resetTactileView")->click();pump(100);check("module resultant screenshot",widget.grab().save(directory+"/module.png"));
        auto *sizeGain=widget.findChild<QDoubleSpinBox *>("tactileResultantSizeGain");
        if(resultant&&sizeGain){
            const auto originalScale=resultant->property("scale").value<QVector3D>();
            const auto anchor=resultant->property("position");
            const auto rotation=resultant->property("rotation");
            const auto originalResultant=bridge->resultant();
            const auto originalPoints=bridge->points();
            sizeGain->setValue(2);
            pump(40);
            check("resultant size doubles on all axes without moving its anchor or changing force and point data",
                (originalScale-QVector3D(.0001f,.0001f,.0001f)).length()<1e-8
                &&(resultant->property("scale").value<QVector3D>()-originalScale*2).length()<1e-8
                &&resultant->property("position")==anchor&&resultant->property("rotation")==rotation
                &&bridge->resultant()==originalResultant&&bridge->points()==originalPoints);
            check("scaled resultant screenshot",widget.grab().save(directory+"/resultant_2x.png"));
            sizeGain->setValue(1);
        }else check("resultant size control and QML node exist",false);
        auto *point=root->findChild<QObject *>("tactilePoint0");TactileTestAccess::advance(widget);pump(100);
        check("accepted frames retain the twelve scene nodes",point&&view->rootObject()==root&&root->findChild<QObject *>("tactilePoint0")==point&&root->findChild<QObject *>("tactilePoint11"));
        const auto paletteFrame=widget.frame();const auto paletteForce=widget.selectedForce();
        const auto originalPalette=widget.palette();QPalette alternate=originalPalette;
        alternate.setColor(QPalette::Window,QColor("#30343b"));alternate.setColor(QPalette::Base,QColor("#242930"));
        alternate.setColor(QPalette::WindowText,QColor("#e8edf3"));alternate.setColor(QPalette::Text,QColor("#e8edf3"));
        alternate.setColor(QPalette::Button,QColor("#3c424b"));alternate.setColor(QPalette::ButtonText,QColor("#e8edf3"));
        alternate.setColor(QPalette::Highlight,QColor("#79b8ed"));alternate.setColor(QPalette::HighlightedText,QColor("#18212b"));
        alternate.setColor(QPalette::Mid,QColor("#9ba6b5"));widget.setPalette(alternate);pump(100);
        check("palette keeps the shared dark scene legible and frozen data unchanged",bridge->background()==QColor("#161b22")&&bridge->textColor().lightnessF()>.7&&bridge->accent()==alternate.color(QPalette::Highlight).lighter(145)&&paletteFrame.sequence==widget.frame().sequence&&tactile::sameForce(paletteForce,widget.selectedForce()));
        const auto gainPalette=widget.findChild<QWidget *>("tactileArrowGain")->palette();
        const auto captionPalette=widget.findChild<QWidget *>("tactileArrowGainLabel")->palette();
        const auto curvePalette=widget.findChild<QWidget *>("tactileCurves")->palette();
        check("settings captions, numeric editors and curves inherit a legible dark palette",
            gainPalette.color(QPalette::Base).lightnessF()<.3&&gainPalette.color(QPalette::Text).lightnessF()>.7
            &&captionPalette.color(QPalette::WindowText).lightnessF()>.7
            &&curvePalette.color(QPalette::Base).lightnessF()<.3&&curvePalette.color(QPalette::Text).lightnessF()>.7);
        auto *displayMode=widget.findChild<QComboBox *>("tactileDisplayMode");
        auto *gain=widget.findChild<QDoubleSpinBox *>("tactileArrowGain");
        const auto comboBackground=displayMode->grab().toImage().pixelColor(displayMode->width()-30,displayMode->height()/2);
        const auto numericBackground=gain->grab().toImage().pixelColor(gain->width()-30,gain->height()/2);
        check("styled dropdown and numeric editor actually paint dark backgrounds after palette changes",
            comboBackground.alpha()>200&&comboBackground.lightnessF()<.3
            &&numericBackground.alpha()>200&&numericBackground.lightnessF()<.3);
        check("alternate palette screenshot",widget.grab().save(directory+"/palette_alternate.png"));
        widget.setPalette(originalPalette);pump(50);
        mode->setCurrentIndex(mode->findData("xyz"));density->setCurrentIndex(0);
        QMetaObject::invokeMethod(root,"frontView");pump(60);
        widget.findChild<QPushButton *>("tactileTare")->click();
        check("raw point clicks retain tare delta semantics",widget.hasDisplayBaseline()&&rawPointPicking(widget,out));
    }
    const auto closedSession=widget.frame().sessionId;widget.close();check("closed view invalidates old forces",!widget.running()&&!bridge->valid());
    widget.show();widget.start();pump(100);
    check("3D reopen stays unavailable until new hardware data",!bridge->valid()&&!widget.frame().valid&&widget.history().samples().empty());
    // Intentional missing resource: require explicit failure and no stale values.
    view->setSource(QUrl("qrc:/qml/__missing_tactile_test__.qml"));pump(30);
    check("QML load failure stops updates and clears stale values",view->status()==QQuickWidget::Error&&!widget.running()&&!bridge->valid()&&widget.findChild<QLabel *>("force0")->text().contains("--")&&widget.findChild<QLabel *>("tactileViewError")->isVisible());
    widget.close();
    out<<(all?"RESULT PASS\n":"RESULT FAIL\n");return all?0:4;
}

// Short render/error check without showing or activating a desktop window.
// It does not claim native focus, physical input or presentation-rate coverage.
int runTactileRenderSmoke(const QString &path)
{
    QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
    QTextStream out(&file);bool all=true;
    auto check=[&](const char *label,bool okay){out<<(okay?"PASS ":"FAIL ")<<label<<'\n';out.flush();all&=okay;};
    check("positive Z including 1 and 199 retained, signed XY, linear lengths, inward Z and native signed-count directions",componentContract());
    check("sensor row counts, aspect ratios, raw forces and two interpolation levels",interpolatedGrids());
    check("all three sensor types keep small fixed sphere radius while force color changes",fixedPointSizes());
    check("all sensor types and both display modes color raw and interpolated points by 0 to 2500 Z pressure",pressureColorsContract());
    check("dot lift ignores XY, interpolates Z, scales linearly through 25x and 100x, clamps nonpositive delta and clears stale lifts",pressureDotsContract());
    {
        QMainWindow host;
        host.setAttribute(Qt::WA_DontShowOnScreen);host.setAttribute(Qt::WA_ShowWithoutActivating);
        auto *dock=new TactileDockWidget(&host);host.addDockWidget(Qt::LeftDockWidgetArea,dock);
        dock->hide();host.resize(1100,750);host.show();pump(200);
        auto *display=dock->display();
        check("hidden restored dock does not start acquisition",!display->running()&&!display->frame().valid);
        dock->show();pump(300);
        auto *adapter=display->findChild<TactileSceneAdapter *>();
        for(int type:{0,1,2}) {
            display->findChild<QPushButton *>(QString("finger%1").arg(type))->click();pump(50);
            check("restored model choices show no data without a connected sensor",display->running()&&!display->frame().valid&&!adapter->valid());
        }
        check("restoring a dock does not open hardware",!display->findChild<QPushButton *>("tactileDisconnect")->isEnabled());
        bool reattached=true;
        for(bool floating:{true,false}) {
            dock->setAttribute(Qt::WA_DontShowOnScreen);dock->setFloating(floating);pump(200);
            auto *quick=display->findChild<QQuickWidget *>("tactileQuickView");
            reattached &= quick->status()==QQuickWidget::Ready&&!quick->grabFramebuffer().isNull()
                &&!display->frame().valid&&!adapter->valid();
        }
        check("dock scene recreation keeps geometry and the unavailable state",reattached);
        dock->hide();pump(250);const auto count=display->refreshCount();pump(80);
        check("hidden dock suspends view refresh",!display->running()&&display->refreshCount()==count);
        dock->show();pump(250);
        check("visible dock resumes refresh without generating data",display->running()&&!display->frame().valid);
        dock->close();pump(200);
        check("closed dock stops and clears data",!display->running()&&!display->frame().valid);
        dock->openSession();pump(200);
        check("reopened dock waits for hardware",display->running()&&!display->frame().valid);
        dock->close();
    }
    TactileVisualizationWidget widget;
    widget.setAttribute(Qt::WA_DontShowOnScreen);widget.setAttribute(Qt::WA_ShowWithoutActivating);
    // The preceding dock fixture persists its dense grid. This fixture starts
    // explicitly on raw points before testing the interpolation controls.
    widget.findChild<QComboBox *>("tactileDensity")->setCurrentIndex(0);
    widget.resize(900,620);widget.show();TactileTestAccess::showSnapshot(widget);pump(100);
    auto *view=widget.findChild<QQuickWidget *>("tactileQuickView");
    check("hidden Quick scene loads and accepts frames",view&&view->status()==QQuickWidget::Ready&&widget.frame().valid&&widget.refreshCount()>0);
    if(view&&view->rootObject()) {
        const auto folder=QFileInfo(path).absolutePath();
        const auto framebuffer=view->grabFramebuffer();
        check("offscreen framebuffer available",!framebuffer.isNull());

        check("loaded QML widths respond independently at minimum, typical, maximum and original thickness",independentArrowWidths(widget,view->rootObject()));
        auto *width=widget.findChild<QDoubleSpinBox *>("tactileArrowWidthGain");
        check("original thickness screenshot",view->grabFramebuffer().save(folder+"/arrow_width_1x.png"));
        width->setValue(2.5);pump(80);
        check("enlarged thickness screenshot",view->grabFramebuffer().save(folder+"/arrow_width_2_5x.png"));
        width->setValue(1);pump(30);
        check("loaded arrow meshes join with fixed heads and half-sphere-width shafts",connectedRigidHeads(view->rootObject(),out));
        bool minimumSpheres=true;
        for(int i=0;i<20;++i) {
            auto *point=view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(i));
            auto *arrow=view->rootObject()->findChild<QObject *>(QString("pointArrow%1").arg(i));
            auto *shaft=arrow?arrow->findChild<QObject *>("arrowShaft"):nullptr;
            minimumSpheres &= point && (point->property("scale").value<QVector3D>()-QVector3D(.035f,.035f,.035f)).length()<1e-6;
            minimumSpheres &= point && shaft && std::abs(shaft->property("scale").value<QVector3D>().x()*2-point->property("scale").value<QVector3D>().x())<1e-6;
        }
        check("all twenty fixed spheres have half-diameter shafts",minimumSpheres);
        auto *density=widget.findChild<QComboBox *>("tactileDensity");
        auto *adapter=widget.findChild<TactileSceneAdapter *>();
        auto *componentButton=widget.findChild<QPushButton *>("componentArrowsToggle");
        auto *resultantButton=widget.findChild<QPushButton *>("resultantArrowToggle");
        bool toggles=componentButton&&resultantButton;
        const auto beforeToggle=widget.frame();const auto beforeHistory=widget.history().samples().size();
        for(bool components:{false,true})for(bool resultant:{false,true}) {
            componentButton->setChecked(components);resultantButton->setChecked(resultant);pump(30);
            toggles &= adapter->componentArrows()==components&&adapter->resultantArrow()==resultant;
            auto *sum=view->rootObject()->findChild<QObject *>("resultantArrow");
            toggles &= sum&&sum->property("visible").toBool()==resultant;
            for(int i=0;i<adapter->points().size();++i)for(int axis=0;axis<3;++axis) {
                auto *arrow=view->rootObject()->findChild<QObject *>(QString("componentArrow%1_%2").arg(i).arg(axis));
                const auto expected=adapter->points()[i].toMap()["components"].toList()[axis].toMap();
                toggles &= arrow&&arrow->property("visible").toBool()==(components&&expected["arrowVisible"].toBool());
                if(arrow)toggles &= std::abs(arrow->property("forceLength").toDouble()-expected["length"].toDouble())<1e-5;
            }
        }
        check("independent component and resultant buttons preserve frozen frame and history",toggles&&widget.frame().sequence==beforeToggle.sequence&&widget.history().samples().size()==beforeHistory);
        const auto held=widget.frame();const auto heldForce=widget.selectedForce();const auto historyCount=widget.history().samples().size();
        const auto heldYaw=view->rootObject()->property("orbitYaw");
        auto *renderSettings=RenderingSettings::instance();
        const bool previousLowCost=renderSettings->lowCostRendering();
        auto *environment=view->rootObject()->findChild<QObject *>("tactileEnvironment");
        auto *keyLight=view->rootObject()->findChild<QObject *>("tactileKeyLight");
        const auto heldPoints=adapter->points(),heldResultant=QVariantList{adapter->resultant()};
        bool qualityOkay=environment&&keyLight&&!widget.findChild<QComboBox *>("tactileRenderQuality");
        for(int preset:{1,0}) {
            renderSettings->setLowCostRendering(preset==1);pump(100);
            QQmlExpression expectedAa(qmlContext(environment),environment,preset?"SceneEnvironment.NoAA":"SceneEnvironment.MSAA");
            const auto aa=expectedAa.evaluate();
            qualityOkay &= !expectedAa.hasError()&&environment->property("antialiasingMode").toInt()==aa.toInt();
            qualityOkay &= keyLight->property("castsShadow").toBool()==(preset==0)&&adapter->lowCostRendering()==(preset==1);
            qualityOkay &= adapter->points()==heldPoints&&QVariantList{adapter->resultant()}==heldResultant
                &&widget.frame().sequence==held.sequence&&widget.history().samples().size()==historyCount
                &&view->rootObject()->property("orbitYaw")==heldYaw;
            check("render quality screenshot",widget.grab().save(folder+QString("/quality_%1.png").arg(preset)));
            TactileVisualizationWidget restored;
            auto *restoredAdapter=restored.findChild<TactileSceneAdapter *>();
            qualityOkay &= restoredAdapter&&restoredAdapter->lowCostRendering()==(preset==1);
        }
        renderSettings->setLowCostRendering(previousLowCost);
        check("global quality controls actual AA and shadows, initializes new views, and preserves all forces history camera",qualityOkay);
        auto *opacity=widget.findChild<QSlider *>("tactileModelOpacity");
        auto *surfaceModel=view->rootObject()->findChild<QObject *>("tactileSensorModel");
        bool opacityOkay=opacity&&surfaceModel;
        for(int percent:{0,45,100,45}) {
            opacity->setValue(percent);pump(50);
            opacityOkay &= std::abs(adapter->modelOpacity()-percent/100.0)<1e-6
                &&std::abs(surfaceModel->property("opacity").toDouble()-percent/100.0)<1e-6
                &&widget.frame().sequence==held.sequence&&widget.history().samples().size()==historyCount
                &&view->rootObject()->property("orbitYaw")==heldYaw;
            check("opacity screenshot",widget.grab().save(folder+QString("/opacity_%1.png").arg(percent)));
        }
        check("opacity slider affects only model and preserves frame history camera",opacityOkay);
        check("two interpolation levels available",density&&density->isEnabled()&&density->count()==3);
        for(int type:{0,1,2,0}) {
            TactileTestAccess::showSnapshot(widget,type);pump(100);
            const int count=type==2?16:20;
            check("type selection updates nodes without changing raw point counts or camera",adapter->points().size()==count
                &&view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(count-1))
                &&!view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(count))
                &&widget.frame().sequence==held.sequence&&widget.history().samples().size()==historyCount
                &&view->rootObject()->property("orbitYaw")==heldYaw);
            QMetaObject::invokeMethod(view->rootObject(),"frontView");pump(80);
            check("sensor layout screenshot",widget.grab().save(folder+QString("/sensor_%1.png").arg(type)));
            auto *model=view->rootObject()->findChild<QObject *>("tactileSensorModel");
            auto *sensorRoot=view->rootObject()->findChild<QObject *>("tactileSensorRoot");
            QQmlExpression bound(qmlContext(model),model,"bounds.maximum.minus(bounds.minimum)");
            const auto actualBounds=bound.evaluate().value<QVector3D>();
            const auto &surface=tactileSurfaceModel(type);
            const QVector3D expectedBounds(surface.boundsSize.x(),surface.boundsSize.z(),surface.boundsSize.y());
            check("project mesh loads at source metre size with one root conversion",surface.valid&&model&&sensorRoot
                &&!bound.hasError()&&(actualBounds-expectedBounds).length()<.0001
                &&sensorRoot->property("scale").value<QVector3D>()==QVector3D(1000,1000,1000));
            QMetaObject::invokeMethod(view->rootObject(),"resetView");pump(100);
            check("model oblique screenshot",widget.grab().save(folder+QString("/model_oblique_%1.png").arg(type)));

            for(int level=1;level<=2;++level) {
                density->setCurrentIndex(level);pump(100);
                const int denseCount=type==2?(level==1?49:100):(level==1?65:136);
                check("dense glyphs render without changing raw history",adapter->points().size()==denseCount
                    &&view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(count-1))
                    &&!view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(count))
                    &&interpolatedInstancesMatch(adapter,view->rootObject(),&out)
                    &&widget.frame().sequence==held.sequence&&widget.history().samples().size()==historyCount);
                if(type==2&&level==1)check("interpolated force arrows use the same independent thickness control",independentArrowWidths(widget,view->rootObject()));
                check("dense surface screenshot",widget.grab().save(folder+QString("/dense_%1_%2.png").arg(type).arg(level)));
                if(type==0&&level==2) {
                    QMetaObject::invokeMethod(view->rootObject(),"frontView");pump(100);
                    check("interpolated picking retains point index and frozen data",interpolatedPicking(view,adapter)
                        &&widget.frame().sequence==held.sequence&&widget.history().samples().size()==historyCount);
                    QMetaObject::invokeMethod(view->rootObject(),"resetView");pump(50);
                }
            }
            density->setCurrentIndex(0);pump(50);
            view->rootObject()->setProperty("orbitYaw",heldYaw);
        }
        auto *mode=widget.findChild<QComboBox *>("tactileDisplayMode");
        auto *gain=widget.findChild<QDoubleSpinBox *>("tactileArrowGain");
        const auto originalGain=gain->value();
        const auto originalPoints=adapter->points();
        mode->setCurrentIndex(mode->findData("dots"));
        bool dotsOkay=adapter->dotMatrix()&&!width->isEnabled()&&!componentButton->isEnabled()&&!resultantButton->isEnabled()
            &&!widget.findChild<QWidget *>("tactileForceScale")->isHidden();
        gain->setValue(1);
        for(int type:{0,1,2}) {
            TactileTestAccess::showSnapshot(widget,type);
            for(int level:{0,1,2}) {
                density->setCurrentIndex(level);pump(60);
                dotsOkay &= interpolatedInstancesMatch(adapter,view->rootObject(),&out);
                auto *sensorRoot=view->rootObject()->findChild<QObject *>("tactileSensorRoot");
                for(int i=0;i<adapter->rawPointCount();++i) {
                    auto *sphere=view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(i));
                    QQmlExpression position(qmlContext(sphere),sphere,"mapPositionToNode(parent.parent.parent, Qt.vector3d(0, 0, 0))");
                    const auto actual=position.evaluate().value<QVector3D>();
                    dotsOkay &= sensorRoot&&!position.hasError()&&(actual-adapter->points()[i].toMap()["dotPosition"].value<QVector3D>()).length()<1e-8;
                    QQmlExpression color(qmlContext(sphere),sphere,"materials[0].baseColor");
                    dotsOkay &= color.evaluate().value<QColor>()==adapter->points()[i].toMap()["color"].value<QColor>()&&!color.hasError();
                    for(int axis=0;axis<3;++axis)dotsOkay &= !view->rootObject()->findChild<QObject *>(QString("componentArrow%1_%2").arg(i).arg(axis))->property("visible").toBool();
                    dotsOkay &= !view->rootObject()->findChild<QObject *>(QString("pointArrow%1").arg(i))->property("visible").toBool();
                }
                dotsOkay &= !view->rootObject()->findChild<QObject *>("resultantArrow")->property("visible").toBool();
                check("dot matrix surface capture",view->grabFramebuffer().save(folder+QString("/dots_%1_%2.png").arg(type).arg(level)));
                if(type==0&&level==2) {
                    QMetaObject::invokeMethod(view->rootObject(),"frontView");pump(60);
                    check("lifted interpolated pressure-colored dots remain pickable",interpolatedPicking(view,adapter));
                    QMetaObject::invokeMethod(view->rootObject(),"resetView");pump(30);
                }
            }
        }
        TactileTestAccess::showSnapshot(widget,0);density->setCurrentIndex(2);
        const auto atOne=adapter->points();gain->setValue(25);pump(60);
        for(int i=0;i<atOne.size();++i) {
            const auto p=atOne[i].toMap();const auto anchor=p["position"].value<QVector3D>();
            const auto expected=anchor+(p["dotPosition"].value<QVector3D>()-anchor)*25;
            dotsOkay &= (adapter->points()[i].toMap()["dotPosition"].value<QVector3D>()-expected).length()<1e-7;
            QQmlExpression projected(qmlContext(view->rootObject()),view->rootObject(),QString("project(display.points[%1].dotPosition)").arg(i));
            const auto screen=projected.evaluate().value<QVector3D>();
            dotsOkay &= !projected.hasError()&&screen.z()>0&&view->rect().contains(QPoint(qRound(screen.x()),qRound(screen.y())));
        }
        check("fixed-snapshot dot controls update raw and instance spheres, suppress all force arrows and preserve source history",dotsOkay
            &&widget.frame().sequence==held.sequence&&widget.history().samples().size()==historyCount
            &&tactile::sameForce(widget.selectedForce(),heldForce));
        check("25x dot displacement capture",view->grabFramebuffer().save(folder+"/dots_25x.png"));
        gain->setValue(originalGain);density->setCurrentIndex(0);mode->setCurrentIndex(mode->findData("xyz"));pump(30);
        check("switching back restores XYZ geometry and independent arrow preferences",!adapter->dotMatrix()
            &&adapter->points()==originalPoints&&width->isEnabled()&&componentButton->isEnabled()&&resultantButton->isEnabled()
            &&componentButton->isChecked()&&resultantButton->isChecked()
            &&view->rootObject()->findChild<QObject *>("resultantArrow")->property("visible").toBool());
        bool pressureRendering=true;
        const auto colorFixture=pressureColorFixture();
        for(const auto &displayMode:{QString("xyz"),QString("dots")})for(int level:{0,1,2}) {
            mode->setCurrentIndex(mode->findData(displayMode));density->setCurrentIndex(level);
            adapter->publish(colorFixture,0,false,0);pump(60);
            pressureRendering &= !widget.findChild<QWidget *>("tactileForceScale")->isHidden()
                &&interpolatedInstancesMatch(adapter,view->rootObject(),&out);
            for(int i=0;i<adapter->rawPointCount();++i) {
                auto *sphere=view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(i));
                if(!sphere){pressureRendering=false;continue;}
                QQmlExpression color(qmlContext(sphere),sphere,"materials[0].baseColor");
                pressureRendering &= color.evaluate().value<QColor>()==adapter->points()[i].toMap()["color"].value<QColor>()&&!color.hasError();
            }
            pressureRendering &= widget.grab().save(folder+QString("/pressure_colors_%1_%2.png").arg(displayMode).arg(level));
        }
        check("QML materials and GPU instances show blue green red pressure colors with a visible scale in both modes at all interpolation levels",pressureRendering);
        mode->setCurrentIndex(mode->findData("xyz"));density->setCurrentIndex(0);adapter->publish(held,0,false,0);pump(30);
        int bindingWarnings=0;QObject warningObserver;
        QObject::connect(view->engine(),&QQmlEngine::warnings,&warningObserver,[&]{++bindingWarnings;});
        auto invalidFrame=held;invalidFrame.valid=false;bool invalidTypes=true;
        for(int type:{2,0}) {
            adapter->publish(invalidFrame,type,false,0);pump(30);
            const int count=type==2?16:20;
            invalidTypes &= !adapter->valid()&&adapter->points().size()==count
                &&view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(count-1))
                &&!view->rootObject()->findChild<QObject *>(QString("tactilePoint%1").arg(count))
                &&interpolatedInstancesMatch(adapter,view->rootObject());
        }
        check("invalid type changes clear instances and resize raw delegates without binding warnings",invalidTypes&&bindingWarnings==0);
        TactileTestAccess::advance(widget);pump(80);
        check("right-side controls render",widget.grab().save(folder+"/tactile_right_layout.png"));
        TactileTestAccess::showSnapshot(widget,2);pump(180);
        check("button selection keeps the same force mapping",widget.selectedFinger()==2&&!widget.moduleMode()&&widget.selectedModule()==0&&tactile::sameForce(widget.selectedForce(),widget.frame().fingers[2].resultant));
        QEnterEvent enter(QPointF(10,10),QPointF(10,10),QPointF(10,10));QApplication::sendEvent(widget.findChild<QPushButton *>("finger1"),&enter);pump(90);
        check("animated controls render",widget.grab().save(folder+"/tactile_selection.png"));
        auto *pointer=view->rootObject()->findChild<QObject *>("tactilePointerArea");
        check("no viewport hover prompts",view->toolTip().isEmpty()&&pointer&&!pointer->property("hoverEnabled").toBool());
        tactileMouseDrag(view,Qt::LeftButton);view->grabFramebuffer();
        check("scene remains ready after interaction",view->status()==QQuickWidget::Ready);
    }
    widget.close();const auto closed=widget.refreshCount();pump(80);
    check("close stops updates",!widget.running()&&widget.refreshCount()==closed);
    out<<(all?"RESULT PASS\n":"RESULT FAIL\n");return all?0:4;
}
