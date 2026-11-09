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
#include <QtGui/QColor>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
using namespace RvAssistFusion;
std::vector<float> decode(const QByteArray &b){
    std::vector<float> z(b.size()/4);
    for(int i=0;i<int(z.size());++i){auto bits=qFromLittleEndian<quint32>(b.constData()+4*i);std::memcpy(&z[i],&bits,4);}return z;
}
struct Scene {
    QString model;
    const RvCameraView::Profile &view;
    Calibration cal;
    RvTofFrame tof;
    std::vector<float> mono;
    explicit Scene(const char *id=RvCameraView::UniDepthVgaNative16,int fov=1):
        model(QLatin1String(id)),view(RvCameraView::profile(model)),cal(calibrationForNativeGrid(fov,16,16)),
        mono(view.outputWidth*view.outputHeight,1.f){
        tof.samples=QByteArray(1024,0);
        tof.metadata={{"work_mode","rgb_tof"},{"source_protocol","HDA1"},{"assist_experiment","H"},{"assist_host_model",model},
            {"dtof_present",true},{"frame_id",1},{"generation",3},{"source_generation",QString(32,'a')},
            {"boot_id","11111111-2222-3333-4444-555555555555"},{"tof_sequence",1},{"tof_timestamp_ns",1000000000.},
            {"rgb_timestamp_ns",990000000.},{"tof_reused",false},{"tof_warning_flags",0},
            {"pairing_policy",RvTofFrame::GlobalScaleReusePolicy},{"pair_clock_source","rtcp_prft_board_realtime_monotonic"},
            {"pair_delta_ms",10.},{"pair_clock_uncertainty_ms",35.},
            {"dtof_width",16},{"dtof_height",16},{"dtof_bytes",1024},{"dtof_fov_correction",fov},
            {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
            {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"},
            {"rgb_width",view.inputWidth},{"rgb_height",view.inputHeight},{"rectify_map_sha256",view.mapSha},
            {"simulation",true},{"input_origin","SIMULATED"}};
        plane(2.);
    }
    void plane(double z){for(int i=0;i<256;++i){const auto r=(z-cal.translation[2])/cal.cameraRays[i][2];
        qToLittleEndian<quint16>(quint16(std::lround(r/.00025)),tof.samples.data()+4*i);tof.samples[4*i+2]=30;tof.samples[4*i+3]=1;}}
    Result fit()const{return fuseGlobalScale(mono,tof,cal,view);}
};
}
int runRvAssistRecoveryTests(const QString &path){
    QFile report(path);if(!report.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
    QTextStream out(&report);bool all=true;
    const auto check=[&](const QString &name,bool ok){out<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;all&=ok;};
    for(const char *model:{RvCameraView::UniDepthVgaNative16})for(int fov:{1,2,6}){
        Scene scene(model,fov);const auto original=scene.tof.samples;const auto fit=scene.fit();
        const QString prefix=scene.model+"_F"+QString::number(fov)+"_";
        check(prefix+"PINNED_NATIVE16_CALIBRATION",scene.cal.valid&&scene.tof.isValidForHostFusion());
        check(prefix+"KNOWN_PLANE_SCALE",fit.ok()&&std::abs(fit.diagnostics["scale"].toDouble()-2.)<.0005);
        const auto z=decode(fit.depth);
        check(prefix+"METRIC_PLANE_AND_NO_LOCAL_FIELD",z.size()==scene.mono.size()&&std::all_of(z.begin(),z.end(),[](float v){return std::abs(v-2)<.0005;})
            &&fit.localReasons==QByteArray(256,NoMeasurement)&&fit.diagnostics["local_used_native_indices"].toArray().isEmpty());
        check(prefix+"INPUT_UNCHANGED",scene.tof.samples==original);
        const auto bounded=limitTofFieldOfView(fit,scene.cal,scene.view);
        const auto bz=decode(bounded.depth);int inside=0,outside=0;bool exact=bounded.ok()&&bz.size()==z.size();
        for(int i=0;i<int(bz.size());++i){if(bz[i]){++inside;exact&=bz[i]==z[i];}else ++outside;}
        check(prefix+"FOV_MASK_PRESERVES_METRES",exact&&inside>0&&outside>0&&fit.depth!=bounded.depth&&decode(fit.depth)==z);
        auto sparse=scene;for(int i=0;i<256;++i)sparse.tof.samples[4*i+3]=0;
        const auto empty=sparse.fit();check(prefix+"NO_ANCHOR_MONOCULAR_FALLBACK",empty.ok()&&empty.diagnostics["dtof_corrected"]==false
            &&empty.diagnostics["used_native_indices"].toArray().isEmpty());
        auto weak=scene;for(int i=0;i<256;++i)weak.tof.samples[4*i+2]=5;
        check(prefix+"LOW_SNR_NOT_USED",weak.fit().diagnostics["dtof_corrected"]==false);
        auto bad=scene;bad.tof.samples[3]=2;check(prefix+"INVALID_NATIVE_BIT_REJECTED",!bad.fit().ok());
        bad.tof=scene.tof;bad.tof.metadata["rectify_map_sha256"]="foreign";check(prefix+"FOREIGN_GEOMETRY_REJECTED",!bad.fit().ok());
        auto invalidMono=scene.mono;invalidMono[0]=std::numeric_limits<float>::quiet_NaN();
        check(prefix+"NONFINITE_MODEL_REJECTED",!fuseGlobalScale(invalidMono,scene.tof,scene.cal,scene.view).ok());
        check(prefix+"WRONG_MODEL_SHAPE_REJECTED",!fuseGlobalScale(std::vector<float>(16,1),scene.tof,scene.cal,scene.view).ok());
        for(double delta:{-100.,100.,-100.001,100.001}){
            auto pair=scene.tof;pair.metadata["pair_delta_ms"]=delta;pair.metadata["rgb_timestamp_ns"]=1000000000.-delta*1e6;
            check(prefix+QString("PAIR_BOUNDARY_%1").arg(delta,0,'f',3),fuseGlobalScale(scene.mono,pair,scene.cal,scene.view).ok()==(std::abs(delta)<=100));
        }
        auto reuse=scene.tof;reuse.metadata["tof_reused"]=true;
        check(prefix+"REUSE_EXPLICIT_AND_BOUNDED",!reuse.isValid()&&reuse.isValidForHostFusion()&&fuseGlobalScale(scene.mono,reuse,scene.cal,scene.view).depth==fit.depth);
        reuse.metadata["pairing_policy"]="foreign";check(prefix+"REUSE_POLICY_REJECTED",!reuse.isValidForHostFusion());
        ScaleContinuity history;auto first=history.apply(fit,1000,1,3,1);
        auto held=history.apply(monocular(scene.mono,"RGB only",scene.view.outputSize()),1033,1,3,2);
        check(prefix+"HELD_SCALE_NO_OLD_ANCHORS",held.ok()&&held.diagnostics["fusion_mode"]=="held_scale"
            &&held.diagnostics["used_native_indices"].toArray().isEmpty()&&held.globalReasons==QByteArray(1536,NoMeasurement)
            &&held.diagnostics["scale_reference_frame_id"]==1&&held.diagnostics["scale_reference_age_ms"]==33);
        scene.plane(4);auto next=history.apply(scene.fit(),1066,1,3,3);
        check(prefix+"SCALE_STEP_BOUNDED",next.ok()&&next.diagnostics["scale"].toDouble()>first.diagnostics["scale"].toDouble()
            &&next.diagnostics["scale"].toDouble()<=first.diagnostics["scale"].toDouble()*1.100001);
        for(int cause=0;cause<3;++cause){ScaleContinuity h;h.apply(fit,1000,1,3,1);
            auto reset=h.apply(monocular(scene.mono,"RGB only",scene.view.outputSize()),cause==0?999:1033,cause==1?2:1,cause==2?4:3,2);
            check(prefix+QString("SCALE_RESET_%1").arg(cause),reset.ok()&&reset.diagnostics["fusion_mode"]=="monocular"&&reset.diagnostics["scale"]==1.);
        }
        const auto payload=RvDepthDiagnostic::encode(scene.mono,bounded,256);
        check(prefix+"DIAGNOSTIC_FIXED_SLOTS",RvDepthDiagnostic::validPayload(payload,scene.view.depthBytes())
            &&payload.mid(scene.view.depthBytes()+256,1280)==QByteArray(1280,0));
    }
    Scene scene;const auto fused=scene.fit();const auto visible=limitTofFieldOfView(fused,scene.cal,scene.view);
    auto m=scene.tof.metadata;m["width"]=scene.view.outputWidth;m["height"]=scene.view.outputHeight;m["depth_unit"]="camera-Z_m_float32";
    m["assist_graph_sha256"]=scene.view.graphSha;m["depth_K"]=RvCameraView::intrinsics(scene.view);
    m["assist_provisional_model"]=true;m["assist_candidate_parity_mean_limit"]=scene.view.parityMeanLimit;
    m["assist_input_rotation_degrees"]=0;m["assist_output_restored_to_calibrated_grid"]=true;
    m["assist_fusion"]=visible.diagnostics;m["assist_fusion_revision"]="global_scale_latest_native_rgb_v2";
    m["assist_output_policy"]=FieldOfViewPolicy;m["assist_diagnostic_native_count"]=256;
    QImage rgb(scene.view.outputWidth,scene.view.outputHeight,QImage::Format_RGB888);rgb.fill(QColor(90,100,110));
    RvDepthDiagnostic::Snapshot snapshot{{visible.depth,{},rgb,m,QDateTime::currentDateTimeUtc()},scene.tof,RvDepthDiagnostic::encode(scene.mono,visible,256),{}};
    check("H_DIAGNOSTIC_MATCHED",snapshot.valid());
    QTemporaryDir temporary(QFileInfo(path).absolutePath()+"/h-diagnostic-SIMULATED-XXXXXX");temporary.setAutoRemove(false);
    QString folder,second;const bool saved=snapshot.save(temporary.path(),&folder);
    check("H_DIAGNOSTIC_EXPLICIT_SAVE",saved&&QFileInfo::exists(folder+"/points.csv")&&QFileInfo::exists(folder+"/tof.bin")&&!QFileInfo::exists(folder+"/legacy.f32"));
    check("H_DIAGNOSTIC_NEVER_OVERWRITES",snapshot.save(temporary.path(),&second)&&second!=folder);
    if(saved){
        const auto rp=temporary.filePath("replay.json");
        check("H_DIAGNOSTIC_OFFLINE_REPLAY",RvDepthDiagnostic::replay(folder,rp)==0);
        QFile f(rp);f.open(QIODevice::ReadOnly);const auto result=QJsonDocument::fromJson(f.readAll()).object();
        check("H_DIAGNOSTIC_REPRODUCES_METRES",result["replayed_correction"]=="H"&&result["repaired_vs_saved_max_m"].toDouble(100)<1e-5);
        const auto comparison=temporary.filePath("comparison");
        check("H_DIAGNOSTIC_EXPORT",RvDepthDiagnostic::replay(folder,temporary.filePath("comparison.json"),comparison)==0
            &&QFileInfo(comparison+"/current_depth.f32").size()==scene.view.depthBytes());
        check("H_DIAGNOSTIC_EXPORT_OVERWRITE_REJECTED",RvDepthDiagnostic::replay(folder,temporary.filePath("overwrite.json"),folder)==2
            &&RvDepthDiagnostic::replay(folder,temporary.filePath("existing.json"),comparison)==2);
        QFile csv(folder+"/points.csv");csv.open(QIODevice::ReadOnly);check("H_DIAGNOSTIC_NATIVE256_CSV",csv.readAll().count('\n')==257);
        QFile raw(folder+"/tof.bin");raw.open(QIODevice::Append);raw.write("!");raw.close();
        check("H_DIAGNOSTIC_NATIVE_TAMPER_REJECTED",RvDepthDiagnostic::replay(folder,temporary.filePath("tampered.json"))==2);
        QFile image(second+"/paired_rgb.png");image.open(QIODevice::Append);image.write("!");image.close();
        check("H_DIAGNOSTIC_RGB_TAMPER_REJECTED",RvDepthDiagnostic::replay(second,temporary.filePath("rgb_tampered.json"))==2);
    }
    auto mismatch=snapshot;mismatch.native.metadata["tof_sequence"]=8;
    check("H_DIAGNOSTIC_OTHER_MEASUREMENT_REJECTED",!mismatch.valid());
    for(const auto *old:{"unidepth","unidepth_wide","colorsr_rgb","depthor_v3_fp32"})
        check(QString("RETIRED_MODEL_REJECTED_")+old,!RvCameraView::isHostDepth(QLatin1String(old))&&!RvCameraView::supportsNativeGrid(QLatin1String(old),8,8));
    auto quantized=snapshot;
    auto &qm=quantized.output.metadata;
    const auto quantizedIdentity=RvInferenceRuntime::identity("openvino:INT8");
    for(auto it=quantizedIdentity.begin();it!=quantizedIdentity.end();++it)qm["assist_"+it.key()]=it.value();
    qm["assist_execution_device"]="openvino:INT8";qm["assist_backend"]="openvino-native";
    check("INT8_DIAGNOSTIC_EXPLICIT_IDENTITY",quantized.valid());
    auto wrongQuantized=quantized;wrongQuantized.output.metadata["assist_source_graph_sha256"]=QString(64,'0');
    check("INT8_DIAGNOSTIC_WRONG_SOURCE_REJECTED",!wrongQuantized.valid());
    wrongQuantized=quantized;wrongQuantized.output.metadata["assist_execution_device"]="cpu";
    check("INT8_DIAGNOSTIC_WRONG_DEVICE_REJECTED",!wrongQuantized.valid());
    wrongQuantized=quantized;wrongQuantized.output.metadata["assist_graph_sha256"]=scene.view.graphSha;
    check("INT8_DIAGNOSTIC_ORIGINAL_HASH_REJECTED",!wrongQuantized.valid());
    return all?0:2;
}
