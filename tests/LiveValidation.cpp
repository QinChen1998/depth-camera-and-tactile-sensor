#include "../sensors/RvCaptureCalibration.h"
#include <QtWidgets/QDockWidget>
#include "../SensorMainWindow.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/MeasurementCaptureDialog.h"
#include "../sensors/PointCloudColorScale.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvDtofBoardStream.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvTunerController.h"
#include "../sensors/DepthSdkClient.h"
#include "../sensors/RvDeviceManagerDialog.h"
#include "../sensors/SceneInstanceBuffer.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QtEndian>
#include <cstring>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include "../sensors/RvPointCloudCanvas.h"
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QSpinBox>
#include <QProgressBar>
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

int runMeasurementLiveValidation(SensorMainWindow &window, const QString &path)
{
    auto *session = window.session();
    if (session->deviceProfile().serial != qEnvironmentVariable("DCTS_LIVE_SERIAL"))
        return 3;
    auto *open = window.findChild<QAction *>("measurementCaptureAction");
    if (!open)
        return 3;
    open->trigger();
    auto *dialog = window.findChild<MeasurementCaptureDialog *>("measurementCaptureDialog");
    auto *source = window.findChild<QComboBox *>("measurementSource");
    auto *directory = window.findChild<QLineEdit *>("measurementDirectory");
    auto *duration = window.findChild<QSpinBox *>("measurementDuration");
    auto *start = window.findChild<QPushButton *>("measurementStart");
    auto *status = window.findChild<QLabel *>("measurementStatus");
    auto *progress = window.findChild<QProgressBar *>("measurementProgress");
    if (!dialog || !source || !directory || !duration || !start || !status || !progress)
        return 3;
    const QString root = QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + "_captures";
    if (!QDir().mkpath(root) || !QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
        return 3;
    const auto integration = qEnvironmentVariable("DCTS_MEASUREMENT_LIVE_1200K");
    const bool highAccuracy = integration == "high_accuracy";
    const bool validate1200K = highAccuracy || integration == "1";
    QString scheme = QLatin1String(RvSessionController::Native48ExtendedIntegrationScheme);
    if (highAccuracy) scheme = QLatin1String(RvSessionController::Native48HighAccuracy1200KScheme);
    else if (validate1200K) scheme = QLatin1String(RvSessionController::Native48ExtendedIntegrationScheme);
    session->selectImagingScheme(scheme, {});
    const auto onlySource = qEnvironmentVariable("DCTS_MEASUREMENT_LIVE_SOURCE");
    if ((!onlySource.isEmpty() && onlySource != "rv1103" && onlySource != "gemini335")
        || (validate1200K && onlySource == "gemini335"))
        return 3;
    const QStringList sources = validate1200K ? QStringList{"rv1103"}
        : onlySource.isEmpty() ? QStringList{"rv1103", "gemini335"} : QStringList{onlySource};
    QJsonArray stages;
    bool passed = true;
    for (const auto &name : sources) {
        const bool rv = name == "rv1103";
        const auto output = root + "/" + name;
        if (!QDir().mkpath(output))
            return 3;
        source->setCurrentIndex(source->findData(name));
        duration->setValue(rv ? 30 : 60);
        directory->setText(QDir::toNativeSeparators(output));
        start->click();
        const bool finished = waitFor([&] { return !dialog->recording(); }, (duration->value() + 55) * 1000);
        if (!finished) {
            dialog->stopRecording();
            waitFor([&] { return !dialog->recording(); }, 5000);
        }
        const auto folders = QDir(output).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        const auto capture = folders.size() == 1 ? QDir(output + "/" + folders.first()) : QDir(output);
        QFile manifest(capture.filePath("session.json"));
        QJsonObject data;
        if (manifest.open(QIODevice::ReadOnly))
            data = QJsonDocument::fromJson(manifest.readAll()).object();
        const auto format = data["format"].toObject();
        const auto frameBytes = format["frame_bytes"].toInteger();
        const auto frames = data["frames"].toInteger();
        const auto span = data["span_ns"].toInteger();
        const qint64 expectedBytes = rv ? 48 * 32 * 4 : 1280 * 800 * 2;
        const qint64 durationNs = qint64(duration->value()) * 1'000'000'000;
        bool valid = finished && folders.size() == 1 && data["source"] == name && data["status"] == "complete"
            && frameBytes == expectedBytes && frames > 20 && span >= durationNs && span <= durationNs + 1'000'000'000
            && QFileInfo(capture.filePath("depth.bin")).size() == frames * frameBytes;
        const int expectedMode = highAccuracy ? RvCaptureContract::Native48HighAccuracy : RvCaptureContract::Native48;
        valid &= rv ? (format["candidate_mode"] == expectedMode && QFileInfo::exists(capture.filePath("geometry.bin")))
                    : (format["device"].toString().contains("Gemini 335") && format["intrinsics"].isObject()
                       && format["scale_mm_per_code"].toDouble() > 0);
        if (validate1200K)
            valid &= format["imaging_scheme"] == scheme
                && format["sensor_readback"].toObject()["main_iterations_k"] == RvCaptureContract::Native48ExtendedIntegrationK
                && data["sequence_gaps"] == 0;
        QFile index(capture.filePath("frames.jsonl"));
        qint64 count = 0, sequence = 0, previousTime = 0, firstTime = 0, gaps = 0;
        if (index.open(QIODevice::ReadOnly)) {
            while (!index.atEnd()) {
                const auto item = QJsonDocument::fromJson(index.readLine()).object();
                const auto seq = item["sequence"].toInteger();
                const auto time = item["source_time_ns"].toInteger();
                valid &= seq > sequence && time > previousTime && item["host_arrival_ns"].toInteger() > 0
                    && item["offset"].toInteger(-1) == count * expectedBytes && item["bytes"].toInteger() == expectedBytes;
                if (count)
                    gaps += seq - sequence - 1;
                else
                    firstTime = time;
                sequence = seq;
                previousTime = time;
                ++count;
            }
        }
        valid &= count == frames && previousTime - firstTime == span && gaps == data["sequence_gaps"].toInteger(-1);
        valid &= progress->value() == progress->maximum();
        QFile depth(capture.filePath("depth.bin"));
        int validPixels = 0;
        if (depth.open(QIODevice::ReadOnly)) {
            const auto sample = depth.read(expectedBytes);
            const int stride = rv ? 4 : 2;
            for (qsizetype i = 0; i + stride <= sample.size(); i += stride)
                validPixels += qFromLittleEndian<quint16>(sample.constData() + i) > 0 && (!rv || sample[i + 3] == 1);
        }
        valid &= validPixels > 0;
        if (rv) {
            session->stop();
            valid &= waitFor([&] { return session->userStopped() && !session->busy(); }, 15000);
        }
        dialog->grab().save(QDir(root).filePath(name + "_page.png"));
        stages.append(QJsonObject{{"source", name}, {"passed", valid}, {"status", data["status"]},
            {"detail", data["detail"]}, {"ui_status", status->text()}, {"capture", capture.absolutePath()},
            {"frames", frames}, {"seconds", span / 1e9}, {"sequence_gaps", gaps}, {"first_valid_pixels", validPixels},
            {"imaging_scheme", format["imaging_scheme"]}, {"candidate_mode", format["candidate_mode"]},
            {"sensor_readback", format["sensor_readback"]}});
        passed &= valid;
    }
    const bool written = save(path, {{"passed", passed}, {"stages", stages}});
    return passed && written ? 0 : 1;
}

// Explicitly authorized real USB onboarding through the SDK device manager.
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
    const bool exerciseStopDuringConnect=qEnvironmentVariable("DCTS_USB_TEST_DEFER_INFERENCE")=="1";
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
            if(message->objectName()=="rvRgbLscPrompt"){message->done(QMessageBox::Cancel);return;}
            report["error"]=message->text()+"\n"+message->informativeText();
            failed=true;message->done(QMessageBox::Close);
        }
    });
    responder.start(20);connectButton->click();
    const bool registered=waitFor([&]{return failed||(!manager->operationPending()
        &&manager->property("rvUsbRuntimeReplaced").isValid()&&session->deviceProfile().serial==serial);},240000)&&!failed
        &&session->deviceProfile().serial==serial;
    report["parameters_requested"]=false;report["registered"]=registered;
    report["first_runtime_replaced"]=manager->property("rvUsbRuntimeReplaced").toBool();
    const auto stopped=[&]{return session->userStopped()&&!session->automaticConnecting();};
    const bool firstDepth=registered&&waitFor([&]{return invalidDepth||stopped()||(positiveFrames>=30&&paintedFrames>=20);},95000)
        &&!invalidDepth&&!session->userStopped()&&positiveFrames>=30&&paintedFrames>=20;
    report["first_depth_received_and_painted"]=firstDepth;
    if(!firstDepth)report["first_capture_detail"]=session->detailText();
    report["first_depth_frames"]=depthFrames;report["first_painted_frames"]=paintedFrames;
    const bool testRecovery=qEnvironmentVariable("DCTS_USB_TEST_STREAM_RECOVERY")=="1";
    bool recovered=!testRecovery;
    if(firstDepth&&testRecovery){
        const auto previous=lastDepth["source_generation"].toString();
        const auto boot=lastDepth["boot_id"];
        const int beforeRecovery=positiveFrames,beforePaint=paintedFrames;
        const auto resumed=[&]{return lastDepth["source_generation"].toString()!=previous
            &&positiveFrames-beforeRecovery>=30&&paintedFrames-beforePaint>=20;};
        session->reconnectNow();
        recovered=waitFor([&]{return invalidDepth||stopped()||resumed();},75000)
            &&!invalidDepth&&!session->userStopped()&&resumed()
            &&lastDepth["boot_id"]==boot;
        report["stream_recovery_passed"]=recovered;
        report["stream_recovery_new_generation"]=lastDepth["source_generation"].toString()!=previous;
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
        reconnected=waitFor([&]{return failed||(!manager->operationPending()
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
    bool deferredStopHonored=!exerciseStopDuringConnect;
    if(exerciseStopDuringConnect&&reconnected&&idle){
        const int beforeFrames=depthFrames;
        connectButton->click();
        const bool connecting=session->depthSdk()->captureTransitionPending();
        session->stop();
        const auto restarted=[&]{return !session->userStopped()||depthFrames!=beforeFrames;};
        deferredStopHonored=connecting&&!failed&&waitFor([&]{return !session->busy();},35000)
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
        &&deferredStopHonored&&report["registry_exported"].toBool();
    if(!registered&&!report.contains("error"))report["error"]="USB registration did not complete";
    save(path,report);manager->close();window.close();return report["passed"].toBool()?0:2;
}

// Explicit real-device integration test. Frame files require a diagnostic directory.
// Uses ordinary session selection/start/switch/stop, never tuner.persist().
static int runHostLiveValidation(SensorMainWindow &window,const QString &path,bool thermalOnly=false){
    constexpr bool hostOnly=true;
    auto *s=window.session();auto *t=s->tuner();
    QJsonObject report{{"passed",false},{"images_saved",false},{"measurement_arrays_saved",false},{"saved_sensor_settings_written",false}};
    const auto diagnosticDirectory=thermalOnly?QString():qEnvironmentVariable("DCTS_HOST_DIAGNOSTIC_DIRECTORY");
    const QString hostModel=QLatin1String(RvCameraView::DepthPro2Model);
    const auto &runtime=RvCameraView::DepthPro2SRuntime;
    const auto executionDevice=QLatin1String(RvCameraView::DepthPro2Backend);
    const auto backend=executionDevice;
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
    const QStringList sequence{QLatin1String(RvSessionController::HostAssistImagingScheme)};
    int stage=0;
    for(const auto &scheme:sequence){
        constexpr bool onHost=true,depthOnly=false;
        constexpr int mode=RvCaptureContract::Native48;
        const QString experiment=QStringLiteral("B");
        const auto &profile=RvCameraView::outputProfile(hostModel);
        const int width=onHost?profile.outputWidth:384,height=onHost?profile.outputHeight:256;
        auto *canvas=host;QObject context;
        int frames=0,positiveFrames=0,painted=0,videos=0,native=0,photon=0,paired=0,reused=0,corrected=0,unpaired=0,historical=0;bool invalid=false,measuring=false;
        double maximumPairDelta=0;QVector<double> inference,total;
        QMap<QString,QVector<double>> performance;
        QString error,generation; qint64 lastId=-1,lastPaint=-1;QJsonObject last;QVector<double> npu,contour,compute,sourcePublish;QJsonArray windows;
        RvDepthDiagnostic::Snapshot pairedDiagnostic,heldDiagnostic;
        int info10=0;double firstPublish=-1,lastPublish=-1,firstCount=0,lastCount=0;
        qint64 nativeSequence=0,nativeTimestamp=0;
        const auto fail=[&](const QString &why){invalid=true;error=why;};
        const auto accept=[&](const QByteArray &bytes,const QJsonObject &m){
            if(s->dtofCandidateMode()!=mode||(!onHost&&m["candidate_mode"]!=mode))return;
            const auto g=m["generation"].toVariant().toString();const auto id=m["frame_id"].toInteger();
            if(!generation.isEmpty()&&generation==g&&id<=lastId){fail("Repeated depth frame");return;}
            generation=g;lastId=id;
            if(bytes.size()!=width*height*4||m["width"]!=width||m["height"]!=height){fail("Wrong output shape");return;}
            if(onHost&&(m["assist_host_model"]!=hostModel||m["assist_experiment"]!=experiment||!RvCameraView::validOutputGrid(m)||m["assist_output_restored_to_calibrated_grid"]!=true)){fail("Host inference grid or identity mismatch");return;}
            const bool graphMatches=m["assist_engine_sha256"]==QLatin1String(runtime.engineSha)
                &&m["assist_graph_sha256"]==QLatin1String(profile.graphSha)&&m["assist_precision"]=="fp16"
                &&m["rgb_width"]==profile.inputWidth&&m["rgb_height"]==profile.inputHeight
                &&m["depth_K"].toArray()==RvCameraView::intrinsics(profile);
            if(onHost&&(m["assist_backend"]!=backend||m["assist_execution_device"]!=executionDevice||!graphMatches||m["depth_unit"]!="camera-Z_m_float32"||m["assist_input_orientation_checked"]!=true||m["assist_input_rotation_degrees"]!=s->assistInputRotationDegrees()||(m["dtof_present"].toBool()&&(m["dtof_width"]!=48||m["dtof_height"]!=32||m["dtof_fov_correction"]!=s->deviceProfile().fov())))){fail("Host backend, orientation or native measurement contract mismatch");return;}
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
            int positive=0;
            for(int i=0;i<width*height;++i){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);float z;std::memcpy(&z,&bits,4);if(!std::isfinite(z)||(z<0||(!onHost&&z>1.4f))){fail("Invalid metric value");return;}positive+=z>0;}
            if(positive)++positiveFrames;
            last=m;++frames;
            if(measuring){
                if(onHost){
                    inference<<m["assist_inference_ms"].toDouble();
                    if(m["assist_total_ms"].isDouble())total<<m["assist_total_ms"].toDouble();
                    if(m["dtof_present"].toBool()){
                        ++paired;reused+=m["tof_reused"].toBool();
                        const auto delta=std::abs(m["pair_delta_ms"].toDouble(1000));
                        maximumPairDelta=std::max(maximumPairDelta,delta);
                        if(!std::isfinite(delta)||delta+m["pair_clock_uncertainty_ms"].toDouble(35)>RvTofFrame::FreshRgbPairBudgetMs)
                            fail("Host exceeded the RGB/ToF pairing budget");
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
        QObject::connect(s,&RvSessionController::assistDepthSnapshotReady,&context,
            [&](const QByteArray &bytes,const QJsonObject &m,const QImage &rgb,const RvTofFrame &tof,const QByteArray &payload) {
                accept(bytes,m);
                if(diagnosticDirectory.isEmpty()||invalid)
                    return;
                const auto fit=m["assist_fusion"].toObject();
                if(RvDepthDiagnostic::matchesPairedNative(m,tof)&&fit["dtof_corrected"]==true
                    &&pairedDiagnostic.output.metresFloat32LE.isEmpty())
                    pairedDiagnostic={{bytes,{},rgb,m,QDateTime::currentDateTimeUtc()},tof,payload,{}};
                if(m["dtof_present"]==false&&fit["fusion_mode"]=="held_scale"&&heldDiagnostic.output.metresFloat32LE.isEmpty())
                    heldDiagnostic={{bytes,{},rgb,m,QDateTime::currentDateTimeUtc()},tof,payload,{}};
            });
        QObject::connect(s,&RvSessionController::cameraFrameReady,&context,[&](const QImage &im){if(measuring){if(depthOnly||im.size()!=QSize(2304,1296))fail("Unexpected RGB stream");++videos;}});
        QObject::connect(s,&RvSessionController::tofFrameReady,&context,[&](const RvTofFrame &f){if(measuring){if(f.columns()!=48||f.rows()!=32)fail("Wrong native grid");++native;nativeSequence=f.metadata["tof_sequence"].toInteger();nativeTimestamp=f.metadata["tof_timestamp_ns"].toInteger();}});
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
        if(stage==0||hostOnly)s->start();
        const bool ready=waitFor([&]{return invalid||s->userStopped()||(frames>=3&&!s->busy());},95000)&&!invalid&&!s->userStopped()&&frames>=3;
        if(!ready&&error.isEmpty())error=s->detailText()+" / "+s->depthInfoText();
        const int requestedSeconds=qEnvironmentVariableIntValue("DCTS_LIVE_SECONDS");
        if(ready&&requestedSeconds>0)waitFor([&]{return invalid||s->userStopped();},10000);
        QString sequenceFolder;
        bool overlayOk=false,alignmentOk=false;
        if(ready&&!diagnosticDirectory.isEmpty()){
            auto *depthDock=window.findChild<QDockWidget *>("depthMapDock");
            if(depthDock){
                depthDock->show();
                depthDock->raise();
            }
            auto *points=window.depth()->findChild<QPushButton *>("depthPairedTofPoints");
            auto *alignment=window.depth()->findChild<QPushButton *>("depthPairedRgb");
            auto *record=window.depth()->findChild<QPushButton *>("depthDiagnosticRecord");
            const auto controlsReady=[&] {
                return points&&alignment&&record&&points->isEnabled()&&alignment->isEnabled()
                    &&record->isEnabled()&&last["dtof_present"]==true;
            };
            const bool pairedReady=waitFor([&]{return invalid||s->userStopped()||controlsReady();},10000)
                &&!invalid&&!s->userStopped()&&controlsReady();
            if(pairedReady){
                const auto baseline=window.depth()->snapshot();
                const auto corrected=canvas->grab().toImage();
                points->click();
                const auto overlaid=canvas->grab().toImage();
                overlayOk=points->isChecked()&&overlaid!=corrected;
                alignment->click();
                const auto aligned=canvas->grab().toImage();
                const auto saved=window.depth()->snapshot();
                alignmentOk=alignment->isChecked()&&aligned!=overlaid&&saved.depthImage==baseline.depthImage
                    &&saved.metresFloat32LE==baseline.metresFloat32LE;
                alignment->click();
                points->click();
                sequenceFolder=window.depth()->startDiagnosticRecording(diagnosticDirectory,&error);
            }else if(error.isEmpty()){
                error="Timed out waiting for a paired frame and ready diagnostic controls";
            }
            if(sequenceFolder.isEmpty())
                invalid=true;
            else{
                report["images_saved"]=true;
                report["measurement_arrays_saved"]=true;
            }
        }
        const int initial=frames;QElapsedTimer clock;clock.start();measuring=ready;
        const int seconds=thermalOnly?600:std::clamp(requestedSeconds>0?requestedSeconds:30,30,300);
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
        QTimer timer;timer.setInterval(thermalOnly||requestedSeconds>0?1000:10000);QObject::connect(&timer,&QTimer::timeout,&context,pulse);
        QTimer render;
        render.setInterval(100);
        QObject::connect(&render,&QTimer::timeout,&context,[canvas]{
            // Exercise actual paints when the validation window is not exposed.
            canvas->presentationImage();
            canvas->grab();
        });
        if(ready){
            progress({{"phase","measuring"},{"scheme",scheme},{"seconds",seconds},{"generation",generation}});
            timer.start();
            render.start();
            waitFor([&]{return invalid||s->userStopped();},seconds*1000);
            render.stop();timer.stop();pulse();
        }
        measuring=false;const double elapsed=clock.nsecsElapsed()/1e9;
        const auto snapshot=window.depth()->snapshot();
        const bool geometry=canvas->presentationSize()==QSizeF(width,height)
            &&snapshot.depthImage.size()==QSize(width,height)&&snapshot.cameraImage.size()==QSize(width,height)
            &&snapshot.metadata["depth_K"].toArray()==RvCameraView::intrinsics(profile);
        auto *videoCanvas=window.findChild<QWidget*>("rtspCameraLatestFrame");
        const bool cameraGeometry=depthOnly?(videoCanvas&&videoCanvas->property("presentationSize").toSizeF().isEmpty()):(videoCanvas&&videoCanvas->property("presentationSize").toSizeF()==QSizeF(2304,1296));
        auto *cameraDock=window.findChild<QDockWidget*>("rtspCameraDock");
        const bool cameraVisibility=cameraDock&&(depthOnly?cameraDock->isHidden():!cameraDock->isHidden());
        if(!hostOnly&&!thermalOnly){if(window.grab().save(path+QString(".stage_%1.png").arg(stage)))report["images_saved"]=true;else fail("Window screenshot save failed");}
        QJsonObject diagnostics;
        if(!diagnosticDirectory.isEmpty()){
            QFile receipt(QDir(sequenceFolder).filePath("recording.json"));
            const auto recorded=receipt.open(QIODevice::ReadOnly)?QJsonDocument::fromJson(receipt.readAll()).object():QJsonObject{};
            const bool recordedOk=recorded["complete"]==true&&recorded["error"]==""&&recorded["frames"].toInt()>=10
                &&recorded["dropped_frames"]==0&&QFileInfo(QDir(sequenceFolder).filePath("frames.bin")).size()==recorded["bytes"].toInteger(-1);
            const QDir output(diagnosticDirectory);
            QString pairedFolder,heldFolder;
            const bool pairedReplay=pairedDiagnostic.save(output.absolutePath(),&pairedFolder)
                &&RvDepthDiagnostic::replay(pairedFolder,output.filePath(experiment+"-paired-replay.json"))==0;
            const bool heldSeen=!heldDiagnostic.output.metresFloat32LE.isEmpty();
            const bool heldReplay=heldSeen&&heldDiagnostic.save(output.absolutePath(),&heldFolder)
                &&RvDepthDiagnostic::replay(heldFolder,output.filePath(experiment+"-held-replay.json"))==0;
            diagnostics={{"overlay",overlayOk},{"rgb_alignment",alignmentOk},{"recording_passed",recordedOk},
                {"sequence",sequenceFolder},{"recorded_frames",recorded["frames"]},{"dropped_frames",recorded["dropped_frames"]},
                {"paired_replay",pairedReplay},{"held_frame_seen",heldSeen},{"held_replay",heldReplay}};
            if(!overlayOk||!alignmentOk||!recordedOk||!pairedReplay||(heldSeen&&!heldReplay)){
                if(error.isEmpty())error="Diagnostic overlay, recording or replay did not pass";
                invalid=true;
            }
        }
        const bool pass=cameraVisibility&&ready&&!invalid&&!s->userStopped()&&elapsed>=seconds-.5&&frames-initial>=10&&positiveFrames>=10&&painted>=10&&(depthOnly?videos==0:videos>=30)&&native>=10&&(onHost?photon==0:photon>=10)&&geometry&&cameraGeometry&&generation!=previousGeneration&&(!onHost||(paired>=10&&corrected>=10));
        if(!pass&&error.isEmpty())error=QString("Incomplete measurement: ready=%1 positive=%2 painted=%3 video=%4 native=%5 photon=%6 geometry=%7").arg(ready).arg(positiveFrames).arg(painted).arg(videos).arg(native).arg(photon).arg(geometry);
        QJsonObject result{{"scheme",scheme},{"mode",mode},{"passed",pass},{"error",error},{"seconds",elapsed},{"depth_frames",frames-initial},{"positive_depth_frames",positiveFrames},{"painted_frames",painted},{"video_frames",videos},{"native_frames",native},{"photon_frames",photon},{"depth_fps",(frames-initial)/std::max(.001,elapsed)},{"painted_fps",painted/std::max(.001,elapsed)},{"video_fps",videos/std::max(.001,elapsed)},{"npu",stats(npu)},{"contour",stats(contour)},{"display_geometry_preserved",geometry},{"generation",generation},{"model_sha256",last["model_sha256"]},{"input_rotation_degrees",last["input_rotation_degrees"]}};
        if(depthOnly)result["tof_distance_mode"]=last["tof_distance_mode"];
        result["camera_dock_policy"]=cameraVisibility;result["camera_disabled"]=depthOnly;
        result["imaging_switch"]=s->imagingSwitchDiagnostics();
        if(onHost){result["inference"]=stats(inference);result["host_total"]=stats(total);result["paired_frames"]=paired;result["reused_native_frames"]=reused;result["dtof_corrected_frames"]=corrected;result["unpaired_frames"]=unpaired;result["historical_scale_frames"]=historical;result["maximum_pair_delta_ms"]=maximumPairDelta;result["backend"]=last["assist_backend"];result["host_model"]=last["assist_host_model"];result["graph_sha256"]=last["assist_graph_sha256"];result["tof_calibration"]=last["tof_calibration"];result["camera_mount_rotation_degrees"]=last["camera_mount_rotation_degrees"];result["input_rotation_degrees"]=last["assist_input_rotation_degrees"];}
        QJsonObject timing;for(auto it=performance.begin();it!=performance.end();++it)timing[it.key()]=stats(it.value());
        result["performance"]=timing;
        if(!diagnostics.isEmpty())result["diagnostics"]=diagnostics;
        result["board_compute"]=stats(compute);result["source_to_publish"]=stats(sourcePublish);result["info10_outputs"]=info10;result["windows"]=windows;
        result["board_publish_fps"]=(lastPublish>firstPublish)?1000.*(lastCount-firstCount)/(lastPublish-firstPublish):0.;
        runs.append(result);report["runs"]=runs;save(path,report);progress({{"phase","mode_finished"},{"passed",pass},{"scheme",scheme}});
        previousGeneration=generation;++stage;if(!pass){success=false;break;}
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
    const QString h=QLatin1String(RvSessionController::HostAssistImagingScheme);
    const QString ordinary=QLatin1String(RvSessionController::Native48ExtendedIntegrationScheme);
    const QString high=QLatin1String(RvSessionController::Native48HighAccuracy1200KScheme);
    const QStringList schemes{h,ordinary,high,ordinary,high,h,high,ordinary,h};
    QJsonObject report{{"passed",false},{"calibration_release",RvCalibration::Release},{"rendering","offscreen_widget_grab"}};
    QJsonArray stages;bool all=true;
    for(const auto &scheme:schemes){
        const auto previousPresentation=session->presentationEpoch();
        QObject scope;int frames=0,painted=0,videos=0,rawFrames=0;bool invalid=false,measuring=false;
        QByteArray rawDepth;QJsonObject last,rawMeta;QString generation;double maxDelta=0;
        auto *canvas=scheme==h?host:native;
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
            invalid|=bytes.size()!=(scheme==h?640*480*4:48*32*4);
            if(scheme!=h){
                invalid|=session->assistDepthEnabled()||bytes!=rawDepth||m["tof_sequence"]!=rawMeta["tof_sequence"]
                    ||m["depth_unit"]!="sensor_radial_m_float32"||m["native_order"]!=true
                    ||m["sensor_mode"]!=(scheme==high?12:11)||m["imaging_scheme"]!=scheme;
                const auto sensor=rawMeta["tof_sensor_readback"].toObject();
                if(sensor.contains("main_iterations_k"))
                    invalid|=sensor["main_iterations_k"]!=RvCaptureContract::Native48ExtendedIntegrationK;
            }else{
                invalid|=!RvCameraView::validOutputGrid(m);
                if(m["dtof_present"]==true){
                    const double delta=std::abs(m["pair_delta_ms"].toDouble());
                    maxDelta=std::max(maxDelta,delta);
                    invalid|=delta+m["pair_clock_uncertainty_ms"].toDouble(35)>RvTofFrame::FreshRgbPairBudgetMs;
                }
            }
        };
        if(scheme==h)QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&scope,
            [&](const QByteArray &b,const QJsonObject &m,const QImage &,const RvTofFrame &,const QByteArray &){accept(b,m);});
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
            &&session->presentationEpoch()>previousPresentation&&snap.metresFloat32LE.size()==(scheme==h?640*480*4:48*32*4);
        QJsonObject result{{"scheme",scheme},{"passed",valid},{"ready",ready},{"invalid_data",invalid},{"frames",frames},
            {"painted",painted},{"raw_frames",rawFrames},{"video_frames",videos},{"seconds",clock.elapsed()/1000.},
            {"width",last["width"]},{"height",last["height"]},{"depth_unit",last["depth_unit"]},{"generation",generation},
            {"sensor_mode",last["sensor_mode"]},{"sensor_readback",rawMeta["tof_sensor_readback"]},
            {"maximum_pair_delta_ms",maxDelta},{"state",session->stateText()},
            {"detail",session->detailText()},{"switch",session->imagingSwitchDiagnostics()}};
        stages.append(result);report["stages"]=stages;save(path,report);
        if(!valid){all=false;break;}
        canvas->grab().save(path+"."+scheme+".png");
    }
    host->performanceFramePainted={};native->performanceFramePainted={};
    QObject stopScope;int late=0;
    QObject::connect(session,&RvSessionController::depthFrameReady,&stopScope,[&]{++late;});
    QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&stopScope,[&]{++late;});
    session->stop();const bool stopped=waitFor([&]{return !session->busy();},35000);waitFor([]{return false;},1500);
    report["normal_stop"]=stopped;report["late_depth_results"]=late;report["passed"]=all&&stages.size()==schemes.size()&&stopped&&late==0;
    save(path,report);window.close();return report["passed"].toBool()?0:2;
}

int runPointCloudLiveValidation(SensorMainWindow &window, const QString &path)
{
    auto *session = window.session();
    if (qEnvironmentVariable("DCTS_LIVE_ENABLE") != "1"
        || qEnvironmentVariable("DCTS_ALLOW_SAVED_TOF_SWITCH") != "1"
        || qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1"
        || session->deviceProfile().serial != qEnvironmentVariable("DCTS_LIVE_SERIAL")) {
        save(path, {{"passed", false}, {"detail", "The live device does not match the session's registered profile"}});
        return 3;
    }

    auto *dock = window.findChild<QDockWidget *>("pointCloudDock");
    auto *view = window.findChild<RvPointCloudCanvas *>("rvPointCloudView");
    auto *instances = window.findChild<SceneInstanceBuffer *>("rvPointCloudInstances");
    auto *depthInstances = window.findChild<SceneInstanceBuffer *>("rvPointCloudDepthInstances");
    auto *diameter = window.findChild<QDoubleSpinBox *>("rvPointCloudPointDiameter");
    auto *colorMinimum = window.findChild<QDoubleSpinBox *>("rvPointCloudColorMinimum");
    auto *colorMaximum = window.findChild<QDoubleSpinBox *>("rvPointCloudColorMaximum");
    auto *applyRange = window.findChild<QPushButton *>("rvPointCloudColorApply");
    auto *menu = window.findChild<QComboBox *>("rvDepthModel");
    auto *start = window.findChild<QPushButton *>("rvDepthStart");
    auto *stop = window.findChild<QPushButton *>("rtspCameraStop");
    if (!dock || !view || !instances || !depthInstances || !diameter || !colorMinimum || !colorMaximum
        || !applyRange || !menu || !start || !stop) {
        save(path, {{"passed", false}, {"detail", "The required point-cloud or shared capture controls are missing"}});
        return 3;
    }
    dock->show();
    dock->raise();
    if (!waitFor([&] { return view->isVisible(); }, 3000)) {
        save(path, {{"passed", false}, {"detail", "Point cloud widget did not become visible"}});
        return 2;
    }

    QJsonArray stages;
    QJsonObject report{{"passed", false}};
    QString previousGeneration;
    int previousMode = -1;
    bool all = true;
    const QString hostScheme = QLatin1String(RvSessionController::HostAssistImagingScheme);
    const auto cloudCleared = [&] { return instances->count() == 0 && depthInstances->count() == 0; };
    const auto cameraDistance = [&] { return view->property("cameraDistance").toDouble(); };
    const auto coloredPixels = [](const QImage &image) {
        // The unlit points are saturated; the neutral grid and muted axes are below this threshold.
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const auto rgb = line[x];
                count += std::max({qRed(rgb), qGreen(rgb), qBlue(rgb)})
                    - std::min({qRed(rgb), qGreen(rgb), qBlue(rgb)}) > 125;
            }
        }
        return count;
    };
    double manualDistance = cameraDistance();
    const QString ordinary=QLatin1String(RvSessionController::Native48ExtendedIntegrationScheme);
    const QString high=QLatin1String(RvSessionController::Native48HighAccuracy1200KScheme);
    const bool expectFallback=qEnvironmentVariable("DCTS_LIVE_EXPECT_BOARD_FALLBACK")=="1";
    const QStringList schemes=expectFallback ? QStringList{ordinary, high} : QStringList{hostScheme, ordinary, high};
    const auto hostIndex=menu->model()->index(menu->findData(hostScheme),0);
    const auto hostDisabled=[&]{return hostIndex.isValid()&&!hostIndex.flags().testFlag(Qt::ItemIsEnabled);};
    if(expectFallback){
        report["initial_scheme"]=session->currentImagingScheme();
        report["initial_host_disabled"]=hostDisabled();
        const bool initiallyUnavailable=!session->hostInferencePreflightError().isEmpty();
        all=hostIndex.isValid()&&session->currentImagingScheme()==(initiallyUnavailable?ordinary:hostScheme)
            &&hostDisabled()==initiallyUnavailable;
    }
    for (const auto &scheme : schemes) {
        if (!all) break;
        QObject scope;
        const bool assisted = scheme == hostScheme;
        const bool startWithAutomaticFallback=expectFallback&&stages.isEmpty();
        const int expectedMode = scheme == high ? RvCaptureContract::Native48HighAccuracy : RvCaptureContract::Native48;
        const auto previousPresentation = session->presentationEpoch();
        int frames = 0, videoFrames = 0, nativeDepthFrames = 0, peakPoints = 0;
        qint64 lastSequence = 0;
        QString generation;
        bool invalid = false;
        QObject::connect(session, &RvSessionController::cameraFrameReady, &scope, [&] { ++videoFrames; });
        const auto observeFrame = [&](const QString &token, qint64 sequence) {
            if (generation.isEmpty())
                generation = token;
            invalid |= token != generation || sequence <= lastSequence;
            lastSequence = sequence;
            ++frames;
        };
        if (assisted) {
            QObject::connect(session, &RvSessionController::assistDepthSnapshotReady, &scope,
                [&](const QByteArray &depth, const QJsonObject &metadata) {
                    const auto &profile = RvCameraView::outputProfile(session->assistHostModel());
                    invalid |= !RvCameraView::validOutputGrid(metadata)
                        || metadata["depth_unit"] != "camera-Z_m_float32"
                        || metadata["depth_K"].toArray() != RvCameraView::intrinsics(profile)
                        || depth.size() != profile.depthBytes() || instances->count() != 0;
                    observeFrame(metadata["source_generation"].toString(), metadata["frame_id"].toInteger());
                });
        } else {
            QObject::connect(session, &RvSessionController::tofFrameReady, &scope, [&](const RvTofFrame &frame) {
                if (session->currentImagingScheme()!=scheme||frame.metadata["candidate_mode"] != expectedMode)
                    return;
                invalid |= frame.columns() != 48 || frame.rows() != 32;
                int valid = 0;
                for (int index = 0; index < frame.sampleCount(); ++index)
                    valid += frame.valid(index) && frame.rangeUq2(index) > 0;
                invalid |= instances->count() != valid || depthInstances->count() != 0;
                peakPoints = std::max(peakPoints, valid);
                observeFrame(frame.metadata["generation"].toString(), frame.metadata["tof_sequence"].toInteger());
            });
            QObject::connect(session,&RvSessionController::depthSnapshotReady,&scope,
                [&](const QByteArray &depth,const QJsonObject &metadata){
                    if(metadata["imaging_scheme"]!=scheme)return;
                    invalid|=depth.size()!=48*32*4||metadata["width"]!=48||metadata["height"]!=32;
                    ++nativeDepthFrames;
                });
        }

        const int index = menu->findData(scheme);
        if (index < 0) {
            all = false;
            break;
        }
        if (!startWithAutomaticFallback) {
            menu->setCurrentIndex(index);
            QMetaObject::invokeMethod(menu, "activated", Qt::DirectConnection, Q_ARG(int, index));
        }
        const bool clearedOnSelection = cloudCleared();
        if (session->userStopped())
            start->click();
        const bool ready = waitFor([&] {
            if (assisted)
                peakPoints = std::max(peakPoints, depthInstances->count());
            return session->userStopped() || (frames >= 18 && videoFrames >= 18
                && (assisted ? depthInstances->count() >= 1536 : nativeDepthFrames >= 18 && instances->count() > 0)
                && !session->busy() && !session->imagingSwitchActive());
        }, assisted ? 95000 : 60000) && !session->userStopped() && session->currentImagingScheme() == scheme;
        if (assisted)
            invalid |= peakPoints > 640 * 480;
        QJsonObject display;
        int rendered = 0;
        if (ready) {
            const auto screenshot = view->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
            rendered = coloredPixels(screenshot);
            if (assisted) {
                const bool savedImage = screenshot.save(path + ".assisted.png");
                const double originalDiameter = diameter->value();
                const double originalMinimum = colorMinimum->value();
                const double originalMaximum = colorMaximum->value();
                const int pointCount = depthInstances->count();
                diameter->setValue(1.0);
                const int smallCoverage = coloredPixels(view->grabFramebuffer().convertToFormat(QImage::Format_RGB32));
                const bool sameCount = pointCount == depthInstances->count();
                diameter->setValue(originalDiameter);
                const bool diameterChanged = diameter->isVisible() && sameCount && smallCoverage < rendered;

                colorMinimum->setValue(.1);
                colorMaximum->setValue(1.8);
                applyRange->click();
                const bool rangeApplied = waitFor([&] {
                    if (applyRange->isEnabled() || depthInstances->count() < 4)
                        return false;
                    int changedColors = 0;
                    for (int n = 1; n <= 3; ++n) {
                        const int index = depthInstances->count() * n / 4;
                        const float z = -depthInstances->instancePosition(index).z() / 1000.f;
                        const QRgb actual = depthInstances->instanceColor(index).rgb();
                        const QRgb expected = PointCloudColorScale::color(z, .1, 1.8);
                        const auto close = [](int a, int b) { return std::abs(a - b) <= 1; };
                        if (!close(qRed(actual), qRed(expected)) || !close(qGreen(actual), qGreen(expected))
                            || !close(qBlue(actual), qBlue(expected)))
                            return false;
                        changedColors += expected != PointCloudColorScale::color(z, originalMinimum, originalMaximum);
                    }
                    return changedColors > 0;
                }, 2000);
                colorMinimum->setValue(originalMinimum);
                colorMaximum->setValue(originalMaximum);
                applyRange->click();

                const QPoint center = view->rect().center();
                QWheelEvent wheel(center, view->mapToGlobal(center), {}, QPoint(0, 120),
                                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(view, &wheel);
                const double zoomed = cameraDistance();
                const bool wheelZoomed = zoomed < manualDistance;
                const int nextFrame = frames + 3;
                const bool viewHeld = waitFor([&] { return frames >= nextFrame; }, 5000)
                    && cameraDistance() == zoomed;
                manualDistance = zoomed;
                display = {{"point_diameter_px", originalDiameter}, {"color_minimum_m", originalMinimum},
                    {"color_maximum_m", originalMaximum}, {"colored_pixels", rendered}, {"colored_pixels_at_1px", smallCoverage},
                    {"diameter_changes_screen_coverage", diameterChanged}, {"color_range_applied", rangeApplied},
                    {"manual_zoom", wheelZoomed}, {"zoom_preserved_on_new_frames", viewHeld}, {"framebuffer_saved", savedImage}};
                invalid |= !diameterChanged || !rangeApplied || !wheelZoomed || !viewHeld || !savedImage;
            } else {
                const bool viewHeld = cameraDistance() == manualDistance;
                display["rendered_in_preserved_view"] = rendered > 0;
                if (!rendered && instances->count() > 0) {
                    // A preserved zoom can put a different depth surface outside the viewport.
                    // Center on a live point to exercise rendering, then restore the view.
                    const auto target = view->property("cameraTarget");
                    view->setProperty("cameraTarget", instances->instancePosition(instances->count() / 2));
                    rendered = coloredPixels(view->grabFramebuffer().convertToFormat(QImage::Format_RGB32));
                    view->setProperty("cameraTarget", target);
                }
                display["colored_pixels"] = rendered;
                display["zoom_preserved_on_switch"] = viewHeld;
                invalid |= !viewHeld || diameter->isVisible();
            }
        }
        // Assisted and ordinary display both use the SDK's normal ToF capture.
        const bool reuseSdkCapture = previousMode == expectedMode
            && session->property("rvTransport") == "cxwl_depth_sdk";
        const bool sourceGenerationValid = !generation.isEmpty()
            && (reuseSdkCapture ? generation == previousGeneration : generation != previousGeneration);
        const bool freshPresentation = session->presentationEpoch() > previousPresentation;
        const bool passed = ready && clearedOnSelection && !invalid && rendered
            && sourceGenerationValid && freshPresentation && (!expectFallback || hostDisabled());
        if (startWithAutomaticFallback) report["board_fallback_started_without_menu_selection"]=passed;
        stages.append(QJsonObject{{"scheme", scheme}, {"passed", passed}, {"ready", ready},
            {"cleared_on_selection", clearedOnSelection},
            {"source_generation_valid", sourceGenerationValid}, {"sdk_capture_reused", reuseSdkCapture && sourceGenerationValid},
            {"presentation_epoch_advanced", freshPresentation},
            {assisted ? "calibrated_depth_frames_valid" : "same_frame_points_match", !invalid && frames > 0},
            {"rendered_cloud", rendered > 0}, {"display", display},
            {"native_depth_frames", nativeDepthFrames},
            {assisted ? "assisted_frames" : "raw_frames", frames}, {"video_frames", videoFrames},
            {"maximum_points", peakPoints}, {"state", session->stateText()}, {"detail", session->detailText()}});
        report["stages"] = stages;
        save(path, report);
        if (!passed) {
            all = false;
            break;
        }
        previousGeneration = generation;
        previousMode = expectedMode;
    }
    QObject stopScope;
    int late = 0;
    QObject::connect(session, &RvSessionController::tofFrameReady, &stopScope, [&] { ++late; });
    QObject::connect(session, &RvSessionController::assistDepthSnapshotReady, &stopScope, [&] { ++late; });
    stop->click();
    const bool clearedOnStop = cloudCleared();
    const bool stopped = waitFor([&] { return session->userStopped() && !session->busy(); }, 35000);
    waitFor([] { return false; }, 1200);
    report["normal_stop"] = stopped;
    report["cleared_on_stop"] = clearedOnStop && cloudCleared();
    report["late_frames"] = late;
    report["passed"] = all && stages.size() == schemes.size() && stopped && report["cleared_on_stop"].toBool() && late == 0;
    const bool saved = save(path, report);
    window.close();
    return saved && report["passed"].toBool() ? 0 : 2;
}
