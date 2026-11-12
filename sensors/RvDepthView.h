#pragma once
#include "ImagePresentation.h"
#include "DepthColorScale.h"
#include "ImageHoverTip.h"
#include <QtWidgets/QWidget>
#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <QtCore/QtEndian>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <vector>
#include <algorithm>
#include <QtCore/QCoreApplication>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <chrono>

// Metric display: host camera-Z or explicitly selected native radial metres.
class RvDepthView : public QWidget {
public:
 double displayedFps()const{
  if(held||!isVisible()||!fpsClock.isValid()||fpsTimes.size()<2||fpsClock.elapsed()-fpsTimes.back()>=1000)return 0;
  const auto span=fpsTimes.back()-fpsTimes.front();
  return span>0?(fpsTimes.size()-1)*1000.0/span:0;
 }
 static QString tr(const char* s){return QCoreApplication::translate("RvDepthView",s);}
 explicit RvDepthView(QWidget* parent=nullptr);
 ~RvDepthView() override;
 void setRadialMode(){radial=true;setDisplayRange(.1,1.4);setBlackOutsideRange(true);}
 void setDisplayRotationDegrees(int degrees){rotation=ImagePresentation::normalizedRotation(degrees);target={};refreshHover();update();}
 int displayRotationDegrees()const{return rotation;}
 void setNativeDisplayRotationDegrees(int degrees){
  if(!radial)return;
  nativeRotationEnabled=true;nativeRotation=(degrees%360+360)%360;
  target={};refreshHover();update();
 }
 int nativeDisplayRotationDegrees()const{return nativeRotation;}
 void setNativeDisplayMirrored(bool enabled){
  if(!radial||nativeDisplayMirrored==enabled)return;
  nativeDisplayMirrored=enabled;target={};refreshHover();update();
 }
 void setNativeGridSize(QSize size){nativeGridSize=size;target={};refreshHover();update();}
 QSizeF presentationSize()const{
  const auto grid=nativeGridSize.isEmpty()?depthSize:nativeGridSize;
  if(radial)return QSizeF(grid);
  if(!imageSourceRect.isEmpty()&&imageSourceRect!=image.rect())return imageSourceRect.size();
  if(image.size()==QSize(480,640))return image.size();
  return QSizeF(144,81);
 }
 QRect displayedSourceRect()const{return imageSourceRect;}
 bool setDepth(const QByteArray& bytes,const QJsonObject& metadata,const QByteArray& diagnostic={});
 // Retain only display pixels; expired metric values must not remain queryable.
 void holdFrame();
 bool frameHeld()const{return held;}
 void clear(const QString& reason=QString());
 bool setDisplayRange(double minimum,double maximum){
  if(!std::isfinite(minimum)||!std::isfinite(maximum)||minimum<0||maximum>1000||maximum<=minimum)return false;
  if(rangeMinimum==minimum&&rangeMaximum==maximum)return true;
  rangeMinimum=minimum;rangeMaximum=maximum;recolor();return true;
 }
 double displayMinimum()const{return rangeMinimum;}
 double displayMaximum()const{return rangeMaximum;}
 void setBlackOutsideRange(bool enabled){if(blackOutside==enabled)return;blackOutside=enabled;recolor();}
 bool blackOutsideRange()const{return blackOutside;}
 QRgb colorAt(int x,int y)const{const auto current=presentationImage();return current.isNull()?qRgb(0,0,0):current.pixel(x,y);}
 // Presentation may include predictions outside the dToF
 // field. Numeric snapshots and depthAt() retain the original geometric mask.
 QImage presentationImage()const;
 QImage snapshotImage()const;
 bool hasFrame()const{return bool(depth)||!image.isNull();}
 bool hasRenderableFrame()const{return depth&&!held&&!image.isNull()&&imageRevision==colorRevision;}
 // Opt-in timing observer for visible, newly painted frames. No images or
 // sample arrays are exposed; normal display leaves this callback empty.
 std::function<void(const QJsonObject&)> performanceFramePainted;
 void setLoadingState(int state){
  if(state==loadingState)return;
  loadingState=state;
  if(state==0){loadingVisible=false;loadingTimer.stop();update();return;}
  if(state==1){loadingProgress=0;loadingVisible=true;loadingClock.start();}
  else {finishFrom=loadingProgress;loadingVisible=true;loadingClock.start();emptyReason=tr("Model ready; waiting for the first depth frame...");}
  if(!loadingConnected){
   loadingConnected=true;loadingTimer.setInterval(33);
   connect(&loadingTimer,&QTimer::timeout,this,[this]{
    const double seconds=loadingClock.elapsed()/1000.0;
    if(loadingState==1)loadingProgress=96.0*(1-std::exp(-seconds/7.0));
    else {loadingProgress=finishFrom+(100-finishFrom)*std::min(1.0,seconds/.2);if(seconds>=.35){loadingVisible=false;loadingTimer.stop();}}
    update();
   });
  }
  loadingTimer.start();update();
 }
 double modelLoadingProgress()const{return loadingProgress;}
 bool modelLoadingVisible()const{return loadingVisible;}
 float depthAt(int x,int y)const{return !held&&depth&&x>=0&&x<depthSize.width()&&y>=0&&y<depthSize.height()?(*depth)[y*depthSize.width()+x]:0;}
protected:
 void showEvent(QShowEvent *event)override;
 void hideEvent(QHideEvent *event)override;
 void paintEvent(QPaintEvent*)override{
  QElapsedTimer paintTimer;if(performanceFramePainted)paintTimer.start();
  QPainter p(this);p.fillRect(rect(),QColor("#11161d"));p.setPen(QColor("#c9d1d9"));
  if(loadingVisible&&!held){
   const double width=std::max(20.0,std::min(340.0,this->width()-40.0));
   const QRectF bar((this->width()-width)/2.0,height()/2.0,width,8);
   p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(QColor("#293340"));p.drawRoundedRect(bar,4,4);
   QRectF fill=bar;fill.setWidth(width*loadingProgress/100.0);p.setBrush(QColor("#249cff"));p.drawRoundedRect(fill,4,4);
   p.setPen(QColor("#9ba9b9"));p.drawText(QRectF(10,bar.bottom()+12,this->width()-20,36),Qt::AlignCenter,loadingState==1?tr("Preparing GPU and model. Please wait..."):tr("Model ready"));return;
  }
  if(image.isNull()){
   p.drawText(rect(),Qt::AlignCenter,emptyReason.isEmpty()?tr("Waiting for RV1103 depth"):emptyReason);return;
  }
  // Fit the calibrated presentation geometry before applying display rotation.
  target=nativeRotationEnabled?nativePresentationTransform().mapRect(QRectF(QPointF(),presentationSize()))
      :ImagePresentation::fittedRect(QRectF(rect()).adjusted(8,8,-8,-8),presentationSize(),rotation);
  setProperty("presentationRect",target);
  if(nativeRotationEnabled){
   p.save();p.setTransform(nativePresentationTransform(),true);
   p.drawImage(QRectF(QPointF(),presentationSize()),image,QRectF(imageSourceRect));p.restore();
  }else ImagePresentation::drawImage(p,target,image,rotation,QRectF(imageSourceRect));
  setProperty("displaySourceRect",imageSourceRect);
  if(!held&&(paintedId!=imageFrameId||paintedGeneration!=imageGeneration)){
   if(!fpsClock.isValid())fpsClock.start();
   const auto now=fpsClock.elapsed();
   if(paintedGeneration!=imageGeneration||(!fpsTimes.empty()&&now-fpsTimes.back()>=1000))fpsTimes.clear();
   while(!fpsTimes.empty()&&now-fpsTimes.front()>2000)fpsTimes.erase(fpsTimes.begin());
   fpsTimes.push_back(now);
   paintedId=imageFrameId;paintedGeneration=imageGeneration;setProperty("paintedFrames",property("paintedFrames").toULongLong()+1);
   if(performanceFramePainted){
    auto data=imagePerformance;data["frame_id"]=imageFrameId;data["generation"]=imageGeneration;
    data["gui_paint_ms"]=paintTimer.nsecsElapsed()/1e6;
    const auto nowNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    data["paint_finished_ns"]=double(nowNs);
    if(data["host_decoded_monotonic_ns"].isDouble())data["source_to_paint_ms"]=(nowNs-data["host_decoded_monotonic_ns"].toDouble())/1e6;
    p.end();performanceFramePainted(data);
   }
  }
 }
 void mouseMoveEvent(QMouseEvent*)override{refreshHover();}
 void leaveEvent(QEvent*)override{hoverTip.clear();}
private:
 QTransform nativePresentationTransform()const{
  const auto size=presentationSize();const auto available=QRectF(rect()).adjusted(8,8,-8,-8);
  if(size.isEmpty()||available.isEmpty())return {};
  QTransform turn;turn.rotate((rotation+nativeRotation)%360);
  const auto bounds=turn.mapRect(QRectF(QPointF(),size));
  const auto scale=std::min(available.width()/bounds.width(),available.height()/bounds.height());
  QTransform transform;transform.translate(available.center().x(),available.center().y());
  // Mirror the final rotated native view horizontally in screen coordinates.
  // The same transform is inverted for hover; samples and saved pixels stay raw.
  if(nativeDisplayMirrored)transform.scale(-1,1);
  transform.rotate((rotation+nativeRotation)%360);transform.scale(scale,scale);transform.translate(-size.width()/2,-size.height()/2);
  return transform;
 }
 QJsonObject depthMetadata;
 QJsonObject imagePerformance;
 double guiSetDepthMs=0;
 ImageHoverTip hoverTip;
 void refreshHover();
 QByteArray monocularDiagnostic;
 double sideScale=1;
 int sidePixels=0;
 mutable int imageSidePixels=0;
 bool held=false;QSize nativeGridSize;
 struct Colorizer;
 std::unique_ptr<Colorizer> colorizer;
 void recolor();
 void requestColorization();
 void takeColoredFrame();
 quint64 colorRevision=0;
 mutable quint64 imageRevision=0;
 mutable double imageFrameId=-1,imageGeneration=-1;
 double paintedId=-1,paintedGeneration=-1;
 QElapsedTimer fpsClock;
 std::vector<qint64> fpsTimes;
 double rangeMinimum=0,rangeMaximum=3;
 bool blackOutside=false;
 bool radial=false;
 int loadingState=0;bool loadingVisible=false,loadingConnected=false;
 double loadingProgress=0,finishFrom=0;QTimer loadingTimer;QElapsedTimer loadingClock;
 int rotation=0;
 int nativeRotation=0;bool nativeRotationEnabled=false,nativeDisplayMirrored=false;
 QString emptyReason;std::shared_ptr<const std::vector<float>> depth;QSize depthSize;mutable QImage image;QRectF target;
 QRect depthSourceRect;mutable QRect imageSourceRect;
};
