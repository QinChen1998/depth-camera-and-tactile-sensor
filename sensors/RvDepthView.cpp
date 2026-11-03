#include "RvDepthView.h"

#include <QtCore/QMetaObject>
#include <QtCore/QJsonArray>
#include "RvCameraView.h"
#include "RvDepthCoverage.h"
#include "RvAssistFusion.h"
#include "RvDepthDiagnostic.h"
#include <QtGui/QHideEvent>
#include <QtGui/QShowEvent>
#include <condition_variable>
#include <array>
#include <mutex>
#include <optional>
#include <thread>

namespace {
float monoPrediction(const QByteArray &diagnostic,int index,double scale){
    const quint32 bits=qFromLittleEndian<quint32>(diagnostic.constData()+4*index);
    float value;std::memcpy(&value,&bits,4);return float(double(value)*scale);
}
}

// One pending input, one completed image and one queued GUI notification.
// The worker owns its reusable buffers and never reads or changes a widget.
struct RvDepthView::Colorizer {
    struct Job {
        std::shared_ptr<const std::vector<float>> values;
        QSize size;
        QByteArray coverage;
        double minimum=0,maximum=3,frameId=-1,generation=-1;
        bool blackOutside=false;
        quint64 revision=0;
        QRect sourceRect;
        bool rawPreview=false;
        QJsonObject performance;
        qint64 queuedNs=0;
        QByteArray sideDiagnostic;
        double sideScale=1;
        int sidePixels=0;
    };
    struct Result { QImage image; double frameId=-1,generation=-1; quint64 revision=0; QRect sourceRect; QJsonObject performance; int sidePixels=0; };

    explicit Colorizer(RvDepthView *view):owner(view),worker([this]{run();}){}
    ~Colorizer(){
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;pending.reset();}
        available.notify_one();worker.join();
    }
    void submit(Job job){
        {std::lock_guard<std::mutex> lock(mutex);pending=std::move(job);}
        available.notify_one();
    }
    void cancel(){std::lock_guard<std::mutex> lock(mutex);pending.reset();completed.reset();}
    std::optional<Result> take(){
        std::lock_guard<std::mutex> lock(mutex);
        notificationQueued=false;
        auto result=std::move(completed);completed.reset();return result;
    }
    static void render(const Job &job,QImage &buffer){
        if(buffer.size()!=job.size||buffer.format()!=QImage::Format_RGB32)
            buffer=QImage(job.size,QImage::Format_RGB32);
        // bits() detaches once if an explicit snapshot still owns this buffer.
        auto *pixels=reinterpret_cast<QRgb *>(buffer.bits());
        const int count=job.size.width()*job.size.height();
        for(int i=0;i<count;++i){
            const float original=(*job.values)[i];
            const float display=original==0&&!job.sideDiagnostic.isEmpty()?monoPrediction(job.sideDiagnostic,i,job.sideScale):original;
            auto color=DepthColorScale::color(display,job.minimum,job.maximum,true,job.blackOutside);
            if(job.rawPreview){
                const float v=(*job.values)[i];
                color=std::isfinite(v)?QColor::fromHsvF((1-std::clamp((double(v)-job.minimum)/(job.maximum-job.minimum),0.0,1.0))*2/3,1,1).rgb():qRgb(255,0,255);
            }
            if(!job.coverage.isEmpty()){
                const int alpha=quint8(job.coverage[i]);
                color=qRgb((qRed(color)*alpha+127)/255,(qGreen(color)*alpha+127)/255,(qBlue(color)*alpha+127)/255);
            }
            pixels[i]=color;
        }
    }
    void run(){
        std::array<QImage,2> buffers;int nextBuffer=0;
        for(;;){
            Job job;
            {std::unique_lock<std::mutex> lock(mutex);
             available.wait(lock,[this]{return stopping||pending.has_value();});
             if(stopping)return;
             job=std::move(*pending);pending.reset();}
            auto &buffer=buffers[nextBuffer];nextBuffer=1-nextBuffer;
            QElapsedTimer renderTimer;
            if(job.queuedNs){const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                job.performance["gui_color_queue_ms"]=(now-job.queuedNs)/1e6;renderTimer.start();}
            render(job,buffer);
            if(job.queuedNs)job.performance["gui_color_ms"]=renderTimer.nsecsElapsed()/1e6;
            bool notify=false;
            {std::lock_guard<std::mutex> lock(mutex);
             if(stopping)return;
             completed=Result{buffer,job.frameId,job.generation,job.revision,job.sourceRect,job.performance,job.sidePixels};
             notify=!notificationQueued;notificationQueued=true;}
            if(notify)QMetaObject::invokeMethod(owner,[view=owner]{view->takeColoredFrame();},Qt::QueuedConnection);
        }
    }
    RvDepthView *owner;
    std::mutex mutex;std::condition_variable available;
    std::optional<Job> pending;std::optional<Result> completed;
    bool stopping=false,notificationQueued=false;
    std::thread worker;
};

RvDepthView::RvDepthView(QWidget *parent):QWidget(parent),hoverTip(this){
    setObjectName("rvDepthCanvas");setMouseTracking(true);
    setMinimumSize(ImagePresentation::MinimumViewportWidth,ImagePresentation::MinimumViewportHeight);
}
RvDepthView::~RvDepthView(){colorizer.reset();}

bool RvDepthView::setDepth(const QByteArray &bytes,const QJsonObject &metadata,const QByteArray &diagnostic){
    QElapsedTimer setTimer;if(performanceFramePainted)setTimer.start();
    depthMetadata={};
    sideDiagnostic.clear();sidePixels=0;sideScale=1;
    setProperty("sidePredictionActive",false);setProperty("sidePredictionPixels",0);
    const int width=metadata["width"].toInt(),height=metadata["height"].toInt();
    const bool rawPreview=metadata["color_raw_preview"]==true&&RvCameraView::isColorLiteSR(metadata["assist_host_model"].toString());
    if(!((width==144&&height==81)||(width==288&&height==162)||(width==384&&height==256)||(width==576&&height==324)||(radial&&((width==256&&height==256)||(width==512&&height==512))))
        ||bytes.size()!=width*height*4||metadata["depth_unit"]!=(rawPreview?"camera_radial_m_float32":radial?"sensor_radial_m_float32":"camera-Z_m_float32")){
        clear();return false;
    }
    const auto fit=metadata["assist_fusion"].toObject();
    const auto mode=fit["fusion_mode"].toString();
    const double scale=fit["scale"].toDouble(0);
    const auto model=metadata["assist_host_model"].toString();
    bool allowSides=!radial&&!rawPreview&&width==576&&height==324&&RvCameraView::isUniDepth(model)&&RvCameraView::isWide(model)
        &&(metadata["assist_experiment"]=="G"||metadata["assist_experiment"]=="H")
        &&metadata["assist_input_orientation_checked"]==true&&metadata["assist_diagnostic_capture"]==true
        &&metadata["assist_diagnostic_payload"]=="mono_reasons_v1"&&diagnostic.size()==RvDepthDiagnostic::PayloadBytes
        &&(mode=="global_scale"||mode=="held_scale"||mode=="monocular")&&fit["local_factor_limit"]==1
        &&std::isfinite(scale)&&scale>0;
    auto values=std::make_shared<std::vector<float>>(width*height);int outside=0;
    for(int i=0;i<width*height;++i){
        const quint32 bits=qFromLittleEndian<quint32>(bytes.constData()+i*4);
        std::memcpy(&(*values)[i],&bits,4);
        if(!rawPreview&&!std::isfinite((*values)[i])){clear();return false;}
        if(allowSides){
            const float mono=monoPrediction(diagnostic,i,1),prediction=monoPrediction(diagnostic,i,scale);
            if(!std::isfinite(mono)||mono<=0||!std::isfinite(prediction)||prediction<=0)allowSides=false;
            else outside+=(*values)[i]==0;
        }
    }
    depth=std::move(values);depthSize=QSize(width,height);held=false;coverage.clear();depthMetadata=metadata;
    const auto field=(radial||rawPreview)?RvDepthCoverage::DepthField():RvDepthCoverage::depthField(bytes,metadata);
    if(allowSides&&field.valid()&&outside){
        for(int i=RvDepthDiagnostic::DepthBytes;i<diagnostic.size();++i)
            if(quint8(diagnostic[i])>quint8(RvAssistFusion::AlreadySupported)){allowSides=false;break;}
        if(allowSides){sideDiagnostic=diagnostic;sideScale=scale;sidePixels=outside;
            setProperty("sidePredictionActive",true);setProperty("sidePredictionPixels",sidePixels);}
    }
    depthSourceRect=field.valid()?field.sourceRect:QRect(QPoint(),depthSize);
    if(image.size()!=depthSize)image={};
    setProperty("frameId",metadata["frame_id"].toDouble());setProperty("generation",metadata["generation"].toDouble());
    setProperty("depthWidth",width);setProperty("depthHeight",height);
    if(performanceFramePainted)guiSetDepthMs=setTimer.nsecsElapsed()/1e6;
    recolor();return true;
}
bool RvDepthView::setCoverage(const QByteArray &values){
    if(!radial||!depth||values.size()!=depthSize.width()*depthSize.height()){clear();return false;}
    coverage=values;recolor();return true;
}
void RvDepthView::holdFrame(){
    if(!hasFrame()||held)return;
    held=true;depth.reset();coverage.clear();depthMetadata={};
    sideDiagnostic.clear();sidePixels=0;setProperty("sidePredictionActive",false);setProperty("sidePredictionPixels",0);
    refreshHover();
    // An already submitted image may finish, but can only restore pixels.
    // Its numeric values never return to depthAt() or the saving snapshot.
}
void RvDepthView::clear(const QString &reason){
    fpsTimes.clear();fpsClock.invalidate();paintedId=paintedGeneration=-1;
    ++colorRevision;if(colorizer)colorizer->cancel();
    loadingVisible=false;loadingTimer.stop();emptyReason=reason;held=false;
    depth.reset();depthSize={};coverage.clear();image={};depthSourceRect={};imageSourceRect={};depthMetadata={};imagePerformance={};
    sideDiagnostic.clear();sidePixels=imageSidePixels=0;sideScale=1;
    setProperty("sidePredictionActive",false);setProperty("sidePredictionPixels",0);
    setProperty("frameId",QVariant());setProperty("generation",QVariant());
    setProperty("displaySourceRect",QVariant());
    hoverTip.clear();update();
}
void RvDepthView::recolor(){
    if(!depth||held)return;
    ++colorRevision;requestColorization();
}
void RvDepthView::requestColorization(){
    if(!isVisible()||!depth||held||imageRevision==colorRevision)return;
    if(!colorizer)colorizer=std::make_unique<Colorizer>(this);
    Colorizer::Job job{depth,depthSize,coverage,rangeMinimum,rangeMaximum,
        property("frameId").toDouble(),property("generation").toDouble(),blackOutside,colorRevision,depthSourceRect,depthMetadata.value("color_raw_preview")==true};
    job.sideDiagnostic=sideDiagnostic;job.sideScale=sideScale;job.sidePixels=sidePixels;
    if(performanceFramePainted){job.performance=depthMetadata;job.performance["gui_set_depth_ms"]=guiSetDepthMs;
        job.queuedNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    colorizer->submit(std::move(job));
    setProperty("colorizationRequests",property("colorizationRequests").toULongLong()+1);
}
void RvDepthView::takeColoredFrame(){
    if(!colorizer)return;
    auto result=colorizer->take();
    if(!result||result->revision!=colorRevision||!isVisible())return;
    image=std::move(result->image);imageRevision=result->revision;
    imageSourceRect=result->sourceRect;
    imageFrameId=result->frameId;imageGeneration=result->generation;
    imagePerformance=std::move(result->performance);
    imageSidePixels=result->sidePixels;
    setProperty("coloredFrameId",imageFrameId);
    setProperty("coloredFrames",property("coloredFrames").toULongLong()+1);refreshHover();update();
}
void RvDepthView::refreshHover(){
    if(!hoverTip.tracking())return;
    const auto position=hoverTip.position();
    target=ImagePresentation::fittedRect(QRectF(rect()).adjusted(8,8,-8,-8),
        presentationSize(),rotation);
    if(target.isEmpty()||!target.contains(position)){
        hoverTip.hideText();return;
    }
    const auto f=ImagePresentation::sourcePosition({(position.x()-target.left())/target.width(),
                                                   (position.y()-target.top())/target.height()},rotation);
    if(image.isNull()||(loadingVisible&&!held)){hoverTip.hideText();return;}
    // Keep the readout in place while the next image is being colored. Update
    // its value only when that image is ready; never query expired held depth.
    if(!held&&imageRevision!=colorRevision)return;
    const QPointF source=nativeRadial?QPointF(1-f.y(),1-f.x()):f;
    const auto sourceRect=nativeRadial?image.rect():imageSourceRect;
    const int x=std::clamp(sourceRect.x()+int(source.x()*sourceRect.width()),sourceRect.left(),sourceRect.right());
    const int y=std::clamp(sourceRect.y()+int(source.y()*sourceRect.height()),sourceRect.top(),sourceRect.bottom());
    const float z=depthAt(x,y);
    if(!held&&z==0&&!sideDiagnostic.isEmpty()){
        const float prediction=monoPrediction(sideDiagnostic,y*depthSize.width()+x,sideScale);
        hoverTip.showText(tr("(%1, %2) prediction: %3 m camera-Z").arg(x).arg(y).arg(prediction,0,'f',4));
        return;
    }
    if(!held&&depthMetadata.value("color_raw_preview")==true){hoverTip.showText(tr("(%1, %2) raw prediction: %3 m (radial, unfiltered)").arg(x).arg(y).arg(z,0,'f',4));return;}
    hoverTip.showText(held?tr("Last displayed frame; depth reading expired"):z>0
        ?QString(radial?"(%1, %2) dToF radial: %3 m":"(%1, %2) camera-Z: %3 m").arg(x).arg(y).arg(z,0,'f',4):tr("Invalid depth"));
}
QImage RvDepthView::presentationImage()const{
    // Explicit inspection/save may request a hidden or not-yet-painted frame.
    // Only that demand generates an image synchronously; receipt never does.
    if(depth&&!held&&imageRevision!=colorRevision){
        Colorizer::Job job{depth,depthSize,coverage,rangeMinimum,rangeMaximum,
            property("frameId").toDouble(),property("generation").toDouble(),blackOutside,colorRevision,depthSourceRect,depthMetadata.value("color_raw_preview")==true};
        job.sideDiagnostic=sideDiagnostic;job.sideScale=sideScale;job.sidePixels=sidePixels;
        Colorizer::render(job,image);imageSidePixels=sidePixels;
        imageRevision=colorRevision;imageSourceRect=depthSourceRect;imageFrameId=property("frameId").toDouble();imageGeneration=property("generation").toDouble();
    }
    return image;
}
QImage RvDepthView::snapshotImage()const{
    const auto displayed=presentationImage();
    // Expired side predictions are retained only as presentation pixels.
    if(held&&imageSidePixels)return {};
    if(sideDiagnostic.isEmpty())return displayed;
    QImage masked;
    Colorizer::render({depth,depthSize,coverage,rangeMinimum,rangeMaximum,
        property("frameId").toDouble(),property("generation").toDouble(),blackOutside,colorRevision,depthSourceRect},masked);
    return masked;
}

void RvDepthView::showEvent(QShowEvent *event){QWidget::showEvent(event);requestColorization();}
void RvDepthView::hideEvent(QHideEvent *event){if(colorizer)colorizer->cancel();QWidget::hideEvent(event);}
