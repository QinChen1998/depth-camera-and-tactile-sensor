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
#include "../sensors/TactileCanvas.h"
#include <QWheelEvent>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <limits>
namespace {
void pump(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}

void clickPoint(TactileCanvas &canvas,const QPointF &local)
{
    const auto global=canvas.mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,local,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&press);
    QApplication::sendEvent(&canvas,&release);
    pump(10);
}

bool rawPointPicking(TactileVisualizationWidget &widget,QTextStream &out,bool expanded=false)
{
    auto *view=widget.findChild<TactileCanvas *>("tactileSceneView");
    auto *adapter=widget.findChild<TactileSceneAdapter *>();
    auto *table=widget.findChild<QTableWidget *>("tactilePointValues");
    if(!view||!adapter||!table)return false;
    const auto frame=widget.frame();
    const auto resultant=widget.selectedForce();
    const auto history=widget.history().samples().size();
    const auto points=adapter->points();
    for(int i=0;i<adapter->rawPointCount();++i){
        const auto point=points[i].toMap();
        const auto screen=view->project(point[adapter->dotMatrix()?"dotPosition":"position"].value<QVector3D>());
        if(screen.z()<=0 || !view->rect().contains(screen.toPoint()))return false;
        QPointF probe=screen.toPointF();
        if(expanded){
            bool found=false;
            for(const auto &offset:QList<QPointF>{{9,0},{-9,0},{0,9},{0,-9},{6.4,6.4},{-6.4,6.4},{6.4,-6.4},{-6.4,-6.4}}){
                if(view->pickPoint(probe+offset)!=i)continue;
                probe+=offset;found=true;break;
            }
            if(!found)return false;
        }
        clickPoint(*view,probe);
        const auto raw=frame.fingers[widget.selectedFinger()].modules[0].points[i].force;
        const auto expected=widget.curveForce(raw,i);
        bool okay=view->inspectedPoint()==i&&widget.selectedPoint()==i&&table->currentRow()==i
            &&table->selectionModel()->isRowSelected(i,QModelIndex())
            &&tactile::sameForce(widget.chartForce(),expected)&&tactile::sameForce(widget.selectedForce(),resultant)
            &&widget.frame().sequence==frame.sequence&&widget.history().samples().size()==history;
        if(!okay){out<<"pick mismatch for sensor "<<widget.selectedFinger()<<", point "<<i<<'\n';return false;}
    }
    return true;
}

bool interpolatedPicking(TactileVisualizationWidget &widget,TactileCanvas &view,TactileSceneAdapter &adapter)
{
    const auto points=adapter.points();
    const int selected=widget.selectedPoint();
    for(int i=adapter.rawPointCount();i<points.size();++i){
        const auto point=points[i].toMap();
        const auto screen=view.project(point[adapter.dotMatrix()?"dotPosition":"position"].value<QVector3D>());
        if(screen.z()<=0||view.pickPoint(screen.toPointF())!=i)continue;
        clickPoint(view,screen.toPointF());
        return view.inspectedPoint()==i&&widget.selectedPoint()==selected;
    }
    return false;
}

bool arrowWidthControls(TactileVisualizationWidget &widget,TactileCanvas &view,TactileSceneAdapter &adapter)
{
    auto *width=widget.findChild<QDoubleSpinBox *>("tactileArrowWidthGain");
    if(!width)return false;
    const auto frame=widget.frame();
    const auto points=adapter.points();
    const auto original=width->value();
    width->setValue(.1);
    const auto narrow=view.grabFramebuffer();
    width->setValue(8);
    const auto wide=view.grabFramebuffer();
    const bool okay=!narrow.isNull()&&narrow!=wide&&adapter.arrowWidthGain()==8
        &&adapter.points()==points&&widget.frame().sequence==frame.sequence;
    width->setValue(original);
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
        okay &= !adapter.valid();
        for(const auto &p:adapter.points())okay &= p.toMap()["dotPosition"]==p.toMap()["position"];
    }
    return okay;
}

}
void tactileMouseDragFromPoint(TactileCanvas *view,Qt::MouseButton button,const QPointF &start)
{
    const QPointF finish=start+QPointF(70,25);
    QMouseEvent press(QEvent::MouseButtonPress,start,view->mapToGlobal(start.toPoint()),button,button,Qt::NoModifier);QApplication::sendEvent(view,&press);
    QMouseEvent move(QEvent::MouseMove,finish,view->mapToGlobal(finish.toPoint()),Qt::NoButton,button,Qt::NoModifier);QApplication::sendEvent(view,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,finish,view->mapToGlobal(finish.toPoint()),button,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(view,&release);pump(80);
}
void tactileMouseDrag(TactileCanvas *view,Qt::MouseButton button)
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
    auto *view=widget.findChild<TactileCanvas *>("tactileSceneView");auto *bridge=widget.findChild<TactileSceneAdapter *>();
    check("independent tactile scene ready",view&&view->ready()&&view&&bridge&&bridge->valid());
    if(view){
        auto *root=view;const auto force=widget.selectedForce();const auto frame=widget.frame();const auto historySize=widget.history().samples().size();const auto yaw=root->property("orbitYaw").toDouble();
        check("force arrow widths change the rendered shafts without changing measurements",arrowWidthControls(widget,*view,*bridge));
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
                density->setCurrentIndex(level);view->frontView();pump(60);
                picked &= rawPointPicking(widget,out);
                expandedPicked &= rawPointPicking(widget,out,true);
            }
            check("every raw sensor point click selects its XYZ curve and visible table row at all interpolation levels",picked);
            check("clicks nine pixels outside raw spheres select the matching curve and row in XYZ and dot modes at all densities",expandedPicked);
        }
        TactileTestAccess::showSnapshot(widget,2);mode->setCurrentIndex(mode->findData("xyz"));density->setCurrentIndex(0);
        view->frontView();pump(60);
        const auto dragPoint=view->project(bridge->points()[0].toMap()["position"].value<QVector3D>());
        const int selected=widget.selectedPoint();
        clickPoint(*view,{1,1});
        check("empty space clears inspection and preserves chart selection",view->inspectedPoint()<0&&widget.selectedPoint()==selected);
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
        widget.findChild<QPushButton *>("resetTactileView")->click();pump(100);
        check("module resultant screenshot",widget.grab().save(directory+"/module.png"));
        auto *sizeGain=widget.findChild<QDoubleSpinBox *>("tactileResultantSizeGain");
        if(sizeGain){
            const auto originalResultant=bridge->resultant();
            const auto originalPoints=bridge->points();
            const auto original=view->grabFramebuffer();
            sizeGain->setValue(2);pump(40);
            check("resultant size changes its render without moving its anchor or changing measurements",
                original!=view->grabFramebuffer()&&bridge->resultant()==originalResultant&&bridge->points()==originalPoints);
            check("scaled resultant screenshot",widget.grab().save(directory+"/resultant_2x.png"));
            sizeGain->setValue(1);
        }else check("resultant size control exists",false);
        TactileTestAccess::advance(widget);pump(100);
        check("accepted frames retain the same viewport and surface point count",view->ready()
            &&bridge->points().size()==tactileDisplayPointCount(widget.selectedFinger(),bridge->gridSize()));
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
        view->frontView();pump(60);
        widget.findChild<QPushButton *>("tactileTare")->click();
        check("raw point clicks retain tare delta semantics",widget.hasDisplayBaseline()&&rawPointPicking(widget,out));
    }
    widget.close();check("closed view invalidates old forces",!widget.running()&&!bridge->valid());
    widget.show();widget.start();pump(100);
    check("3D reopen stays unavailable until new hardware data",!bridge->valid()&&!widget.frame().valid&&widget.history().samples().empty());
    const auto *unavailable=view->findChild<QLabel *>("tactileNoDataMessage");
    check("disconnected view retains its housing and clears force values",view->ready()&&unavailable&&unavailable->isVisible()
        &&widget.findChild<QLabel *>("force0")->text().contains("--"));
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
            auto *quick=display->findChild<TactileCanvas *>("tactileSceneView");
            reattached &= quick->ready()&&!quick->grabFramebuffer().isNull()
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
    auto *view=widget.findChild<TactileCanvas *>("tactileSceneView");
    check("hidden tactile scene accepts frames",view&&view->ready()&&widget.frame().valid&&widget.refreshCount()>0);
    if(view) {
        const auto folder=QFileInfo(path).absolutePath();
        const auto framebuffer=view->grabFramebuffer();
        check("offscreen framebuffer available",!framebuffer.isNull());

        auto *adapter=widget.findChild<TactileSceneAdapter *>();
        auto *density=widget.findChild<QComboBox *>("tactileDensity");
        auto *mode=widget.findChild<QComboBox *>("tactileDisplayMode");
        auto *gain=widget.findChild<QDoubleSpinBox *>("tactileArrowGain");
        auto *width=widget.findChild<QDoubleSpinBox *>("tactileArrowWidthGain");
        auto *opacity=widget.findChild<QSlider *>("tactileModelOpacity");
        check("arrow widths change the actual offscreen render",arrowWidthControls(widget,*view,*adapter));
        const auto held=widget.frame();
        const auto heldYaw=view->property("orbitYaw");
        const auto points=adapter->points();
        const auto resultant=adapter->resultant();
        const auto history=widget.history().samples().size();
        auto *renderSettings=RenderingSettings::instance();
        const bool originalQuality=renderSettings->lowCostRendering();
        renderSettings->setLowCostRendering(false);
        const auto standard=view->grabFramebuffer();
        renderSettings->setLowCostRendering(true);
        check("quality setting changes rendered edges and preserves force and camera state",view->grabFramebuffer()!=standard
            &&adapter->lowCostRendering()&&adapter->points()==points&&adapter->resultant()==resultant
            &&view->property("orbitYaw")==heldYaw&&widget.frame().sequence==held.sequence&&widget.history().samples().size()==history);
        renderSettings->setLowCostRendering(originalQuality);
        const int originalOpacity=opacity->value();
        opacity->setValue(0);
        const auto transparent=view->grabFramebuffer();
        opacity->setValue(100);
        check("opacity changes housing pixels without updating measurements",transparent!=view->grabFramebuffer()
            &&adapter->points()==points&&widget.frame().sequence==held.sequence&&view->property("orbitYaw")==heldYaw);
        opacity->setValue(originalOpacity);
        check("two interpolation levels available",density->isEnabled()&&density->count()==3);
        for(int type:{0,1,2}){
            TactileTestAccess::showSnapshot(widget,type);
            const auto &surface=tactileSurfaceModel(type);
            check("converted sensor housing has indexed geometry and valid measurements",view->ready()
                &&!surface.meshVertices.isEmpty()&&surface.meshIndices.size()%3==0&&surface.meshIndices.size()>1000);
            for(int level:{0,1,2}){
                density->setCurrentIndex(level);
                mode->setCurrentIndex(mode->findData("xyz"));view->frontView();pump(40);
                check("surface projects the selected point density",adapter->points().size()==tactileDisplayPointCount(type,level+2));
                if(level>0) check("interpolated points inspect independently of the native curve",interpolatedPicking(widget,*view,*adapter));
                mode->setCurrentIndex(mode->findData("dots"));view->frontView();pump(40);
                check("dot matrix shows depth displacement and disables arrow thickness",adapter->dotMatrix()&&!width->isEnabled()
                    &&view->grabFramebuffer().save(folder+QString("/dots_%1_%2.png").arg(type).arg(level)));
            }
        }
        gain->setValue(25);pump(40);
        const auto projected=[&] {
            for(const auto &point:adapter->points()){
                const auto screen=view->project(point.toMap()["dotPosition"].value<QVector3D>());
                if(screen.z()<=0||!view->rect().contains(screen.toPoint()))return false;
            }
            return true;
        };
        check("automatic framing keeps lifted dots visible at 25x",projected()&&view->grabFramebuffer().save(folder+"/dots_25x.png"));
        mode->setCurrentIndex(mode->findData("xyz"));density->setCurrentIndex(0);gain->setValue(1);view->frontView();
        auto invalid=widget.frame();invalid.valid=false;
        adapter->publish(invalid,0,false,0);pump(40);
        check("invalid data clears points and preserves the housing",!adapter->valid()
            &&view->ready()&&view->findChild<QLabel *>("tactileNoDataMessage")->isVisible()&&view->pickPoint(view->rect().center())==-1);
        TactileTestAccess::showSnapshot(widget,2);pump(80);
        check("module controls retain force mapping",widget.selectedFinger()==2
            &&tactile::sameForce(widget.selectedForce(),widget.frame().fingers[2].resultant));
        check("tactile render screenshot",widget.grab().save(folder+"/tactile_right_layout.png"));
    }
    widget.close();const auto closed=widget.refreshCount();pump(80);
    check("close stops updates",!widget.running()&&widget.refreshCount()==closed);
    out<<(all?"RESULT PASS\n":"RESULT FAIL\n");return all?0:4;
}
