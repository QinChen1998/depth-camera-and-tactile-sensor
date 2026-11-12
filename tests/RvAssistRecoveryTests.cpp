#include "RvAssistRecoveryTests.h"
#include "../sensors/RvAssistFusion.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvInferenceRuntime.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTextStream>
#include <QtCore/QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
using namespace RvAssistFusion;

struct Scene {
    const RvCameraView::Profile &view=RvCameraView::Portrait;
    const Calibration &cal=calibrationForFov(6);
    RvTofFrame tof;
    std::vector<float> mono=std::vector<float>(view.outputWidth*view.outputHeight,1.f);

    Scene(){
        tof.samples=QByteArray(cal.nativeCount()*4,0);
        tof.metadata={{"work_mode","rgb_tof"},{"source_protocol","HDA1"},{"assist_experiment","G"},
            {"assist_host_model",RvCameraView::UniDepthVgaNative48},{"dtof_present",true},
            {"frame_id",1},{"generation",3},{"source_generation",QString(32,'a')},
            {"boot_id","11111111-2222-3333-4444-555555555555"},{"tof_sequence",1},{"tof_timestamp_ns",1000000000.},
            {"rgb_timestamp_ns",835000000.},{"tof_reused",false},{"tof_warning_flags",0},
            {"pairing_policy",RvTofFrame::FreshRgbPairPolicy},{"pair_clock_source","rtcp_prft_board_realtime_monotonic"},
            {"pair_delta_ms",165.},{"pair_clock_uncertainty_ms",35.},
            {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},{"dtof_fov_correction",6},
            {"source_candidate_mode",24},{"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
            {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"},
            {"rgb_width",view.inputWidth},{"rgb_height",view.inputHeight},{"rectify_map_sha256",view.mapSha},
            {"simulation",true},{"input_origin","SIMULATED"}};
        plane(.18);
        tof.metadata.insert(NativeProjectionField,defaultNativeProjection(tof));
    }

    void plane(double z){
        for(int i=0;i<cal.nativeCount();++i){
            const auto radial=(z-cal.translation[2])/cal.cameraRays[i][2];
            qToLittleEndian<quint16>(quint16(std::lround(radial/.00025)),tof.samples.data()+4*i);
            tof.samples[4*i+2]=30;
            tof.samples[4*i+3]=1;
        }
    }

    Result fit(GlobalScaleFit policy=GlobalScaleFit::SourceG)const{
        return fuseGlobalScale(mono,tof,cal,view,{},policy);
    }

    RvDepthDiagnostic::Snapshot snapshot(const Result &result,const char *revision=GlobalRgbRevision)const{
        const auto visible=limitTofFieldOfView(result,cal,view);
        QImage rgb(view.outputSize(),QImage::Format_RGB888);
        rgb.fill(Qt::gray);
        auto metadata=tof.metadata;
        metadata["width"]=view.outputWidth;metadata["height"]=view.outputHeight;
        metadata["depth_unit"]="camera-Z_m_float32";
        metadata["assist_output_grid"]=RvCameraView::PortraitGrid;
        metadata["assist_model_depth_size"]=QJsonArray{640,480};
        metadata["assist_model_crop_xywh"]=QJsonArray{140,0,360,480};
        metadata["depth_K"]=RvCameraView::intrinsics(view);
        metadata["assist_graph_sha256"]=view.graphSha;
        metadata["assist_provisional_model"]=true;
        metadata["assist_candidate_parity_mean_limit"]=view.parityMeanLimit;
        metadata["assist_input_rotation_degrees"]=0;
        metadata["assist_output_restored_to_calibrated_grid"]=true;
        metadata["assist_fusion"]=visible.diagnostics;
        metadata["assist_fusion_revision"]=revision;
        metadata["assist_output_policy"]=FieldOfViewPolicy;
        metadata["assist_diagnostic_native_count"]=cal.nativeCount();
        return {{visible.depth,{},rgb,metadata,QDateTime::currentDateTimeUtc()},tof,RvDepthDiagnostic::encode(mono,visible),{}};
    }
};
}

int runRvAssistRecoveryTests(const QString &path){
    QFile report(path);
    if(!report.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
    QTextStream out(&report);
    bool all=true;
    const auto check=[&](const QString &name,bool ok){
        out<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;
        all&=ok;
    };
    Scene scene;
    const auto &graph=RvCameraView::Rgb672VgaFp16;
    const auto &view=scene.view;
    check("PORTRAIT_GRAPH_AND_OUTPUT_DISTINCT",graph.outputSize()==QSize(640,480)&&view.outputSize()==QSize(480,640)
        &&view.graphSha==graph.graphSha&&view.mapSha==graph.mapSha&&view.inputSize()==graph.inputSize());
    bool aligned=true;
    for(int i=0;i<scene.cal.nativeCount();++i)for(double radial:{.05,.2,1.}){
        const auto landscape=projectNativePoint(radial,i,scene.cal,graph);
        const auto portrait=projectNativePoint(radial,i,scene.cal,view);
        if(portrait.cameraZ<=0)continue;
        const auto input=RvCameraView::inputPixel(portrait.pixel,view.outputSize());
        aligned&=std::abs(input.x()-landscape.pixel.x()-16)<1e-9&&std::abs(input.y()-landscape.pixel.y()-12)<1e-9;
    }
    check("PORTRAIT_SAME_PHYSICAL_RAYS_NO_AXIS_SWAP",aligned);
    std::vector<float> ramp(640*480);
    for(int y=0;y<480;++y)for(int x=0;x<640;++x)ramp[y*640+x]=1.f+x*.001f+y*.002f;
    const auto resized=RvCameraView::portraitDepth(ramp);
    check("PORTRAIT_ISOTROPIC_METRIC_CROP",resized.size()==480*640
        &&std::abs(resized[320*480+240]-(1+320*.001+240*.002))<.002);

    const auto original=scene.tof.samples;
    auto distant=scene;distant.plane(2.);
    const auto distantFit=distant.fit();
    bool beyond256=false;
    for(const auto id:distantFit.diagnostics["used_native_indices"].toArray())beyond256|=id.toInt()>=256;
    check("NATIVE48_FITS_FULL_MEASURED_PLANE",distantFit.ok()&&distantFit.diagnostics["dtof_corrected"]==true
        &&distantFit.diagnostics["tof_total_zones"]==1536&&beyond256
        &&std::abs(distantFit.diagnostics["scale"].toDouble()-2)<.0005);
    auto confused=scene;confused.tof.metadata["assist_host_model"]="unidepth_rgb672_vga_fp16_tof16";
    auto conflicting=scene;conflicting.tof.metadata["tof_fov_correction"]=1;
    auto wrongProjection=scene;wrongProjection.tof.metadata[NativeProjectionField]="invalid";
    check("G_REJECTS_WRONG_MODEL_FOV_OR_PROJECTION",!confused.fit().ok()&&!conflicting.fit().ok()&&!wrongProjection.fit().ok());
    auto invalidModel=scene;invalidModel.mono[0]=std::numeric_limits<float>::quiet_NaN();
    check("G_REJECTS_NONFINITE_MODEL",!invalidModel.fit().ok());

    GlobalRgbContinuity history;
    const auto global=history.apply(scene.fit(),1000,1,3,1);
    int projected=0;
    for(int id=0;id<scene.cal.nativeCount();++id){
        const auto point=projectNativePoint(scene.tof.rangeUq2(id)*.00025,id,scene.cal,view);
        const auto pixel=point.pixel+QPointF(.0065,.00725)*(view.focal/point.cameraZ);
        projected+=pixel.x()>=3&&pixel.x()<=476&&pixel.y()>=3&&pixel.y()<=636;
    }
    check("G_FRESH_PAIR_USES_CORRECTED_PROJECTION_AND_GLOBAL_SCALE_ONLY",
        global.ok()&&global.diagnostics["fusion_mode"]=="global_scale"
        &&global.diagnostics["tof_projected_in_view"]==projected&&scene.tof.samples==original
        &&global.diagnostics["local_used_native_indices"].toArray().isEmpty()
        &&global.localReasons==QByteArray(1536,NoMeasurement)
        &&std::abs(global.diagnostics["scale"].toDouble()-.18)<.0005);

    auto reused=scene;reused.tof.metadata["tof_reused"]=true;
    auto late=scene;late.tof.metadata["pair_delta_ms"]=165.01;late.tof.metadata["rgb_timestamp_ns"]=834990000.;
    check("G_REJECTS_REUSED_OR_OUT_OF_BUDGET_TOF",!reused.fit().ok()&&!late.fit().ok());
    auto warning=scene;warning.tof.metadata["tof_warning_flags"]=8;
    auto tinyGain=scene;std::fill(tinyGain.mono.begin(),tinyGain.mono.end(),100.f);
    check("G_CURRENT_WARNING_AND_GAIN_LIMITS",!warning.fit().ok()
        &&tinyGain.fit(GlobalScaleFit::Extended).diagnostics["dtof_corrected"]==true
        &&tinyGain.fit().diagnostics["fusion_mode"]=="monocular");
    const auto rgbOnly=monocular(scene.mono,"RGB only",view.outputSize());
    const auto between=history.apply(rgbOnly,7033,1,3,2);
    check("G_RGB_ONLY_KEEPS_LAST_SCALAR_AND_REFERENCE_AGE",between.ok()&&between.diagnostics["fusion_mode"]=="held_scale"
        &&between.diagnostics["used_native_indices"].toArray().isEmpty()&&between.diagnostics["scale_reference_age_ms"]==6033);
    auto far=scene;far.plane(1.8);
    const auto next=history.apply(far.fit(),7283,1,3,3);
    check("G_GAIN_UPDATE_IS_CAPPED",next.ok()&&next.diagnostics["scale_reference_frame_id"]==3
        &&std::abs(next.diagnostics["scale"].toDouble()/global.diagnostics["scale"].toDouble()-1.1)<1e-12);
    const auto reset=history.apply(rgbOnly,7316,1,4,4);
    check("G_NEW_GENERATION_SHOWS_RAW_MONOCULAR",reset.depth==rgbOnly.depth
        &&reset.diagnostics["fusion_mode"]=="monocular"&&reset.diagnostics["scale_reference_age_ms"]==-1);

    auto firstB=scene;
    firstB.tof.metadata["assist_experiment"]="B";
    GlobalRgbContinuity historyB;
    const auto freshB=historyB.apply(firstB.fit(),1000,1,3,1);
    check("B_FRESH_PAIR_USES_G_FIT",freshB.ok()&&freshB.depth==global.depth
        &&freshB.diagnostics["used_native_indices"]==global.diagnostics["used_native_indices"]);
    auto reusedB=firstB;
    reusedB.tof.metadata["frame_id"]=2;
    reusedB.tof.metadata["rgb_timestamp_ns"]=1033000000.;
    reusedB.tof.metadata["pair_delta_ms"]=-33.;
    reusedB.tof.metadata["tof_reused"]=true;
    std::fill(reusedB.mono.begin(),reusedB.mono.end(),1.25f);
    const auto nextB=historyB.apply(reusedB.fit(),1033,1,3,2);
    check("B_REUSED_TOF_REFITS_THE_CURRENT_RGB",nextB.ok()&&nextB.diagnostics["dtof_corrected"]==true
        &&reusedB.tof.samples==firstB.tof.samples&&nextB.diagnostics["scale_reference_frame_id"]==2
        &&std::abs(nextB.diagnostics["measured_scale"].toDouble()-.18/1.25)<.0005);
    auto lateB=reusedB;
    lateB.tof.metadata["pair_delta_ms"]=165.01;
    lateB.tof.metadata["rgb_timestamp_ns"]=834990000.;
    check("B_REUSE_STILL_REJECTS_EXCESS_TIMESTAMP_DELTA",!lateB.fit().ok());

    QTemporaryDir temporary;
    const auto replayPath=[&](const char *name){return temporary.filePath(QString::fromLatin1(name)+".json");};
    const auto bSnapshot=reusedB.snapshot(nextB,ReuseRgbRevision);
    QString bFolder;
    check("B_REUSED_TOF_DIAGNOSTIC_SAVES_AND_REPLAYS",bSnapshot.save(temporary.path(),&bFolder)
        &&RvDepthDiagnostic::replay(bFolder,replayPath("reused_b"))==0);
    auto wrongReuse=bSnapshot;wrongReuse.native.metadata["tof_reused"]=false;
    auto wrongExperiment=bSnapshot;wrongExperiment.output.metadata["assist_experiment"]="G";
    check("B_DIAGNOSTIC_REJECTS_MISMATCHED_REUSE_OR_EXPERIMENT",!wrongReuse.valid()&&!wrongExperiment.valid());
    const auto snapshot=scene.snapshot(global);
    QString folder,second;
    const bool saved=snapshot.save(temporary.path(),&folder);
    check("G_DIAGNOSTIC_SAVE_AND_REPLAY",saved&&QFileInfo(folder+"/tof.bin").size()==6144
        &&RvDepthDiagnostic::replay(folder,replayPath("current"))==0);
    auto archivedGraph=snapshot;
    auto &archivedMetadata=archivedGraph.output.metadata;
    archivedMetadata["assist_execution_device"]="tensorrt";
    archivedMetadata["assist_backend"]="onnxruntime-tensorrt-rtx";
    archivedMetadata["assist_precision"]=RvCameraView::Rgb672VgaFp16.precision;
    archivedMetadata["assist_graph_sha256"]="34c123a60912769fd161300b5302948fa5f3b068628cd02bba46bad5b215cad9";
    archivedMetadata["assist_source_graph_sha256"]="3c2f99b9fda0fa5e0efe815c5d57f5eed8b8a24a227961a8d11dc531806f8d64";
    archivedMetadata["assist_conversion"]="ort-rgb672-vga-frozen-depth-confidence-fp32-v1";
    QString archivedFolder;
    check("G_CONFIDENCE_TRIAL_DIAGNOSTIC_REPLAYS",archivedGraph.save(temporary.path(),&archivedFolder)
        &&RvDepthDiagnostic::replay(archivedFolder,replayPath("archived_graph"))==0);
    check("G_DIAGNOSTIC_NEVER_OVERWRITES",snapshot.save(temporary.path(),&second)&&second!=folder);
    const auto comparison=temporary.filePath("comparison");
    check("G_DIAGNOSTIC_EXPORT",saved&&RvDepthDiagnostic::replay(folder,replayPath("export"),comparison)==0
        &&QFileInfo::exists(comparison+"/current_depth.f32")
        &&RvDepthDiagnostic::replay(folder,replayPath("overwrite"),folder)==2);
    auto malformed=snapshot;malformed.output.metadata["assist_model_crop_xywh"]=QJsonArray{0,0,480,640};
    auto mismatch=snapshot;mismatch.native.metadata["tof_sequence"]=8;
    auto projectionMismatch=snapshot;projectionMismatch.native.metadata.remove(NativeProjectionField);
    check("G_DIAGNOSTIC_REJECTS_OTHER_PAIR_AND_GRID",!malformed.valid()&&!mismatch.valid()&&!projectionMismatch.valid());
    for(const auto *retired:{"H","I"}){
        auto disabled=snapshot;
        disabled.output.metadata["assist_experiment"]=retired;
        check(QString("RETIRED_DIAGNOSTIC_REJECTED_")+retired,!disabled.valid());
    }
    if(saved){
        QFile csv(folder+"/points.csv");
        check("G_DIAGNOSTIC_FULL_NATIVE_GRID",csv.open(QIODevice::ReadOnly)&&csv.readAll().count('\n')==1537);
        QFile native(folder+"/tof.bin");
        const bool tampered=native.open(QIODevice::ReadWrite)&&native.write("!",1)==1;
        native.close();
        check("G_DIAGNOSTIC_NATIVE_TAMPER_REJECTED",tampered&&RvDepthDiagnostic::replay(folder,replayPath("tampered"))==2);
    }

    auto older=warning.fit(GlobalScaleFit::Extended);
    older.diagnostics["measured_scale"]=older.diagnostics.value("scale");
    const auto legacy=warning.snapshot(older,LegacyGlobalRgbRevision);
    QString legacyFolder;
    check("G_LEGACY_DIAGNOSTIC_REPLAYS_WITH_RECORDED_POLICY",legacy.save(temporary.path(),&legacyFolder)
        &&RvDepthDiagnostic::replay(legacyFolder,replayPath("legacy"))==0);

    auto unpaired=snapshot;
    unpaired.native={};
    auto &metadata=unpaired.output.metadata;
    for(const auto *key:{"tof_sequence","tof_timestamp_ns","rgb_timestamp_ns","tof_reused","pair_delta_ms",
                         "dtof_width","dtof_height","dtof_bytes","dtof_fov_correction",NativeProjectionField})
        metadata.remove(key);
    metadata["frame_id"]=2;
    metadata["work_mode"]="host_rgb";
    metadata["dtof_present"]=false;
    metadata["assist_tof_fov_correction"]=6;
    const auto visible=limitTofFieldOfView(between,scene.cal,view);
    metadata["assist_fusion"]=visible.diagnostics;
    unpaired.output.metresFloat32LE=visible.depth;
    unpaired.payload=RvDepthDiagnostic::encode(scene.mono,visible);
    QString unpairedFolder;
    check("G_HELD_SCALE_DIAGNOSTIC_REPLAYS_WITHOUT_NATIVE_POINTS",unpaired.save(temporary.path(),&unpairedFolder)
        &&RvDepthDiagnostic::replay(unpairedFolder,replayPath("held"))==0);

    RvDepthDiagnostic::SequenceRecorder recorder;
    QString recordingError;
    check("SEQUENCE_EXPLICIT_START",recorder.start(temporary.path(),&recordingError));
    QImage input(672,504,QImage::Format_RGB888);
    input.fill(QColor(25,70,90));
    recorder.append(snapshot.output.metresFloat32LE,snapshot.output.metadata,input,snapshot.native,snapshot.payload);
    recorder.append(snapshot.output.metresFloat32LE,snapshot.output.metadata,input,snapshot.native,snapshot.payload);
    recorder.stop();
    for(int i=0;i<200&&!recorder.finished();++i)QThread::msleep(5);
    const auto status=recorder.status();
    check("SEQUENCE_BOUNDED_WRITER_FINISHES",recorder.finished()&&status["frames"]==1&&status["dropped_frames"]==1&&status["error"]=="");
    QFile stream(status["folder"].toString()+"/frames.bin");
    const auto length=stream.open(QIODevice::ReadOnly)?stream.read(4):QByteArray{};
    const auto json=length.size()==4?stream.read(qFromLittleEndian<quint32>(length.constData())):QByteArray{};
    const auto header=QJsonDocument::fromJson(json).object();
    const auto pixels=stream.read(header["rgb_bytes"].toInt());
    const auto diagnostic=stream.read(header["diagnostic_bytes"].toInt());
    check("SEQUENCE_PRESERVES_FULL_INPUT_MONO_AND_IDENTITY",header["input_width"]==672&&header["input_height"]==504
        &&header["metadata"]==snapshot.output.metadata&&header["native_metadata"]==snapshot.native.metadata
        &&pixels.size()==672*504*3&&diagnostic==snapshot.payload&&stream.read(6144)==scene.tof.samples
        &&stream.readAll()==snapshot.output.metresFloat32LE);

    auto quantized=snapshot;
    auto &qm=quantized.output.metadata;
    const auto identity=RvInferenceRuntime::identity("openvino:INT8");
    for(auto it=identity.begin();it!=identity.end();++it)qm["assist_"+it.key()]=it.value();
    qm["assist_execution_device"]="openvino:INT8";qm["assist_backend"]="openvino-native";
    check("INT8_DIAGNOSTIC_EXPLICIT_IDENTITY",quantized.valid());
    auto wrongQuantized=quantized;wrongQuantized.output.metadata["assist_source_graph_sha256"]=QString(64,'0');
    check("INT8_DIAGNOSTIC_WRONG_SOURCE_REJECTED",!wrongQuantized.valid());
    wrongQuantized=quantized;wrongQuantized.output.metadata["assist_execution_device"]="cpu";
    check("INT8_DIAGNOSTIC_WRONG_DEVICE_REJECTED",!wrongQuantized.valid());
    wrongQuantized=quantized;wrongQuantized.output.metadata["assist_graph_sha256"]=view.graphSha;
    check("INT8_DIAGNOSTIC_ORIGINAL_HASH_REJECTED",!wrongQuantized.valid());
    return all?0:2;
}
