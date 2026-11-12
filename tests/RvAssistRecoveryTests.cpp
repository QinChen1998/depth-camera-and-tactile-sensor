#include "RvAssistRecoveryTests.h"
#include "../sensors/RvAssistFusion.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvDepthDiagnostic.h"
#include <QtCore/QFile>
#include <QtCore/QTextStream>
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
        tof.metadata={{"work_mode","rgb_tof"},{"source_protocol","HDA1"},{"assist_experiment","B"},
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

    Result fit()const{
        return fuseGlobalScale(mono,tof,cal,view);
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
    check("B_REJECTS_WRONG_MODEL_FOV_OR_PROJECTION",!confused.fit().ok()&&!conflicting.fit().ok()&&!wrongProjection.fit().ok());
    auto invalidModel=scene;invalidModel.mono[0]=std::numeric_limits<float>::quiet_NaN();
    check("B_REJECTS_NONFINITE_MODEL",!invalidModel.fit().ok());

    GlobalRgbContinuity history;
    const auto global=history.apply(scene.fit(),1000,1,3,1);
    int projected=0;
    for(int id=0;id<scene.cal.nativeCount();++id){
        const auto point=projectNativePoint(scene.tof.rangeUq2(id)*.00025,id,scene.cal,view);
        const auto pixel=point.pixel+QPointF(.0065,.00725)*(view.focal/point.cameraZ);
        projected+=pixel.x()>=3&&pixel.x()<=476&&pixel.y()>=3&&pixel.y()<=636;
    }
    check("B_FRESH_PAIR_USES_CORRECTED_PROJECTION_AND_GLOBAL_SCALE_ONLY",
        global.ok()&&global.diagnostics["fusion_mode"]=="global_scale"
        &&global.diagnostics["tof_projected_in_view"]==projected&&scene.tof.samples==original
        &&global.diagnostics["local_used_native_indices"].toArray().isEmpty()
        &&global.localReasons==QByteArray(1536,NoMeasurement)
        &&std::abs(global.diagnostics["scale"].toDouble()-.18)<.0005);

    auto retired=scene;retired.tof.metadata["assist_experiment"]="G";
    auto late=scene;late.tof.metadata["pair_delta_ms"]=165.01;late.tof.metadata["rgb_timestamp_ns"]=834990000.;
    check("B_REJECTS_RETIRED_SCHEME_OR_OUT_OF_BUDGET_TOF",!retired.fit().ok()&&!late.fit().ok());
    auto warning=scene;warning.tof.metadata["tof_warning_flags"]=8;
    auto tinyGain=scene;std::fill(tinyGain.mono.begin(),tinyGain.mono.end(),100.f);
    check("B_CURRENT_WARNING_AND_GAIN_LIMITS",!warning.fit().ok()
        &&tinyGain.fit().diagnostics["fusion_mode"]=="monocular");
    const auto rgbOnly=monocular(scene.mono,"RGB only",view.outputSize());
    const auto between=history.apply(rgbOnly,7033,1,3,2);
    check("B_RGB_ONLY_KEEPS_LAST_SCALAR_AND_REFERENCE_AGE",between.ok()&&between.diagnostics["fusion_mode"]=="held_scale"
        &&between.diagnostics["used_native_indices"].toArray().isEmpty()&&between.diagnostics["scale_reference_age_ms"]==6033);
    auto far=scene;far.plane(1.8);
    const auto next=history.apply(far.fit(),7283,1,3,3);
    check("B_GAIN_UPDATE_IS_CAPPED",next.ok()&&next.diagnostics["scale_reference_frame_id"]==3
        &&std::abs(next.diagnostics["scale"].toDouble()/global.diagnostics["scale"].toDouble()-1.1)<1e-12);
    const auto reset=history.apply(rgbOnly,7316,1,4,4);
    check("B_NEW_GENERATION_SHOWS_RAW_MONOCULAR",reset.depth==rgbOnly.depth
        &&reset.diagnostics["fusion_mode"]=="monocular"&&reset.diagnostics["scale_reference_age_ms"]==-1);

    GlobalRgbContinuity historyB;
    historyB.apply(scene.fit(),1000,1,3,1);
    auto reusedB=scene;
    reusedB.tof.metadata["frame_id"]=2;
    reusedB.tof.metadata["rgb_timestamp_ns"]=1033000000.;
    reusedB.tof.metadata["pair_delta_ms"]=-33.;
    reusedB.tof.metadata["tof_reused"]=true;
    std::fill(reusedB.mono.begin(),reusedB.mono.end(),1.25f);
    const auto nextB=historyB.apply(reusedB.fit(),1033,1,3,2);
    check("B_REUSED_TOF_REFITS_THE_CURRENT_RGB",nextB.ok()&&nextB.diagnostics["dtof_corrected"]==true
        &&reusedB.tof.samples==scene.tof.samples&&nextB.diagnostics["scale_reference_frame_id"]==2
        &&std::abs(nextB.diagnostics["measured_scale"].toDouble()-.18/1.25)<.0005);
    auto lateB=reusedB;
    lateB.tof.metadata["pair_delta_ms"]=165.01;
    lateB.tof.metadata["rgb_timestamp_ns"]=834990000.;
    check("B_REUSE_STILL_REJECTS_EXCESS_TIMESTAMP_DELTA",!lateB.fit().ok());

    const auto visible=limitTofFieldOfView(nextB,scene.cal,view);
    const auto payload=RvDepthDiagnostic::encode(reusedB.mono,visible);
    check("B_VIEWPORT_PAYLOAD_HAS_POSITIVE_PREDICTION_AND_REASON_SLOTS",visible.ok()
        &&visible.depth.size()==view.depthBytes()&&RvDepthDiagnostic::validPayload(payload));
    return all?0:2;
}
