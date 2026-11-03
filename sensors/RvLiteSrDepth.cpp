#include "RvLiteSrDepth.h"
#include "RvLiteSrContour.h"
#include "LiteSrNativeBindings.h"
#include "../RuntimeDiscovery.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSettings>
#include <QtCore/QUuid>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
constexpr int DepthBytes=256*256*4,HeaderLimit=16384;
QString pinned(const QString &model) {
    if(model=="area_full"||model=="area_full_contour"||model=="interpolation")return "519cb9b3efe3ebd30aac6a9ff832ee287e761f8af5eb907c9614b30b69623943";
    if(model=="area_no_guidance")return "54ed33a88c36844a56738901fb83420d21b4fc310cabed4a1f56052f4a7c3dd2";
    if(model=="wta_full")return "b4971ed45b5c8cd872fec4a0ec84777e35fd13fc43146a863ae20b8cf9fcf689";
    if(model=="near_full_quality12")return "ba4e3ea6bb79139bd3eb998c0f0e74edb5c9da3a059c25476ef5d4d68a459bfb";
    if(model=="near_no_guidance_quality12")return "bc6b97896d98c0432551f0e2d67e8a342c8179d79b740fc4f9f9aa2550e0bcb5";
    return {};
}
void require(bool condition,const char *reason){if(!condition)throw std::runtime_error(reason);}
QByteArray hash(const QByteArray &bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();}
QByteArray readFile(const QString &path,qint64 limit){QFile f(path);require(f.open(QIODevice::ReadOnly)&&f.size()>0&&f.size()<=limit,"LiteSR runtime resource missing or oversized");return f.readAll();}
QByteArray packet(const QJsonObject &json,const QByteArray &native,const QByteArray &signal){
    const auto header=QJsonDocument(json).toJson(QJsonDocument::Compact);
    require(header.size()<=HeaderLimit,"LiteSR metadata limit");
    QByteArray bytes(12,0);qToLittleEndian<quint32>(header.size(),bytes.data());
    qToLittleEndian<quint32>(native.size(),bytes.data()+4);qToLittleEndian<quint32>(signal.size(),bytes.data()+8);
    return bytes+header+native+signal;
}
}

RvLiteSrDepth::RvLiteSrDepth(QObject *parent):QObject(parent){
    setObjectName("rvLiteSrDepth");
    const QSettings settings;m_white=std::clamp(settings.value("depth/litesrWhiteCounts",512).toInt(),1,8388352);
    m_rotation=settings.value("depth/litesrInputRotation",0).toInt()==180?180:0;
    const int holeMode=settings.value("depth/litesrHoleMode",1).toInt();
    m_repairSettings.holeMode=holeMode>=0&&holeMode<=2?holeMode:1;
    m_repairSettings.removeOutliers=settings.value("depth/litesrRemoveOutliers",true).toBool();
    m_timeout.setSingleShot(true);m_expiry.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,[this]{fail(tr("LiteSR preview timed out. Reselect the option to retry."));});
    connect(&m_expiry,&QTimer::timeout,this,[this]{setStatus(tr("LiteSR preview expired; waiting for a matched D/I frame."));emit cleared(m_status);});
    connect(&m_process,&QProcess::readyReadStandardOutput,this,&RvLiteSrDepth::readOutput);
    connect(&m_process,&QProcess::readyReadStandardError,this,[this]{m_stderr=(m_stderr+m_process.readAllStandardError()).right(8192);});
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError){if(!m_stopping&&!m_destroying)fail(tr("LiteSR native process unavailable. Check its optional runtime."));});
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int,QProcess::ExitStatus){if(!m_stopping&&!m_destroying)fail(tr("LiteSR preview process stopped. Reselect the option to retry."));});
}
RvLiteSrDepth::~RvLiteSrDepth(){m_destroying=true;m_process.disconnect(this);cancel({});if(m_refinement.joinable())m_refinement.join();}
bool RvLiteSrDepth::supportsModel(const QString &){return false;}
bool RvLiteSrDepth::isNearModel(const QString &model){return model=="near_full_quality12"||model=="near_no_guidance_quality12";}
float RvLiteSrDepth::minimumDepthM(const QString &model){return isNearModel(model)?.01f:.1f;}
bool RvLiteSrDepth::runtimeAvailable(const QString &model){
    if(!supportsModel(model))return false;
    try{
        const QDir dir(isNearModel(model)?RuntimeDiscovery::rvLiteSrNearRoot():RuntimeDiscovery::rvLiteSrRoot());
        const auto info=QJsonDocument::fromJson(readFile(dir.filePath("runtime.json"),65536)).object();
        const auto binding=info["models"].toObject()[inferenceModel(model)].toObject();
        const bool interpolation=model=="interpolation";
        const auto graph=binding[interpolation?"contract":"graph"].toString();
        const auto* fixed=liteSrNativeBinding(inferenceModel(model));
        return fixed&&info["schema"]=="litesr_native_onnx_v1"&&info["backend"]=="onnxruntime-cuda-fp32"
            &&info["precision"]=="FP32"&&binding["checkpoint_sha256"]==pinned(model)
            &&graph==QString::fromLatin1(fixed->file)&&binding[interpolation?"contract_sha256":"graph_sha256"]==QString::fromLatin1(fixed->graph)
            &&QFileInfo::exists(dir.filePath(graph))&&QFileInfo::exists(dir.filePath("bin/onnxruntime.dll"));
    }catch(...){return false;}
}
QString RvLiteSrDepth::inferenceModel(const QString &model){return model=="area_full_contour"?QStringLiteral("area_full"):model;}
bool RvLiteSrDepth::compatibleFrame(const RvPhotonFrame &f){
    const auto declared=f.tof.metadata.contains("dtof_fov_correction")?f.tof.metadata["dtof_fov_correction"]:f.tof.metadata["tof_fov_correction"];
    return f.isValid()&&f.tof.columns()==48&&f.tof.rows()==32&&f.fovCorrection>=0&&f.fovCorrection<=255
        &&(declared.isUndefined()||(declared.isDouble()&&declared.toDouble()==f.fovCorrection));
}
void RvLiteSrDepth::setStatus(const QString &reason){m_status=reason;if(!m_destroying)emit changed();}
void RvLiteSrDepth::cancel(const QString &reason){
    ++m_inputRevision;
    m_stopping=true;m_timeout.stop();m_expiry.stop();m_ready=m_busy=m_failed=false;m_loading=0;
    m_pending={};m_current={};m_buffer.clear();m_token.clear();m_lastSequence=m_lastStamp=0;
    if(m_process.state()!=QProcess::NotRunning){m_process.kill();m_process.waitForFinished(1500);}
    m_stopping=false;setStatus(reason);if(!m_destroying)emit cleared(reason);
}
void RvLiteSrDepth::pauseInput(const QString &reason){
    // A transient PHC1 failure revokes publication, not the loaded model.
    // Retain the exact in-flight request so its reply can be validated/drained.
    ++m_inputRevision;m_pending={};m_expiry.stop();
    setStatus(reason);if(!m_destroying)emit cleared(reason);
}
void RvLiteSrDepth::fail(const QString &reason){cancel(reason);m_failed=true;}
void RvLiteSrDepth::setModel(const QString &model){
    if(!model.isEmpty()&&!supportsModel(model))return;
    if(model==m_model&&!m_failed)return;
    cancel(tr("LiteSR selection changed"));m_model=model;
    setStatus(model.isEmpty()?QString():tr("Experimental LiteSR: waiting for native 32 x 48 D/I. Counts mapping is uncalibrated."));
}
void RvLiteSrDepth::setWhiteCounts(int counts){
    if(counts<1||counts>8388352||counts==m_white)return;
    cancel(tr("LiteSR counts mapping changed; waiting for a new frame."));m_white=counts;
    QSettings().setValue("depth/litesrWhiteCounts",counts);emit changed();
}
void RvLiteSrDepth::setInputRotation(int degrees){
    if((degrees!=0&&degrees!=180)||degrees==m_rotation)return;
    cancel(tr("LiteSR native input orientation changed."));m_rotation=degrees;
    QSettings().setValue("depth/litesrInputRotation",degrees);emit changed();
}
void RvLiteSrDepth::setRepairSettings(const RvLiteSrContour::RepairSettings &settings){
    if(settings.holeMode<0||settings.holeMode>2||settings==m_repairSettings)return;
    m_repairSettings=settings;
    QSettings prefs;prefs.setValue("depth/litesrHoleMode",settings.holeMode);
    prefs.setValue("depth/litesrRemoveOutliers",settings.removeOutliers);
    pauseInput(tr("LiteSR repair settings changed; waiting for a new frame."));
}
void RvLiteSrDepth::submit(const RvPhotonFrame &frame,quint64 epoch){
    if(!selected()||m_failed||m_destroying)return;
    if(!compatibleFrame(frame)){
        pauseInput(tr("LiteSR requires a fresh complete 32 x 48 first-peak D/I frame with actual FOV."));return;
    }
    const auto seq=quint64(frame.tof.metadata["tof_sequence"].toInteger()),stamp=quint64(frame.tof.metadata["tof_timestamp_ns"].toInteger());
    if(m_current.clock.isValid()&&(m_current.epoch!=epoch||m_current.frame.bootId!=frame.bootId
        ||m_current.frame.tof.metadata["generation"]!=frame.tof.metadata["generation"]))cancel(tr("LiteSR input session changed."));
    if(seq<=m_lastSequence||stamp<=m_lastStamp)return;
    m_lastSequence=seq;m_lastStamp=stamp;
    m_pending={frame,{},epoch,++m_request,m_inputRevision};m_pending.clock.start();
    if(m_process.state()==QProcess::NotRunning)startWorker();else if(m_ready&&!m_busy)sendLatest();
}
void RvLiteSrDepth::startWorker(){
    try{
        m_root=isNearModel(m_model)?RuntimeDiscovery::rvLiteSrNearRoot():RuntimeDiscovery::rvLiteSrRoot();const QDir dir(m_root);
        const auto raw=readFile(dir.filePath("runtime.json"),65536);m_runtimeSha=hash(raw);
        const auto info=QJsonDocument::fromJson(raw).object();
        require(info["schema"]=="litesr_native_onnx_v1"&&info["precision"]=="FP32"&&info["backend"]=="onnxruntime-cuda-fp32","LiteSR runtime contract mismatch");
        const auto binding=info["models"].toObject()[inferenceModel(m_model)].toObject();
        if(isNearModel(m_model)){
            const auto range=binding["range"].toObject();
            require(range["near_m"].toDouble()==.01&&range["far_m"].toDouble()==1.4,"LiteSR near range contract mismatch");
        }
        const auto files=info["files"].toObject();
        const auto* fixed=liteSrNativeBinding(inferenceModel(m_model));require(fixed,"Unknown native LiteSR graph");
        const bool interpolation=m_model=="interpolation";
        require(binding[interpolation?"contract":"graph"]==QString::fromLatin1(fixed->file)&&binding[interpolation?"contract_sha256":"graph_sha256"]==QString::fromLatin1(fixed->graph),"LiteSR frozen graph identity mismatch");
        const QStringList required{QString::fromLatin1(fixed->file)};
        for(const auto &relative:required){
            require(!relative.isEmpty()&&!QDir::isAbsolutePath(relative)&&!relative.contains("..")&&!relative.contains('\\'),"LiteSR runtime path rejected");
            require(files.contains(relative)&&hash(readFile(dir.filePath(relative),8*1024*1024))==files[relative].toString().toLatin1(),"LiteSR runtime fingerprint mismatch");
        }
        m_checkpointSha=pinned(m_model);
        require(binding["checkpoint_sha256"]==m_checkpointSha,"LiteSR candidate checkpoint identity mismatch");
        m_backend=interpolation?"native-cpp-fp32":"onnxruntime-cuda-fp32";
        m_token=QUuid::createUuid().toString(QUuid::WithoutBraces);m_buffer.clear();m_stderr.clear();m_loading=1;
        auto environment=QProcessEnvironment::systemEnvironment();
        for(const auto& key:environment.keys())if(key.startsWith("PYTHON")||key.startsWith("CONDA"))environment.remove(key);
        m_process.setProcessEnvironment(environment);m_process.setWorkingDirectory(m_root);
        m_process.setProgram(QCoreApplication::applicationFilePath());m_process.setArguments({"--rv-litesr-native-worker",m_root,inferenceModel(m_model),m_token});
        m_timeout.start(90000);setStatus(tr("Loading optional LiteSR host preview..."));m_process.start();
    }catch(const std::exception &e){fail(tr("LiteSR unavailable: %1").arg(QString::fromUtf8(e.what())));}
}
void RvLiteSrDepth::sendLatest(){
    if(!m_ready||m_busy||!m_pending.clock.isValid())return;
    const double age=m_pending.frame.ageMs+m_pending.clock.nsecsElapsed()/1e6;
    if(age>=500){m_pending={};return;}
    m_current=m_pending;m_pending={};m_busy=true;
    auto meta=m_current.frame.tof.metadata;meta["sr_boot_id"]=m_current.frame.bootId;
    meta["sr_result_format"]=m_current.frame.resultFormat;meta["sr_pairing"]="PHC1_exact_native_identity";
    meta["dtof_fov_correction"]=m_current.frame.fovCorrection;
    const QJsonObject header{{"protocol",1},{"type","infer"},{"token",m_token},{"model",inferenceModel(m_model)},
        {"request",qint64(m_current.request)},{"epoch",qint64(m_current.epoch)},
        {"white_counts",m_white},{"rotation",m_rotation},{"metadata",meta}};
    const auto bytes=packet(header,m_current.frame.tof.samples,m_current.frame.signalWords);
    m_timeout.start(10000);if(m_process.write(bytes)!=bytes.size())fail(tr("LiteSR request failed"));
}
void RvLiteSrDepth::readOutput(){
    m_buffer+=m_process.readAllStandardOutput();
    try{
        require(m_buffer.size()<=HeaderLimit+DepthBytes+12,"LiteSR output buffer limit");
        while(m_buffer.size()>=12){
            const auto h=qFromLittleEndian<quint32>(m_buffer.constData()),b=qFromLittleEndian<quint32>(m_buffer.constData()+4),a=qFromLittleEndian<quint32>(m_buffer.constData()+8);
            require(h>0&&h<=HeaderLimit&&a==0&&(b==0||b==DepthBytes),"LiteSR output envelope");
            if(m_buffer.size()<12+h+b)return;
            const auto json=QJsonDocument::fromJson(m_buffer.mid(12,h)).object();const auto bytes=m_buffer.mid(12+h,b);m_buffer.remove(0,12+h+b);
            require(json["protocol"]==1&&json["token"]==m_token,"LiteSR output token mismatch");
            if(json["type"]=="fatal"){fail(tr("LiteSR unavailable: %1").arg(json["error"].toString()));return;}
            require(json["model"]==inferenceModel(m_model)&&json["runtime_sha256"]==m_runtimeSha&&json["checkpoint_sha256"]==m_checkpointSha&&json["backend"]==m_backend,"LiteSR output identity mismatch");
            if(json["type"]=="ready"){
                require(!m_ready&&b==0,"LiteSR duplicate readiness");m_timeout.stop();m_ready=true;m_loading=2;
                setStatus(tr("LiteSR ready; waiting for a fresh matched D/I frame."));sendLatest();continue;
            }
            require(json["type"]=="result"&&m_ready&&m_busy&&json["request"].toInteger()==qint64(m_current.request)&&json["epoch"].toInteger()==qint64(m_current.epoch),"LiteSR result request mismatch");
            m_timeout.stop();m_busy=false;
            if(m_current.revision!=m_inputRevision){m_current={};sendLatest();continue;}
            const double age=m_current.frame.ageMs+m_current.clock.nsecsElapsed()/1e6;
            if(json.contains("error")){m_expiry.stop();setStatus(tr("LiteSR input rejected: %1").arg(json["error"].toString()));emit cleared(m_status);}
            else if(age<1000){
                require(b==DepthBytes,"LiteSR depth output size");
                for(int i=0;i<DepthBytes;i+=4){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+i);float value;std::memcpy(&value,&bits,4);require(std::isfinite(value)&&(value==0||(value>=minimumDepthM(m_model)&&value<=1.4f)),"LiteSR invalid radial depth output");}
                const auto diagnostic=json["diagnostic"].toObject();
                if(m_model=="area_full_contour"){refineResult(bytes,diagnostic);continue;}
                publishResult(bytes,diagnostic);
            }else{setStatus(tr("LiteSR result too old; waiting for a new matched D/I frame."));emit cleared(m_status);}
            m_current={};sendLatest();
        }
    }catch(const std::exception &e){fail(tr("LiteSR protocol rejected: %1").arg(QString::fromUtf8(e.what())));}
}
void RvLiteSrDepth::refineResult(const QByteArray &depth,const QJsonObject &diagnostic){
    // Keep one in-flight job through both native inference and refinement. Cancellation
    // revokes the token/revision; queued CPU results cannot revive old data.
    m_busy=true;
    if(m_refinement.joinable())m_refinement.join();
    const auto token=m_token;const auto request=m_current.request;
    const auto native=m_current.frame.tof.samples;const auto settings=m_repairSettings;const int rotation=m_rotation;
    m_refinement=std::thread([this,depth,diagnostic,token,request,native,settings,rotation]{
        RvLiteSrContour::Result result;QString error;
        try{result=RvLiteSrContour::refine(depth,native,rotation,settings);}catch(const std::exception &e){error=QString::fromUtf8(e.what());}
        QMetaObject::invokeMethod(this,[this,result=std::move(result),diagnostic,token,request,error]{
            if(m_destroying||m_token!=token||!m_busy||m_current.request!=request)return;
            m_busy=false;
            if(m_current.revision==m_inputRevision){
                if(!error.isEmpty()){fail(tr("LiteSR contour refinement failed: %1").arg(error));return;}
                publishResult(result.depth,diagnostic,result.coverage,result.diagnostic);
            }
            m_current={};sendLatest();
        },Qt::QueuedConnection);
    });
}
void RvLiteSrDepth::publishResult(const QByteArray &bytes,const QJsonObject &diagnostic,const QByteArray &coverage,const QJsonObject &postprocess){
                const double age=m_current.frame.ageMs+m_current.clock.nsecsElapsed()/1e6;
                if(age>=1000){setStatus(tr("LiteSR result too old; waiting for a new matched D/I frame."));emit cleared(m_status);return;}
                auto meta=m_current.frame.tof.metadata;
                // Source camera projection metadata cannot describe this radial
                // index grid. Keep it under an explicitly named source scope.
                meta["sr_source_model"]=meta["model"];
                if(meta.contains("depth_K"))meta["sr_source_camera_depth_K"]=meta.take("depth_K");
                for(const auto *key:{"base_width","base_height","pixel_center","target_width","target_height","transmitted_width","transmitted_height"})meta.remove(key);
                meta["width"]=256;meta["height"]=256;meta["model"]="LiteSR "+m_model+" / EXPERIMENTAL";
                meta["depth_unit"]="sensor_radial_m_float32";meta["depth_format"]="float32_le";
                meta["depth_location"]="host";meta["depth_source"]="host_litesr_native_preview";
                meta["depth_stride"]=256*4;meta["depth_bytes"]=DepthBytes;meta["depth_method"]="litesr_native_index_grid_preview";
                meta["sr_geometry_scope"]="uncalibrated_HR_native_index_grid";meta["sr_rgb_used"]=false;
                meta["sr_checkpoint_sha256"]=m_checkpointSha;meta["sr_runtime_sha256"]=m_runtimeSha;
                meta["sr_host_model"]=m_model;meta["sr_backend"]=m_backend;meta["sr_diagnostic"]=diagnostic;
                meta["sr_range_near_m"]=isNearModel(m_model)?.01:.1;meta["sr_range_far_m"]=1.4;
                meta["sr_near_range_repair_applied"]=false;
                meta["sr_display_reference_width"]=48;meta["sr_display_reference_height"]=32;
                if(!postprocess.isEmpty()){
                    meta["sr_base_model"]="area_full";meta["sr_postprocess"]=postprocess;
                    meta["depth_method"]="litesr_area_full_contour_repair_v2";
                    meta["depth_usage"]="experimental_prediction_with_optional_spatial_repairs_not_native_measurements";
                    meta["sr_display_coverage"]="uint8_opacity_same_grid_not_depth_confidence";
                }
                meta["sr_presentation_epoch"]=qint64(m_current.epoch);
                meta["sr_frame_age_ms"]=age;meta["sr_boot_id"]=m_current.frame.bootId;
                meta["sr_fov_correction"]=m_current.frame.fovCorrection;meta["sr_result_format"]=m_current.frame.resultFormat;
                meta["sr_input_native_sha256"]=QString::fromLatin1(hash(m_current.frame.tof.samples));
                meta["sr_input_signal_sha256"]=QString::fromLatin1(hash(m_current.frame.signalWords));
                meta["physical_accuracy_verified"]=false;meta["intensity_output_published"]=false;
                m_expiry.start(std::max(1,1000-int(age)));
                setStatus(tr("Experimental LiteSR %1 | radial 256 x 256 | %2 ms | counts %3-%4 / white %5 | uncalibrated signal mapping")
                    .arg(m_model).arg(diagnostic["inference_ms"].toDouble(),0,'f',1).arg(diagnostic["count_min"].toInt()).arg(diagnostic["count_max"].toInt()).arg(m_white));
                if(!postprocess.isEmpty())setStatus(tr("LiteSR area full + contour | model %1 ms + refinement %2 ms | filled %3 / outliers %4 cells | radial 256 x 256")
                    .arg(diagnostic["inference_ms"].toDouble(),0,'f',1).arg(postprocess["postprocess_ms"].toDouble(),0,'f',1)
                    .arg(postprocess["filled_native_cells"].toInt()).arg(postprocess["outlier_native_cells"].toInt()));
                emit resultReady(bytes,meta,coverage);
}
