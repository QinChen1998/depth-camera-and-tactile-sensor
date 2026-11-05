#include "../SensorMainWindow.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvTunerController.h"
#include <QApplication>
#include <QComboBox>
#include <QtEndian>
#include <cstring>
#include <QDateTime>
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
int runLiveValidation(SensorMainWindow &window,const QString &path,int seconds){
    auto *session=window.session();auto *tuner=session->tuner();
    QJsonObject report{{"passed",false},{"requested_seconds_per_mode",seconds},{"images_saved",false},{"measurement_arrays_saved",false},
        {"device_serial",session->deviceProfile().serial},{"device_host",session->deviceProfile().host},{"calibration",session->deviceProfile().calibration},
        {"camera_mount_rotation_degrees",session->cameraMountRotationDegrees()},{"model_input_rotation_degrees",session->assistInputRotationDegrees()}};
    if(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||qEnvironmentVariable("DCTS_ALLOW_SAVED_TOF_SWITCH")!="1"
        ||session->deviceProfile().serial!=qEnvironmentVariable("DCTS_LIVE_SERIAL")||seconds!=300){
        report["error"]="Explicit live/parameter-switch authorization and exact device identity required";save(path,report);return 3;
    }
    QFile journal(path+".progress.jsonl");if(!journal.open(QIODevice::WriteOnly|QIODevice::Truncate))return 3;
    const auto progress=[&](QJsonObject value){value["unix_ms"]=QDateTime::currentMSecsSinceEpoch();journal.write(QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n');journal.flush();};
    const auto inspect=[&](QJsonObject &value){
        tuner->read();if(!waitFor([&]{return !tuner->pending();},95000)||tuner->state()!=RvTunerController::Idle)return false;
        value=tuner->inspection();return !value.isEmpty();
    };
    QJsonObject before,after;QString error;
    if(!inspect(before)||before["owner"]!="idle"){
        report["error"]="Read-only idle-device preflight: "+tuner->detail();save(path,report);return 2;
    }
    const auto original=RvTuner::Settings::fromJson(before["settings"].toObject());
    if(!original.liteSrCompatible()||before["litesr_settings"]!=before["settings"]){
        report["error"]="Expected matching saved M48/Signal settings for first LiteSR run";report["before"]=before;save(path,report);return 2;
    }
    bool changed=false,restored=false;
    const auto persistSettings=[&](const RvTuner::Settings &settings){
        progress({{"phase","sensor_configuration"},{"mode",settings.mode}});
        tuner->start(settings);
        if(!waitFor([&]{return !tuner->pending()&&(tuner->state()==RvTunerController::Running||tuner->state()==RvTunerController::Fault);},95000)
            ||tuner->state()!=RvTunerController::Running)return false;
        tuner->persist();
        const bool saved=waitFor([&]{return !tuner->pending();},95000)&&tuner->state()==RvTunerController::Running&&tuner->savedSettings()==settings.json();
        tuner->stop();
        const bool stopped=waitFor([&]{return !tuner->shutdownPending();},95000);
        QJsonObject verify;return saved&&stopped&&inspect(verify)&&verify["owner"]=="idle"
            &&verify["settings"]==settings.json()&&verify["litesr_settings"]==settings.json();
    };
    const auto restore=[&]{
        if(!changed)return true;
        session->stop();waitFor([]{return false;},1500);
        if(!persistSettings(original))return false;
        restored=true;changed=false;return true;
    };
    auto restoration=qScopeGuard([&]{if(changed)restore();});
    QJsonArray runs;
    const auto run=[&](const QString &scheme,bool host){
        QJsonObject result{{"scheme",scheme},{"passed",false},{"requested_seconds",seconds}};
        progress({{"phase","starting"},{"scheme",scheme}});
        session->selectImagingScheme(scheme,session->dtofCandidateSettings());
        if((host&&(session->assistHostModel()!="unidepth_wide_576_fp16_tof16"||session->assistExperiment()!="H"))
            ||(!host&&session->dtofCandidateMode()!=13)){result["error"]="Mode selection rejected";runs.append(result);return false;}
        auto *canvas=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>(host?"rvAssistDepthCanvas":"rvDtofFastDepthCanvas"));
        if(!canvas){result["error"]="Actual GUI depth canvas missing";runs.append(result);return false;}
        QObject context;int videos=0,raw=0,depths=0,paints=0,reused=0,corrected=0,held=0,monocular=0,late=0;
        bool measuring=false,accepting=true,invalid=false;QString failure,lastIdentity; qint64 lastId=-1,lastPaint=-1,lastOutputMs=0;
        QElapsedTimer clock;QMap<QString,QVector<double>> timings;QJsonArray windows;QJsonObject lastMetadata;
        const auto fail=[&](const QString &why){if(invalid)return;invalid=true;failure=why;};
        QObject::connect(session,&RvSessionController::cameraFrameReady,&context,[&](const QImage &image){
            if(image.size()!=QSize(2304,1296))fail("Unexpected RGB dimensions");if(measuring)++videos;
        });
        QObject::connect(session,&RvSessionController::tofFrameReady,&context,[&](const RvTofFrame &frame){
            if(!frame.isValid()||frame.columns()!=(host?16:48)||frame.rows()!=(host?16:32))fail("Unexpected actual native ToF grid");if(measuring)++raw;
        });
        const auto received=[&](const QByteArray &bytes,const QJsonObject &m){
            if(!accepting){++late;return;}if(invalid)return;
            const auto identity=m["generation"].toVariant().toString()+"/"+m["source_generation"].toVariant().toString();
            const auto id=m["frame_id"].toInteger();
            if(identity==lastIdentity&&id<=lastId){fail("Duplicate or replayed depth output");return;}
            lastIdentity=identity;lastId=id;
            if(bytes.size()!=(host?576*324:256*256)*4){fail("Invalid depth array dimensions");return;}
            if(host){const auto &view=RvCameraView::profile(QStringLiteral("unidepth_wide_576_fp16_tof16"));
                if(m["assist_host_model"]!="unidepth_wide_576_fp16_tof16"||m["assist_experiment"]!="H"
                    ||m["assist_graph_sha256"]!=QLatin1String(view.graphSha)||m["assist_output_restored_to_calibrated_grid"]!=true
                    ||m["assist_input_orientation_checked"]!=true||m["assist_input_rotation_degrees"].toInt(-1)!=session->assistInputRotationDegrees()){
                    fail("FP16/H model identity or restored orientation mismatch");return;}
                if(m["dtof_present"]==true&&(m["dtof_width"]!=16||m["dtof_height"]!=16||std::abs(m["pair_delta_ms"].toDouble(1000))>100.)){
                    fail("Native16 pairing contract mismatch");return;}
            }else if(m["model_sha256"]!="a916354dce57bd4cf4559cde91270cbd50bb2b14302ed277e8653e02b525268b"
                ||m["candidate_mode"]!=13||m["host_contour_enabled"]!=true||m["rgb_used"]!=false){fail("Board LiteSR/host contour identity mismatch");return;}
            lastMetadata=m;++depths;if(!measuring)return;lastOutputMs=clock.elapsed();
            const auto fit=m["assist_fusion"].toObject();reused+=m["tof_reused"].toBool();corrected+=fit["dtof_corrected"].toBool();
            held+=fit["fusion_mode"]=="held_scale";monocular+=fit["fusion_mode"]=="monocular";
            for(const auto *key:{"assist_inference_ms","assist_total_ms","assist_frame_age_ms","assist_parent_validation_ms","npu_ms","board_compute_ms","host_contour_ms","candidate_age_ms","video_age_ms"})
                if(m[key].isDouble())timings[key]<<m[key].toDouble();
        };
        QObject::connect(session,&RvSessionController::assistDepthSnapshotReady,&context,[&](const QByteArray &b,const QJsonObject &m){if(host)received(b,m);});
        QObject::connect(session,&RvSessionController::dtofCandidateReady,&context,[&](const RvDtofCandidates::Result &r){if(!host&&r.fresh)received(r.depth,r.metadata);});
        canvas->performanceFramePainted=[&](const QJsonObject &m){
            if(!measuring)return;const auto id=m["frame_id"].toInteger();if(id==lastPaint)return;lastPaint=id;++paints;
            for(const auto *key:{"gui_color_ms","gui_paint_ms","source_to_paint_ms"})if(m[key].isDouble())timings[key]<<m[key].toDouble();
        };
        auto canvasGuard=qScopeGuard([&]{canvas->performanceFramePainted={};});
        session->start();
        const bool ready=waitFor([&]{return invalid||session->userStopped()||(depths>=3&&!session->busy());},75000)&&!invalid&&!session->userStopped()&&depths>=3;
        if(ready)waitFor([&]{return invalid||session->userStopped();},5000);
        const int firstDepth=depths;double previousSeconds=0;int previousDepth=depths,previousPaint=0,previousVideo=0,previousRaw=0;
        clock.start();measuring=ready&&!invalid&&!session->userStopped();
        const auto pulse=[&]{const double elapsed=clock.nsecsElapsed()/1e9,span=elapsed-previousSeconds;if(span<.1)return;
            QJsonObject value{{"phase","measuring"},{"scheme",scheme},{"seconds",elapsed},{"window_seconds",span},
                {"depth_array_fps",(depths-previousDepth)/span},{"painted_fps",(paints-previousPaint)/span},{"video_fps",(videos-previousVideo)/span},
                {"native_fps",(raw-previousRaw)/span},{"depth_arrays",depths-firstDepth},{"painted_frames",paints},{"reused_tof_outputs",reused},
                {"state",session->stateText()},{"detail",session->detailText()},{"depth_detail",session->depthInfoText()}};
            progress(value);windows.append(value);previousSeconds=elapsed;previousDepth=depths;previousPaint=paints;previousVideo=videos;previousRaw=raw;
        };
        QTimer timer;timer.setInterval(10000);QObject::connect(&timer,&QTimer::timeout,&context,pulse);
        if(measuring){timer.start();waitFor([&]{if(clock.elapsed()-lastOutputMs>10000)fail("No new valid depth for more than ten seconds");return invalid||session->userStopped();},seconds*1000);timer.stop();pulse();}
        measuring=false;const double duration=clock.nsecsElapsed()/1e9;const int outputs=depths-firstDepth;
        if(!ready&&failure.isEmpty())failure=session->detailText()+" / "+session->assistDepthInfoText();
        const auto video=session->videoDiagnostics(true);accepting=false;canvas->performanceFramePainted={};session->stop();waitFor([]{return false;},1500);
        QJsonObject stopped;bool idle=false;QElapsedTimer cleanup;cleanup.start();
        do{if(inspect(stopped)&&stopped["owner"]=="idle"){idle=true;break;}waitFor([]{return false;},500);}while(cleanup.elapsed()<20000);
        const bool pass=ready&&!invalid&&duration>=seconds-.5&&outputs>100&&paints>100&&videos>300&&raw>100&&late==0&&idle&&session->userStopped();
        result["passed"]=pass;result["ready"]=ready;result["error"]=failure.isEmpty()?(pass?QString():session->detailText()):failure;
        result["seconds"]=duration;result["depth_arrays"]=outputs;result["depth_array_fps"]=outputs/std::max(.001,duration);
        result["painted_frames"]=paints;result["painted_fps"]=paints/std::max(.001,duration);result["video_fps"]=videos/std::max(.001,duration);
        result["native_fps"]=raw/std::max(.001,duration);result["reused_tof_outputs"]=reused;result["corrected_outputs"]=corrected;
        result["held_scale_outputs"]=held;result["monocular_outputs"]=monocular;result["late_results_after_stop"]=late;
        result["normal_device_cleanup"]=idle;result["video_diagnostics"]=video;result["windows"]=windows;
        QJsonObject summary;for(auto it=timings.begin();it!=timings.end();++it)summary[it.key()]=stats(it.value());result["timing"]=summary;
        runs.append(result);report["runs"]=runs;save(path,report);progress({{"phase","mode_finished"},{"scheme",scheme},{"passed",pass}});return pass;
    };
    window.setWindowFlag(Qt::WindowStaysOnTopHint,true);window.show();
    const bool lite=run(QStringLiteral("board_litesr_near_full_host_contour_hd"),false);
    bool host=false;
    if(lite){auto native16=original;native16.mode=5;native16.noise=false;native16.objects=1;
        changed=true;if(persistSettings(native16))host=run(QStringLiteral("host_unidepth_wide_576_fp16_tof16"),true);
        else error="Saving and verifying authorized Native16 settings failed: "+tuner->detail();
    }
    const bool restoreOK=restore();restoration.dismiss();
    const bool readback=inspect(after);
    const bool unchanged=readback&&before["settings"]==after["settings"]&&before["litesr_settings"]==after["litesr_settings"]
        &&before["service_sha"]==after["service_sha"]&&before["litesr_sha"]==after["litesr_sha"];
    report["before"]=before;report["after"]=after;report["runs"]=runs;report["error"]=error;
    report["saved_sensor_files_restored_identically"]=unchanged;report["restore_succeeded"]=restoreOK;
    report["host_stopped"]=session->userStopped();report["final_owner_idle"]=readback&&after["owner"]=="idle";
    report["passed"]=lite&&host&&restoreOK&&unchanged&&report["final_owner_idle"].toBool();
    save(path,report);progress({{"phase","complete"},{"passed",report["passed"]}});window.close();return report["passed"].toBool()?0:2;
}

// Explicit real-device integration test. Only scalar reports are saved.
// Uses ordinary session selection/start/switch/stop, never tuner.persist().
static int runBoardLiteSrLiveValidation(SensorMainWindow &window,const QString &path,bool native16,bool vgaSwitch=false){
    auto *s=window.session();auto *t=s->tuner();
    QJsonObject report{{"passed",false},{"images_saved",false},{"measurement_arrays_saved",false},{"saved_sensor_settings_written",false}};
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
    const QString rect="board_litesr_near_384x256_host_contour_hd",old="board_litesr_near_full_host_contour_hd",unidepth="host_unidepth_wide_576_fp16_tof16";
    const QString small="board_litesr_near_128x128_depth_only";
    const QString vga="host_unidepth_rgb672_vga_fp16_tof16";
    const QStringList sequence=vgaSwitch?QStringList{vga,small,vga}:native16?QStringList{small,rect,old,unidepth,small}:QStringList{rect,old,unidepth,rect};
    int stage=0;
    for(const auto &scheme:sequence){
        const bool onHost=scheme==unidepth||scheme==vga,smallMode=scheme==small;const int mode=onHost?14:smallMode?17:scheme==rect?16:13;
        const auto hostModel=onHost?scheme.mid(5):QString();
        const auto &profile=RvCameraView::profile(hostModel);
        const int width=onHost?profile.outputWidth:smallMode?128:mode==16?384:256,height=onHost?profile.outputHeight:smallMode?128:256;
        auto *canvas=onHost?host:board;QObject context;
        int frames=0,positiveFrames=0,painted=0,videos=0,native=0,photon=0;bool invalid=false,measuring=false;
        QString error,generation; qint64 lastId=-1,lastPaint=-1;QJsonObject last;QVector<double> npu,contour,compute,sourcePublish;QJsonArray windows;
        int info10=0;double firstPublish=-1,lastPublish=-1,firstCount=0,lastCount=0;
        const auto fail=[&](const QString &why){invalid=true;error=why;};
        const auto accept=[&](const QByteArray &bytes,const QJsonObject &m){
            if(s->dtofCandidateMode()!=mode||(!onHost&&m["candidate_mode"]!=mode))return;
            const auto g=m["generation"].toVariant().toString();const auto id=m["frame_id"].toInteger();
            if(!generation.isEmpty()&&generation==g&&id<=lastId){fail("Repeated depth frame");return;}
            generation=g;lastId=id;
            if(bytes.size()!=width*height*4||m["width"]!=width||m["height"]!=height){fail("Wrong output shape");return;}
            if(!onHost&&(m["model_sha256"]!=LiteSrBoardContract::modelSha(mode)||m["host_contour_enabled"].toBool()!=!smallMode||m["rgb_used"]!=false||m["input_rotation_degrees"]!=0||m["depth_unit"]!="sensor_radial_m_float32")){fail("Board identity, units or contour contract changed");return;}
            if(onHost&&(m["assist_host_model"]!=hostModel||m["assist_experiment"]!="H"||m["assist_output_restored_to_calibrated_grid"]!=true)){fail("Legacy host inference contract changed");return;}
            if(smallMode&&(m["video_transport"]!="none"||m["video_fps"]!=0||m["rgb_paused"]!=true||m["photon_data"]!=true||m["input_columns"]!=16||m["input_rows"]!=16
                ||m["photon_sequence"].toString().toLongLong()!=id||m["photon_timestamp_ns"].toString().toLongLong()!=m["tof_timestamp_ns"].toInteger())){fail("Native16 same-frame/camera-off contract mismatch");return;}
            int positive=0;
            for(int i=0;i<width*height;++i){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);float z;std::memcpy(&z,&bits,4);if(!std::isfinite(z)||z<0||(!onHost&&z>1.4f)){fail("Invalid metric value");return;}positive+=z>0;}
            if(positive)++positiveFrames;
            last=m;++frames;
            if(measuring){
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
        QObject::connect(s,&RvSessionController::cameraFrameReady,&context,[&](const QImage &im){if(measuring){if(smallMode||im.size()!=QSize(2304,1296))fail("Unexpected RGB stream");++videos;}});
        QObject::connect(s,&RvSessionController::tofFrameReady,&context,[&](const RvTofFrame &f){if(measuring){if(f.columns()!=((onHost||smallMode)?16:48)||f.rows()!=((onHost||smallMode)?16:32))fail("Wrong native grid");++native;}});
        QObject::connect(s,&RvSessionController::photonFrameReady,&context,[&](const RvPhotonFrame &){if(measuring)++photon;});
        canvas->performanceFramePainted=[&](const QJsonObject &m){if(measuring&&m["frame_id"].toInteger()!=lastPaint){lastPaint=m["frame_id"].toInteger();++painted;}};
        auto observer=qScopeGuard([&]{canvas->performanceFramePainted={};});
        progress({{"phase","selecting"},{"scheme",scheme}});
        const int index=menu->findData(scheme); menu->setCurrentIndex(index); QMetaObject::invokeMethod(menu,"activated",Qt::DirectConnection,Q_ARG(int,index));
        if(stage==0)s->start();
        const bool ready=waitFor([&]{return invalid||s->userStopped()||(frames>=3&&!s->busy());},95000)&&!invalid&&!s->userStopped()&&frames>=3;
        if(!ready&&error.isEmpty())error=s->detailText()+" / "+s->depthInfoText();
        const int initial=frames;QElapsedTimer clock;clock.start();measuring=ready;
        const int seconds=vgaSwitch?30:stage==0?(native16?300:60):30;
        int previousFrames=initial,previousPainted=0;double previousSeconds=0;
        const auto pulse=[&]{const double elapsed=clock.nsecsElapsed()/1e9,span=elapsed-previousSeconds;if(span<.1)return;
            QJsonObject item{{"phase","window"},{"scheme",scheme},{"elapsed_seconds",elapsed},{"window_seconds",span},{"depth_fps",(frames-previousFrames)/span},{"painted_fps",(painted-previousPainted)/span},{"info10_outputs",info10},{"last_frame_id",lastId}};
            windows.append(item);progress(item);if(frames==previousFrames)fail("No fresh output in measurement window");previousFrames=frames;previousPainted=painted;previousSeconds=elapsed;
        };
        QTimer timer;timer.setInterval(10000);QObject::connect(&timer,&QTimer::timeout,&context,pulse);
        if(ready){progress({{"phase","measuring"},{"scheme",scheme},{"seconds",seconds},{"generation",generation}});timer.start();waitFor([&]{return invalid||s->userStopped();},seconds*1000);timer.stop();pulse();}
        measuring=false;const double elapsed=clock.nsecsElapsed()/1e9;
        const bool geometry=onHost||(canvas->property("depthWidth").toInt()==width&&canvas->property("depthHeight").toInt()==height&&canvas->presentationSize()==(smallMode?QSizeF(16,16):QSizeF(32,48)));
        const bool pass=ready&&!invalid&&!s->userStopped()&&elapsed>=seconds-.5&&frames-initial>=10&&positiveFrames>=10&&painted>=10&&(smallMode?videos==0:videos>=30)&&native>=10&&(onHost?photon==0:photon>=10)&&geometry&&generation!=previousGeneration;
        QJsonObject result{{"scheme",scheme},{"mode",mode},{"passed",pass},{"error",error},{"seconds",elapsed},{"depth_frames",frames-initial},{"positive_depth_frames",positiveFrames},{"painted_frames",painted},{"video_frames",videos},{"native_frames",native},{"photon_frames",photon},{"depth_fps",(frames-initial)/std::max(.001,elapsed)},{"painted_fps",painted/std::max(.001,elapsed)},{"video_fps",videos/std::max(.001,elapsed)},{"npu",stats(npu)},{"contour",stats(contour)},{"display_geometry_preserved",geometry},{"generation",generation},{"model_sha256",last["model_sha256"]},{"input_rotation_degrees",last["input_rotation_degrees"]}};
        result["board_compute"]=stats(compute);result["source_to_publish"]=stats(sourcePublish);result["info10_outputs"]=info10;result["windows"]=windows;
        result["board_publish_fps"]=(lastPublish>firstPublish)?1000.*(lastCount-firstCount)/(lastPublish-firstPublish):0.;
        runs.append(result);report["runs"]=runs;save(path,report);progress({{"phase","mode_finished"},{"passed",pass},{"scheme",scheme}});
        previousGeneration=generation;++stage;if(!pass){success=false;break;}
    }
    board->performanceFramePainted={};host->performanceFramePainted={};
    int late=0;QObject stopObserver;
    QObject::connect(s,&RvSessionController::dtofCandidateReady,&stopObserver,[&](const RvDtofCandidates::Result &r){if(r.fresh)++late;});
    s->stop();waitFor([&]{return !s->busy();},30000);waitFor([]{return false;},1500);
    bool idle=false;QElapsedTimer stopClock;stopClock.start(); do { idle=inspect(after);if(!idle)waitFor([]{return false;},500); } while(!idle&&stopClock.elapsed()<25000);const bool unchanged=idle&&before["settings"]==after["settings"]&&before["litesr_settings"]==after["litesr_settings"]&&before["service_sha"]==after["service_sha"]&&before["litesr_sha"]==after["litesr_sha"];
    report["after"]=after;report["normal_stop"]=idle&&s->userStopped();report["late_results_after_stop"]=late;report["saved_sensor_files_unchanged"]=unchanged;
    report["passed"]=success&&stage==sequence.size()&&idle&&unchanged&&late==0;save(path,report);window.close();return report["passed"].toBool()?0:2;
}

int runLiteSr384LiveValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,false);}
int runLiteSr128LiveValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,true);}

int runLiteSr128VgaSwitchValidation(SensorMainWindow &w,const QString &p){return runBoardLiteSrLiveValidation(w,p,true,true);}
