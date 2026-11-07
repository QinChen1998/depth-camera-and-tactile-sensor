#include "../sensors/RvCaptureCalibration.h"
#include <QtWidgets/QDockWidget>
#include "../SensorMainWindow.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvTunerController.h"
#include "../sensors/RvUsbEnrollment.h"
#include "../sensors/RvDeviceManagerDialog.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QtEndian>
#include <cstring>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>

namespace {
bool waitFor(const std::function<bool()> &done,int ms){
    QEventLoop loop;QTimer pulse,timeout;pulse.setInterval(20);timeout.setSingleShot(true);
    QObject::connect(&pulse,&QTimer::timeout,&loop,[&]{if(done())loop.quit();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    if(!done()){pulse.start();timeout.start(ms);loop.exec();}return done();
}
QJsonObject stats(QVector<double> values){
    if(values.isEmpty())return {};std::sort(values.begin(),values.end());double sum=0;for(double x:values)sum+=x;
    return {{"samples",values.size()},{"mean_ms",sum/values.size()},{"p95_ms",values[int((values.size()-1)*.95)]},{"max_ms",values.last()}};
}
bool save(const QString &path,const QJsonObject &value){QSaveFile file(path);const auto bytes=QJsonDocument(value).toJson();return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();}
}

// Explicitly gated real GUI test. All frame products remain in memory. Only
// scalar counters, timings, settings and model identities enter the reports.
int runTinyNv12SwitchValidation(SensorMainWindow &,const QString &);
int runLiveValidation(SensorMainWindow &window,const QString &path,int seconds){
    return runTinyNv12SwitchValidation(window,path);
}

// Explicitly authorized real USB onboarding through the ordinary UI/worker.
// Device registration is isolated by main.cpp and exported only as test evidence.
int runUsbEnrollmentLiveValidation(SensorMainWindow &window,const QString &path){
    auto *session=window.session();const auto serial=qEnvironmentVariable("DCTS_LIVE_SERIAL");
    if(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||serial.isEmpty()
        ||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1")return 3;
    QJsonObject report{{"passed",false},{"serial",serial},{"images_saved",false}};
    if(qEnvironmentVariable("DCTS_USB_TEST_NEW")=="1") {
        RvDeviceStore store(session->deviceStorePath());store.save({},{});session->reloadDeviceProfiles();
    }
    RvDeviceStore initialStore(session->deviceStorePath());bool previouslyRegistered=false;
    for(const auto &profile:initialStore.profiles())previouslyRegistered|=profile.serial==serial;
    report["previously_registered"]=previouslyRegistered;
    watchRvUsbDevices(session,&window);
    auto *action=window.findChild<QAction*>("rvDeviceManagerAction");if(!action)return 3;
    action->trigger();auto *manager=window.findChild<RvDeviceManagerDialog*>();if(!manager)return 3;
    auto *connectButton=manager->findChild<QPushButton*>("rvDeviceConnectUsb");
    if(!connectButton)return 3;
    auto *depthCanvas=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvDtofFastDepthCanvas"));
    auto *hostCanvas=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvAssistDepthCanvas"));
    if(!depthCanvas||!hostCanvas)return 3;
    QObject depthObserver;int depthFrames=0,positiveFrames=0,paintedFrames=0;bool invalidDepth=false;
    QString generation; qint64 lastFrame=-1,lastPaint=-1;QElapsedTimer depthClock;QJsonObject lastDepth;
    const auto acceptDepth=[&](const QByteArray &bytes,const QJsonObject &metadata){
        const int width=metadata["width"].toInt(),height=metadata["height"].toInt();
        const auto current=metadata["generation"].toVariant().toString();const auto id=metadata["frame_id"].toInteger();
        if(width<=0||height<=0||bytes.size()!=width*height*4||(current==generation&&id<=lastFrame)){invalidDepth=true;return;}
        int positive=0;
        for(int i=0;i<width*height;++i){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);float z;std::memcpy(&z,&bits,4);if(!std::isfinite(z)||z<0){invalidDepth=true;return;}positive+=z>0;}
        if(!depthClock.isValid())depthClock.start();
        generation=current;lastFrame=id;lastDepth=metadata;++depthFrames;positiveFrames+=positive>0;
    };
    QObject::connect(session,&RvSessionController::dtofCandidateReady,&depthObserver,[&](const RvDtofCandidates::Result &r){if(r.fresh)acceptDepth(r.depth,r.metadata);});
    QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&depthObserver,[&](const QByteArray &b,const QJsonObject &m){acceptDepth(b,m);});
    const auto painted=[&](const QJsonObject &m){const auto id=m["frame_id"].toInteger();if(id!=lastPaint){lastPaint=id;++paintedFrames;}};
    depthCanvas->performanceFramePainted=painted;hostCanvas->performanceFramePainted=painted;
    const auto clearCallbacks=qScopeGuard([&]{depthCanvas->performanceFramePainted={};hostCanvas->performanceFramePainted={};});
    bool confirmed=false,failed=false;QObject context;QTimer responder;
    QObject::connect(&responder,&QTimer::timeout,&context,[&]{
        auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog)return;
        if(dialog->objectName()=="rvUsbEnrollmentPrompt"){
            bool matches=false;
            for(auto *label:dialog->findChildren<QLabel*>())matches|=label->text().contains(serial);
            const auto angles=dialog->findChildren<QComboBox*>();
            auto *buttons=dialog->findChild<QDialogButtonBox*>();
            if(!matches||angles.size()!=2||!buttons){failed=true;dialog->reject();return;}
            angles[0]->setCurrentIndex(angles[0]->findData(180));
            angles[1]->setCurrentIndex(angles[1]->findData(0));
            confirmed=true;buttons->button(QDialogButtonBox::Ok)->click();
        }else if(auto *message=qobject_cast<QMessageBox*>(dialog)){
            report["error"]=message->text()+"\n"+message->informativeText();
            failed=true;message->done(QMessageBox::Close);
        }
    });
    responder.start(20);connectButton->click();
    const bool registered=waitFor([&]{return failed||((confirmed||previouslyRegistered)&&connectButton->isEnabled()
        &&manager->property("rvUsbRuntimeReplaced").isValid()&&session->deviceProfile().serial==serial);},240000)&&!failed&&(confirmed||previouslyRegistered)
        &&session->deviceProfile().serial==serial;
    report["confirmation_accepted"]=confirmed;report["registered"]=registered;
    report["first_runtime_replaced"]=manager->property("rvUsbRuntimeReplaced").toBool();
    const bool firstDepth=registered&&waitFor([&]{return invalidDepth||session->userStopped()||(positiveFrames>=30&&paintedFrames>=20);},95000)
        &&!invalidDepth&&!session->userStopped()&&positiveFrames>=30&&paintedFrames>=20;
    report["first_depth_received_and_painted"]=firstDepth;
    report["first_depth_frames"]=depthFrames;report["first_painted_frames"]=paintedFrames;
    if(registered){
        report["profile"]=session->deviceProfile().json();
        report["registry_exported"]=QFile::copy(session->deviceStorePath(),path+".devices.json");
    }
    const auto stopAndInspect=[&]{
        waitFor([&]{return !session->busy();},60000);
        session->stop();if(!waitFor([&]{return !session->busy();},35000))return false;
        auto *tuner=session->tuner();QElapsedTimer timer;timer.start();
        do {
            tuner->read();
            if(!waitFor([&]{return !tuner->pending();},95000))return false;
            if(tuner->state()==RvTunerController::Idle&&tuner->inspection()["owner"]=="idle"){
                report["inspection"]=tuner->inspection();return true;
            }
            waitFor([]{return false;},200);
        }while(timer.elapsed()<35000);
        report["error"]=tuner->detail();return false;
    };
    bool idle=registered&&stopAndInspect();
    bool reconnected=false;
    if(idle){
        const auto firstProfile=session->deviceProfile().json();
        const auto count=RvDeviceStore(session->deviceStorePath()).profiles().size();
        const int beforeFrames=positiveFrames,beforePainted=paintedFrames;
        confirmed=false;connectButton->click();
        reconnected=waitFor([&]{return failed||(connectButton->isEnabled()
            &&manager->property("rvUsbRuntimeReplaced").isValid());},240000)&&!failed&&!confirmed
            &&session->deviceProfile().json()==firstProfile
            &&RvDeviceStore(session->deviceStorePath()).profiles().size()==count
            &&!manager->property("rvUsbRuntimeReplaced").toBool();
        report["repeat_import_selected_existing_device"]=reconnected;
        report["repeat_runtime_replaced"]=manager->property("rvUsbRuntimeReplaced").toBool();
        const bool repeatDepth=reconnected&&waitFor([&]{return invalidDepth||session->userStopped()||(positiveFrames-beforeFrames>=30&&paintedFrames-beforePainted>=20);},95000)
            &&!invalidDepth&&!session->userStopped()&&positiveFrames-beforeFrames>=30&&paintedFrames-beforePainted>=20;
        report["repeat_depth_received_and_painted"]=repeatDepth;reconnected&=repeatDepth;
        idle=stopAndInspect();
    }
    responder.stop();
    report["normal_stop"]=idle;
    const bool expectedRepair=qEnvironmentVariable("DCTS_USB_EXPECT_REPAIR")!="1"||report["first_runtime_replaced"].toBool();
    report["depth_frames"]=depthFrames;report["positive_depth_frames"]=positiveFrames;report["painted_frames"]=paintedFrames;
    report["depth_width"]=lastDepth["width"];report["depth_height"]=lastDepth["height"];report["depth_unit"]=lastDepth["depth_unit"];
    report["model_sha256"]=lastDepth["model_sha256"];
    report["passed"]=registered&&firstDepth&&idle&&reconnected&&expectedRepair&&report["registry_exported"].toBool();
    if(!registered&&!report.contains("error"))report["error"]="USB registration did not complete";
    save(path,report);manager->close();window.close();return report["passed"].toBool()?0:2;
}

// Explicit real-device integration test. Only scalar reports are saved.
// Uses ordinary session selection/start/switch/stop, never tuner.persist().
static int runBoardLiteSrLiveValidation(SensorMainWindow &window,const QString &path,bool tinySwitch=false,bool hostOnly=false,bool thermalOnly=false){
    auto *s=window.session();auto *t=s->tuner();
    QJsonObject report{{"passed",false},{"images_saved",false},{"measurement_arrays_saved",false},{"saved_sensor_settings_written",false}};
    report["host_only"]=hostOnly;
    if(thermalOnly){report["thermal_only"]=true;report["measurement_seconds"]=600;report["sample_interval_seconds"]=1;}
    if(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||s->deviceProfile().serial!=qEnvironmentVariable("DCTS_LIVE_SERIAL")||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1")return 3;
    const auto inspect=[&](QJsonObject &o){t->read();if(!waitFor([&]{return !t->pending();},95000)||t->state()!=RvTunerController::Idle)return false;o=t->inspection();return o["owner"]=="idle";};
    QJsonObject before,after;
    if(!inspect(before)){report["error"]="Idle preflight failed: "+t->detail();save(path,report);return 2;}
    report["before"]=before;
    auto *menu=window.findChild<QComboBox*>("rvDepthModel");
    auto *board=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvDtofFastDepthCanvas"));
    auto *host=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvAssistDepthCanvas"));
    if(!menu||!board||!host)return 3;
    QJsonArray runs;bool success=true;QString previousGeneration;
    QFile journal(path+".progress.jsonl");if(!journal.open(QIODevice::WriteOnly|QIODevice::Truncate))return 3;
    const auto progress=[&](QJsonObject o){o["unix_ms"]=QDateTime::currentMSecsSinceEpoch();journal.write(QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n');journal.flush();};
    const QString rect="board_litesr_near_384x256_host_contour_hd";
    const QString vga="host_unidepth_rgb672_vga_fp16_tof16";
    const QString tiny=TinyNv12Contract::Scheme;
    const QStringList sequence=thermalOnly?QStringList{hostOnly?vga:rect}:hostOnly?QStringList{vga,vga}:tinySwitch?QStringList{rect,tiny,vga,rect,vga,tiny,rect,vga}:QStringList{rect,vga,rect,vga};
    int stage=0;
    for(const auto &scheme:sequence){
        const bool onHost=scheme==vga,depthOnly=scheme==rect,tinyMode=scheme==tiny;const int mode=tinyMode?18:onHost?14:16;
        const auto hostModel=onHost?scheme.mid(5):QString();
        const auto &profile=RvCameraView::profile(hostModel);
        const int width=tinyMode?80:onHost?profile.outputWidth:384,height=tinyMode?128:onHost?profile.outputHeight:256;
        auto *canvas=onHost?host:board;QObject context;
        int frames=0,positiveFrames=0,painted=0,videos=0,native=0,photon=0,paired=0,reused=0,corrected=0,unpaired=0,historical=0;bool invalid=false,measuring=false;
        double maximumPairDelta=0;QVector<double> inference,total;
        int colorChecks=0,colorMismatches=0,brightColorChecks=0;QString colorScreenshot,cameraScreenshot;
        QString error,generation; qint64 lastId=-1,lastPaint=-1;QJsonObject last;QVector<double> npu,contour,compute,sourcePublish;QJsonArray windows;
        int info10=0;double firstPublish=-1,lastPublish=-1,firstCount=0,lastCount=0;
        qint64 nativeSequence=0,nativeTimestamp=0;
        const auto fail=[&](const QString &why){invalid=true;error=why;};
        const auto accept=[&](const QByteArray &bytes,const QJsonObject &m){
            if(s->dtofCandidateMode()!=mode||(!onHost&&m["candidate_mode"]!=mode))return;
            const auto g=m["generation"].toVariant().toString();const auto id=m["frame_id"].toInteger();
            if(!generation.isEmpty()&&generation==g&&id<=lastId){fail("Repeated depth frame");return;}
            generation=g;lastId=id;
            if(bytes.size()!=width*height*4||m["width"]!=width||m["height"]!=height){fail("Wrong output shape");return;}
            if(!onHost&&!tinyMode&&(m["model_sha256"]!=LiteSrBoardContract::modelSha(mode)||m["host_contour_enabled"].toBool()!=true||m["rgb_used"]!=false||m["input_rotation_degrees"]!=0||m["depth_unit"]!="sensor_radial_m_float32")){fail("Board identity, units or contour contract changed");return;}
            if(onHost&&(m["assist_host_model"]!=hostModel||m["assist_experiment"]!="H"||m["assist_output_restored_to_calibrated_grid"]!=true)){fail("Legacy host inference contract changed");return;}
            if(onHost&&(m["assist_backend"]!="onnxruntime-tensorrt-rtx"||m["depth_unit"]!="camera-Z_m_float32"||m["assist_input_orientation_checked"]!=true||m["assist_input_rotation_degrees"]!=s->assistInputRotationDegrees()||(m["dtof_present"].toBool()&&(m["dtof_width"]!=16||m["dtof_height"]!=16||m["dtof_fov_correction"]!=s->deviceProfile().fov())))){fail("Host backend, orientation or native measurement contract mismatch");return;}
            if(onHost&&s->deviceProfile().temporaryFovCalibration){
                const auto calibration=m["tof_calibration"].toObject();
                const int sensor=calibration["sensor_fov_correction"].toInt(-1);
                if(!RvCaptureCalibration::valid(calibration,m["source_generation"].toString(),s->deviceProfile().fov(),true)
                    ||m["sensor_fov_correction"]!=sensor||m["processing_fov_correction"]!=s->deviceProfile().fov()
                    ||m["temporary_fov_calibration"].toBool()!=(sensor!=s->deviceProfile().fov())){
                    fail("Temporary calibration lost actual sensor FOV or generation");return;
                }
            }
            if(onHost&&!m["dtof_present"].toBool()){
                const auto fit=m["assist_fusion"].toObject();
                if(fit["dtof_corrected"].toBool()||!fit["used_native_indices"].toArray().isEmpty()||!fit["global_used_native_indices"].toArray().isEmpty()||!fit["local_used_native_indices"].toArray().isEmpty()){fail("Historical scale reused old native correction points");return;}
            }
            if(depthOnly&&(m["video_transport"]!="none"||m["video_fps"]!=0||m["rgb_paused"]!=true)){fail("LiteSR384 camera-off contract mismatch");return;}
            if(tinyMode&&(m["model_sha256"]!=TinyNv12Contract::ModelSha||m["rgb_used"]!=true||m["depth_unit"]!="camera-Z_m_float32"||m["host_contour_enabled"]!=false||std::abs(m["pair_delta_ms"].toDouble(1000))>50)){fail("Tiny NV12 identity/geometry mismatch");return;}
            int positive=0;
            for(int i=0;i<width*height;++i){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);float z;std::memcpy(&z,&bits,4);if(!std::isfinite(z)||z<0||(!onHost&&z>(tinyMode?3.f:1.4f))){fail("Invalid metric value");return;}positive+=z>0;}
            if(positive)++positiveFrames;
            last=m;++frames;
            if(measuring){
                if(onHost){
                    inference<<m["assist_inference_ms"].toDouble();total<<m["assist_total_ms"].toDouble();
                    if(m["dtof_present"].toBool()){
                        ++paired;reused+=m["tof_reused"].toBool();
                        const auto delta=std::abs(m["pair_delta_ms"].toDouble(1000));
                        maximumPairDelta=std::max(maximumPairDelta,delta);
                        if(!std::isfinite(delta)||delta>100.)fail("Host accepted a native measurement older than 100 ms");
                    }
                    else {++unpaired;historical+=m["assist_fusion"].toObject()["fusion_mode"]=="held_scale";}
                    corrected+=m["assist_fusion"].toObject()["dtof_corrected"].toBool();
                }
                if(m["npu_ms"].isDouble())npu<<m["npu_ms"].toDouble();if(m["host_contour_ms"].isDouble())contour<<m["host_contour_ms"].toDouble();
                if(m["compute_ms"].isDouble())compute<<m["compute_ms"].toDouble();
                info10+=m["depth_native_warning_flags"]==16;
                if(m["publish_count"].isDouble()){
                    lastCount=m["publish_count"].toDouble();lastPublish=m["publish_monotonic_ms"].toDouble();
                    if(firstPublish<0){firstPublish=lastPublish;firstCount=lastCount;}
                    sourcePublish<<lastPublish-m["tof_timestamp_ns"].toInteger()/1e6;
                }
            }
        };
        QObject::connect(s,&RvSessionController::dtofCandidateReady,&context,[&](const RvDtofCandidates::Result &r){if(!onHost&&r.fresh)accept(r.depth,r.metadata);});
        QObject::connect(s,&RvSessionController::assistDepthSnapshotReady,&context,[&](const QByteArray &b,const QJsonObject &m){if(onHost)accept(b,m);});
        QObject::connect(s,&RvSessionController::cameraFrameReady,&context,[&](const QImage &im){if(measuring){if(depthOnly||im.size()!=(tinyMode?QSize(80,128):QSize(2304,1296)))fail("Unexpected RGB stream");++videos;}});
        QObject::connect(s,&RvSessionController::tofFrameReady,&context,[&](const RvTofFrame &f){if(measuring){if(f.columns()!=((onHost||tinyMode)?16:48)||f.rows()!=((onHost||tinyMode)?16:32))fail("Wrong native grid");++native;nativeSequence=f.metadata["tof_sequence"].toInteger();nativeTimestamp=f.metadata["tof_timestamp_ns"].toInteger();}});
        QObject::connect(s,&RvSessionController::photonFrameReady,&context,[&](const RvPhotonFrame &){if(measuring)++photon;});
        canvas->performanceFramePainted=[&](const QJsonObject &m){if(measuring&&m["frame_id"].toInteger()!=lastPaint){
            lastPaint=m["frame_id"].toInteger();++painted;
            if(tinyMode&&(painted==1||painted%30==0)){
                const auto pixels=canvas->presentationImage();int bright=0,mismatches=0;
                if(pixels.size()!=QSize(width,height)){fail("Tiny rendered image shape mismatch");return;}
                for(int y=0;y<height;++y)for(int x=0;x<width;++x){
                    const auto expected=DepthColorScale::color(canvas->depthAt(x,y),canvas->displayMinimum(),canvas->displayMaximum(),true,canvas->blackOutsideRange());
                    const auto actual=pixels.pixel(x,y);mismatches+=actual!=expected;
                    bright+=std::max({qRed(actual),qGreen(actual),qBlue(actual)})>1;
                }
                ++colorChecks;colorMismatches+=mismatches;brightColorChecks+=bright>0;
                if(mismatches)fail("Tiny valid depth was darkened by the display mask");
            }
        }};
        auto observer=qScopeGuard([&]{canvas->performanceFramePainted={};});
        progress({{"phase","selecting"},{"scheme",scheme}});
        const int index=menu->findData(scheme);
        if(index>=0&&menu->isEnabled()&&(menu->model()->flags(menu->model()->index(index,0))&Qt::ItemIsEnabled)){
            menu->setCurrentIndex(index);QMetaObject::invokeMethod(menu,"activated",Qt::DirectConnection,Q_ARG(int,index));
        }
        else {report["error"]="Required visible imaging scheme is missing or disabled";success=false;break;}
        if(stage==0||hostOnly)s->start();
        const bool ready=waitFor([&]{return invalid||s->userStopped()||(frames>=3&&!s->busy());},95000)&&!invalid&&!s->userStopped()&&frames>=3;
        if(!ready&&error.isEmpty())error=s->detailText()+" / "+s->depthInfoText();
        const int initial=frames;QElapsedTimer clock;clock.start();measuring=ready;
        const int seconds=thermalOnly?600:30;
        int previousFrames=initial,previousPainted=0;double previousSeconds=0;
        int previousNative=0,previousPhoton=0,previousNpu=0,previousCompute=0,previousContour=0;
        int previousInference=0,previousTotal=0,previousVideos=0,previousPaired=0,previousCorrected=0,previousUnpaired=0;
        double previousPublish=-1,previousCount=-1; qint64 previousNativeSequence=0,previousNativeTimestamp=0;
        const auto pulse=[&]{const double elapsed=clock.nsecsElapsed()/1e9,span=elapsed-previousSeconds;if(span<.1)return;
            QJsonObject item{{"phase","window"},{"scheme",scheme},{"elapsed_seconds",elapsed},{"window_seconds",span},{"depth_fps",(frames-previousFrames)/span},{"painted_fps",(painted-previousPainted)/span},{"info10_outputs",info10},{"last_frame_id",lastId}};
            if(thermalOnly){
                item["received_native_fps"]=(native-previousNative)/span;item["received_photon_fps"]=(photon-previousPhoton)/span;
                item["npu"]=stats(npu.mid(previousNpu));item["board_compute"]=stats(compute.mid(previousCompute));item["host_contour"]=stats(contour.mid(previousContour));
                if(hostOnly){
                    item["host_inference"]=stats(inference.mid(previousInference));item["host_total"]=stats(total.mid(previousTotal));
                    item["video_fps"]=(videos-previousVideos)/span;item["paired_frames"]=paired-previousPaired;
                    item["corrected_frames"]=corrected-previousCorrected;item["unpaired_frames"]=unpaired-previousUnpaired;
                    item["pair_delta_ms"]=last["pair_delta_ms"];item["tof_reused"]=last["tof_reused"];
                    previousInference=inference.size();previousTotal=total.size();previousVideos=videos;
                    previousPaired=paired;previousCorrected=corrected;previousUnpaired=unpaired;
                }
                item["publish_count"]=lastCount;item["publish_monotonic_ms"]=lastPublish;
                item["board_publish_fps"]=(previousPublish>=0&&lastPublish>previousPublish&&lastCount>=previousCount)
                    ?QJsonValue(1000.*(lastCount-previousCount)/(lastPublish-previousPublish)):QJsonValue();
                item["native_sequence"]=nativeSequence;item["native_timestamp_ns"]=QString::number(nativeTimestamp);
                item["native_source_fps"]=(previousNativeTimestamp>0&&nativeTimestamp>previousNativeTimestamp&&nativeSequence>=previousNativeSequence)
                    ?QJsonValue(1e9*(nativeSequence-previousNativeSequence)/(nativeTimestamp-previousNativeTimestamp)):QJsonValue();
                item["candidate_age_ms"]=last["candidate_age_ms"];item["raw_age_ms"]=last["raw_age_ms"];
                item["board_send_dropped"]=last["send_dropped"];item["temporary_fov_calibration"]=s->deviceProfile().temporaryFovCalibration;
                previousNative=native;previousPhoton=photon;previousNpu=npu.size();previousCompute=compute.size();previousContour=contour.size();
                previousPublish=lastPublish;previousCount=lastCount;previousNativeSequence=nativeSequence;previousNativeTimestamp=nativeTimestamp;
            }
            windows.append(item);progress(item);if(!thermalOnly&&frames==previousFrames)fail("No fresh output in measurement window");previousFrames=frames;previousPainted=painted;previousSeconds=elapsed;
        };
        QTimer timer;timer.setInterval(thermalOnly?1000:10000);QObject::connect(&timer,&QTimer::timeout,&context,pulse);
        if(ready){progress({{"phase","measuring"},{"scheme",scheme},{"seconds",seconds},{"generation",generation}});timer.start();waitFor([&]{return invalid||s->userStopped();},seconds*1000);timer.stop();pulse();}
        measuring=false;const double elapsed=clock.nsecsElapsed()/1e9;
        const bool geometry=onHost||(canvas->property("depthWidth").toInt()==width&&canvas->property("depthHeight").toInt()==height&&canvas->presentationSize()==(tinyMode?QSizeF(1248,1296):QSizeF(32,48)));
        auto *videoCanvas=window.findChild<QWidget*>("rtspCameraLatestFrame");
        const auto aspectMatches=[](QWidget *view,QSizeF source,int rotation){
            if(!view)return false;const auto actual=view->property("presentationRect").toRectF();
            const auto expected=ImagePresentation::rotatedSize(source,rotation);
            return actual.height()>0&&std::abs(actual.width()/actual.height()-expected.width()/expected.height())<1e-6;
        };
        const bool cameraGeometry=depthOnly?(videoCanvas&&videoCanvas->property("presentationSize").toSizeF().isEmpty()):(videoCanvas&&videoCanvas->property("presentationSize").toSizeF()==(tinyMode?QSizeF(1248,1296):QSizeF(2304,1296)));
        const bool displayAspect=!tinyMode||(aspectMatches(canvas,QSizeF(1248,1296),canvas->displayRotationDegrees())
            &&aspectMatches(videoCanvas,QSizeF(1248,1296),videoCanvas?videoCanvas->property("displayRotationDegrees").toInt():0));
        if(tinyMode&&ready){
            cameraScreenshot=path+QString(".tiny_camera_stage_%1.png").arg(stage);
            if(!videoCanvas||!videoCanvas->grab().save(cameraScreenshot))fail("Tiny camera screenshot save failed");
            colorScreenshot=path+QString(".tiny_stage_%1.png").arg(stage);
            if(canvas->grab().save(colorScreenshot))report["images_saved"]=true;else fail("Tiny screenshot save failed");
        }
        auto *cameraDock=window.findChild<QDockWidget*>("rtspCameraDock");
        const bool cameraVisibility=cameraDock&&(depthOnly?cameraDock->isHidden():!cameraDock->isHidden());
        if(!hostOnly&&!thermalOnly)window.grab().save(path+QString(".stage_%1.png").arg(stage));
        const bool pass=cameraVisibility&&ready&&!invalid&&!s->userStopped()&&elapsed>=seconds-.5&&frames-initial>=10&&positiveFrames>=10&&painted>=10&&(depthOnly?videos==0:videos>=30)&&native>=10&&(onHost?photon==0:photon>=10)&&geometry&&cameraGeometry&&displayAspect&&generation!=previousGeneration&&(!hostOnly||(paired>=10&&corrected>=10))&&(!tinyMode||(colorChecks>=10&&colorMismatches==0&&brightColorChecks>=10));
        if(!pass&&error.isEmpty())error=QString("Incomplete measurement: ready=%1 positive=%2 painted=%3 video=%4 native=%5 photon=%6 geometry=%7").arg(ready).arg(positiveFrames).arg(painted).arg(videos).arg(native).arg(photon).arg(geometry);
        QJsonObject result{{"scheme",scheme},{"mode",mode},{"passed",pass},{"error",error},{"seconds",elapsed},{"depth_frames",frames-initial},{"positive_depth_frames",positiveFrames},{"painted_frames",painted},{"video_frames",videos},{"native_frames",native},{"photon_frames",photon},{"depth_fps",(frames-initial)/std::max(.001,elapsed)},{"painted_fps",painted/std::max(.001,elapsed)},{"video_fps",videos/std::max(.001,elapsed)},{"npu",stats(npu)},{"contour",stats(contour)},{"display_geometry_preserved",geometry},{"generation",generation},{"model_sha256",last["model_sha256"]},{"input_rotation_degrees",last["input_rotation_degrees"]}};
        result["camera_dock_policy"]=cameraVisibility;result["camera_disabled"]=depthOnly;
        result["imaging_switch"]=s->imagingSwitchDiagnostics();
        if(onHost){result["inference"]=stats(inference);result["host_total"]=stats(total);result["paired_frames"]=paired;result["reused_native_frames"]=reused;result["dtof_corrected_frames"]=corrected;result["unpaired_frames"]=unpaired;result["historical_scale_frames"]=historical;result["maximum_pair_delta_ms"]=maximumPairDelta;result["backend"]=last["assist_backend"];result["host_model"]=last["assist_host_model"];result["graph_sha256"]=last["assist_graph_sha256"];result["tof_calibration"]=last["tof_calibration"];result["camera_mount_rotation_degrees"]=last["camera_mount_rotation_degrees"];result["input_rotation_degrees"]=last["assist_input_rotation_degrees"];}
        if(tinyMode)result["rendered_color_validation"]=QJsonObject{{"checks",colorChecks},{"mismatched_pixels",colorMismatches},{"checks_with_visible_color",brightColorChecks},{"minimum_m",canvas->displayMinimum()},{"maximum_m",canvas->displayMaximum()},{"black_outside_range",canvas->blackOutsideRange()},{"screenshot",colorScreenshot},{"camera_screenshot",cameraScreenshot},{"camera_geometry_preserved",cameraGeometry},{"crop_aspect_preserved",displayAspect}};
        result["board_compute"]=stats(compute);result["source_to_publish"]=stats(sourcePublish);result["info10_outputs"]=info10;result["windows"]=windows;
        result["board_publish_fps"]=(lastPublish>firstPublish)?1000.*(lastCount-firstCount)/(lastPublish-firstPublish):0.;
        runs.append(result);report["runs"]=runs;save(path,report);progress({{"phase","mode_finished"},{"passed",pass},{"scheme",scheme}});
        previousGeneration=generation;++stage;if(!pass){success=false;if(!tinySwitch||!ready||invalid||s->userStopped())break;}
        if(hostOnly&&stage<sequence.size()){
            s->stop();
            if(!waitFor([&]{return !s->busy();},30000)){success=false;break;}
            QJsonObject stopped;bool released=false;QElapsedTimer releaseClock;releaseClock.start();
            do{released=inspect(stopped);if(!released)waitFor([]{return false;},500);}
            while(!released&&releaseClock.elapsed()<25000);
            report["restart_stop_idle"]=released;
            report["restart_stop_wait_ms"]=releaseClock.elapsed();
            if(!released){report["error"]="Board owner did not become idle before restart: "+t->detail();success=false;break;}
        }
    }
    board->performanceFramePainted={};host->performanceFramePainted={};
    int late=0;QObject stopObserver;
    QObject::connect(s,&RvSessionController::dtofCandidateReady,&stopObserver,[&](const RvDtofCandidates::Result &r){if(r.fresh)++late;});
    QObject::connect(s,&RvSessionController::assistDepthSnapshotReady,&stopObserver,[&](const QByteArray &,const QJsonObject &){++late;});
    s->stop();waitFor([&]{return !s->busy();},30000);waitFor([]{return false;},1500);
    bool idle=false;QElapsedTimer stopClock;stopClock.start(); do { idle=inspect(after);if(!idle)waitFor([]{return false;},500); } while(!idle&&stopClock.elapsed()<25000);const bool unchanged=idle&&before["settings"]==after["settings"]&&before["litesr_settings"]==after["litesr_settings"]&&before["service_sha"]==after["service_sha"]&&before["litesr_sha"]==after["litesr_sha"];
    report["after"]=after;report["normal_stop"]=idle&&s->userStopped();report["late_results_after_stop"]=late;report["saved_sensor_files_unchanged"]=unchanged;
    report["passed"]=success&&stage==sequence.size()&&idle&&unchanged&&late==0;save(path,report);window.close();return report["passed"].toBool()?0:2;
}

int runLiteSr384LiveValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,false);}

int runHostAssistLiveValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,false,true);}

int runBoardThermalLiveValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,false,false,true);}
int runHostThermalLiveValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,false,true,true);}


int runTinyNv12SwitchValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,true);}

