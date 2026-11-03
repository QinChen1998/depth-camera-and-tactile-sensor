#include "RvAssistRecoveryTests.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvCameraView.h"
#include <QtCore/QTextStream>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonArray>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <memory>

namespace {
constexpr int W=576,H=324,N=W*H;
QJsonObject metadata(){return {{"model","SIMULATED_RECOVERY"},{"work_mode","rgb_tof"},{"generation",3},{"frame_id",7},
    {"boot_id","SIMULATED_BOOT"},{"tof_sequence",7},{"tof_timestamp_ns",1000000000.},{"tof_reused",false},
    {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},{"dtof_fov_correction",1},
    {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},{"dtof_source","tmf8829_ipc_first_peak"},
    {"dtof_coordinate","sensor_radial_native"},{"rgb_width",W},{"rgb_height",H},{"pair_delta_ms",10},{"pair_clock_uncertainty_ms",35},
    {"rectify_map_sha256",RvCameraView::Standard.mapSha},{"simulation",true},{"input_origin","SIMULATED"}};}
std::vector<float> decode(const QByteArray &b){std::vector<float> z(b.size()/4);for(int i=0;i<int(z.size());++i){auto bits=qFromLittleEndian<quint32>(b.constData()+4*i);std::memcpy(&z[i],&bits,4);}return z;}
struct Scene {
    bool wide=false;
    RvAssistFusion::Calibration cal;
    RvTofFrame tof{QByteArray(6144,0),metadata()};
    std::vector<float> mono=std::vector<float>(N,3),truth=mono;
    QImage rgb{W,H,QImage::Format_RGB888};
    explicit Scene(bool wideView=false):wide(wideView){cal.valid=true;tof.metadata["rectify_map_sha256"]=RvCameraView::profile(wide).mapSha;
        rgb.fill(QColor(90,100,110));for(int n=0;n<1536;++n)point(n,22+11*(n%48),20+9*(n/48),3);}
    void point(int n,double u,double v,double z){const double focal=RvCameraView::profile(wide).focal;
        const double x=(u-287.5)/focal,y=(v-161.5)/focal,norm=std::sqrt(x*x+y*y+1);
        cal.cameraRays[n]={x/norm,y/norm,1/norm};qToLittleEndian<quint16>(quint16(std::lround(z*norm/.00025)),tof.samples.data()+4*n);tof.samples[4*n+2]=30;tof.samples[4*n+3]=1;}
};
// Original B composition. The public residual entry deliberately retains its
// uncached dense solver, so this checks the optimized B execution independently.
RvAssistFusion::Result denseRecoveryReference(const Scene &scene,const RvTofFrame &tof,
        RvAssistFusion::ScaleContinuity *history,qint64 time,qint64 epoch,qint64 frame,
        RvAssistFusion::RecoveryVersion version=RvAssistFusion::RecoveryVersion::SurfaceV3){
    const double delta=tof.metadata["pair_delta_ms"].toDouble(1000),uncertainty=tof.metadata["pair_clock_uncertainty_ms"].toDouble();
    if(!std::isfinite(delta)||!std::isfinite(uncertainty)||uncertainty<0||uncertainty>35||std::abs(delta)+uncertainty>200){
        RvAssistFusion::Result invalid;invalid.error="Unmatched RGB/dToF frame or calibration";return invalid;
    }
    auto base=RvAssistFusion::fuse(scene.mono,scene.rgb,tof,scene.cal,scene.wide);
    if(history)base=history->apply(std::move(base),time,epoch,3,frame);
    if(!base.ok())return base;
    bool missing=false;for(char reason:base.localReasons)missing|=reason==RvAssistFusion::Boundary||reason==RvAssistFusion::GlobalOutlier
        ||reason==RvAssistFusion::GlobalRejected||reason==RvAssistFusion::WeakLocalGroup;
    if(!missing){base.diagnostics["recovery_applied"]=false;return base;}
    return RvAssistFusion::fuseResidual(scene.mono,scene.rgb,tof,scene.cal,scene.wide,nullptr,time,epoch,3,frame,&base,version);
}
bool sameResult(const RvAssistFusion::Result &a,const RvAssistFusion::Result &b){
    return a.depth==b.depth&&a.globalReasons==b.globalReasons&&a.localReasons==b.localReasons
        &&a.diagnostics==b.diagnostics&&a.error==b.error;
}
double regionError(const std::vector<float> &z,const std::vector<float> &truth,int x0,int x1,int y0,int y1){double e=0;for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)e+=std::abs(z[y*W+x]-truth[y*W+x]);return e/((x1-x0)*(y1-y0));}
}
int runRvAssistRecoveryTests(const QString &path){
    QFile report(path);if(!report.open(QIODevice::WriteOnly|QIODevice::Text))return 3;QTextStream stream(&report);bool all=true;
    const auto check=[&](const QString &name,bool ok){stream<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;all&=ok;};
    for(bool wide:{false,true}){
        auto near=std::make_unique<Scene>(wide);
        near->mono.assign(N,.2f);
        for(int n=0;n<1536;++n)near->point(n,22+11*(n%48),20+9*(n/48),.05);
        const auto bytes=near->tof.samples;
        const QString prefix=wide?"NEAR_WIDE_":"NEAR_STANDARD_";
        for(const QString mode:{"A","B"}){
            const auto result=mode=="A"?RvAssistFusion::fuse(near->mono,near->rgb,near->tof,near->cal,wide):
                RvAssistFusion::fuseRecovering(near->mono,near->rgb,near->tof,near->cal,wide);
            const auto z=decode(result.depth);
            check(prefix+mode+"_FIVE_CM_PLANE_CORRECTED",result.ok()&&result.diagnostics["dtof_corrected"].toBool()
                &&result.diagnostics["used_native_indices"].toArray().size()>1000&&z.size()==N&&std::abs(z[160*W+280]-.05)<.002);
            check(prefix+mode+"_NEAR_AGREEMENT_COUNTS_MEASUREMENTS",
                RvAssistFusion::agreement(near->mono,result,near->tof,near->cal,wide)["all"].toObject()["points"].toInt()>1000);
            check(prefix+mode+"_NEAR_SUPPORT_VALIDATION_ACCEPTS_POINTS",
                RvAssistFusion::limitTofSupport(result,near->tof,near->cal,wide).ok());
        }
        auto boundary=near->tof;
        const std::array<quint16,8> ranges={0,39,40,41,399,400,40000,40001};
        for(int i=0;i<int(ranges.size());++i)qToLittleEndian<quint16>(ranges[i],boundary.samples.data()+4*i);
        boundary.samples[4*8+2]=5;boundary.samples[4*9+3]=0;
        const auto result=RvAssistFusion::fuseRecovering(near->mono,near->rgb,boundary,near->cal,wide);
        bool accepted=true,rejected=true;
        for(int n:{2,3,4,5,6})accepted&=result.globalReasons[n]!=RvAssistFusion::Quality&&result.localReasons[n]!=RvAssistFusion::Quality;
        for(int n:{0,1,7,8})rejected&=result.globalReasons[n]==RvAssistFusion::Quality&&result.localReasons[n]==RvAssistFusion::Quality;
        check(prefix+"ONE_CM_INCLUSIVE_UPPER_LIMIT_AND_QUALITY_GUARDS",result.ok()&&accepted&&rejected
            &&result.globalReasons[9]==RvAssistFusion::Invalid&&result.localReasons[9]==RvAssistFusion::Invalid&&near->tof.samples==bytes);
        // A thin near surface missed by the global fit must reach B's recovery,
        // including its native-group and ownership evidence, in both views.
        auto finger=std::make_unique<Scene>(wide);
        for(int y=40;y<270;++y)for(int x=186;x<190;++x){finger->mono[y*W+x]=1.2f;finger->truth[y*W+x]=.05f;}
        for(int row=3;row<28;++row)finger->point(row*48+15,187.25,20+9*row,.05);
        const auto original=RvAssistFusion::fuse(finger->mono,finger->rgb,finger->tof,finger->cal,wide);
        const auto recovered=RvAssistFusion::fuseRecovering(finger->mono,finger->rgb,finger->tof,finger->cal,wide);
        const auto oldZ=decode(original.depth),newZ=decode(recovered.depth);
        check(prefix+"B_RECOVERS_FIVE_CM_THIN_SURFACE",recovered.ok()&&recovered.diagnostics["recovery_anchors"].toInt()>=20
            &&regionError(newZ,finger->truth,186,190,80,240)<regionError(oldZ,finger->truth,186,190,80,240)*.5);
    }
    for(bool wide:{false,true}){
        const QString prefix=wide?"B_WIDE_":"B_STANDARD_";
        RvAssistFusion::ScaleContinuity denseHistory,optimizedHistory,legacyHistory,legacyReferenceHistory;qint64 frame=0;
        const auto equivalent=[&](const Scene &scene,const RvTofFrame &tof,qint64 time,qint64 epoch=1,bool largeSupport=false){
            ++frame;const auto reference=denseRecoveryReference(scene,tof,&denseHistory,time,epoch,frame);
            const auto optimized=RvAssistFusion::fuseRecovering(scene.mono,scene.rgb,tof,scene.cal,wide,&optimizedHistory,time,epoch,3,frame);
            const int support=optimized.diagnostics["recovery_direct_support_pixels"].toInt()
                +optimized.diagnostics["recovery_propagated_support_pixels"].toInt();
            return sameResult(reference,optimized)&&(!largeSupport||support>N/2);
        };
        Scene plane(wide);
        for(int n=0;n<1536;++n)plane.point(n,22+11*(n%48),20+9*(n/48),2.4);
        check(prefix+"NO_RECOVERY_FAST_PATH_IS_EXACT",equivalent(plane,plane.tof,1000));
        Scene thin(wide);
        for(int y=40;y<270;++y)for(int x=186;x<190;++x)thin.mono[y*W+x]=1.2f;
        for(int row=3;row<28;++row)thin.point(row*48+15,187.25,20+9*row,.4);
        check(prefix+"SPARSE_RECOVERY_WITH_SMOOTHED_SCALE_IS_EXACT",equivalent(thin,thin.tof,1066));
        auto edge=thin;
        for(int y=0;y<40;++y)for(int x=186;x<190;++x)edge.mono[y*W+x]=float(std::exp(std::log(3.)+(std::log(1.2)-std::log(3.))*y/39));
        for(int row=5;row<28;++row)edge.point(row*48+15,189.,20+9*row,.4);
        check(prefix+"INTEGER_H_CORNERS_AT_FIXED_GEOMETRY_EDGE_ARE_EXACT",equivalent(edge,edge.tof,1132));
        auto spill=thin;for(int row=3;row<28;++row)spill.point(row*48+14,176,20+9*row,.4);
        check(prefix+"BOUNDARY_REJECTION_AND_POINT_IDS_ARE_EXACT",equivalent(spill,spill.tof,1198));
        Scene pinned(wide);for(int n=0;n<1536;++n)pinned.point(n,100+4*(n%48),70+4*(n/48),3);
        for(int row=12;row<17;++row)for(int col=20;col<25;++col)pinned.point(row*48+col,100+4*col,70+4*row,1);
        check(prefix+"PINNED_LEGACY_SAMPLES_ARE_EXACT",equivalent(pinned,pinned.tof,1264));
        Scene dense(wide);
        for(int y=0;y<H;++y)for(int x=0;x<W;++x)dense.mono[y*W+x]=x<288?.4f:2.f;
        for(int n=0;n<1536;++n)dense.point(n,6+12*(n%48),20+9*(n/48),1);
        check(prefix+"LARGE_PACKED_RECOVERY_AND_HELD_REFERENCE_ARE_EXACT",equivalent(dense,dense.tof,7300,1,true));
        auto none=dense.tof;none.samples.fill(0);
        check(prefix+"NO_POINTS_AND_TEMPORAL_REFERENCE_ARE_EXACT",equivalent(dense,none,7366));
        auto bad=thin.tof;bad.metadata["pair_delta_ms"]=190;
        check(prefix+"INVALID_PAIR_PRESERVES_HISTORY_AND_ERROR",equivalent(thin,bad,7432));
        check(prefix+"POST_REJECTION_RECOVERY_IS_EXACT",equivalent(thin,thin.tof,7498));
        check(prefix+"EPOCH_RESET_IS_EXACT",equivalent(thin,thin.tof,8000,2));
        const auto global=RvAssistFusion::fuseGlobalScale(plane.mono,plane.rgb,plane.tof,plane.cal,wide);
        const auto original=RvAssistFusion::fuse(plane.mono,plane.rgb,plane.tof,plane.cal,wide);
        const auto gz=decode(global.depth);
        check(prefix+"G_USES_SAME_ROBUST_SCALE_AND_METRIC_PLANE",global.ok()&&global.diagnostics["scale"]==original.diagnostics["scale"]
            &&global.diagnostics["global_used_native_indices"]==original.diagnostics["global_used_native_indices"]
            &&global.diagnostics["local_used_native_indices"].toArray().isEmpty()
            &&std::abs(gz[160*W+280]-2.4)<.002);
        const auto scalar=RvAssistFusion::fuseGlobalScale(thin.mono,thin.rgb,thin.tof,thin.cal,wide);
        const auto scalarZ=decode(scalar.depth);const double scalarValue=scalar.diagnostics["scale"].toDouble();
        bool onlyScale=scalar.ok();for(int i=0;i<N;++i)onlyScale&=scalarZ[i]==float(thin.mono[i]*scalarValue);
        check(prefix+"G_DOES_NOT_CLAIM_OR_APPLY_LOCAL_RECOVERY",onlyScale&&scalar.diagnostics["fusion_mode"]=="global_scale"
            &&scalar.diagnostics["local_used_native_indices"].toArray().isEmpty()
            &&std::all_of(scalar.localReasons.begin(),scalar.localReasons.end(),[](char reason){return reason==RvAssistFusion::NoMeasurement;}));
        RvAssistFusion::ScaleContinuity globalHistory;
        const auto seeded=globalHistory.apply(global,1000,9,3,1);auto current=plane.mono;
        for(auto &v:current)v+=.25f;
        const auto between=globalHistory.apply(RvAssistFusion::monocular(current,"No new current ToF measurement"),1033,9,3,2);
        const auto updated=decode(between.depth);
        check(prefix+"G_NEW_RGB_UPDATES_BETWEEN_MEASUREMENTS_WITH_HONEST_HELD_SCALE",between.ok()
            &&between.diagnostics["fusion_mode"]=="held_scale"&&between.diagnostics["dtof_corrected"]==false
            &&between.diagnostics["used_native_indices"].toArray().isEmpty()
            &&updated[0]!=decode(seeded.depth)[0]
            &&updated[0]==float(current[0]*between.diagnostics["scale"].toDouble()));
        const auto reset=globalHistory.apply(RvAssistFusion::monocular(current,"No current ToF"),2101,10,3,3);
        check(prefix+"G_NEW_EPOCH_REVOKES_HISTORICAL_SCALE",reset.ok()
            &&reset.diagnostics["scale"]==1.&&reset.diagnostics["dtof_corrected"]==false);
        auto wrong=thin.tof;wrong.metadata["rectify_map_sha256"]="wrong";
        check(prefix+"G_FOREIGN_CALIBRATION_FAILS_CLOSED",!RvAssistFusion::fuseGlobalScale(thin.mono,thin.rgb,wrong,thin.cal,wide).ok());
    }
    Scene flat;const auto original=RvAssistFusion::fuse(flat.mono,flat.rgb,flat.tof,flat.cal),same=RvAssistFusion::fuseRecovering(flat.mono,flat.rgb,flat.tof,flat.cal);
    check("B_NORMAL_SUPPORTED_PLANE_IS_BIT_EXACT_LEGACY",same.ok()&&same.depth==original.depth&&same.diagnostics["recovery_applied"]==false);
    Scene finger;
    for(int y=40;y<270;++y)for(int x=186;x<190;++x){finger.mono[y*W+x]=1.2f;finger.truth[y*W+x]=.4f;}
    for(int row=3;row<28;++row)finger.point(row*48+15,187.25,20+9*row,.4);
    const auto before=RvAssistFusion::fuse(finger.mono,finger.rgb,finger.tof,finger.cal),after=RvAssistFusion::fuseRecovering(finger.mono,finger.rgb,finger.tof,finger.cal);
    const auto bz=decode(before.depth),az=decode(after.depth);
    const double oldError=regionError(bz,finger.truth,186,190,80,240),newError=regionError(az,finger.truth,186,190,80,240);
    double anchorError=0;for(int row=5;row<26;++row){const int y=20+9*row;anchorError+=std::abs(.75*az[y*W+187]+.25*az[y*W+188]-.4);}anchorError/=21;
    stream<<"THIN_FINGER_METRICS "<<QJsonDocument(QJsonObject{{"legacy_mae_m",oldError},{"repaired_mae_m",newError},{"repaired_anchor_mae_m",anchorError},{"recovery_anchors",after.diagnostics["recovery_anchors"]}}).toJson(QJsonDocument::Compact)<<Qt::endl;
    check("B_THIN_FINGER_GROUP_RECOVERS_WITHOUT_FOOTPRINT_VETO",after.ok()&&after.diagnostics["recovery_applied"]==true&&newError<oldError*.2&&anchorError<.05);
    bool noLeak=true;for(int y=40;y<270;++y)for(int x:{185,190,196})noLeak&=az[y*W+x]==bz[y*W+x];
    check("B_RECOVERY_FIXED_GEOMETRY_STOPS_AT_NARROW_FINGER_EDGE",noLeak&&std::all_of(az.begin(),az.end(),[](float z){return z>0&&std::isfinite(z);}));
    Scene spill=finger;
    for(int row=3;row<28;++row)spill.point(row*48+14,176,20+9*row,.4);
    const auto spillLegacy=RvAssistFusion::fuse(spill.mono,spill.rgb,spill.tof,spill.cal);
    const auto spillFixed=RvAssistFusion::fuseRecovering(spill.mono,spill.rgb,spill.tof,spill.cal);
    const auto sz=decode(spillFixed.depth),slz=decode(spillLegacy.depth);bool rejectedSpill=true,keptBackground=true;
    for(int row=3;row<28;++row)rejectedSpill&=spillFixed.localReasons[row*48+14]==RvAssistFusion::Boundary;
    for(int y=40;y<270;++y)for(int x=150;x<186;++x)keptBackground&=sz[y*W+x]==slz[y*W+x];
    check("B_COHERENT_FOREGROUND_RETURNS_CANNOT_PULL_BACKGROUND_ACROSS_ZONE_EDGE",rejectedSpill&&keptBackground);
    check("B_SPILL_REJECTION_PRESERVES_GENUINE_THIN_FINGER_RECOVERY",regionError(sz,spill.truth,186,190,80,240)<oldError*.2);
    // A connected near return can extend beyond the old two-zone ownership
    // window. Its outer centres still lie on background in the RGB/mono grid.
    // The intermediate zones are measurements, even when local fitting rejects
    // them; dropping them must not sever the ownership evidence.
    Scene extendedSpill=finger;
    // The surfaces reconnect through a distant gradual transition. Global
    // connected-component equality is not evidence of local edge ownership.
    for(int y=0;y<40;++y)for(int x=186;x<190;++x)
        extendedSpill.mono[y*W+x]=float(std::exp(std::log(3.)+(std::log(1.2)-std::log(3.))*y/39));
    for(int row=3;row<28;++row)for(int col=12;col<15;++col)
        extendedSpill.point(row*48+col,154+11*(col-12),20+9*row,.4);
    const auto extendedBase=RvAssistFusion::fuse(extendedSpill.mono,extendedSpill.rgb,extendedSpill.tof,extendedSpill.cal);
    const auto extendedFixed=RvAssistFusion::fuseRecovering(extendedSpill.mono,extendedSpill.rgb,extendedSpill.tof,extendedSpill.cal);
    const auto ez=decode(extendedFixed.depth),eb=decode(extendedBase.depth);bool rejectedExtended=true,unchangedExtended=true;
    for(int row=3;row<28;++row)for(int col=12;col<15;++col)
        rejectedExtended&=extendedFixed.localReasons[row*48+col]==RvAssistFusion::Boundary;
    for(int y=50;y<260;++y)for(int x=145;x<182;++x)unchangedExtended&=ez[y*W+x]==eb[y*W+x];
    check("B_CONNECTED_NEAR_RETURNS_THREE_ZONES_FROM_FOREGROUND_KEEP_BACKGROUND",rejectedExtended&&unchangedExtended);
    check("B_EXTENDED_OWNERSHIP_GUARD_KEEPS_THIN_FOREGROUND_RECOVERY",regionError(ez,extendedSpill.truth,186,190,80,240)<oldError*.2);
    // A smooth monocular surface with a coherent new measured patch. Keep the
    // old anchors far away so a support edge is not a real depth/pinned edge.
    Scene smoothPatch;smoothPatch.tof.samples.fill(0);
    for(int row=0;row<3;++row)for(int col=0;col<8;++col)smoothPatch.point(row*48+col,30+8*col,30+8*row,3);
    for(int row=12;row<15;++row)for(int col=20;col<23;++col)smoothPatch.point(row*48+col,260+11*(col-20),140+9*(row-12),1);
    const auto patchFixed=RvAssistFusion::fuseRecovering(smoothPatch.mono,smoothPatch.rgb,smoothPatch.tof,smoothPatch.cal);
    const auto patchBase=decode(RvAssistFusion::fuse(smoothPatch.mono,smoothPatch.rgb,smoothPatch.tof,smoothPatch.cal).depth);
    const auto pz=decode(patchFixed.depth);
    double jump=0,patchAnchorError=0;
    for(int y=90;y<210;++y)for(int x=210;x<340;++x)for(int step:{1,W}){
        jump=std::max(jump,double(std::abs(pz[y*W+x]-pz[y*W+x+step])));
    }
    for(int row=12;row<15;++row)for(int col=20;col<23;++col)
        patchAnchorError+=std::abs(pz[(140+9*(row-12))*W+260+11*(col-20)]-1);
    stream<<"SMOOTH_RECOVERY_PATCH_METRICS "<<QJsonDocument(QJsonObject{{"max_adjacent_jump_m",jump},{"anchor_mae_m",patchAnchorError/9}}).toJson(QJsonDocument::Compact)<<Qt::endl;
    check("B_RECOVERY_SUPPORT_EDGE_RETURNS_CONTINUOUSLY_TO_BASE",patchFixed.ok()&&jump<.2);
    check("B_CONTINUOUS_RECOVERY_STILL_FITS_CURRENT_MEASUREMENTS",patchAnchorError/9<.1);
    check("B_CONTINUOUS_RECOVERY_KEEPS_DISTANT_UNSUPPORTED_BASE",pz[260*W+450]==patchBase[260*W+450]&&pz[80*W+180]==patchBase[80*W+180]);
    // Hold out a native spatial block BEFORE global fitting, local eligibility,
    // consensus and solving. Only the untouched copy supplies evaluation Z.
    auto held=finger.tof;QJsonArray heldIds;
    for(int row=11;row<13;++row)for(int col=14;col<17;++col){const int n=row*48+col;held.samples[4*n+3]=0;heldIds.append(n);}
    const auto heldLegacy=RvAssistFusion::fuse(finger.mono,finger.rgb,held,finger.cal);
    const auto heldRecovery=RvAssistFusion::fuseRecovering(finger.mono,finger.rgb,held,finger.cal);
    const auto hz=decode(heldRecovery.depth),hlz=decode(heldLegacy.depth);double he=0,hle=0;bool excluded=true;
    for(const auto id:heldIds){const int n=id.toInt();excluded&=!heldRecovery.diagnostics["global_used_native_indices"].toArray().contains(n)
        &&!heldRecovery.diagnostics["local_used_native_indices"].toArray().contains(n);}
    for(int row=11;row<13;++row){const int y=20+9*row;he+=std::abs(.75*hz[y*W+187]+.25*hz[y*W+188]-.4);
        hle+=std::abs(.75*hlz[y*W+187]+.25*hlz[y*W+188]-.4);}
    stream<<"SPATIAL_BLOCK_HOLDOUT_METRICS "<<QJsonDocument(QJsonObject{{"legacy_finger_mae_m",hle/2},{"repaired_finger_mae_m",he/2},{"excluded_native_ids",heldIds}}).toJson(QJsonDocument::Compact)<<Qt::endl;
    check("B_SPATIAL_BLOCK_HOLDOUT_NEVER_ENTERS_ANY_FIT_OR_LOCAL_SELECTION",excluded&&he<hle);
    Scene nearSupport;for(int n=0;n<1536;++n)nearSupport.point(n,100+4*(n%48),70+4*(n/48),3);
    for(int row=12;row<17;++row)for(int col=20;col<25;++col)nearSupport.point(row*48+col,100+4*col,70+4*row,1);
    const auto protectedLegacy=RvAssistFusion::fuse(nearSupport.mono,nearSupport.rgb,nearSupport.tof,nearSupport.cal);
    const auto protectedRecovery=RvAssistFusion::fuseRecovering(nearSupport.mono,nearSupport.rgb,nearSupport.tof,nearSupport.cal);
    const auto pl=decode(protectedLegacy.depth),pr=decode(protectedRecovery.depth);bool pinnedUnchanged=true;
    for(const auto id:protectedLegacy.diagnostics["local_used_native_indices"].toArray()){
        const int n=id.toInt();const auto &ray=nearSupport.cal.cameraRays[n];
        // Use the actual radial projection, including its integer-pixel phase.
        // Cancelling radius algebraically can round an exact grid coordinate
        // across an integer boundary and test a neighbouring, unpinned pixel.
        const double radius=nearSupport.tof.rangeUq2(n)*.00025,z=radius*ray[2]+nearSupport.cal.translation[2];
        const int x=int(574*(radius*ray[0]+nearSupport.cal.translation[0])/z+287.5),
                  y=int(574*(radius*ray[1]+nearSupport.cal.translation[1])/z+161.5);
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx)pinnedUnchanged&=pl[(y+dy)*W+x+dx]==pr[(y+dy)*W+x+dx];
    }
    check("B_RECOVERY_PRESERVES_ADOPTED_LEGACY_SAMPLES_ON_CONNECTED_GEOMETRY",protectedRecovery.diagnostics["recovery_applied"]==true&&pinnedUnchanged);
    Scene mixed;for(int y=0;y<H;++y)for(int x=0;x<W;++x){mixed.mono[y*W+x]=x<288?.4f:2.f;mixed.truth[y*W+x]=1;}
    for(int n=0;n<1536;++n)mixed.point(n,6+12*(n%48),20+9*(n/48),1);
    const auto rejected=RvAssistFusion::fuse(mixed.mono,mixed.rgb,mixed.tof,mixed.cal),recovered=RvAssistFusion::fuseRecovering(mixed.mono,mixed.rgb,mixed.tof,mixed.cal);
    const auto rz=decode(recovered.depth);
    check("B_GLOBAL_REJECTION_NO_LONGER_VETOES_ALL_CURRENT_LOCAL_POINTS",rejected.diagnostics["dtof_corrected"]==false&&recovered.ok()
        &&recovered.diagnostics["dtof_corrected"]==true&&recovered.diagnostics["global_fit_accepted"]==false&&recovered.diagnostics["recovery_anchors"].toInt()>24
        &&regionError(rz,mixed.truth,30,540,30,290)<regionError(decode(rejected.depth),mixed.truth,30,540,30,290));
    check("B_TRUE_BACKGROUND_WITH_DISTINCT_SCALE_REMAINS_CORRECTABLE",recovered.diagnostics["local_used_native_indices"].toArray().contains(16*48+32)
        &&std::abs(rz[164*W+390]-1)<.1);
    RvAssistFusion::ScaleContinuity history;
    const auto reference=RvAssistFusion::fuseRecovering(flat.mono,flat.rgb,flat.tof,flat.cal,false,&history,1000,1,3,1);
    const auto current=RvAssistFusion::fuseRecovering(mixed.mono,mixed.rgb,mixed.tof,mixed.cal,false,&history,7300,1,3,2);
    check("B_CURRENT_LOCAL_RECOVERY_DOES_NOT_REFRESH_HELD_GLOBAL_REFERENCE",reference.ok()&&current.ok()
        &&current.diagnostics["dtof_corrected"]==true&&current.diagnostics["global_fit_accepted"]==false
        &&current.diagnostics["scale_source"]=="held_dtof"&&current.diagnostics["scale_reference_age_ms"]==6300
        &&current.diagnostics["scale_reference_frame_id"]==1);
    auto sparse=mixed.tof;for(int n=1;n<1536;++n)sparse.samples[4*n+3]=0;
    const auto noGroup=RvAssistFusion::fuseRecovering(mixed.mono,mixed.rgb,sparse,mixed.cal);
    check("B_RECOVERY_SINGLE_UNSUPPORTED_POINT_CANNOT_FORM_A_GROUP",noGroup.ok()&&noGroup.diagnostics["dtof_corrected"]==false&&decode(noGroup.depth)==mixed.mono);
    auto noPoints=mixed.tof;noPoints.samples.fill(0);const auto no=RvAssistFusion::fuseRecovering(mixed.mono,mixed.rgb,noPoints,mixed.cal);
    check("B_RECOVERY_NO_POINTS_PRESERVES_COMPLETE_MONOCULAR_BASE",decode(no.depth)==mixed.mono&&no.localReasons.size()==1536);
    auto bad=finger.tof;bad.metadata["pair_delta_ms"]=190;
    check("B_RECOVERY_REJECTS_TIME_BUDGET_MISMATCH",!RvAssistFusion::fuseRecovering(finger.mono,finger.rgb,bad,finger.cal).ok());
    auto outlier=flat.tof;qToLittleEndian<quint16>(2000, outlier.samples.data()+4*600);
    const auto robust=RvAssistFusion::fuseRecovering(flat.mono,flat.rgb,outlier,flat.cal);
    check("B_RECOVERY_DOES_NOT_ADOPT_ISOLATED_OUTLIER",!robust.diagnostics["local_used_native_indices"].toArray().contains(600));
    for(int fov:{1,2})for(bool wide:{false,true}){
        auto tof=flat.tof;const auto &cal=RvAssistFusion::calibrationForFov(fov);tof.metadata["dtof_fov_correction"]=fov;tof.metadata["rectify_map_sha256"]=RvCameraView::profile(wide).mapSha;
        for(int n=0;n<1536;++n){const double r=(2-cal.translation[2])/cal.cameraRays[n][2];qToLittleEndian<quint16>(quint16(std::lround(r/.00025)),tof.samples.data()+4*n);}
        std::vector<float> mono(N,2);auto fixed=RvAssistFusion::fuseRecovering(mono,flat.rgb,tof,cal,wide);
        auto bounded=wide?RvAssistFusion::limitTofFieldOfView(fixed,cal,true):fixed;
        const auto z=decode(bounded.depth);check(QString("B_RECOVERY_F%1_WIDE%2_CONTINUOUS_GEOMETRIC_ONLY").arg(fov).arg(wide),bounded.ok()
            &&std::all_of(z.begin(),z.end(),[&](float v){return std::isfinite(v)&&(wide?v>=0:v>0);}));
    }
    // Snapshot/replay uses actual frozen geometry and synthetic radial returns.
    const auto &cal=RvAssistFusion::calibrationForFov(1);auto tof=flat.tof;
    for(int n=0;n<1536;++n){const double r=(2-cal.translation[2])/cal.cameraRays[n][2];qToLittleEndian<quint16>(quint16(std::lround(r/.00025)),tof.samples.data()+4*n);}
    std::vector<float> mono(N,2);const auto fused=RvAssistFusion::fuseRecovering(mono,flat.rgb,tof,cal);
    auto m=tof.metadata;m["width"]=W;m["height"]=H;m["depth_unit"]="camera-Z_m_float32";m["assist_host_model"]="unidepth";
    m["assist_graph_sha256"]=RvCameraView::Standard.graphSha;m["depth_K"]=RvCameraView::intrinsics(false);m["assist_input_rotation_degrees"]=0;
    m["assist_output_restored_to_calibrated_grid"]=true;m["assist_fusion"]=fused.diagnostics;m["assist_experiment"]="B";
    m["assist_fusion_revision"]="local_recovery_v3";
    RvDepthDiagnostic::Snapshot snapshot{{fused.depth,{},flat.rgb,m,QDateTime::currentDateTimeUtc()},tof,RvDepthDiagnostic::encode(mono,fused),{}};
    check("DIAGNOSTIC_PAYLOAD_IS_BOUNDED_AND_MATCHED",snapshot.valid()&&snapshot.payload.size()==RvDepthDiagnostic::PayloadBytes);
    QTemporaryDir temporary(QFileInfo(path).absolutePath()+"/diagnostic-SIMULATED-XXXXXX");QString folder,second;
    temporary.setAutoRemove(false);
    const bool saved=snapshot.save(temporary.path(),&folder);check("DIAGNOSTIC_EXPLICIT_SAVE_COMPLETE_BUNDLE",saved&&QFileInfo::exists(folder+"/points.csv")&&QFileInfo::exists(folder+"/paired_rgb.png")&&QFileInfo::exists(folder+"/tof.bin"));
    check("DIAGNOSTIC_REPEATED_SAVES_NEVER_OVERWRITE",snapshot.save(temporary.path(),&second)&&second!=folder);
    stream<<"DIAGNOSTIC_SIMULATED_REPLAY_FOLDER "<<second<<Qt::endl;
    if(saved){
        check("DIAGNOSTIC_OFFLINE_REPLAY_WITHOUT_MODEL_OR_DEVICE",RvDepthDiagnostic::replay(folder,temporary.filePath("replay.json"))==0);
        QFile f(temporary.filePath("replay.json"));f.open(QIODevice::ReadOnly);const auto r=QJsonDocument::fromJson(f.readAll()).object();
        check("DIAGNOSTIC_B_REPLAY_REPRODUCES_RECORDED_NUMBERS",r["repaired_vs_saved_max_m"].toDouble(100)<1e-5);
        auto scalarSnapshot=snapshot;const auto scalar=RvAssistFusion::fuseGlobalScale(mono,flat.rgb,tof,cal);
        scalarSnapshot.output.metresFloat32LE=scalar.depth;scalarSnapshot.output.metadata["assist_experiment"]="G";
        scalarSnapshot.output.metadata["assist_fusion_revision"]="global_scale_rgb_v1";
        scalarSnapshot.output.metadata["assist_fusion"]=scalar.diagnostics;
        scalarSnapshot.payload=RvDepthDiagnostic::encode(mono,scalar);QString scalarFolder;
        const bool scalarSaved=scalarSnapshot.save(temporary.path(),&scalarFolder);
        const auto scalarReport=temporary.filePath("global_replay.json");
        const bool scalarReplayed=scalarSaved&&RvDepthDiagnostic::replay(scalarFolder,scalarReport)==0;
        QFile scalarFile(scalarReport);scalarFile.open(QIODevice::ReadOnly);const auto gr=QJsonDocument::fromJson(scalarFile.readAll()).object();
        check("DIAGNOSTIC_G_REPLAYS_RECORDED_GLOBAL_ONLY_SCHEME",scalarReplayed&&gr["replayed_correction"]=="G"
            &&gr["repaired_vs_saved_max_m"].toDouble(100)<1e-5);
        auto hSnapshot=scalarSnapshot;
        const QJsonObject hIdentity{{"assist_experiment","H"},{"assist_fusion_revision","global_scale_latest_native_rgb_v1"},
            {"source_protocol","HDA1"},{"work_mode","rgb_tof"},{"dtof_present",true},{"tof_reused",true},
            {"pairing_policy",RvTofFrame::GlobalScaleReusePolicy},{"tof_warning_flags",0},{"pair_delta_ms",8.},
            {"pair_clock_uncertainty_ms",35.},{"pair_clock_source","rtcp_prft_board_realtime_monotonic"},
            {"rgb_timestamp_ns",tof.metadata["tof_timestamp_ns"].toDouble()-8000000.}};
        for(auto it=hIdentity.begin();it!=hIdentity.end();++it){hSnapshot.output.metadata[it.key()]=it.value();hSnapshot.native.metadata[it.key()]=it.value();}
        QString hFolder;const bool hSaved=hSnapshot.save(temporary.path(),&hFolder);const auto hReport=temporary.filePath("h_replay.json");
        const bool hReplayed=hSaved&&RvDepthDiagnostic::replay(hFolder,hReport)==0;
        QFile hFile(hReport);hFile.open(QIODevice::ReadOnly);const auto hr=QJsonDocument::fromJson(hFile.readAll()).object();
        check("DIAGNOSTIC_H_REPLAYS_EXPLICIT_REUSED_MEASUREMENT_AND_ORIGINAL_GLOBAL_FIT",hReplayed&&hr["replayed_correction"]=="H"
            &&hr["repaired_vs_saved_max_m"].toDouble(100)<1e-5);
        for(bool reuse:{false,true}){
            auto currentSnapshot=reuse?hSnapshot:scalarSnapshot;
            const QString revision=reuse?"global_scale_latest_native_rgb_v2":"global_scale_rgb_v2";
            currentSnapshot.output.metadata["assist_fusion_revision"]=revision;QString currentFolder;
            const auto currentReport=temporary.filePath(reuse?"h_v2_replay.json":"g_v2_replay.json");
            const bool currentReplayed=currentSnapshot.save(temporary.path(),&currentFolder)&&RvDepthDiagnostic::replay(currentFolder,currentReport)==0;
            QFile currentFile(currentReport);currentFile.open(QIODevice::ReadOnly);const auto cr=QJsonDocument::fromJson(currentFile.readAll()).object();
            check("DIAGNOSTIC_G_H_V2_REPLAY_SELECTS_RECORDED_GLOBAL_CONSENSUS_REVISION",currentReplayed
                &&cr["replayed_fusion_revision"]==revision&&cr["repaired_vs_saved_max_m"].toDouble(100)<1e-5);
            currentSnapshot.output.metadata["assist_fusion_revision"]="unknown_global_fit";QString unknownFolder;
            check("DIAGNOSTIC_GLOBAL_UNKNOWN_REVISION_REMAINS_FAIL_CLOSED",currentSnapshot.save(temporary.path(),&unknownFolder)
                &&RvDepthDiagnostic::replay(unknownFolder,temporary.filePath(reuse?"h_unknown.json":"g_unknown.json"))==2);
        }
        hSnapshot.output.metadata["tof_reused"]=false;
        check("DIAGNOSTIC_H_REJECTS_FORGED_REUSE_FLAG",!hSnapshot.valid());
        const auto comparison=temporary.filePath("comparison");
        check("DIAGNOSTIC_EXPLICIT_COMPARISON_EXPORTS_CURRENT_AND_A",RvDepthDiagnostic::replay(folder,temporary.filePath("comparison.json"),comparison)==0
              &&QFileInfo(comparison+"/current_depth.f32").size()==N*4&&QFileInfo(comparison+"/current_depth.png").exists()
              &&QFileInfo(comparison+"/a_depth.f32").size()==N*4);
        check("DIAGNOSTIC_COMPARISON_CANNOT_OVERWRITE_ORIGINAL_OR_EXISTING",RvDepthDiagnostic::replay(folder,temporary.filePath("original_export_rejected.json"),folder)==2
              &&RvDepthDiagnostic::replay(folder,temporary.filePath("existing_export_rejected.json"),comparison)==2);
        QFile csv(folder+"/points.csv");csv.open(QIODevice::ReadOnly);const auto data=csv.readAll();check("DIAGNOSTIC_CSV_HAS_EACH_NATIVE_ID_AND_REASONS",data.count('\n')==1537&&data.contains("global_reason,local_reason")&&data.contains("adopted"));
        QFile raw(folder+"/tof.bin");raw.open(QIODevice::Append);raw.write("!");raw.close();
        check("DIAGNOSTIC_REPLAY_TAMPER_FAILS_CLOSED",RvDepthDiagnostic::replay(folder,temporary.filePath("tampered.json"))==2);
        QString third;if(snapshot.save(temporary.path(),&third)){
            QFile image(third+"/paired_rgb.png");image.open(QIODevice::Append);image.write("!");image.close();
            check("DIAGNOSTIC_REPLAY_RGB_TAMPER_FAILS_CLOSED",RvDepthDiagnostic::replay(third,temporary.filePath("rgb_tampered.json"))==2);
        }else check("DIAGNOSTIC_REPLAY_RGB_TAMPER_FAILS_CLOSED",false);
    }
    auto mismatch=snapshot;mismatch.native.metadata["tof_sequence"]=8;check("DIAGNOSTIC_OTHER_MEASUREMENT_CANNOT_BE_SAVED",!mismatch.valid());
    mismatch=snapshot;mismatch.payload[0]=char(255);mismatch.payload[1]=char(255);mismatch.payload[2]=char(255);mismatch.payload[3]=char(127);
    check("DIAGNOSTIC_NONFINITE_MONOCULAR_PAYLOAD_REJECTED",!mismatch.valid());
    return all?0:2;
}
