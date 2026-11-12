#include "../sensors/RvCaptureCalibration.h"
#include <QtWidgets/QDockWidget>
#include "../SensorMainWindow.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvDtofBoardStream.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvInferenceRuntime.h"
#include "../sensors/RvInferenceSettings.h"
#include "../sensors/RvTunerController.h"
#include "../sensors/RvUsbEnrollment.h"
#include "../sensors/RvDeviceManagerDialog.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QtEndian>
#include <cstring>
#include <QDateTime>
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
#include <QMap>
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
int runImagingSwitchValidation(SensorMainWindow &,const QString &);
int runLiveValidation(SensorMainWindow &window,const QString &path,int seconds){
    return runImagingSwitchValidation(window,path);
}

struct RvDtofBoardStreamTestAccess {
    static void disconnectReceiver(RvDtofBoardStream &stream) { stream.socket.disconnectFromHost(); }
};

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
    auto *inference=RvInferenceSettings::instance();
    const bool exerciseDeferredStart=qEnvironmentVariable("DCTS_USB_TEST_DEFER_INFERENCE")=="1";
    bool preparingObserved=false,cancelNextDeferredStart=false,deferredStopIssued=false;
    QObject inferenceObserver;
    if(exerciseDeferredStart){
        // Hold the detection state until this real enrollment has queued a start.
        inference->m_enumerating=true;
        QObject::connect(session,&RvSessionController::stateChanged,&inferenceObserver,[&]{
            if((preparingObserved&&!cancelNextDeferredStart)
                ||session->automaticConnectionStage()!=RvSessionController::AutomaticConnectionStage::PreparingInference)return;
            preparingObserved=true;
            const bool cancel=cancelNextDeferredStart;
            cancelNextDeferredStart=false;
            QTimer::singleShot(50,&inferenceObserver,[&,cancel]{
                if(cancel){session->stop();deferredStopIssued=true;}
                inference->detectDevices();
            });
        });
    }else inference->detectDevices();
    auto *action=window.findChild<QAction*>("rvDeviceManagerAction");if(!action)return 3;
    action->trigger();auto *manager=window.findChild<RvDeviceManagerDialog*>();if(!manager)return 3;
    auto *connectButton=manager->findChild<QPushButton*>("rvDeviceConnectUsb");
    if(!connectButton)return 3;
    auto *depthCanvas=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvNativeDepthCanvas"));
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
    QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&depthObserver,[&](const QByteArray &b,const QJsonObject &m){acceptDepth(b,m);});
    const auto painted=[&](const QJsonObject &m){const auto id=m["frame_id"].toInteger();if(id!=lastPaint){lastPaint=id;++paintedFrames;}};
    depthCanvas->performanceFramePainted=painted;hostCanvas->performanceFramePainted=painted;
    const auto clearCallbacks=qScopeGuard([&]{depthCanvas->performanceFramePainted={};hostCanvas->performanceFramePainted={};});
    QTimer render;render.setInterval(50);
    QObject::connect(&render,&QTimer::timeout,&depthObserver,[&]{hostCanvas->grab();});render.start();
    bool failed=false;QObject context;QTimer responder;
    QObject::connect(&responder,&QTimer::timeout,&context,[&]{
        auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog)return;
        if(auto *message=qobject_cast<QMessageBox*>(dialog)){
            report["error"]=message->text()+"\n"+message->informativeText();
            failed=true;message->done(QMessageBox::Close);
        }
    });
    responder.start(20);connectButton->click();
    const bool registered=waitFor([&]{return failed||(connectButton->isEnabled()
        &&manager->property("rvUsbRuntimeReplaced").isValid()&&session->deviceProfile().serial==serial);},240000)&&!failed
        &&session->deviceProfile().serial==serial;
    report["parameters_requested"]=false;report["registered"]=registered;
    report["first_runtime_replaced"]=manager->property("rvUsbRuntimeReplaced").toBool();
    const auto stopped=[&]{return session->userStopped()&&!session->automaticConnecting();};
    const bool firstDepth=registered&&waitFor([&]{return invalidDepth||stopped()||(positiveFrames>=30&&paintedFrames>=20);},95000)
        &&!invalidDepth&&!session->userStopped()&&positiveFrames>=30&&paintedFrames>=20;
    report["first_depth_received_and_painted"]=firstDepth;
    report["first_depth_frames"]=depthFrames;report["first_painted_frames"]=paintedFrames;
    report["deferred_inference_start_observed"]=preparingObserved;
    const bool testRecovery=qEnvironmentVariable("DCTS_USB_TEST_STREAM_RECOVERY")=="1";
    bool recovered=!testRecovery;
    if(firstDepth&&testRecovery&&session->m_dtofStream){
        const auto previous=session->m_dtofReceipt["generation"].toString();
        const auto boot=session->m_dtofReceipt["boot_id"];
        const int beforeRecovery=positiveFrames,beforePaint=paintedFrames;
        const auto resumed=[&]{return lastDepth["source_generation"].toString()!=previous
            &&positiveFrames-beforeRecovery>=30&&paintedFrames-beforePaint>=20;};
        RvDtofBoardStreamTestAccess::disconnectReceiver(*session->m_dtofStream);
        recovered=waitFor([&]{return invalidDepth||stopped()||resumed();},75000)
            &&!invalidDepth&&!session->userStopped()&&resumed()
            &&session->m_dtofReceipt["boot_id"]==boot
            &&session->m_dtofReceipt["candidate_stream_recovery"]==QLatin1String(RvCaptureContract::CandidateRecoveryProtocol);
        report["stream_recovery_passed"]=recovered;
        report["stream_recovery_new_generation"]=session->m_dtofReceipt["generation"].toString()!=previous;
        if(!recovered)report["stream_recovery_detail"]=session->detailText();
    }
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
        connectButton->click();
        reconnected=waitFor([&]{return failed||(connectButton->isEnabled()
            &&manager->property("rvUsbRuntimeReplaced").isValid());},240000)&&!failed
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
    bool deferredStopHonored=!exerciseDeferredStart;
    if(exerciseDeferredStart&&reconnected&&idle){
        const int beforeFrames=depthFrames;
        cancelNextDeferredStart=true;
        inference->m_enumerating=true;
        connectButton->click();
        const auto restarted=[&]{return !session->userStopped()||depthFrames!=beforeFrames;};
        deferredStopHonored=waitFor([&]{return failed||deferredStopIssued;},240000)&&deferredStopIssued&&!failed
            &&waitFor([&]{return !inference->detecting()||restarted();},15000)&&!inference->detecting()
            &&!waitFor(restarted,3500);
        report["deferred_user_stop_honored"]=deferredStopHonored;
        idle=stopAndInspect();
    }
    responder.stop();
    report["normal_stop"]=idle;
    const bool expectedRepair=qEnvironmentVariable("DCTS_USB_EXPECT_REPAIR")!="1"||report["first_runtime_replaced"].toBool();
    report["depth_frames"]=depthFrames;report["positive_depth_frames"]=positiveFrames;report["painted_frames"]=paintedFrames;
    report["depth_width"]=lastDepth["width"];report["depth_height"]=lastDepth["height"];report["depth_unit"]=lastDepth["depth_unit"];
    report["model_sha256"]=lastDepth["model_sha256"];
    report["passed"]=registered&&firstDepth&&idle&&reconnected&&expectedRepair&&recovered
        &&(!exerciseDeferredStart||preparingObserved)&&deferredStopHonored&&report["registry_exported"].toBool();
    if(!registered&&!report.contains("error"))report["error"]="USB registration did not complete";
    save(path,report);manager->close();window.close();return report["passed"].toBool()?0:2;
}

// Explicit real-device integration test with scalar reports.
// Uses ordinary session selection/start/switch/stop, never tuner.persist().
static int runHostLiveValidation(SensorMainWindow &window,const QString &path,bool thermalOnly=false){
    auto *s=window.session();auto *t=s->tuner();
    QJsonObject report{{"passed",false},{"images_saved",false},{"measurement_arrays_saved",false},{"saved_sensor_settings_written",false}};
    const auto executionDevice=qEnvironmentVariable("RV_INFERENCE_BACKEND","tensorrt");
    if(!RvInferenceRuntime::validBackend(executionDevice))return 3;
    report["execution_device"]=executionDevice;
    if(thermalOnly){report["thermal_only"]=true;report["measurement_seconds"]=600;report["sample_interval_seconds"]=1;}
    if(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||s->deviceProfile().serial!=qEnvironmentVariable("DCTS_LIVE_SERIAL")||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1")return 3;
    const auto inspect=[&](QJsonObject &o){t->read();if(!waitFor([&]{return !t->pending();},95000)||t->state()!=RvTunerController::Idle)return false;o=t->inspection();return o["owner"]=="idle";};
    QJsonObject before,after;
    if(!inspect(before)){report["error"]="Idle preflight failed: "+t->detail();save(path,report);return 2;}
    report["before"]=before;
    auto *menu=window.findChild<QComboBox*>("rvDepthModel");
    auto *host=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvAssistDepthCanvas"));
    if(!menu||!host)return 3;
    QJsonArray runs;bool success=true;QString previousGeneration;
    QFile journal(path+".progress.jsonl");if(!journal.open(QIODevice::WriteOnly|QIODevice::Truncate))return 3;
    const auto progress=[&](QJsonObject o){o["unix_ms"]=QDateTime::currentMSecsSinceEpoch();journal.write(QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n');journal.flush();};
    const auto hostScheme=QString::fromLatin1(RvSessionController::ReuseLatestTofImagingScheme);
    const QStringList sequence=thermalOnly?QStringList{hostScheme}:QStringList{hostScheme,hostScheme};
    int stage=0;
    for(const auto &scheme:sequence){
        const auto hostModel=RvSessionController::imagingHostModel(scheme);
        const int mode=RvCameraView::captureMode(hostModel);
        const auto &profile=RvCameraView::outputProfile(hostModel);
        const int width=profile.outputWidth,height=profile.outputHeight;
        auto *canvas=host;QObject context;
        int frames=0,positiveFrames=0,painted=0,videos=0,native=0,photon=0,paired=0,reused=0,corrected=0,unpaired=0,historical=0;bool invalid=false,measuring=false;
        double maximumPairDelta=0;QVector<double> inference,total;
        QMap<QString,QVector<double>> performance;
        QString error,generation; qint64 lastId=-1,lastPaint=-1;QJsonObject last;QJsonArray windows;
        qint64 nativeSequence=0,nativeTimestamp=0,lastPairSequence=0,lastPairTimestamp=0;
        const auto fail=[&](const QString &why){invalid=true;error=why;};
        const auto accept=[&](const QByteArray &bytes,const QJsonObject &m){
            if(s->dtofCandidateMode()!=mode)return;
            const auto g=m["generation"].toVariant().toString();const auto id=m["frame_id"].toInteger();
            if(!generation.isEmpty()&&generation==g&&id<=lastId){fail("Repeated depth frame");return;}
            generation=g;lastId=id;
            if(bytes.size()!=width*height*4||m["width"]!=width||m["height"]!=height){fail("Wrong output shape");return;}
            if((!RvCameraView::validOutputGrid(m)||m["depth_K"].toArray()!=RvCameraView::intrinsics(profile))){fail("Portrait projection contract mismatch");return;}
            if((m["assist_host_model"]!=hostModel||m["assist_experiment"]!="B"||m["assist_output_restored_to_calibrated_grid"]!=true)){fail("Host inference identity mismatch");return;}
            if((m["assist_backend"]!=RvInferenceRuntime::backendName(executionDevice)||m["assist_execution_device"]!=executionDevice||!RvInferenceRuntime::validGraphMetadata(m)||m["depth_unit"]!="camera-Z_m_float32"||m["assist_input_orientation_checked"]!=true||m["assist_input_rotation_degrees"]!=s->assistInputRotationDegrees()||(m["dtof_present"].toBool()&&(m["dtof_width"]!=RvCameraView::nativeColumns(hostModel)||m["dtof_height"]!=RvCameraView::nativeRows(hostModel)||m["dtof_fov_correction"]!=s->deviceProfile().fov())))){fail("Host backend, orientation or native measurement contract mismatch");return;}
            if(s->deviceProfile().temporaryFovCalibration){
                const auto calibration=m["tof_calibration"].toObject();
                const int sensor=calibration["sensor_fov_correction"].toInt(-1);
                if(!RvCaptureCalibration::valid(calibration,m["source_generation"].toString(),s->deviceProfile().fov(),true)
                    ||m["sensor_fov_correction"]!=sensor||m["processing_fov_correction"]!=s->deviceProfile().fov()
                    ||m["temporary_fov_calibration"].toBool()!=(sensor!=s->deviceProfile().fov())){
                    fail("Temporary calibration lost actual sensor FOV or generation");return;
                }
            }
            if(!m["dtof_present"].toBool()){
                const auto fit=m["assist_fusion"].toObject();
                if(fit["dtof_corrected"].toBool()||!fit["used_native_indices"].toArray().isEmpty()||!fit["global_used_native_indices"].toArray().isEmpty()||!fit["local_used_native_indices"].toArray().isEmpty()){fail("Historical scale reused old native correction points");return;}
            }
            int positive=0;
            for(int i=0;i<width*height;++i){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);float z;std::memcpy(&z,&bits,4);if(!std::isfinite(z)||z<0){fail("Invalid metric value");return;}positive+=z>0;}
            if(positive)++positiveFrames;
            last=m;++frames;
            if(measuring){
                inference<<m["assist_inference_ms"].toDouble();total<<m["assist_total_ms"].toDouble();
                if(m["dtof_present"].toBool()){
                    ++paired;reused+=m["tof_reused"].toBool();
                    const auto delta=std::abs(m["pair_delta_ms"].toDouble(1000));
                    maximumPairDelta=std::max(maximumPairDelta,delta);
                    const auto uncertainty=m["pair_clock_uncertainty_ms"].toDouble(-1);
                    const auto sequence=m["tof_sequence"].toInteger(),stamp=m["tof_timestamp_ns"].toInteger();
                    if(!m["tof_reused"].isBool()||sequence<=0||stamp<=0||sequence<lastPairSequence
                       ||(sequence==lastPairSequence&&(m["tof_reused"]!=true||stamp!=lastPairTimestamp))
                       ||m["tof_warning_flags"]!=0||uncertainty<0
                       ||!std::isfinite(delta)||delta+uncertainty>RvTofFrame::FreshRgbPairBudgetMs)
                        fail("Host accepted an invalid ToF pair or reuse identity");
                    lastPairSequence=sequence;lastPairTimestamp=stamp;
                }else {++unpaired;historical+=m["assist_fusion"].toObject()["fusion_mode"]=="held_scale";}
                corrected+=m["assist_fusion"].toObject()["dtof_corrected"].toBool();
            }
        };
        QObject::connect(s,&RvSessionController::assistDepthSnapshotReady,&context,
            [&](const QByteArray &b,const QJsonObject &m,const QImage &pairedRgb){
                if(pairedRgb.size()!=profile.outputSize())fail("Paired RGB and metric depth grid differ");
                accept(b,m);
            });
        QObject::connect(s,&RvSessionController::cameraFrameReady,&context,[&](const QImage &im){if(measuring){if(im.size()!=QSize(2304,1296))fail("Unexpected RGB stream");++videos;}});
        QObject::connect(s,&RvSessionController::tofFrameReady,&context,[&](const RvTofFrame &f){if(measuring){if(f.columns()!=RvCameraView::nativeColumns(hostModel)||f.rows()!=RvCameraView::nativeRows(hostModel))fail("Wrong native grid");++native;nativeSequence=f.metadata["tof_sequence"].toInteger();nativeTimestamp=f.metadata["tof_timestamp_ns"].toInteger();}});
        QObject::connect(s,&RvSessionController::photonFrameReady,&context,[&](const RvPhotonFrame &){if(measuring)++photon;});
        canvas->performanceFramePainted=[&](const QJsonObject &m){if(measuring&&m["frame_id"].toInteger()!=lastPaint){
            lastPaint=m["frame_id"].toInteger();++painted;
            for(const auto *key:{"host_hd_prepare_ms","host_hd_prepare_queue_ms","assist_input_prepare_ms",
                    "assist_parent_validation_ms","assist_previous_gui_publish_ms","assist_frame_age_ms",
                    "gui_set_depth_ms","gui_color_queue_ms","gui_color_ms","gui_paint_ms","source_to_paint_ms"}){
                const auto value=m.value(QLatin1String(key));
                if(value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0)
                    performance[QLatin1String(key)].append(value.toDouble());
            }
        }};
        auto observer=qScopeGuard([&]{canvas->performanceFramePainted={};});
        progress({{"phase","selecting"},{"scheme",scheme}});
        const int index=menu->findData(scheme);
        if(index>=0&&menu->isEnabled()&&(menu->model()->flags(menu->model()->index(index,0))&Qt::ItemIsEnabled)){
            menu->setCurrentIndex(index);QMetaObject::invokeMethod(menu,"activated",Qt::DirectConnection,Q_ARG(int,index));
        }
        else {report["error"]="Required visible imaging scheme is missing or disabled";success=false;break;}
        s->start();
        bool ready=waitFor([&]{return invalid||s->userStopped()||(frames>=3&&!s->busy());},95000)&&!invalid&&!s->userStopped()&&frames>=3;
        const int requestedSeconds=thermalOnly?0:qEnvironmentVariableIntValue("DCTS_LIVE_SECONDS");
        if(ready&&requestedSeconds>0){
            waitFor([&]{return invalid||s->userStopped();},10000);
            ready=!invalid&&!s->userStopped();
        }
        if(!ready&&error.isEmpty())error=s->detailText()+" / "+s->depthInfoText();
        const int initial=frames;QElapsedTimer clock;clock.start();measuring=ready;
        const int seconds=thermalOnly?600:std::clamp(requestedSeconds>0?requestedSeconds:30,30,300);
        int previousFrames=initial,previousPainted=0;double previousSeconds=0;
        int previousNative=0,previousPhoton=0;
        int previousInference=0,previousTotal=0,previousVideos=0,previousPaired=0,previousCorrected=0,previousUnpaired=0;
        qint64 previousNativeSequence=0,previousNativeTimestamp=0;
        const auto pulse=[&]{const double elapsed=clock.nsecsElapsed()/1e9,span=elapsed-previousSeconds;if(span<.1)return;
            QJsonObject item{{"phase","window"},{"scheme",scheme},{"elapsed_seconds",elapsed},{"window_seconds",span},{"depth_fps",(frames-previousFrames)/span},{"painted_fps",(painted-previousPainted)/span},{"last_frame_id",lastId}};
            if(thermalOnly){
                item["received_native_fps"]=(native-previousNative)/span;item["received_photon_fps"]=(photon-previousPhoton)/span;
                item["host_inference"]=stats(inference.mid(previousInference));item["host_total"]=stats(total.mid(previousTotal));
                item["video_fps"]=(videos-previousVideos)/span;item["paired_frames"]=paired-previousPaired;
                item["corrected_frames"]=corrected-previousCorrected;item["unpaired_frames"]=unpaired-previousUnpaired;
                item["pair_delta_ms"]=last["pair_delta_ms"];item["tof_reused"]=last["tof_reused"];
                previousInference=inference.size();previousTotal=total.size();previousVideos=videos;
                previousPaired=paired;previousCorrected=corrected;previousUnpaired=unpaired;
                item["native_sequence"]=nativeSequence;item["native_timestamp_ns"]=QString::number(nativeTimestamp);
                item["native_source_fps"]=(previousNativeTimestamp>0&&nativeTimestamp>previousNativeTimestamp&&nativeSequence>=previousNativeSequence)
                    ?QJsonValue(1e9*(nativeSequence-previousNativeSequence)/(nativeTimestamp-previousNativeTimestamp)):QJsonValue();
                item["candidate_age_ms"]=last["candidate_age_ms"];item["raw_age_ms"]=last["raw_age_ms"];
                item["board_send_dropped"]=last["send_dropped"];item["temporary_fov_calibration"]=s->deviceProfile().temporaryFovCalibration;
                previousNative=native;previousPhoton=photon;
                previousNativeSequence=nativeSequence;previousNativeTimestamp=nativeTimestamp;
            }
            windows.append(item);progress(item);if(!thermalOnly&&frames==previousFrames)fail("No fresh output in measurement window");previousFrames=frames;previousPainted=painted;previousSeconds=elapsed;
        };
        QTimer timer;timer.setInterval(thermalOnly||requestedSeconds>0?1000:10000);QObject::connect(&timer,&QTimer::timeout,&context,pulse);
        if(ready){progress({{"phase","measuring"},{"scheme",scheme},{"seconds",seconds},{"generation",generation}});timer.start();waitFor([&]{return invalid||s->userStopped();},seconds*1000);timer.stop();pulse();}
        measuring=false;const double elapsed=clock.nsecsElapsed()/1e9;
        const bool geometry=canvas->property("depthWidth").toInt()==width&&canvas->property("depthHeight").toInt()==height;
        auto *videoCanvas=window.findChild<QWidget*>("rtspCameraLatestFrame");
        const bool cameraGeometry=videoCanvas&&videoCanvas->property("presentationSize").toSizeF()==QSizeF(2304,1296);
        auto *cameraDock=window.findChild<QDockWidget*>("rtspCameraDock");
        const bool cameraVisibility=cameraDock&&!cameraDock->isHidden();
        const bool pass=cameraVisibility&&ready&&!invalid&&!s->userStopped()&&elapsed>=seconds-.5&&frames-initial>=10&&positiveFrames>=10&&painted>=10&&videos>=30&&native>=10&&photon==0&&geometry&&cameraGeometry&&generation!=previousGeneration&&paired>=10&&corrected>=10;
        if(!pass&&error.isEmpty())error=QString("Incomplete measurement: ready=%1 positive=%2 painted=%3 video=%4 native=%5 photon=%6 geometry=%7").arg(ready).arg(positiveFrames).arg(painted).arg(videos).arg(native).arg(photon).arg(geometry);
        QJsonObject result{{"scheme",scheme},{"mode",mode},{"passed",pass},{"error",error},{"seconds",elapsed},{"depth_frames",frames-initial},{"positive_depth_frames",positiveFrames},{"painted_frames",painted},{"video_frames",videos},{"native_frames",native},{"photon_frames",photon},{"depth_fps",(frames-initial)/std::max(.001,elapsed)},{"painted_fps",painted/std::max(.001,elapsed)},{"video_fps",videos/std::max(.001,elapsed)},{"display_geometry_preserved",geometry},{"generation",generation},{"model_sha256",last["model_sha256"]},{"input_rotation_degrees",last["input_rotation_degrees"]}};
        result["camera_dock_policy"]=cameraVisibility;result["camera_disabled"]=false;
        result["imaging_switch"]=s->imagingSwitchDiagnostics();
        result["inference"]=stats(inference);result["host_total"]=stats(total);result["paired_frames"]=paired;result["reused_native_frames"]=reused;result["dtof_corrected_frames"]=corrected;result["unpaired_frames"]=unpaired;result["historical_scale_frames"]=historical;result["maximum_pair_delta_ms"]=maximumPairDelta;result["backend"]=last["assist_backend"];result["host_model"]=last["assist_host_model"];result["graph_sha256"]=last["assist_graph_sha256"];result["tof_calibration"]=last["tof_calibration"];result["camera_mount_rotation_degrees"]=last["camera_mount_rotation_degrees"];result["input_rotation_degrees"]=last["assist_input_rotation_degrees"];
        QJsonObject timing;
        for(auto it=performance.cbegin();it!=performance.cend();++it)
            timing[it.key()]=stats(it.value());
        result["performance"]=timing;
        result["windows"]=windows;
        runs.append(result);report["runs"]=runs;save(path,report);progress({{"phase","mode_finished"},{"passed",pass},{"scheme",scheme}});
        previousGeneration=generation;++stage;if(!pass){success=false;break;}
        if(stage<sequence.size()){
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
    host->performanceFramePainted={};
    int late=0;QObject stopObserver;
    QObject::connect(s,&RvSessionController::assistDepthSnapshotReady,&stopObserver,[&](const QByteArray &,const QJsonObject &){++late;});
    s->stop();waitFor([&]{return !s->busy();},30000);waitFor([]{return false;},1500);
    bool idle=false;QElapsedTimer stopClock;stopClock.start(); do { idle=inspect(after);if(!idle)waitFor([]{return false;},500); } while(!idle&&stopClock.elapsed()<25000);const bool unchanged=idle&&before["settings"]==after["settings"]&&before["capture_settings"]==after["capture_settings"]&&before["service_sha"]==after["service_sha"]&&before["capture_sha"]==after["capture_sha"];
    report["after"]=after;report["normal_stop"]=idle&&s->userStopped();report["late_results_after_stop"]=late;report["saved_sensor_files_unchanged"]=unchanged;
    report["passed"]=success&&stage==sequence.size()&&idle&&unchanged&&late==0;save(path,report);window.close();return report["passed"].toBool()?0:2;
}

int runHostAssistLiveValidation(SensorMainWindow &w,const QString &p){return runHostLiveValidation(w,p);}
int runHostThermalLiveValidation(SensorMainWindow &w,const QString &p){return runHostLiveValidation(w,p,true);}
int runImagingSwitchValidation(SensorMainWindow &window,const QString &path){
    auto *session=window.session();
    if(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"
       ||session->deviceProfile().serial!=qEnvironmentVariable("DCTS_LIVE_SERIAL"))return 3;
    auto *menu=window.findChild<QComboBox*>("rvDepthModel");
    auto *host=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvAssistDepthCanvas"));
    auto *native=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvNativeDepthCanvas"));
    if(!menu||!host||!native)return 3;
    const QString hostScheme=QLatin1String(RvSessionController::ReuseLatestTofImagingScheme),ordinary="board_native_tof48",high="board_native_tof48_high_accuracy";
    QJsonObject report{{"passed",false},{"calibration_release",RvCalibration::Release},{"rendering","offscreen_widget_grab"}};
    QJsonArray stages;bool all=true;QString priorGeneration;
    for(const auto &scheme:QStringList{hostScheme,ordinary,high,hostScheme,high,ordinary,hostScheme}){
        QObject scope;int frames=0,painted=0,videos=0,rawFrames=0,filledPoints=0;bool invalid=false,measuring=false;
        QByteArray rawDepth;QJsonObject last,rawMeta;QString generation;double maxDelta=0;
        auto *canvas=scheme==hostScheme?host:native;
        qint64 lastPaint=-1;
        canvas->performanceFramePainted=[&](const QJsonObject &m){
            const auto frame=m["frame_id"].toInteger(-1);
            if(measuring&&frame!=lastPaint){lastPaint=frame;++painted;}
        };
        // A hidden validation process must still exercise actual widget paints.
        QTimer render;render.setInterval(50);
        QObject::connect(&render,&QTimer::timeout,&scope,[&]{if(measuring)canvas->grab();});render.start();
        QObject::connect(session,&RvSessionController::cameraFrameReady,&scope,[&](const QImage&){if(measuring)++videos;});
        QObject::connect(session,&RvSessionController::tofFrameReady,&scope,[&](const RvTofFrame &f){
            if(!measuring)return;++rawFrames;rawMeta=f.metadata;rawDepth=f.radialMetres();
            invalid|=f.columns()!=48||f.rows()!=32||f.metadata["dtof_fov_correction"]!=6;
        });
        const auto accept=[&](const QByteArray &bytes,const QJsonObject &m){
            if(!measuring)return;++frames;last=m;generation=m["source_generation"].toString();
            if(generation.isEmpty())generation=m["generation"].toVariant().toString();
            invalid|=bytes.size()!=(scheme==hostScheme?640*480*4:48*32*4);
            if(scheme!=hostScheme){
                invalid|=session->assistDepthEnabled()||m["tof_sequence"]!=rawMeta["tof_sequence"]
                    ||m["depth_unit"]!="sensor_radial_m_float32"||m["native_order"]!=true
                    ||m["sensor_mode"]!=(scheme==ordinary?11:12);
                const auto indices=m["native_hole_fill_indices"].toArray();
                auto restored=bytes;
                for(const auto index:indices){
                    const int offset=4*index.toInt(-1);
                    if(offset<0||offset+4>bytes.size()||rawDepth.mid(offset,4)!=QByteArray(4,0)||restored.mid(offset,4)==QByteArray(4,0)){
                        invalid=true;break;
                    }
                    restored.replace(offset,4,QByteArray(4,0));
                }
                filledPoints+=indices.size();
                invalid|=restored!=rawDepth||m["interpolation"].toBool()!=!indices.isEmpty();
            }else if(m["dtof_present"]==true){
                const auto delta=std::abs(m["pair_delta_ms"].toDouble(1000));
                const auto uncertainty=m["pair_clock_uncertainty_ms"].toDouble(-1);
                maxDelta=std::max(maxDelta,delta);
                invalid|=!m["tof_reused"].isBool()||m["tof_warning_flags"]!=0||uncertainty<0
                    ||!std::isfinite(delta)||delta+uncertainty>RvTofFrame::FreshRgbPairBudgetMs;
            }
        };
        if(scheme==hostScheme)QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&scope,
            [&](const QByteArray &b,const QJsonObject &m,const QImage &){accept(b,m);});
        else QObject::connect(session,&RvSessionController::depthSnapshotReady,&scope,
            [&](const QByteArray &b,const QJsonObject &m,const QImage &){accept(b,m);});
        const int index=menu->findData(scheme);if(index<0){all=false;break;}
        menu->setCurrentIndex(index);QMetaObject::invokeMethod(menu,"activated",Qt::DirectConnection,Q_ARG(int,index));
        if(session->userStopped())session->start();
        const bool ready=waitFor([&]{return session->userStopped()||(!session->busy()&&!session->imagingSwitchActive());},95000)
            &&!session->userStopped()&&session->currentImagingScheme()==scheme;
        measuring=true;QElapsedTimer clock;clock.start();
        if(ready)waitFor([&]{return invalid||session->userStopped();},12000);
        measuring=false;canvas->performanceFramePainted={};
        const auto snap=window.depth()->snapshot();
        const bool valid=ready&&!invalid&&!session->userStopped()&&frames>=10&&painted>=10&&rawFrames>=10&&videos>=20
            &&generation!=priorGeneration&&snap.metresFloat32LE.size()==(scheme==hostScheme?640*480*4:48*32*4);
        QJsonObject result{{"scheme",scheme},{"passed",valid},{"ready",ready},{"invalid_data",invalid},{"frames",frames},
            {"painted",painted},{"raw_frames",rawFrames},{"filled_points",filledPoints},{"video_frames",videos},{"seconds",clock.elapsed()/1000.},
            {"width",last["width"]},{"height",last["height"]},{"depth_unit",last["depth_unit"]},{"generation",generation},
            {"sensor_mode",last["sensor_mode"]},{"maximum_pair_delta_ms",maxDelta},{"state",session->stateText()},
            {"detail",session->detailText()},{"switch",session->imagingSwitchDiagnostics()}};
        stages.append(result);report["stages"]=stages;save(path,report);
        if(!valid){all=false;break;}
        canvas->grab().save(path+"."+scheme+".png");
        priorGeneration=generation;
    }
    host->performanceFramePainted={};native->performanceFramePainted={};
    QObject stopScope;int late=0;
    QObject::connect(session,&RvSessionController::depthFrameReady,&stopScope,[&]{++late;});
    QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&stopScope,[&]{++late;});
    session->stop();const bool stopped=waitFor([&]{return !session->busy();},35000);waitFor([]{return false;},1500);
    report["normal_stop"]=stopped;report["late_depth_results"]=late;report["passed"]=all&&stages.size()==7&&stopped&&late==0;
    save(path,report);window.close();return report["passed"].toBool()?0:2;
}
