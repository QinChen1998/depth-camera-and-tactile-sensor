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
float floatPixel(const QByteArray &bytes,int index){
    const quint32 bits=qFromLittleEndian<quint32>(bytes.constData()+4*index);
    float value;std::memcpy(&value,&bits,4);
    return value;
}
float monoPrediction(const QByteArray &diagnostic,int index,double scale){
    return float(double(floatPixel(diagnostic,index))*scale);
}
}

// One pending input, one completed image and one queued GUI notification.
// The worker owns its reusable buffers and never reads or changes a widget.
struct RvDepthView::Colorizer {
    struct Job {
        std::shared_ptr<const std::vector<float>> values;
        QSize size;
        double minimum=0,maximum=3,frameId=-1,generation=-1;
        bool blackOutside=false;
        quint64 revision=0;
        QRect sourceRect;
        QJsonObject performance;
        qint64 queuedNs=0;
        QByteArray diagnostic;
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
    static Job forView(const RvDepthView &view,bool presentation=true){
        Job job{view.depth,view.depthSize,view.rangeMinimum,view.rangeMaximum,
            view.property("frameId").toDouble(),view.property("generation").toDouble(),view.blackOutside,
            view.colorRevision,view.depthSourceRect};
        if(presentation&&view.sidePixels>0){
            job.sideScale=view.sideScale;job.sidePixels=view.sidePixels;job.diagnostic=view.monocularDiagnostic;
        }
        return job;
    }
    static void render(const Job &job,QImage &buffer){
        if(buffer.size()!=job.size||buffer.format()!=QImage::Format_RGB32)buffer=QImage(job.size,QImage::Format_RGB32);
        // bits() detaches once if an explicit snapshot still owns this buffer.
        auto *pixels=reinterpret_cast<QRgb *>(buffer.bits());
        const int count=job.size.width()*job.size.height();
        for(int i=0;i<count;++i){
            const float original=(*job.values)[i];
            const float display=original==0&&job.sidePixels>0?monoPrediction(job.diagnostic,i,job.sideScale):original;
            pixels[i]=DepthColorScale::color(display,job.minimum,job.maximum,true,job.blackOutside);
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
    monocularDiagnostic.clear();sidePixels=0;sideScale=1;
    setProperty("sidePredictionActive",false);setProperty("sidePredictionPixels",0);
    const int width=metadata["width"].toInt(),height=metadata["height"].toInt();
    const auto model=metadata["assist_host_model"].toString();
    const auto &view=RvCameraView::Portrait;
    if(metadata.contains("assist_output_grid")&&!RvCameraView::validOutputGrid(metadata)){clear();return false;}
    const bool hostGrid=RvCameraView::isUniDepth(model)&&RvCameraView::validOutputGrid(metadata);
    if(!((width==144&&height==81)||(width==288&&height==162)||(width==384&&height==256)||(width==576&&height==324)||hostGrid||(radial&&width==48&&height==32))
        ||bytes.size()!=width*height*4||metadata["depth_unit"]!=(radial?"sensor_radial_m_float32":"camera-Z_m_float32")){
        clear();return false;
    }
    const auto fit=metadata["assist_fusion"].toObject();
    const auto mode=fit["fusion_mode"].toString();
    const double scale=fit["scale"].toDouble(0);
    const bool hostExperiment=RvAssistFusion::isReuseRgbRevision(metadata);
    bool validMonocular=!radial&&hostGrid&&RvCameraView::isWide(model)
        &&hostExperiment
        &&metadata["assist_input_orientation_checked"]==true&&metadata["assist_diagnostic_capture"]==true
        &&metadata["assist_output_restored_to_calibrated_grid"]==true&&metadata["assist_diagnostic_payload"]=="mono_reasons_v1"
        &&diagnostic.size()==view.depthBytes()+2*RvDepthDiagnostic::ReasonBytes;
    bool allowSides=validMonocular&&(mode=="global_scale"||mode=="held_scale"||mode=="monocular")
        &&fit["local_factor_limit"]==1&&std::isfinite(scale)&&scale>0;
    auto values=std::make_shared<std::vector<float>>();
    RvDepthCoverage::DepthField field;
    if(!RvDepthCoverage::decodeDepth(bytes,metadata,*values,field)){clear();return false;}
    int outside=0;
    for(int i=0;validMonocular&&i<width*height;++i){
        const float mono=monoPrediction(diagnostic,i,1.);
        if(!std::isfinite(mono)||mono<=0){
            validMonocular=false;
            allowSides=false;
        }else if(allowSides){
            const float prediction=float(double(mono)*scale);
            if(!std::isfinite(prediction)||prediction<=0)allowSides=false;
            else outside+=(*values)[i]==0;
        }
    }
    depth=std::move(values);depthSize=QSize(width,height);held=false;depthMetadata=metadata;
    if(validMonocular){
        for(int i=view.depthBytes();i<diagnostic.size();++i)
            if(quint8(diagnostic[i])>quint8(RvAssistFusion::AlreadySupported)){validMonocular=false;break;}
        if(validMonocular)monocularDiagnostic=diagnostic;
    }
    if(validMonocular&&allowSides&&field.valid()&&outside){
        sideScale=scale;sidePixels=outside;
        setProperty("sidePredictionActive",true);setProperty("sidePredictionPixels",sidePixels);
    }
    depthSourceRect=field.valid()?field.sourceRect:QRect(QPoint(),depthSize);
    if(image.size()!=depthSize)image={};
    setProperty("frameId",metadata["frame_id"].toDouble());setProperty("generation",metadata["generation"].toDouble());
    setProperty("depthWidth",width);setProperty("depthHeight",height);
    if(performanceFramePainted)guiSetDepthMs=setTimer.nsecsElapsed()/1e6;
    recolor();return true;
}
void RvDepthView::holdFrame(){
    if(!hasFrame()||held)return;
    held=true;depth.reset();depthMetadata={};
    monocularDiagnostic.clear();sidePixels=0;setProperty("sidePredictionActive",false);setProperty("sidePredictionPixels",0);
    refreshHover();update();
    // An already submitted image may finish, but can only restore pixels.
    // Its numeric values never return to depthAt() or the saving snapshot.
}
void RvDepthView::clear(const QString &reason){
    fpsTimes.clear();fpsClock.invalidate();paintedId=paintedGeneration=-1;
    ++colorRevision;if(colorizer)colorizer->cancel();
    loadingVisible=false;loadingTimer.stop();emptyReason=reason;held=false;
    depth.reset();depthSize={};image={};depthSourceRect={};imageSourceRect={};depthMetadata={};imagePerformance={};
    monocularDiagnostic.clear();sidePixels=imageSidePixels=0;sideScale=1;
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
    auto job=Colorizer::forView(*this);
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
    target=nativeRotationEnabled?nativePresentationTransform().mapRect(QRectF(QPointF(),presentationSize()))
        :ImagePresentation::fittedRect(QRectF(rect()).adjusted(8,8,-8,-8),presentationSize(),rotation);
    if(target.isEmpty()||!target.contains(position)){
        hoverTip.hideText();return;
    }
    QPointF f;
    if(nativeRotationEnabled){
        const auto point=nativePresentationTransform().inverted().map(position);
        const auto size=presentationSize();
        if(!QRectF(QPointF(),size).contains(point)){hoverTip.hideText();return;}
        f={point.x()/size.width(),point.y()/size.height()};
    }else f=ImagePresentation::sourcePosition({(position.x()-target.left())/target.width(),
                                               (position.y()-target.top())/target.height()},rotation);
    if(image.isNull()||(loadingVisible&&!held)){hoverTip.hideText();return;}
    // Keep the readout in place while the next image is being colored. Update
    // its value only when that image is ready; never query expired held depth.
    if(!held&&imageRevision!=colorRevision)return;
    const auto sourceRect=imageSourceRect;
    const int x=std::clamp(sourceRect.x()+int(f.x()*sourceRect.width()),sourceRect.left(),sourceRect.right());
    const int y=std::clamp(sourceRect.y()+int(f.y()*sourceRect.height()),sourceRect.top(),sourceRect.bottom());
    const float z=depthAt(x,y);
    if(!held&&z==0&&sidePixels>0){
        const float prediction=monoPrediction(monocularDiagnostic,y*depthSize.width()+x,sideScale);
        hoverTip.showText(tr("(%1, %2) prediction: %3 m camera-Z").arg(x).arg(y).arg(prediction,0,'f',4));
        return;
    }
    if(!held&&radial&&depthMetadata["native_hole_fill_indices"].toArray().contains(y*depthSize.width()+x)){
        hoverTip.showText(tr("(%1, %2) dToF estimate: %3 m").arg(x).arg(y).arg(z,0,'f',4));
        return;
    }
    hoverTip.showText(held?tr("Last displayed frame; depth reading expired"):z>0
        ?QString(radial?"(%1, %2) dToF radial: %3 m":"(%1, %2) camera-Z: %3 m").arg(x).arg(y).arg(z,0,'f',4):tr("Invalid depth"));
}
QImage RvDepthView::presentationImage()const{
    // Explicit inspection/save may request a hidden or not-yet-painted frame.
    // Only that demand generates an image synchronously; receipt never does.
    if(depth&&!held&&imageRevision!=colorRevision){
        const auto job=Colorizer::forView(*this);
        Colorizer::render(job,image);imageSidePixels=job.sidePixels;
        imageRevision=colorRevision;imageSourceRect=depthSourceRect;imageFrameId=property("frameId").toDouble();imageGeneration=property("generation").toDouble();
    }
    return image;
}
QImage RvDepthView::snapshotImage()const{
    // Side predictions belong only to the viewport.
    if(held)return imageSidePixels||image.hasAlphaChannel()?QImage():image;
    if(!depth)return {};
    const auto displayed=presentationImage();
    if(sidePixels==0&&!displayed.hasAlphaChannel())return displayed;
    QImage corrected;
    Colorizer::render(Colorizer::forView(*this,false),corrected);
    return corrected;
}

void RvDepthView::showEvent(QShowEvent *event){QWidget::showEvent(event);requestColorization();}
void RvDepthView::hideEvent(QHideEvent *event){if(colorizer)colorizer->cancel();QWidget::hideEvent(event);}
