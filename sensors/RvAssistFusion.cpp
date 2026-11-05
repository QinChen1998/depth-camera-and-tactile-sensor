#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QtEndian>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtGui/QPainter>
#include <QtGui/QPolygonF>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <numeric>
#include <limits>

namespace RvAssistFusion {
namespace {
constexpr int W=576, H=324, GW=144, GH=81;
// These are algorithm guards, not claims about the sensor's accuracy.
constexpr int MinimumAnchors=12;
double median(std::vector<double> v) {
    const auto n=v.size(); std::nth_element(v.begin(),v.begin()+n/2,v.end());
    const double upper=v[n/2];
    return n%2 ? upper : (upper+*std::max_element(v.begin(),v.begin()+n/2))/2;
}
template<class T> double sample(const std::vector<T> &v,int w,int h,double x,double y) {
    x=std::clamp(x,0.0,double(w-1)); y=std::clamp(y,0.0,double(h-1));
    const int ix=int(x),iy=int(y),jx=std::min(ix+1,w-1),jy=std::min(iy+1,h-1);
    const double ax=x-ix,ay=y-iy;
    return (1-ay)*((1-ax)*v[iy*w+ix]+ax*v[iy*w+jx])
        +ay*((1-ax)*v[jy*w+ix]+ax*v[jy*w+jx]);
}
double readDouble(const QByteArray &bytes,int offset) {
    const quint64 bits=qFromLittleEndian<quint64>(bytes.constData()+offset);
    double value;std::memcpy(&value,&bits,8);return value;
}
struct Anchor { double x,y,z,ratio; int nativeIndex; };
// Recover the existing linear R*M transform from audited GEO2 unit rays.
// Remove the original normalization before evaluating M8 cell directions;
// no measured ranges or native index grids are interpolated.
std::array<std::array<double,3>,64> makeNative8Rays(int fov){
    using Ray=std::array<double,3>;
    std::array<Ray,64> result{};
    const auto &cal=calibrationForFov(fov);if(!cal.valid)return result;
    const double dx=((fov&3)-1.5)/40.,dy=(((fov>>2)&3)-1.5)/40.;
    const double x0=-23.5/36.-dx,y0=-15.5/32.-dy;
    const auto unnormalized=[&](int i,double x,double y){auto ray=cal.cameraRays[i];const double n=std::sqrt(x*x+y*y+1);for(auto &v:ray)v*=n;return ray;};
    const auto a=unnormalized(0,x0,y0),b=unnormalized(1,x0+1./36,y0),c=unnormalized(48,x0,y0+1./32);
    std::array<Ray,3> columns{};
    for(int k=0;k<3;++k){columns[0][k]=(b[k]-a[k])*36;columns[1][k]=(c[k]-a[k])*32;columns[2][k]=a[k]-columns[0][k]*x0-columns[1][k]*y0;}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double dot=0;for(int k=0;k<3;++k)dot+=columns[i][k]*columns[j][k];if(std::abs(dot-(i==j?1.:0.))>1e-8)return result;}
    for(int i=0;i<64;++i){const double x=(i%8-3.5)/6.-dx,y=(i/8-3.5)/8.-dy,n=std::sqrt(x*x+y*y+1);
        for(int k=0;k<3;++k)result[i][k]=(columns[0][k]*x+columns[1][k]*y+columns[2][k])/n;}
    return result;
}
std::array<std::array<double,3>,256> makeNative16Rays(int fov){
    using Ray=std::array<double,3>;
    std::array<Ray,256> result{};
    const auto &cal=calibrationForFov(fov);if(!cal.valid)return result;
    const double dx=((fov&3)-1.5)/40.,dy=(((fov>>2)&3)-1.5)/40.;
    const double x0=-23.5/36.-dx,y0=-15.5/32.-dy;
    const auto unnormalized=[&](int i,double x,double y){auto ray=cal.cameraRays[i];const double n=std::sqrt(x*x+y*y+1);for(auto &v:ray)v*=n;return ray;};
    const auto a=unnormalized(0,x0,y0),b=unnormalized(1,x0+1./36,y0),c=unnormalized(48,x0,y0+1./32);
    std::array<Ray,3> columns{};
    for(int k=0;k<3;++k){columns[0][k]=(b[k]-a[k])*36;columns[1][k]=(c[k]-a[k])*32;columns[2][k]=a[k]-columns[0][k]*x0-columns[1][k]*y0;}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double dot=0;for(int k=0;k<3;++k)dot+=columns[i][k]*columns[j][k];if(std::abs(dot-(i==j?1.:0.))>1e-8)return result;}
    for(int i=0;i<256;++i){const double x=(i%16-7.5)/12.-dx,y=(i/16-7.5)/16.-dy,n=std::sqrt(x*x+y*y+1);
        for(int k=0;k<3;++k)result[i][k]=(columns[0][k]*x+columns[1][k]*y+columns[2][k])/n;}
    return result;
}
bool nativeGridMatches(const RvTofFrame &tof,const Calibration &cal){
    if(tof.columns()!=cal.columns||tof.rows()!=cal.rows)return false;
    if(cal.columns==48&&cal.rows==32)return true;
    return cal.columns==16&&cal.rows==16&&RvCameraView::isUniDepthNative16(tof.metadata["assist_host_model"].toString());
}
struct ContiguousRows {
    struct Iterator {
        int index;
        constexpr int operator*() const {return index;}
        constexpr Iterator &operator++() {++index;return *this;}
        constexpr bool operator!=(const Iterator &other) const {return index!=other.index;}
    };
    constexpr Iterator begin() const {return {0};}
    int count=W*H;
    constexpr Iterator end() const {return {count};}
};
}

bool loadCalibration(const QByteArray &bytes,Calibration &out,QString &error) {
    out={};
    if(bytes.size()!=281932 || (QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex() != "54802b37740d0075d8a9da95a0dcb5fde27b2f1e20249312a9f2325280c51729"
            && QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex() != "83b49ace957a4f70982380c6f22a7c39236764ff7311232d03724e85273324db")
        || qFromLittleEndian<quint32>(bytes.constData())!=0x47454f32
        || qFromLittleEndian<qint32>(bytes.constData()+4)!=144
        || qFromLittleEndian<qint32>(bytes.constData()+8)!=81
        || qFromLittleEndian<qint32>(bytes.constData()+44)!=32
        || qFromLittleEndian<qint32>(bytes.constData()+48)!=48) {
        error=QStringLiteral("Unsupported dToF calibration GEO2 contract");return false;
    }
    // Checked at runtime against the embedded, audited calibration fingerprint.
    const std::array<double,4> k={143.5,143.5,71.5,40.0};
    for(int i=0;i<4;++i)if(std::abs(readDouble(bytes,12+8*i)-k[i])>1e-10) {
        error=QStringLiteral("dToF calibration and rectified camera intrinsics differ");return false;
    }
    for(int i=0;i<3;++i)out.translation[i]=readDouble(bytes,84+8*i);
    for(int i=0;i<48*32;++i) {
        double norm=0;
        for(int c=0;c<3;++c) {
            const double value=readDouble(bytes,124+8*(i*3+c));
            if(!std::isfinite(value)) {error=QStringLiteral("Non-finite calibration ray");return false;}
            out.cameraRays[i][c]=value;norm+=value*value;
        }
        if(std::abs(norm-1)>1e-6) {error=QStringLiteral("Calibration rays must be unit radial directions");return false;}
    }
    for(double t:out.translation)if(!std::isfinite(t)) {error=QStringLiteral("Non-finite calibration translation");return false;}
    out.valid=true;error.clear();return true;
}

const Calibration &calibrationForFov(int fov) {
    const auto load=[](const QString &path){Calibration cal;QString error;QFile file(path);if(file.open(QIODevice::ReadOnly))loadCalibration(file.readAll(),cal,error);return cal;};
    static const Calibration f1=load(":/camera/dtof_m48_geometry.bin"),f2=load(":/camera/dtof_m48_geometry_f2.bin"),invalid;
    return fov==1?f1:fov==2?f2:invalid;
}

const Calibration &calibrationForNativeGrid(int fov,int columns,int rows){
    if(columns==48&&rows==32)return calibrationForFov(fov);
    const auto make=[](int f){Calibration out;out.columns=out.rows=16;
        const auto &base=calibrationForFov(f);const auto rays=makeNative16Rays(f);
        if(!base.valid||rays[0][2]<=0)return out;
        out.translation=base.translation;std::copy(rays.begin(),rays.end(),out.cameraRays.begin());out.valid=true;return out;};
    static const auto f1=make(1),f2=make(2),invalid=Calibration{};
    if(columns!=16||rows!=16)return invalid;
    return fov==1?f1:fov==2?f2:invalid;
}

const std::array<std::array<double,3>,64> &native8RaysForFov(int fov){
    static const auto f1=makeNative8Rays(1),f2=makeNative8Rays(2),empty=std::array<std::array<double,3>,64>{};
    return fov==1?f1:fov==2?f2:empty;
}

Result monocular(const std::vector<float> &mono,const char *reason,QSize size) {
    const int pixels=size.width()*size.height();
    Result result;
    result.globalReasons=result.localReasons=QByteArray(1536,NoMeasurement);
    if(mono.size()!=pixels) {result.error="Invalid monocular dimensions";return result;}
    for(float z:mono)if(!std::isfinite(z)||z<=0) {result.error="Invalid monocular camera-Z output";return result;}
    result.depth.resize(pixels*4);
    qToLittleEndian<quint32>(mono.data(),qsizetype(mono.size()),result.depth.data());
    result.diagnostics["fusion_mode"]="monocular";result.diagnostics["dtof_corrected"]=false;
    result.diagnostics["fallback_reason"]=QString::fromLatin1(reason);
    result.diagnostics["scale"]=1.0;result.diagnostics["local_factor_limit"]=1.0;
    result.diagnostics["used_native_indices"]=QJsonArray{};
    return result;
}

static Result fuseImpl(const std::vector<float> &mono,const QImage &camera,
            const RvTofFrame &tof,const Calibration &cal,const RvCameraView::Profile &view,bool globalOnly,QSize cameraSize,
            GlobalScaleFit globalFit=GlobalScaleFit::LegacyV1) {
    const int nativeCount=cal.nativeCount();
    const int width=view.outputWidth,height=view.outputHeight;
    Result result;
    result.globalReasons=result.localReasons=QByteArray(nativeCount,Invalid);
    const auto fail=[&](const char *message) {result.depth.clear();result.error=QString::fromLatin1(message);return result;};
    if(!cal.valid || mono.size()!=width*height || cameraSize!=QSize(width,height) || !tof.isValidForHostFusion() || !nativeGridMatches(tof,cal)
        || tof.metadata["rgb_width"].toInt()!=view.inputWidth
        || tof.metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha)
        || !tof.metadata["pair_delta_ms"].isDouble()
        || std::abs(tof.metadata["pair_delta_ms"].toDouble())>200)
        return fail("Unmatched RGB/dToF frame or calibration");
    for(float z:mono)if(!std::isfinite(z) || z<=0) return fail("Invalid monocular camera-Z output");
    // Only measurement support failures may fall back; frame/calibration and
    // model validity have already been checked. Temporal scaling is separate.
    const auto monocular=[&](const char *reason) {
        auto output=RvAssistFusion::monocular(mono,reason,cameraSize);
        // Preserve the already collected diagnostics of the paired path.
        for(auto it=output.diagnostics.begin();it!=output.diagnostics.end();++it)result.diagnostics[it.key()]=it.value();
        result.depth=std::move(output.depth);
        for(int i=0;i<result.globalReasons.size();++i){
            if(result.globalReasons[i]==Adopted)result.globalReasons[i]=GlobalRejected;
            if(result.localReasons[i]==Adopted)result.localReasons[i]=GlobalRejected;
        }
        return result;
    };
    std::vector<Anchor> anchors;
    anchors.reserve(nativeCount);
    int good=0,edgeRejected=0,projected=0;
    for(int i=0;i<nativeCount;++i) {
        const double radius=tof.rangeUq2(i)*.00025;
        if(!tof.valid(i))continue;
        if(tof.snr(i)<6 || radius<MinimumRadialMetres || radius>10){result.globalReasons[i]=result.localReasons[i]=Quality;continue;}
        ++good;
        const auto &q=cal.cameraRays[i];
        const double x=radius*q[0]+cal.translation[0],y=radius*q[1]+cal.translation[1];
        const double z=radius*q[2]+cal.translation[2];
        if(z<=0){result.globalReasons[i]=result.localReasons[i]=Outside;continue;}
        const double u=view.focal*x/z+view.cx(),v=view.focal*y/z+view.cy();
        if(u<3 || u>width-4 || v<3 || v>height-4){result.globalReasons[i]=result.localReasons[i]=Outside;continue;}
        ++projected;
        double low=mono[int(v)*width+int(u)],high=low;
        for(int dy=-3;dy<=3;dy+=3)for(int dx=-3;dx<=3;dx+=3) {
            const double d=mono[(int(v)+dy)*width+int(u)+dx];low=std::min(low,d);high=std::max(high,d);
        }
        if(high>1.4*low) {result.globalReasons[i]=result.localReasons[i]=Boundary;++edgeRejected;continue;}
        result.globalReasons[i]=result.localReasons[i]=GlobalRejected;
        anchors.push_back({u,v,z,std::log(z/sample(mono,width,height,u,v)),i});
    }
    result.diagnostics={{"tof_total_zones",nativeCount},{"tof_good_zones",good},{"tof_projected_in_view",projected},
        {"anchors_projected",int(anchors.size())},{"anchors_edge_rejected",edgeRejected}};
    if(anchors.size()<MinimumAnchors)return monocular("Too few reliable dToF anchors in the camera field of view");
    std::vector<double> ratios;for(const auto &a:anchors)ratios.push_back(a.ratio);
    // Estimate the common scale before rejecting discrepancies. Large uniform
    // monocular scale errors must not cause all valid ToF measurements to vanish.
    const double centre=median(ratios);
    std::vector<double> deviations;for(double r:ratios)deviations.push_back(std::abs(r-centre));
    const double mad=median(deviations);
    const double gate=std::clamp(3*1.4826*mad,std::log(1.15),std::log(1.75));
    std::vector<Anchor> inliers;std::vector<double> accepted;
    double minX=width,maxX=0,minY=height,maxY=0;
    std::set<int> occupied;
    int inconsistent=0,duplicates=0;
    for(const auto &a:anchors) {
        if(std::abs(a.ratio-centre)>gate){result.globalReasons[a.nativeIndex]=result.localReasons[a.nativeIndex]=GlobalOutlier;++inconsistent;continue;}
        const int cell=int(a.y/4)*GW+int(a.x/4);
        if(!occupied.insert(cell).second){result.globalReasons[a.nativeIndex]=result.localReasons[a.nativeIndex]=Duplicate;++duplicates;continue;}
        result.globalReasons[a.nativeIndex]=result.localReasons[a.nativeIndex]=Adopted;
        inliers.push_back(a);accepted.push_back(a.ratio);
        minX=std::min(minX,a.x);maxX=std::max(maxX,a.x);minY=std::min(minY,a.y);maxY=std::max(maxY,a.y);
    }
    result.diagnostics["anchors_inlier"]=int(inliers.size());result.diagnostics["log_ratio_mad"]=mad;
    result.diagnostics["anchors_inconsistent_rejected"]=inconsistent;
    result.diagnostics["anchors_duplicate_rejected"]=duplicates;
    result.diagnostics["anchor_span_x"]=maxX-minX;result.diagnostics["anchor_span_y"]=maxY-minY;
    const bool supported=inliers.size()>=MinimumAnchors&&inliers.size()>=.5*anchors.size()
        &&(maxX-minX>=32||maxY-minY>=24);
    double fitMad=mad;
    if(globalOnly&&globalFit==GlobalScaleFit::RetainedConsensusV2&&supported&&mad>std::log(1.3)){
        // Outliers and duplicate projections already cannot constrain scale.
        // They must not be the sole reason to discard a coherent retained
        // majority. Keep the original gate, count/coverage checks, point IDs,
        // and fitting arithmetic; do not retry legacy A/B or an accepted fit.
        const double retainedCentre=median(accepted);
        std::vector<double> retainedDeviations;retainedDeviations.reserve(accepted.size());
        for(double ratio:accepted)retainedDeviations.push_back(std::abs(ratio-retainedCentre));
        fitMad=median(std::move(retainedDeviations));
        result.diagnostics["global_consensus_recheck"]=true;
        result.diagnostics["retained_log_ratio_mad"]=fitMad;
        result.diagnostics["global_consensus_limit_log"]=std::log(1.3);
        result.diagnostics["global_consensus_accepted"]=fitMad<=std::log(1.3);
    }
    if(!supported||fitMad>std::log(1.3))
        return monocular("dToF anchors are inconsistent or have insufficient spatial coverage");
    const double logScale=median(accepted),scale=std::exp(logScale);
    if(!std::isfinite(scale) || scale<.02 || scale>50)return monocular("Unstable monocular scale fit");
    if(globalOnly){
        result.depth.resize(width*height*4);
        auto *depth=result.depth.data();
        for(int i=0;i<width*height;++i){const float z=float(mono[i]*scale);
            if(!std::isfinite(z)||z<=0)return fail("Non-finite fused depth");
            quint32 bits;std::memcpy(&bits,&z,4);qToLittleEndian(bits,depth+4*i);
        }
        QJsonArray used;for(const auto &a:inliers)used.append(a.nativeIndex);
        result.localReasons=QByteArray(nativeCount,NoMeasurement);
        result.diagnostics["scale"]=scale;result.diagnostics["local_factor_limit"]=1.;
        result.diagnostics["fusion_mode"]="global_scale";result.diagnostics["dtof_corrected"]=true;
        result.diagnostics["local_correction_disabled"]=true;
        result.diagnostics["used_native_indices"]=used;result.diagnostics["global_used_native_indices"]=used;
        result.diagnostics["local_used_native_indices"]=QJsonArray{};
        return result;
    }
    // A single scale parameter can be constrained by fewer samples than a 2D
    // residual field. Sparse but consistent coverage enables scale-only output;
    // it must never masquerade as a supported local correction field.
    const bool useLocal=inliers.size()>=24 && maxX-minX>=32 && maxY-minY>=24;
    if(!useLocal)for(const auto &a:inliers)result.localReasons[a.nativeIndex]=WeakLocalGroup;
    std::vector<float> lowMono(GW*GH),weights(GW*GH,0),rhs(GW*GH,0),field(GW*GH,0),next(GW*GH,0);
    const QImage guide=camera.convertToFormat(QImage::Format_RGB888).scaled(GW,GH,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    for(int y=0;y<GH;++y)for(int x=0;x<GW;++x)lowMono[y*GW+x]=float(std::log(sample(mono,width,height,4*x+1.5,4*y+1.5)));
    const double localLimit=std::log(1.35);
    for(const auto &a:inliers) {
        const double x=std::clamp((a.x+.5)/4-.5,0.0,double(GW-1)),y=std::clamp((a.y+.5)/4-.5,0.0,double(GH-1));
        const int ix=int(x),iy=int(y);
        const double residual=std::clamp(a.ratio-logScale,-localLimit,localLimit);
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
            const int j=std::min(iy+dy,GH-1)*GW+std::min(ix+dx,GW-1);
            const double w=2*(dx?x-ix:1-x+ix)*(dy?y-iy:1-y+iy);
            weights[j]+=float(w);rhs[j]+=float(w*residual);
        }
    }
    std::vector<std::array<float,4>> edges(GW*GH);
    const std::array<int,4> offsets={-1,1,-GW,GW};
    for(int y=0;y<GH;++y)for(int x=0;x<GW;++x) {
        const int i=y*GW+x;
        for(int n=0;n<4;++n) {
            if((n==0&&x==0)||(n==1&&x==GW-1)||(n==2&&y==0)||(n==3&&y==GH-1))continue;
            const int j=i+offsets[n];const auto *a=guide.constScanLine(y)+3*x;
            const auto *b=guide.constScanLine(j/GW)+3*(j%GW);
            double colour=0;for(int c=0;c<3;++c)colour+=std::abs(int(a[c])-int(b[c]))/(3.0*255);
            edges[i][n]=float(2*std::exp(-colour/.12-std::abs(lowMono[i]-lowMono[j])/.12));
        }
    }
    // Soft signed log residuals, not positive depths: the existing DepthPost
    // filter cannot represent these values. Regularization tends to zero away
    // from measurements and suppresses corrections across image/depth edges.
    for(int iteration=0;iteration<(useLocal?60:0);++iteration) {
        for(int i=0;i<GW*GH;++i) {
            double sum=rhs[i],denominator=.06+weights[i];
            for(int n=0;n<4;++n)if(edges[i][n]>0) {sum+=edges[i][n]*field[i+offsets[n]];denominator+=edges[i][n];}
            next[i]=float(std::clamp(sum/denominator,-localLimit,localLimit));
        }
        field.swap(next);
    }
    result.depth.resize(width*height*4);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const float z=float(mono[y*width+x]*scale*std::exp(sample(field,GW,GH,(x+.5)/4-.5,(y+.5)/4-.5)));
        if(!std::isfinite(z)||z<=0)return fail("Non-finite fused depth");
        quint32 bits;std::memcpy(&bits,&z,4);qToLittleEndian(bits,result.depth.data()+4*(y*width+x));
    }
    result.diagnostics["anchors_inlier"]=int(inliers.size());result.diagnostics["scale"]=scale;
    result.diagnostics["log_ratio_mad"]=mad;result.diagnostics["local_factor_limit"]=useLocal?1.35:1.0;
    result.diagnostics["fusion_mode"]=useLocal?"scale_local":"global_scale";
    result.diagnostics["dtof_corrected"]=true;
    // Display diagnostics only: retain the native identity of the anchors that
    // passed every existing gate. This does not alter any fit or depth value.
    QJsonArray used;for(const auto &a:inliers)used.append(a.nativeIndex);
    result.diagnostics["used_native_indices"]=used;
    result.diagnostics["global_used_native_indices"]=used;
    result.diagnostics["local_used_native_indices"]=useLocal?used:QJsonArray{};
    return result;
}

Result fuse(const std::vector<float> &mono,const QImage &camera,const RvTofFrame &tof,const Calibration &cal,bool wideView){
    return fuseImpl(mono,camera,tof,cal,RvCameraView::profile(wideView),false,camera.size());
}
Result fuseGlobalScale(const std::vector<float> &mono,const QImage &camera,const RvTofFrame &tof,const Calibration &cal,bool wideView,GlobalScaleFit fit){
    return fuseImpl(mono,camera,tof,cal,RvCameraView::profile(wideView),true,camera.size(),fit);
}
Result fuseGlobalScale(const std::vector<float> &mono,QSize cameraSize,const RvTofFrame &tof,const Calibration &cal,bool wideView,GlobalScaleFit fit){
    return fuseImpl(mono,QImage{},tof,cal,RvCameraView::profile(wideView),true,cameraSize,fit);
}

Result fuseGlobalScale(const std::vector<float> &mono,const RvTofFrame &tof,const Calibration &cal,
                       const RvCameraView::Profile &view,GlobalScaleFit fit){
    return fuseImpl(mono,QImage{},tof,cal,view,true,view.outputSize(),fit);
}

static Result fuseResidualImpl(const std::vector<float> &mono,const QImage &camera,const RvTofFrame &tof,
                    const Calibration &cal,bool wideView,ScaleContinuity *continuity,
                    qint64 measurementMs,qint64 epoch,qint64 generation,qint64 frameId,const Result *recoveryBase,
                    const Result *cachedGlobal,RecoveryVersion recoveryVersion) {
    constexpr int N=W*H;
    const int nativeCount=cal.nativeCount(),columns=cal.columns,rows=cal.rows;
    const bool surfaceRecovery=recoveryBase&&recoveryVersion==RecoveryVersion::SurfaceV3;
    auto result=monocular(mono,"No trustworthy current dToF anchors");
    const double delta=tof.metadata["pair_delta_ms"].toDouble(1000);
    const double uncertainty=tof.metadata["pair_clock_uncertainty_ms"].toDouble(0);
    if(!result.ok()||!cal.valid||camera.size()!=QSize(W,H)||!tof.isValid()||!nativeGridMatches(tof,cal)
       ||tof.metadata["rectify_map_sha256"]!=QLatin1String(RvCameraView::profile(wideView).mapSha)
       ||!std::isfinite(delta)||!std::isfinite(uncertainty)||uncertainty<0||uncertainty>35
       ||std::abs(delta)+uncertainty>200) {
        result.depth.clear();result.error="Unmatched RGB/dToF frame or calibration";return result;
    }
    // Geometry is taken ONLY from uncorrected monocular Z. RGB texture/shadow
    // attenuates an edge but can never disconnect a surface by itself.
    const auto guide=camera.convertToFormat(QImage::Format_RGB888);
    std::vector<float> logZ(N),right(N,0),down(N,0);
    for(int i=0;i<N;++i)logZ[i]=float(std::log(mono[i]));
    const auto affinity=[&](int i,int j) {
        const double geometry=std::abs(logZ[i]-logZ[j]);
        if(geometry>std::log(1.18))return 0.f;
        const auto *a=guide.constScanLine(i/W)+3*(i%W),*b=guide.constScanLine(j/W)+3*(j%W);
        double colour=0;for(int c=0;c<3;++c)colour+=std::abs(int(a[c])-int(b[c]))/(3.*255);
        return float(std::exp(-geometry/.08)*(.15+.85*std::exp(-colour/.12)));
    };
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){const int i=y*W+x;
        if(x+1<W)right[i]=affinity(i,i+1);if(y+1<H)down[i]=affinity(i,i+W);}
    const auto neighbours=[&](int i,const auto &visit) {
        if(i%W&&right[i-1]>0)visit(i-1,right[i-1]);if(i%W<W-1&&right[i]>0)visit(i+1,right[i]);
        if(i>=W&&down[i-W]>0)visit(i-W,down[i-W]);if(i<N-W&&down[i]>0)visit(i+W,down[i]);
    };
    const auto crossesGeometryEdge=[&](int x0,int y0,int x1,int y1){
        const int steps=std::max(std::abs(x1-x0),std::abs(y1-y0));int x=x0,y=y0;
        for(int k=1;k<=steps;++k){const int nx=int(std::lround(x0+double(x1-x0)*k/steps)),
                                                     ny=int(std::lround(y0+double(y1-y0)*k/steps));
            if(nx!=x&&right[y*W+std::min(x,nx)]<=0)return true;
            if(ny!=y&&down[std::min(y,ny)*W+nx]<=0)return true;
            x=nx;y=ny;
        }
        return false;
    };
    std::vector<int> component(N,-1),queue;queue.reserve(N);int components=0;
    for(int seed=0;seed<N;++seed)if(component[seed]<0){queue.clear();queue.push_back(seed);component[seed]=components;
        for(size_t k=0;k<queue.size();++k)neighbours(queue[k],[&](int j,float){if(component[j]<0){component[j]=components;queue.push_back(j);}});
        ++components;}
    struct Point {double x,y,z,ratio,weight,confidence=0,radius=0;int native,cell,part;
        std::array<int,4> ids;std::array<float,4> h;double target=0,purity=1,evidence=1,radiusLimit=8;bool mixed=false;};
    std::vector<Point> candidates,anchors;
    QByteArray localReasons(nativeCount,Invalid);
    std::set<int> legacyGlobal,legacyLocal;
    if(recoveryBase){
        if(!recoveryBase->ok()||recoveryBase->depth.size()!=N*4){result.depth.clear();result.error="Invalid recovery base";return result;}
        for(const auto id:recoveryBase->diagnostics["global_used_native_indices"].toArray())legacyGlobal.insert(id.toInt());
        for(const auto id:recoveryBase->diagnostics["local_used_native_indices"].toArray())legacyLocal.insert(id.toInt());
    }
    int valid=0,quality=0,outside=0,boundary=0,conflict=0,duplicates=0;
    double footprintSum=0,footprintMax=0;int footprints=0;
    std::set<int> occupied;
    const double focal=RvCameraView::profile(wideView).focal;
    struct Return {double x=0,y=0,z=0,mono=0;bool valid=false;};
    std::vector<Return> returns(nativeCount);
    std::vector<int> returnGroup(nativeCount,-1);
    if(surfaceRecovery){
        // Ownership evidence precedes local eligibility. A rejected mixed zone
        // must not break a connected near return into a fake background group.
        for(int n=0;n<nativeCount;++n){const double radius=tof.rangeUq2(n)*.00025;
            if(!tof.valid(n)||tof.snr(n)<6||radius<MinimumRadialMetres||radius>10)continue;
            const auto &q=cal.cameraRays[n];const double z=radius*q[2]+cal.translation[2];if(z<=0)continue;
            const double u=focal*(radius*q[0]+cal.translation[0])/z+287.5,v=focal*(radius*q[1]+cal.translation[1])/z+161.5;
            if(!std::isfinite(u)||!std::isfinite(v)||u<3||u>W-4||v<3||v>H-4)continue;
            returns[n]={u,v,z,sample(mono,W,H,u,v),true};
        }
        int groups=0;
        for(int seed=0;seed<nativeCount;++seed)if(returns[seed].valid&&returnGroup[seed]<0){
            queue.clear();queue.push_back(seed);returnGroup[seed]=groups;
            for(size_t k=0;k<queue.size();++k){const int n=queue[k];
                for(int step:{-1,1,-columns,columns}){const int j=n+step;
                    if(j<0||j>=nativeCount||(step==-1&&n%columns==0)||(step==1&&n%columns==columns-1)
                       ||!returns[j].valid||returnGroup[j]>=0)continue;
                    if(std::abs(std::log(returns[j].z/returns[n].z))<=std::log(1.2)){
                        returnGroup[j]=groups;queue.push_back(j);}
                }
            }
            ++groups;
        }
    }
    int ownershipRejected=0;
    std::vector<unsigned char> pinned(recoveryBase?N:0,0);
    if(recoveryBase)for(int n:legacyLocal){const double radius=tof.rangeUq2(n)*.00025;const auto &ray=cal.cameraRays[n];const double z=radius*ray[2]+cal.translation[2];
        const int x=int(focal*(radius*ray[0]+cal.translation[0])/z+287.5),y=int(focal*(radius*ray[1]+cal.translation[1])/z+161.5);
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx)if(x+dx>=0&&x+dx<W&&y+dy>=0&&y+dy<H)pinned[(y+dy)*W+x+dx]=1;
    }
    for(int n=0;n<nativeCount;++n){
        if(!tof.valid(n))continue;++valid;const double radius=tof.rangeUq2(n)*.00025;
        if(tof.snr(n)<6||radius<MinimumRadialMetres||radius>10){localReasons[n]=Quality;++quality;continue;}
        const auto &ray=cal.cameraRays[n];const double z=radius*ray[2]+cal.translation[2];
        const double u=focal*(radius*ray[0]+cal.translation[0])/z+287.5,v=focal*(radius*ray[1]+cal.translation[1])/z+161.5;
        if(!std::isfinite(u)||!std::isfinite(v)||z<=0||u<3||u>W-4||v<3||v>H-4){localReasons[n]=Outside;++outside;continue;}

        const int x=int(u),y=int(v),i=y*W+x;double lo=logZ[i],hi=lo;
        // One native-zone pitch is a conservative surface-association guard,
        // not a claimed optical PSF or a change to calibration. Derive it from
        // frozen adjacent rays at THIS radial distance (including parallax).
        // A fixed tiny RGB patch misses coarse-zone foreground/background mixes.
        double footprintX=2,footprintY=2;
        for(int step:{-1,1,-columns,columns}){
            const int j=n+step;if(j<0||j>=nativeCount||(step==-1&&n%columns==0)||(step==1&&n%columns==columns-1))continue;
            const auto &q=cal.cameraRays[j];const double zz=radius*q[2]+cal.translation[2];if(zz<=0)continue;
            footprintX=std::max(footprintX,std::abs(focal*(radius*q[0]+cal.translation[0])/zz+287.5-u));
            footprintY=std::max(footprintY,std::abs(focal*(radius*q[1]+cal.translation[1])/zz+161.5-v));
        }
        const int rx=int(std::ceil(footprintX))+1,ry=int(std::ceil(footprintY))+1;
        footprintSum+=std::max(rx,ry);footprintMax=std::max(footprintMax,double(std::max(rx,ry)));++footprints;
        if(rx>64||ry>64){localReasons[n]=Boundary;++boundary;continue;}
        const int x0=std::max(0,x-rx),x1=std::min(W-1,x+rx),y0=std::max(0,y-ry),y1=std::min(H-1,y+ry);
        for(int yy=y0;yy<=y1;++yy)for(int xx=x0;xx<=x1;++xx){lo=std::min(lo,double(logZ[yy*W+xx]));hi=std::max(hi,double(logZ[yy*W+xx]));}
        const bool mixed=hi-lo>std::log(1.18);
        if(mixed&&!recoveryBase){localReasons[n]=Boundary;++boundary;continue;}
        // A narrow surface may occupy only part of a coarse native footprint.
        // Recovery still requires the four H samples on one fixed surface and
        // a coherent connected native group; its propagation stays very local.
        double purity=1.;
        if(mixed){int matching=0;lo=hi=logZ[i];
            for(int yy=y0;yy<=y1;++yy)for(int xx=x0;xx<=x1;++xx){const int j=yy*W+xx;
                if(component[j]==component[i]&&std::abs(logZ[j]-logZ[i])<=std::log(1.18)){++matching;lo=std::min(lo,double(logZ[j]));hi=std::max(hi,double(logZ[j]));}}
            purity=double(matching)/((x1-x0+1)*(y1-y0+1));
        }
        const int cell=(y/3)*((W+2)/3)+x/3;
        if(!occupied.insert(cell).second){localReasons[n]=Duplicate;++duplicates;continue;}
        const double ax=u-x,ay=v-y;
        Point p{u,v,z,std::log(z/sample(mono,W,H,u,v)),
            std::min(1.,tof.snr(n)/20.)*std::exp(-std::pow((std::abs(delta)+uncertainty)/200.,2))
                *std::exp(-(hi-lo)/.08)*purity*double((x1-x0+1)*(y1-y0+1))/((2*rx+1)*(2*ry+1)),0,0,n,i,component[i],
            {i,i+1,i+W,i+W+1},{float((1-ax)*(1-ay)),float(ax*(1-ay)),float((1-ax)*ay),float(ax*ay)}};
        bool same=true;double sampledLow=logZ[i],sampledHigh=sampledLow;
        for(int k=0;k<4;++k){const int id=p.ids[k];same=same&&component[id]==p.part;
            if(p.h[k]>0){sampledLow=std::min(sampledLow,double(logZ[id]));sampledHigh=std::max(sampledHigh,double(logZ[id]));}}
        same=same&&sampledHigh-sampledLow<=std::log(1.18);
        if(!same){localReasons[n]=Boundary;++boundary;continue;}p.mixed=mixed;p.purity=purity;
        if(recoveryBase&&!legacyLocal.count(n)){
            // A coherent near return can spill onto the background side of a
            // coarse zone boundary. Consensus of those returns alone does not
            // establish ownership of the centre RGB pixel. Overlapping native
            // footprints observing the same Z but a nearer monocular surface
            // make this farther assignment ambiguous. This only rejects a NEW
            // local constraint: no pixel/ray is moved and true background away
            // from the ambiguous band can still receive a large correction.
            bool ambiguous=false;const double centreMono=sample(mono,W,H,u,v);
            for(int dy=-2;dy<=2&&!ambiguous;++dy)for(int dx=-2;dx<=2&&!ambiguous;++dx){
                const int col=n%columns+dx,row=n/columns+dy;if(col<0||col>=columns||row<0||row>=rows)continue;
                const int other=row*columns+col;const double rr=tof.rangeUq2(other)*.00025;
                if(other==n||!tof.valid(other)||tof.snr(other)<6||rr<MinimumRadialMetres||rr>10)continue;
                const auto &q=cal.cameraRays[other];const double zz=rr*q[2]+cal.translation[2];
                if(zz<=0||std::abs(std::log(zz/z))>std::log(1.2))continue;
                const double uu=focal*(rr*q[0]+cal.translation[0])/zz+287.5,vv=focal*(rr*q[1]+cal.translation[1])/zz+161.5;
                if(uu<3||uu>W-4||vv<3||vv>H-4||std::abs(uu-u)>2*rx||std::abs(vv-v)>2*ry)continue;
                ambiguous=sample(mono,W,H,uu,vv)*1.18<centreMono;
            }
            if(surfaceRecovery&&!ambiguous){
                // Inspect the same connected return over a bounded image
                // neighbourhood, rather than stopping at +/-2 native cells.
                // Require a local fixed-geometry edge along the image segment.
                // Global components can reconnect through a distant soft edge;
                // a gradual slope without a local edge cannot veto recovery.
                const double reach=std::min(48.,4.*std::max(footprintX,footprintY));
                for(int other=0;other<nativeCount&&!ambiguous;++other){const auto &q=returns[other];
                    if(other==n||!q.valid||returnGroup[other]!=returnGroup[n]||q.mono*1.18>=centreMono
                       ||std::hypot(q.x-u,q.y-v)>reach||std::abs(std::log(q.z/z))>std::log(1.2))continue;
                    ambiguous=crossesGeometryEdge(x,y,int(q.x),int(q.y));
                }
                if(ambiguous)++ownershipRejected;
            }
            if(ambiguous){localReasons[n]=Boundary;++boundary;continue;}
        }

        // Coherent samples on a narrow surface may bridge one native pitch.
        // The fixed geometry still blocks propagation across its silhouette;
        // a constant four-pixel radius leaves gaps even between valid zones.
        if(mixed)p.radiusLimit=std::max(4.,.5*(footprintX+footprintY)+2);
        localReasons[n]=WeakLocalGroup;candidates.push_back(p);
    }
    std::vector<int> candidateAt(nativeCount,-1);
    for(int i=0;i<int(candidates.size());++i)candidateAt[candidates[i].native]=i;
    std::vector<int> nativeGroup(candidates.size(),-1),nativeGroupSizes;
    if(recoveryBase)for(int seed=0;seed<int(candidates.size());++seed)if(nativeGroup[seed]<0){
        queue.clear();queue.push_back(seed);const int group=int(nativeGroupSizes.size());nativeGroup[seed]=group;
        for(size_t k=0;k<queue.size();++k){const auto &p=candidates[queue[k]];
            for(int step:{-1,1,-columns,columns}){const int n=p.native+step;
                if(n<0||n>=nativeCount||(step==-1&&p.native%columns==0)||(step==1&&p.native%columns==columns-1))continue;
                const int j=candidateAt[n];if(j<0||nativeGroup[j]>=0)continue;const auto &q=candidates[j];
                if(q.part==p.part&&std::abs(std::log(q.z/p.z))<=std::log(1.2)){nativeGroup[j]=group;queue.push_back(j);}}}
        nativeGroupSizes.push_back(int(queue.size()));
    }
    // A local consensus is independent of the global scale fit. Consistent
    // large residuals initialize themselves, rather than being Huber outliers
    // merely because the zero initialization is far from their surface.
    for(auto p:candidates){
        if(recoveryBase){
            bool supported=legacyLocal.count(p.native)!=0;
            for(int id:p.ids)supported=supported||pinned[id]!=0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int x=p.native%columns+dx,y=p.native/columns+dy;
                if(x<0||x>=columns||y<0||y>=rows||!legacyLocal.count(y*columns+x))continue;
                const int n=y*columns+x;const double radius=tof.rangeUq2(n)*.00025;const auto &ray=cal.cameraRays[n];
                const double z=radius*ray[2]+cal.translation[2];
                const int u=int(focal*(radius*ray[0]+cal.translation[0])/z+287.5),v=int(focal*(radius*ray[1]+cal.translation[1])/z+161.5);
                if(u>=0&&u<W&&v>=0&&v<H&&component[v*W+u]==p.part&&std::abs(std::log(z/p.z))<=std::log(1.2))supported=true;
            }
            if(supported){localReasons[p.native]=AlreadySupported;continue;}
            const int index=candidateAt[p.native];
            if(nativeGroupSizes[nativeGroup[index]]<3){localReasons[p.native]=WeakLocalGroup;continue;}
        }
        const auto sameNativeGroup=[&](const Point &q){return !recoveryBase||nativeGroup[candidateAt[q.native]]==nativeGroup[candidateAt[p.native]];};
        std::vector<double> ratios;double sum=0,minX=p.x,maxX=p.x,minY=p.y,maxY=p.y;
        for(const auto &q:candidates)if(q.part==p.part&&sameNativeGroup(q)&&std::hypot(q.x-p.x,q.y-p.y)<=48){ratios.push_back(q.ratio);}
        const double centre=median(ratios);std::vector<double> dev;for(double r:ratios)dev.push_back(std::abs(r-centre));
        const double gate=std::max(std::log(1.2),3*1.4826*median(dev));
        if(ratios.size()>=3&&std::abs(p.ratio-centre)>gate){localReasons[p.native]=LocalConflict;++conflict;continue;}
        int count=0;
        for(const auto &q:candidates)if(q.part==p.part&&sameNativeGroup(q)&&std::hypot(q.x-p.x,q.y-p.y)<=48&&std::abs(q.ratio-p.ratio)<=gate){
            // Count independent native evidence without applying the footprint
            // area penalty twice. The measurement weight still carries it.
            ++count;sum+=q.weight/(recoveryBase?std::max(q.purity,1e-6):1.);minX=std::min(minX,q.x);maxX=std::max(maxX,q.x);minY=std::min(minY,q.y);maxY=std::max(maxY,q.y);}
        // Independent cells, total evidence, and spread set strength/range.
        // One point acts only within four graph steps. No amplitude clamp.
        const double spread=std::hypot(maxX-minX,maxY-minY);
        p.confidence=std::min(1.,sum/3)*std::min(1.,count/3.)*(.25+.75*std::min(1.,spread/24))*std::exp(-median(dev)/.15);
        if(recoveryBase)p.evidence=std::max(1.,std::min(double(count),sum));
        p.radius=4+48*p.confidence*(count>1?1:0);
        if(recoveryBase&&(!surfaceRecovery||p.mixed))p.radius=std::min(p.radius,p.radiusLimit);
        localReasons[p.native]=Adopted;anchors.push_back(p);
    }
    // Preserve the reference GLOBAL estimate, independently of the stricter
    // LOCAL footprint/consensus above. Only its scalar is used, never its local
    // field or selected IDs. Global rejection cannot veto local constraints;
    // local boundary rejection cannot silently change the reference scale.
    const auto globalEstimate=cachedGlobal?*cachedGlobal:fuse(mono,camera,tof,cal,wideView);
    if(!globalEstimate.ok())return globalEstimate;
    if(recoveryBase&&anchors.empty()){
        result=*recoveryBase;result.diagnostics["recovery_applied"]=false;result.localReasons=localReasons;
        for(int n:legacyLocal)result.localReasons[n]=Adopted;
        if(surfaceRecovery){result.diagnostics["recovery_anchors"]=0;result.diagnostics["recovery_ownership_rejected"]=ownershipRejected;}
        return result;
    }
    const bool global=globalEstimate.diagnostics["dtof_corrected"].toBool();
    const double scale=global?globalEstimate.diagnostics["scale"].toDouble(1):1.;
    auto base=monocular(mono,"No current global scale support");
    for(int i=0;i<N;++i){float z=float(mono[i]*scale);quint32 bits;std::memcpy(&bits,&z,4);qToLittleEndian(bits,base.depth.data()+4*i);}
    base.diagnostics["scale"]=scale;base.diagnostics["dtof_corrected"]=global;
    if(continuity)base=continuity->apply(std::move(base),measurementMs,epoch,generation,frameId);
    if(recoveryBase)base=*recoveryBase;
    if(!base.ok())return base;
    const double applied=base.diagnostics["scale"].toDouble(1),logScale=std::log(applied);
    result=base;
    result.globalReasons=globalEstimate.globalReasons;result.localReasons=localReasons;
    std::vector<float> baseValues;
    if(recoveryBase){baseValues.resize(N);for(int i=0;i<N;++i){quint32 bits=qFromLittleEndian<quint32>(base.depth.constData()+4*i);std::memcpy(&baseValues[i],&bits,4);}}
    for(auto &a:anchors)a.target=recoveryBase?std::log(a.z/sample(baseValues,W,H,a.x,a.y)):a.ratio-logScale;
    // Support is an annotation, never a validity mask. These graph floods
    // cannot cross a fixed geometric boundary, including a one-pixel gap.
    std::vector<float> strength(N,0),seedWeight(N,0),field(N,0),mu(N,1),rhs(N,0),diagonal(N,1);
    std::vector<float> groupEvidence(recoveryBase?N:0,0);
    std::vector<int> stamp(N,-1),distance(N,0);std::vector<unsigned char> direct(N,0);
    QJsonArray used;
    for(size_t n=0;n<anchors.size();++n){const auto &p=anchors[n];used.append(p.native);
        for(int k=0;k<4;++k)if(p.h[k]>0)direct[p.ids[k]]=1;
        queue.clear();queue.push_back(p.cell);stamp[p.cell]=int(n);distance[p.cell]=0;
        for(size_t k=0;k<queue.size();++k){const int i=queue[k];const double d=distance[i];
            const float w=float(p.weight*p.confidence*std::pow(std::max(0.,1-d/(p.radius+1)),2));
            strength[i]+=w;seedWeight[i]+=w;field[i]+=w*float(p.target);
            if(recoveryBase)groupEvidence[i]+=w*float(p.evidence);
            if(d+1<=p.radius)neighbours(i,[&](int j,float){if(stamp[j]!=int(n)){

                stamp[j]=int(n);distance[j]=int(d)+1;queue.push_back(j);
            }});
        }
    }
    int supported=0,directCount=0;QJsonArray distribution;std::array<int,96> bins{};
    for(int i=0;i<N;++i){
        if(seedWeight[i]>0){field[i]/=seedWeight[i];++supported;++bins[(i/W/27)*8+(i%W/72)];}
        directCount+=direct[i]!=0;
        // A coherent independent group weakens the zero-correction prior;
        // footprint uncertainty remains in the measurement weight and range.
        const double evidence=recoveryBase&&seedWeight[i]>0?std::max(1.,double(groupEvidence[i]/seedWeight[i])):1.;
        // V3 lets the zero prior grow as support fades. The old .02 floor
        // leaves a finite correction right up to the hard support cut-off.
        mu[i]=strength[i]>0?float(.003/(std::max(surfaceRecovery?1e-6f:.02f,strength[i])*evidence)):1.f;
        if(recoveryBase&&pinned[i])field[i]=0;
    }
    for(int n:bins)distribution.append(n);
    // Matrix-free H'WH + fixed weighted Laplacian + zero prior. Bilinear H
    // is retained exactly (not four independent pseudo measurements).
    std::vector<int> activeRows;
    if(cachedGlobal&&recoveryBase&&!anchors.empty()) {
        std::vector<unsigned char> active(N,0);
        for(int i=0;i<N;++i)if(strength[i]>0&&!pinned[i])active[i]=1;
        // H can touch a row beyond the finite graph flood. Include every H
        // sample, even a zero-weight corner, rather than using support alone.
        bool touchesPinned=false;
        for(const auto &a:anchors)for(int id:a.ids) {
            if(pinned[id])touchesPinned=true;
            else active[id]=1;
        }
        if(!touchesPinned)for(int i=0;i<N;++i)if(active[i]) {
            activeRows.push_back(i);
            // Current B packs even large systems. Other callers keep the
            // original dense-kernel crossover and reference arithmetic.
            if(activeRows.size()>=N/2&&(!surfaceRecovery)){
                activeRows.clear();break;
            }
        }
    }
    // Current B's closed sparse system can use contiguous solver vectors.
    // Keep ascending pixel order, neighbour order and every H corner exactly.
    // Other schemes and the dense reference retain their original execution.
    const bool compact=surfaceRecovery&&!activeRows.empty();
    struct PackedEdges {std::array<int,4> index{};std::array<float,4> weight{};int count=0;};
    std::vector<PackedEdges> packedEdges;
    std::vector<float> packedDiagonal;
    const int solveSize=compact?int(activeRows.size()):N;
    if(compact){
        // The extra element is an immutable zero for pinned/outside neighbours.
        std::vector<int> mapping(N,solveSize);
        for(int k=0;k<solveSize;++k)mapping[activeRows[k]]=k;
        packedEdges.resize(solveSize);packedDiagonal.resize(solveSize+1,1);
        std::vector<float> packedMu(solveSize+1,1),packedField(solveSize+1,0);
        for(int k=0;k<solveSize;++k){const int i=activeRows[k];
            packedMu[k]=mu[i];packedField[k]=field[i];packedDiagonal[k]=mu[i];
            if(strength[i]>0)neighbours(i,[&](int j,float a){
                auto &edges=packedEdges[k];const int e=edges.count++;
                edges.index[e]=(pinned[j]||strength[j]<=0)?solveSize:mapping[j];
                edges.weight[e]=a;packedDiagonal[k]+=a;
            });
        }
        mu=std::move(packedMu);field=std::move(packedField);
        for(auto &a:anchors)for(int &i:a.ids)i=mapping[i];
        rhs.assign(solveSize+1,0);
    }
    const int vectorSize=compact?solveSize+1:N;
    std::vector<float> weights(anchors.size()),r(vectorSize),z(vectorSize),p(vectorSize),ap(vectorSize);
    int iterations=0;double relative=0;
    const auto solve=[&](const auto &rows) {
    for(int outer=0;outer<2&&!anchors.empty();++outer){
        std::fill(rhs.begin(),rhs.end(),0);diagonal=compact?packedDiagonal:mu;
        if(!compact)for(const int i:rows)if(strength[i]>0)neighbours(i,[&](int j,float a){if(surfaceRecovery||strength[j]>0)diagonal[i]+=a;});
        for(size_t n=0;n<anchors.size();++n){const auto &a=anchors[n];double prediction=0;
            for(int k=0;k<4;++k)prediction+=a.h[k]*field[a.ids[k]];
            const double target=a.target,error=std::abs(prediction-target);
            weights[n]=float(32*a.weight*a.confidence*(error>.15?.15/error:1));
            for(int k=0;k<4;++k){rhs[a.ids[k]]+=float(weights[n]*a.h[k]*target);diagonal[a.ids[k]]+=weights[n]*a.h[k]*a.h[k];}
        }
        if(recoveryBase&&!compact)for(int i=0;i<N;++i)if(pinned[i]){rhs[i]=0;diagonal[i]=1;}
        const auto multiply=[&](const std::vector<float> &v,std::vector<float> &out){
            if(compact){
                for(int i=0;i<solveSize;++i){
                    out[i]=mu[i]*v[i];const auto &edges=packedEdges[i];
                    for(int e=0;e<edges.count;++e)out[i]+=edges.weight[e]*(v[i]-v[edges.index[e]]);
                }
            }else for(const int i:rows){if(recoveryBase&&pinned[i]){out[i]=v[i];continue;}
                out[i]=mu[i]*v[i];if(strength[i]>0)neighbours(i,[&](int j,float a){
                    if(surfaceRecovery||strength[j]>0)out[i]+=a*(v[i]-((recoveryBase&&pinned[j])||(surfaceRecovery&&strength[j]<=0)?0:v[j]));});}
            for(size_t n=0;n<anchors.size();++n){const auto &a=anchors[n];float hv=0;for(int k=0;k<4;++k)hv+=a.h[k]*v[a.ids[k]];
                for(int k=0;k<4;++k)out[a.ids[k]]+=weights[n]*a.h[k]*hv;}
        };
        // Outside the closed active/H system all vectors stay exactly zero.
        // Ascending original indices preserve the serial reduction order of
        // every retained term; do not parallelize or reassociate this sum.
        const auto dot=[&rows](const std::vector<float> &a,const std::vector<float> &b){double s=0;for(const int i:rows)s+=double(a[i])*b[i];return s;};
        multiply(field,ap);for(const int i:rows){r[i]=rhs[i]-ap[i];p[i]=z[i]=r[i]/diagonal[i];}
        double rz=dot(r,z),norm=std::max(1e-20,dot(rhs,rhs));
        for(int step=0;step<100&&rz>1e-20;++step){
            multiply(p,ap);const double denominator=dot(p,ap);if(denominator<=0)break;
            const double alpha=rz/denominator;
            for(const int i:rows){field[i]+=float(alpha*p[i]);r[i]-=float(alpha*ap[i]);}
            ++iterations;relative=std::sqrt(dot(r,r)/norm);if(relative<1e-4)break;
            for(const int i:rows)z[i]=r[i]/diagonal[i];const double next=dot(r,z),beta=next/rz;rz=next;
            for(const int i:rows)p[i]=z[i]+float(beta*p[i]);
        }
    }
    };
    if(compact)solve(ContiguousRows{solveSize});
    else if(activeRows.empty())solve(ContiguousRows{});else solve(activeRows);
    if(compact){
        std::vector<float> expanded(N,0);
        for(int k=0;k<solveSize;++k)expanded[activeRows[k]]=field[k];
        field=std::move(expanded);
    }
    double before=0,smoothed=0,after=0,weightSum=0,maxFactor=1;
    std::vector<float> output(N);
    for(int i=0;i<N;++i){output[i]=float((recoveryBase?double(baseValues[i]):mono[i]*applied)*std::exp(field[i]));
        if(!std::isfinite(output[i])||output[i]<=0){result.depth.clear();result.error="Non-finite fused depth";return result;}
        quint32 bits;std::memcpy(&bits,&output[i],4);qToLittleEndian(bits,result.depth.data()+4*i);
        maxFactor=std::max(maxFactor,std::exp(std::abs(double(field[i]))));}
    for(const auto &a:anchors){const double m=sample(mono,W,H,a.x,a.y);before+=std::abs(m*scale-a.z);smoothed+=std::abs(m*applied-a.z);
        after+=std::abs(sample(output,W,H,a.x,a.y)-a.z);weightSum+=a.weight;}
    const double count=std::max(size_t(1),anchors.size());
    auto &d=result.diagnostics;
    if(!anchors.empty())d["fusion_mode"]="edge_residual_v1";else if(global)d["fusion_mode"]="global_scale";
    d["dtof_corrected"]=global||!anchors.empty();d["current_tof_correction_complete"]=global||!anchors.empty();
    std::set<int> participating;for(const auto id:globalEstimate.diagnostics["used_native_indices"].toArray())participating.insert(id.toInt());
    for(const auto &a:anchors)participating.insert(a.native);
    d["global_anchors_inlier"]=globalEstimate.diagnostics["anchors_inlier"].toInt();d["local_anchors_inlier"]=int(anchors.size());
    d["participating_unique_native_points"]=int(participating.size());
    d["global_used_native_indices"]=global?globalEstimate.diagnostics["used_native_indices"].toArray():QJsonArray{};
    d["local_used_native_indices"]=used;
    d["tof_total_zones"]=nativeCount;d["tof_valid_zones"]=valid;d["tof_good_zones"]=valid-quality;
    d["tof_quality_rejected"]=quality;d["tof_outside_view"]=outside;d["tof_projected_in_view"]=valid-quality-outside;
    d["anchors_edge_rejected"]=boundary;d["anchors_inconsistent_rejected"]=conflict;d["anchors_duplicate_rejected"]=duplicates;
    d["projection_footprint_mean_radius_px"]=footprintSum/std::max(1,footprints);d["projection_footprint_max_radius_px"]=footprintMax;
    d["anchors_projected"]=int(candidates.size());d["anchors_inlier"]=int(anchors.size());d["used_native_indices"]=used;
    d["global_fit_accepted"]=global;d["measured_scale"]=scale;d["scale"]=applied;d["anchor_weight_sum"]=weightSum;
    d["direct_support_pixels"]=directCount;d["propagated_support_pixels"]=supported-directCount;d["monocular_only_pixels"]=N-supported;
    d["support_grid_8x12"]=distribution;d["residual_grid"]=QJsonArray{W,H};d["solver_iterations"]=iterations;d["solver_relative_residual"]=relative;
    d["max_local_factor"]=maxFactor;d["adopted_global_raw_mae_m"]=before/count;d["adopted_global_smoothed_mae_m"]=smoothed/count;
    d["adopted_final_mae_m"]=after/count;d["agreement_reference"]="adopted_points_not_independent_ground_truth";
    if(recoveryBase){
        std::set<int> local=legacyLocal,all=legacyGlobal;
        for(const auto &a:anchors)local.insert(a.native);
        all.insert(local.begin(),local.end());QJsonArray localIds,allIds;
        for(int n:local){localIds.append(n);result.localReasons[n]=Adopted;}
        for(int n:all)allIds.append(n);
        d["fusion_mode"]="scale_local_recovery_v1";d["recovery_applied"]=true;d["recovery_anchors"]=int(anchors.size());
        d["used_native_indices"]=allIds;d["anchors_inlier"]=int(all.size());d["used_native_semantics"]="union";
        d["global_used_native_indices"]=recoveryBase->diagnostics["global_used_native_indices"];
        d["local_used_native_indices"]=localIds;d["local_anchors_inlier"]=int(local.size());d["participating_unique_native_points"]=int(all.size());
        d["recovery_direct_support_pixels"]=d.take("direct_support_pixels");d["recovery_propagated_support_pixels"]=d.take("propagated_support_pixels");
        d.remove("monocular_only_pixels");d["support_scope"]="recovery_only";
        double effective=0;for(const auto &a:anchors)effective=std::max(effective,a.evidence);
        d["recovery_max_effective_support"]=effective;
        d["recovery_preserved_pixels"]=int(std::count(pinned.begin(),pinned.end(),1));
        if(surfaceRecovery)d["recovery_ownership_rejected"]=ownershipRejected;
    }
    return result;
}

Result fuseResidual(const std::vector<float> &mono,const QImage &camera,const RvTofFrame &tof,
                    const Calibration &cal,bool wide,ScaleContinuity *continuity,
                    qint64 time,qint64 epoch,qint64 generation,qint64 frame,const Result *recoveryBase,
                    RecoveryVersion recoveryVersion) {
    // Shared residual reference used by B regression and diagnostic replay.
    return fuseResidualImpl(mono,camera,tof,cal,wide,continuity,time,epoch,generation,frame,recoveryBase,nullptr,recoveryVersion);
}

Result fuseRecovering(const std::vector<float> &mono,const QImage &camera,const RvTofFrame &tof,const Calibration &cal,bool wide,
                      ScaleContinuity *continuity,qint64 time,qint64 epoch,qint64 generation,qint64 frame){
    const double delta=tof.metadata["pair_delta_ms"].toDouble(1000),uncertainty=tof.metadata["pair_clock_uncertainty_ms"].toDouble();
    if(!std::isfinite(delta)||!std::isfinite(uncertainty)||uncertainty<0||uncertainty>35||std::abs(delta)+uncertainty>200){
        Result invalid;invalid.error="Unmatched RGB/dToF frame or calibration";return invalid;
    }
    auto base=fuse(mono,camera,tof,cal,wide);
    // Continuity changes scale/depth, never local eligibility. Avoid retaining
    // an extra full depth buffer when there is no recovery work to perform.
    bool missing=false;for(char reason:base.localReasons)missing|=reason==Boundary||reason==GlobalOutlier||reason==GlobalRejected||reason==WeakLocalGroup;
    if(!missing) {
        if(continuity)base=continuity->apply(std::move(base),time,epoch,generation,frame);
        if(!base.ok())return base;
        base.diagnostics["recovery_applied"]=false;return base;
    }
    // Keep the raw global reference separate from the temporally scaled base.
    const auto rawGlobal=base;
    if(continuity)base=continuity->apply(std::move(base),time,epoch,generation,frame);
    if(!base.ok())return base;
    return fuseResidualImpl(mono,camera,tof,cal,wide,nullptr,time,epoch,generation,frame,&base,&rawGlobal,RecoveryVersion::SurfaceV3);
}

QString pointReasonName(char reason){
    switch(reason){case Invalid:return "invalid_return";case Quality:return "snr_or_range";case Outside:return "outside_view";
    case Boundary:return "ambiguous_boundary";case GlobalOutlier:return "global_ratio_outlier";case Duplicate:return "duplicate_projection";
    case GlobalRejected:return "global_fit_rejected";case LocalConflict:return "local_conflict";case Adopted:return "adopted";
    case WeakLocalGroup:return "insufficient_local_group";case AlreadySupported:return "already_supported";default:return "no_measurement";}
}

Result limitTofSupport(Result result,const RvTofFrame &tof,const Calibration &cal,bool wideView) {
    const int nativeCount=cal.nativeCount(),columns=cal.columns,rows=cal.rows;
    if(!result.ok())return result;
    const auto fail=[&]{result.depth.clear();result.error=QStringLiteral("Invalid dToF output support contract");return result;};
    if(result.depth.size()!=W*H*4||!cal.valid||!tof.isValid()||!nativeGridMatches(tof,cal)
       ||tof.metadata["rectify_map_sha256"]!=QLatin1String(RvCameraView::profile(wideView).mapSha)
       ||!tof.metadata["pair_delta_ms"].isDouble()||std::abs(tof.metadata["pair_delta_ms"].toDouble())>200)return fail();
    result.diagnostics["output_policy"]=SupportPolicy;
    result.diagnostics["support_pixels"]=0;result.diagnostics["support_triangles"]=0;
    result.diagnostics["support_native_points"]=0;
    if(!result.diagnostics["dtof_corrected"].toBool()) {result.depth.fill(0);return result;}
    const auto ids=result.diagnostics["used_native_indices"].toArray();
    if(ids.isEmpty()||ids.size()!=result.diagnostics["anchors_inlier"].toInt(-1))return fail();
    struct Point {QPointF pixel;double z=0;bool valid=false;};
    std::vector<Point> points(nativeCount);std::set<int> used;
    const double focal=RvCameraView::profile(wideView).focal;
    for(const auto id:ids){
        const int i=id.toInt(-1);
        if(!id.isDouble()||id.toDouble()!=i||i<0||i>=nativeCount||!used.insert(i).second)return fail();
        const double r=tof.rangeUq2(i)*.00025;
        if(!tof.valid(i)||tof.snr(i)<6||r<MinimumRadialMetres||r>10)return fail();
        const auto &q=cal.cameraRays[i];const double z=r*q[2]+cal.translation[2];
        if(!std::isfinite(z)||z<=0)return fail();
        const QPointF p(focal*(r*q[0]+cal.translation[0])/z+287.5,focal*(r*q[1]+cal.translation[1])/z+161.5);
        if(!std::isfinite(p.x())||!std::isfinite(p.y())||p.x()<3||p.x()>W-4||p.y()<3||p.y()>H-4)return fail();
        points[i]={p,z,true};
    }
    QImage mask(W,H,QImage::Format_RGB32);mask.fill(Qt::black);
    QPainter painter(&mask);painter.setPen(Qt::NoPen);painter.setBrush(Qt::white);
    painter.setRenderHint(QPainter::Antialiasing,false);
    int triangles=0;std::set<int> connected;
    const auto triangle=[&](int a,int b,int c){
        if(!points[a].valid||!points[b].valid||!points[c].valid)return;
        const double low=std::min({points[a].z,points[b].z,points[c].z}),high=std::max({points[a].z,points[b].z,points[c].z});
        if(high>1.4*low)return;
        const auto ab=points[b].pixel-points[a].pixel,ac=points[c].pixel-points[a].pixel;
        if(std::abs(ab.x()*ac.y()-ab.y()*ac.x())<.01)return;
        // Translate integer-centred projection coordinates to raster centres.
        const QPointF half(.5,.5);
        painter.drawPolygon(QPolygonF{points[a].pixel+half,points[b].pixel+half,points[c].pixel+half});
        ++triangles;connected.insert(a);connected.insert(b);connected.insert(c);
    };
    for(int y=0;y<rows-1;++y)for(int x=0;x<columns-1;++x){const int a=y*columns+x;
        triangle(a,a+1,a+columns);triangle(a+1,a+columns+1,a+columns);}
    painter.end();int valid=0;
    for(int y=0;y<H;++y){const auto *line=reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        for(int x=0;x<W;++x){const int i=y*W+x;const quint32 bits=qFromLittleEndian<quint32>(result.depth.constData()+4*i);float z;std::memcpy(&z,&bits,4);
            if(!std::isfinite(z)||z<=0)return fail();
            if(qRed(line[x]))++valid;else qToLittleEndian<quint32>(0,result.depth.data()+4*i);
        }
    }
    result.diagnostics["support_pixels"]=valid;result.diagnostics["support_triangles"]=triangles;
    result.diagnostics["support_native_points"]=int(connected.size());
    return result;
}

Result limitTofFieldOfView(Result result,const Calibration &cal,bool wideView) {
    return limitTofFieldOfView(std::move(result),cal,RvCameraView::profile(wideView));
}

Result limitTofFieldOfView(Result result,const Calibration &cal,const RvCameraView::Profile &view) {
    const int width=view.outputWidth,height=view.outputHeight;
    if(!result.ok())return result;
    const auto fail=[&]{result.depth.clear();result.error=QStringLiteral("Invalid dToF output support contract");return result;};
    if(!cal.valid||result.depth.size()!=width*height*4)return fail();
    const std::array<int,4> corners={0,cal.columns-1,cal.nativeCount()-1,(cal.rows-1)*cal.columns};
    const auto &centre=cal.cameraRays[(cal.rows/2)*cal.columns+cal.columns/2];
    std::array<std::array<double,3>,4> planes{};
    for(int side=0;side<4;++side){
        const auto &a=cal.cameraRays[corners[side]],&b=cal.cameraRays[corners[(side+1)%4]];
        auto &n=planes[side];n={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        double length=0,direction=0;for(int c=0;c<3;++c){length+=n[c]*n[c];direction+=n[c]*centre[c];}
        if(!std::isfinite(length)||length<1e-12||!std::isfinite(direction)||std::abs(direction)<1e-9)return fail();
        const double factor=(direction>0?1.:-1.)/std::sqrt(length);for(auto &v:n)v*=factor;
    }
    for(double t:cal.translation)if(!std::isfinite(t))return fail();
    const double focal=view.focal;int pixels=0;
    // Detach at most once. Retained snapshots keep their original depth bytes.
    auto *depth=result.depth.data();
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const int i=y*width+x;const quint32 bits=qFromLittleEndian<quint32>(depth+4*i);float z;std::memcpy(&z,&bits,4);
        if(!std::isfinite(z)||z<=0)return fail();
        const std::array<double,3> ray={(x-view.cx())*z/focal-cal.translation[0],(y-view.cy())*z/focal-cal.translation[1],z-cal.translation[2]};
        bool inside=ray[0]*centre[0]+ray[1]*centre[1]+ray[2]*centre[2]>1e-9;
        for(const auto &n:planes)inside=inside&&(n[0]*ray[0]+n[1]*ray[1]+n[2]*ray[2]>=-1e-9);
        if(inside)++pixels;else qToLittleEndian<quint32>(0,depth+4*i);
    }
    result.diagnostics["output_policy"]=FieldOfViewPolicy;
    result.diagnostics["fov_pixels"]=pixels;
    result.diagnostics["fov_geometry_only"]=true;
    return result;
}

QJsonObject agreement(const std::vector<float> &mono,const Result &output,
                      const RvTofFrame &tof,const Calibration &cal,bool wideView) {
    const int nativeCount=cal.nativeCount();
    if(!output.ok()||output.depth.size()!=W*H*4||mono.size()!=W*H||!cal.valid
       ||!tof.isValidForHostFusion()||!nativeGridMatches(tof,cal))return {};
    std::vector<float> depth(W*H);
    for(int i=0;i<W*H;++i){const quint32 bits=qFromLittleEndian<quint32>(output.depth.constData()+4*i);std::memcpy(&depth[i],&bits,4);}
    struct Metrics {
        std::vector<double> before,after,relativeBefore,relativeAfter;
        double beforeBias=0,afterBias=0,measured=0;
        void add(double a,double b,double z){before.push_back(std::abs(a-z));after.push_back(std::abs(b-z));
            relativeBefore.push_back(std::abs(a-z)/z);relativeAfter.push_back(std::abs(b-z)/z);
            beforeBias+=a-z;afterBias+=b-z;measured+=z;}
        QJsonObject json()const{
            if(before.empty())return {{"points",0}};
            const auto mean=[](const std::vector<double>&v){double sum=0;for(double n:v)sum+=n;return sum/v.size();};
            return {{"points",int(before.size())},{"tof_mean_z_m",measured/before.size()},
                {"before_mae_m",mean(before)},{"after_mae_m",mean(after)},
                {"before_median_abs_relative",median(relativeBefore)},{"after_median_abs_relative",median(relativeAfter)},
                {"before_bias_m",beforeBias/before.size()},{"after_bias_m",afterBias/before.size()}};
        }
    } all,adopted,near,middle,far;
    std::set<int> used;for(const auto id:output.diagnostics["used_native_indices"].toArray())used.insert(id.toInt(-1));
    const double focal=RvCameraView::profile(wideView).focal;
    for(int i=0;i<nativeCount;++i){
        const double r=tof.rangeUq2(i)*.00025;if(!tof.valid(i)||tof.snr(i)<6||r<MinimumRadialMetres||r>10)continue;
        const auto &q=cal.cameraRays[i];const double z=r*q[2]+cal.translation[2];if(z<=0)continue;
        const double u=focal*(r*q[0]+cal.translation[0])/z+287.5,v=focal*(r*q[1]+cal.translation[1])/z+161.5;
        if(u<3||u>W-4||v<3||v>H-4)continue;
        const double a=sample(mono,W,H,u,v),b=sample(depth,W,H,u,v);
        if(!std::isfinite(a)||a<=0||!std::isfinite(b)||b<=0)continue;
        all.add(a,b,z);if(used.count(i))adopted.add(a,b,z);
        (z<.7?near:z<1.5?middle:far).add(a,b,z);
    }
    return {{"reference","current_projected_dtof_camera_z_not_independent_ground_truth"},
        {"all",all.json()},{"adopted",adopted.json()},{"near_under_0.7m",near.json()},
        {"middle_0.7_to_1.5m",middle.json()},{"far_over_1.5m",far.json()}};
}

Result ScaleContinuity::apply(Result result,qint64 nowMs,qint64 epoch,qint64 generation,qint64 frameId) {
    if(!result.ok()) {reset();return result;}
    if(initialized&&(epoch!=boundEpoch||generation!=boundGeneration||nowMs<lastMs))reset();
    const bool reliable=result.diagnostics["dtof_corrected"].toBool();
    const double measured=result.diagnostics["scale"].toDouble(1.0);
    if(!std::isfinite(measured)||measured<=0) {
        reset();result.depth.clear();result.error=QStringLiteral("Invalid temporal scale input");return result;
    }
    if(!initialized) {
        logApplied=reliable?std::log(measured):0;
        boundEpoch=epoch;boundGeneration=generation;lastMs=nowMs;initialized=true;
    } else if(reliable) {
        // Monotonic time, capped elapsed interval and a 10% multiplicative step:
        // a pause cannot turn into an immediate catch-up jump.
        const double alpha=1-std::exp(-std::clamp(nowMs-lastMs,qint64(0),qint64(250))/350.0);
        logApplied+=std::clamp(alpha*(std::log(measured)-logApplied),-std::log(1.1),std::log(1.1));
    }
    lastMs=nowMs;
    if(reliable) {hasReference=true;referenceMs=nowMs;referenceFrame=frameId;}
    const double applied=std::exp(logApplied),factor=applied/measured;
    auto *depth=result.depth.data();
    for(int i=0;i<result.depth.size()/4;++i) {
        quint32 bits=qFromLittleEndian<quint32>(depth+4*i);float value;
        std::memcpy(&value,&bits,4);value=float(value*factor);
        if(!std::isfinite(value)||value<=0) {reset();result.depth.clear();result.error=QStringLiteral("Invalid temporally scaled depth");return result;}
        std::memcpy(&bits,&value,4);qToLittleEndian(bits,depth+4*i);
    }
    result.diagnostics["measured_scale"]=measured;
    result.diagnostics["scale"]=applied;
    result.diagnostics["scale_reference_frame_id"]=double(referenceFrame);
    result.diagnostics["scale_reference_age_ms"]=hasReference?double(nowMs-referenceMs):-1.0;
    result.diagnostics["scale_source"]=reliable?"current_dtof_smoothed":(hasReference?"held_dtof":"unadjusted");
    if(!reliable&&hasReference)result.diagnostics["fusion_mode"]="held_scale";
    return result;
}
}
