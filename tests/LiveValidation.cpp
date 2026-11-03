#include "../SensorMainWindow.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvTunerController.h"
#include <QApplication>
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
