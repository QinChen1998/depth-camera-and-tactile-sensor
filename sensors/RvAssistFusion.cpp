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

namespace RvAssistFusion {
namespace {
constexpr int W=576, H=324, GW=144;
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
}

bool loadCalibration(const QByteArray &bytes,Calibration &out,QString &error) {
    out={};
    if(bytes.size()!=281932 || (QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex() != RvCalibration::GeometrySha
            && QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex() != "54802b37740d0075d8a9da95a0dcb5fde27b2f1e20249312a9f2325280c51729"
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
    static const Calibration f1=load(":/camera/dtof_m48_geometry.bin"),f2=load(":/camera/dtof_m48_geometry_f2.bin"),f6=load(RvCalibration::GeometryResource),invalid;
    return fov==6?f6:fov==1?f1:fov==2?f2:invalid;
}

const Calibration &calibrationForNativeGrid(int fov,int columns,int rows){
    if(columns==48&&rows==32)return calibrationForFov(fov);
    const auto make=[](int f){Calibration out;out.columns=out.rows=16;
        const auto &base=calibrationForFov(f);const auto rays=makeNative16Rays(f);
        if(!base.valid||rays[0][2]<=0)return out;
        out.translation=base.translation;std::copy(rays.begin(),rays.end(),out.cameraRays.begin());out.valid=true;return out;};
    static const auto f1=make(1),f2=make(2),f6=make(6),invalid=Calibration{};
    if(columns!=16||rows!=16)return invalid;
    return fov==6?f6:fov==1?f1:fov==2?f2:invalid;
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

Result fuseGlobalScale(const std::vector<float> &mono,const RvTofFrame &tof,
            const Calibration &cal,const RvCameraView::Profile &view) {
    const auto cameraSize=view.outputSize();
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
    if(supported&&mad>std::log(1.3)){
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

QString pointReasonName(char reason){
    switch(reason){case Invalid:return "invalid_return";case Quality:return "snr_or_range";case Outside:return "outside_view";
    case Boundary:return "ambiguous_boundary";case GlobalOutlier:return "global_ratio_outlier";case Duplicate:return "duplicate_projection";
    case GlobalRejected:return "global_fit_rejected";case LocalConflict:return "local_conflict";case Adopted:return "adopted";
    case WeakLocalGroup:return "insufficient_local_group";case AlreadySupported:return "already_supported";default:return "no_measurement";}
}

Result limitTofSupport(Result result,const RvTofFrame &tof,const Calibration &cal,const RvCameraView::Profile &view) {
    const int nativeCount=cal.nativeCount(),columns=cal.columns,rows=cal.rows;
    if(!result.ok())return result;
    const auto fail=[&]{result.depth.clear();result.error=QStringLiteral("Invalid dToF output support contract");return result;};
    if(result.depth.size()!=W*H*4||!cal.valid||!tof.isValid()||!nativeGridMatches(tof,cal)
       ||tof.metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha)
       ||!tof.metadata["pair_delta_ms"].isDouble()||std::abs(tof.metadata["pair_delta_ms"].toDouble())>200)return fail();
    result.diagnostics["output_policy"]=SupportPolicy;
    result.diagnostics["support_pixels"]=0;result.diagnostics["support_triangles"]=0;
    result.diagnostics["support_native_points"]=0;
    if(!result.diagnostics["dtof_corrected"].toBool()) {result.depth.fill(0);return result;}
    const auto ids=result.diagnostics["used_native_indices"].toArray();
    if(ids.isEmpty()||ids.size()!=result.diagnostics["anchors_inlier"].toInt(-1))return fail();
    struct Point {QPointF pixel;double z=0;bool valid=false;};
    std::vector<Point> points(nativeCount);std::set<int> used;
    const double focal=view.focal;
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
                      const RvTofFrame &tof,const Calibration &cal,const RvCameraView::Profile &view) {
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
    const double focal=view.focal;
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
