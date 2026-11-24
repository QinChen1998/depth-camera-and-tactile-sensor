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
#include <utility>

namespace RvAssistFusion {
namespace {
constexpr int W=576, H=324;
// These are algorithm guards, not claims about the sensor's accuracy.
constexpr int MinimumAnchors=12;
double median(std::vector<double> v) {
    const auto n=v.size(); std::nth_element(v.begin(),v.begin()+n/2,v.end());
    const double upper=v[n/2];
    return n%2 ? upper : (upper+*std::max_element(v.begin(),v.begin()+n/2))/2;
}
template<class T> double sample(const std::vector<T> &v,int w,int h,double x,double y,bool requirePositive=false) {
    x=std::clamp(x,0.0,double(w-1)); y=std::clamp(y,0.0,double(h-1));
    const int ix=int(x),iy=int(y),jx=std::min(ix+1,w-1),jy=std::min(iy+1,h-1);
    if(requirePositive&&std::min({v[iy*w+ix],v[iy*w+jx],v[jy*w+ix],v[jy*w+jx]})<=0)return 0;
    const double ax=x-ix,ay=y-iy;
    return (1-ay)*((1-ax)*v[iy*w+ix]+ax*v[iy*w+jx])
        +ay*((1-ax)*v[jy*w+ix]+ax*v[jy*w+jx]);
}
double readDouble(const QByteArray &bytes,int offset) {
    const quint64 bits=qFromLittleEndian<quint64>(bytes.constData()+offset);
    double value;std::memcpy(&value,&bits,8);return value;
}
struct Anchor { double x,y,z,ratio; int nativeIndex; };
bool usableNativeRange(const RvTofFrame &tof,int id,double radius) {
    return tof.snr(id)>=6 && radius>=MinimumRadialMetres && radius<=10;
}
bool awayFromModelEdge(const std::vector<float> &mono,int width,int x,int y) {
    double low=mono[y*width+x],high=low;
    for(int dy=-3;dy<=3;dy+=3)for(int dx=-3;dx<=3;dx+=3) {
        const double d=mono[(y+dy)*width+x+dx];low=std::min(low,d);high=std::max(high,d);
    }
    return high<=1.4*low;
}
struct ScaleFit {
    std::vector<Anchor> inliers;
    double scale=0,mad=0,spanX=0,spanY=0;
    double retainedMad=0;
    int inconsistent=0,duplicates=0;
    bool rechecked=false,supported=false;
};
ScaleFit fitScale(const std::vector<Anchor> &anchors,int width,int height,QByteArray *reasons=nullptr) {
    ScaleFit fit;
    if(anchors.size()<MinimumAnchors)return fit;
    std::vector<double> ratios,deviations;
    ratios.reserve(anchors.size());deviations.reserve(anchors.size());
    for(const auto &a:anchors)ratios.push_back(a.ratio);
    // Centre the scale before rejecting outliers, even if the model's scale is far off.
    const double centre=median(ratios);
    for(double r:ratios)deviations.push_back(std::abs(r-centre));
    fit.mad=median(std::move(deviations));
    const double gate=std::clamp(3*1.4826*fit.mad,std::log(1.15),std::log(1.75));
    std::vector<double> accepted;
    double minX=width,maxX=0,minY=height,maxY=0;
    const int columns=(width+3)/4;
    std::set<int> occupied;
    for(const auto &a:anchors) {
        if(std::abs(a.ratio-centre)>gate){if(reasons)(*reasons)[a.nativeIndex]=GlobalOutlier;++fit.inconsistent;continue;}
        const int cell=int(a.y/4)*columns+int(a.x/4);
        if(!occupied.insert(cell).second){if(reasons)(*reasons)[a.nativeIndex]=Duplicate;++fit.duplicates;continue;}
        if(reasons)(*reasons)[a.nativeIndex]=Adopted;
        fit.inliers.push_back(a);accepted.push_back(a.ratio);
        minX=std::min(minX,a.x);maxX=std::max(maxX,a.x);minY=std::min(minY,a.y);maxY=std::max(maxY,a.y);
    }
    fit.spanX=maxX-minX;fit.spanY=maxY-minY;
    fit.supported=fit.inliers.size()>=MinimumAnchors&&fit.inliers.size()>=.5*anchors.size()
        &&(fit.spanX>=32||fit.spanY>=24);
    if(fit.supported&&fit.mad>std::log(1.3)){
        // Recheck the retained consensus without refitting the discarded returns.
        const double retainedCentre=median(accepted);
        std::vector<double> retainedDeviations;retainedDeviations.reserve(accepted.size());
        for(double ratio:accepted)retainedDeviations.push_back(std::abs(ratio-retainedCentre));
        fit.retainedMad=median(std::move(retainedDeviations));
        fit.rechecked=true;
        fit.supported=fit.retainedMad<=std::log(1.3);
    }
    if(fit.supported)fit.scale=std::exp(median(std::move(accepted)));
    return fit;
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
    return cal.columns==16&&cal.rows==16&&tof.metadata["assist_host_model"]==QLatin1String(RvCameraView::LegacyNative16Model);
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

QJsonValue defaultNativeProjection(const RvTofFrame &tof) {
    const auto &m=tof.metadata;
    const auto fov=m.contains("dtof_fov_correction")?m["dtof_fov_correction"]:m["tof_fov_correction"];
    if(!tof.samples.isEmpty()&&fov==6&&tof.columns()==48&&tof.rows()==32
        &&RvTofFrame::isHostFusionExperiment(m["assist_experiment"].toString())
        &&RvCameraView::isHostDepth(m["assist_host_model"].toString()))
        return QString::fromLatin1(CloseNative48Projection);
    return QJsonValue(QJsonValue::Undefined);
}

NativeProjection projectNativePoint(double radius,int id,const Calibration &cal,
                                    const RvCameraView::Profile &view,const RvTofFrame &tof) {
    const auto &q=cal.cameraRays[id];
    const double z=radius*q[2]+cal.translation[2];
    if(z<=0)return {{-1,-1},z};
    QPointF pixel(view.focal*(radius*q[0]+cal.translation[0])/z+view.cx(),
                  view.focal*(radius*q[1]+cal.translation[1])/z+view.cy());
    // The recorded F6 48×32 near-range correction is a lateral residual.
    if(tof.metadata[NativeProjectionField]==QLatin1String(CloseNative48Projection)&&z>.06&&z<.33){
        const double weight=std::min({1.,(z-.06)/.04,(.33-z)/.13});
        pixel+=QPointF(.0065,.00725)*(view.focal*weight/z);
    }
    return {pixel,z};
}

Result monocular(const std::vector<float> &mono,const char *reason,QSize size,bool allowMissing) {
    const int pixels=size.width()*size.height();
    Result result;
    result.globalReasons=result.localReasons=QByteArray(1536,NoMeasurement);
    if(mono.size()!=pixels) {result.error="Invalid monocular dimensions";return result;}
    for(float z:mono)if(!std::isfinite(z)||z<0||(!allowMissing&&z==0)) {result.error="Invalid monocular camera-Z output";return result;}
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
    const auto projection=tof.metadata.value(NativeProjectionField);
    const auto model=tof.metadata["assist_host_model"].toString();
    const bool allowMissing=RvCameraView::isDepthPro2(model);
    if(!projection.isUndefined()&&(projection!=defaultNativeProjection(tof)||view.outputSize()!=RvCameraView::outputProfile(model).outputSize()))
        return fail("Invalid native ToF projection");
    if(!cal.valid || mono.size()!=width*height || cameraSize!=QSize(width,height) || !tof.isValidForHostFusion() || !nativeGridMatches(tof,cal)
        || tof.metadata["rgb_width"].toInt()!=view.inputWidth
        || tof.metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha)
        || !tof.metadata["pair_delta_ms"].isDouble()
        || std::abs(tof.metadata["pair_delta_ms"].toDouble())>200)
        return fail("Unmatched RGB/dToF frame or calibration");
    for(float z:mono)if(!std::isfinite(z)||z<0||(!allowMissing&&z==0))return fail("Invalid monocular camera-Z output");
    // Only measurement support failures may fall back; frame/calibration and
    // model validity have already been checked. Temporal scaling is separate.
    const auto monocular=[&](const char *reason) {
        auto output=RvAssistFusion::monocular(mono,reason,cameraSize,allowMissing);
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
        if(!usableNativeRange(tof,i,radius)){result.globalReasons[i]=result.localReasons[i]=Quality;continue;}
        ++good;
        const auto point=projectNativePoint(radius,i,cal,view,tof);
        const double z=point.cameraZ;
        if(z<=0){result.globalReasons[i]=result.localReasons[i]=Outside;continue;}
        const double u=point.pixel.x(),v=point.pixel.y();
        if(u<3 || u>width-4 || v<3 || v>height-4){result.globalReasons[i]=result.localReasons[i]=Outside;continue;}
        ++projected;
        const double prediction=sample(mono,width,height,u,v,allowMissing);
        if(prediction<=0||!awayFromModelEdge(mono,width,int(u),int(v))) {result.globalReasons[i]=result.localReasons[i]=Boundary;++edgeRejected;continue;}
        result.globalReasons[i]=result.localReasons[i]=GlobalRejected;
        anchors.push_back({u,v,z,std::log(z/prediction),i});
    }
    result.diagnostics={{"tof_total_zones",nativeCount},{"tof_good_zones",good},{"tof_projected_in_view",projected},
        {"anchors_projected",int(anchors.size())},{"anchors_edge_rejected",edgeRejected}};
    if(anchors.size()<MinimumAnchors)return monocular("Too few reliable dToF anchors in the camera field of view");
    const auto fit=fitScale(anchors,width,height,&result.globalReasons);
    result.localReasons=result.globalReasons;
    result.diagnostics["anchors_inlier"]=int(fit.inliers.size());result.diagnostics["log_ratio_mad"]=fit.mad;
    result.diagnostics["anchors_inconsistent_rejected"]=fit.inconsistent;
    result.diagnostics["anchors_duplicate_rejected"]=fit.duplicates;
    result.diagnostics["anchor_span_x"]=fit.spanX;result.diagnostics["anchor_span_y"]=fit.spanY;
    if(fit.rechecked){
        result.diagnostics["global_consensus_recheck"]=true;
        result.diagnostics["retained_log_ratio_mad"]=fit.retainedMad;
        result.diagnostics["global_consensus_limit_log"]=std::log(1.3);
        result.diagnostics["global_consensus_accepted"]=fit.supported;
    }
    if(!fit.supported)
        return monocular("dToF anchors are inconsistent or have insufficient spatial coverage");
    const double scale=fit.scale;
    if(!std::isfinite(scale) || scale<.02 || scale>50)return monocular("Unstable monocular scale fit");
    result.depth.resize(width*height*4);
    auto *depth=result.depth.data();
    for(int i=0;i<width*height;++i){const float z=float(mono[i]*scale);
        if(!std::isfinite(z)||z<0||(!allowMissing&&z==0))return fail("Non-finite fused depth");
        quint32 bits;std::memcpy(&bits,&z,4);qToLittleEndian(bits,depth+4*i);
    }
    QJsonArray used;for(const auto &a:fit.inliers)used.append(a.nativeIndex);
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
    const int width=view.outputWidth,height=view.outputHeight,pixels=width*height;
    if(!output.ok()||output.depth.size()!=pixels*4||mono.size()!=pixels||!cal.valid
       ||!tof.isValidForHostFusion()||!nativeGridMatches(tof,cal))return {};
    std::vector<float> depth(pixels);
    for(int i=0;i<pixels;++i){const quint32 bits=qFromLittleEndian<quint32>(output.depth.constData()+4*i);std::memcpy(&depth[i],&bits,4);}
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
    for(int i=0;i<nativeCount;++i){
        const double r=tof.rangeUq2(i)*.00025;if(!tof.valid(i)||tof.snr(i)<6||r<MinimumRadialMetres||r>10)continue;
        const auto point=projectNativePoint(r,i,cal,view,tof);
        const double z=point.cameraZ;if(z<=0)continue;
        const double u=point.pixel.x(),v=point.pixel.y();
        if(u<3||u>width-4||v<3||v>height-4)continue;
        const double a=sample(mono,width,height,u,v),b=sample(depth,width,height,u,v);
        if(!std::isfinite(a)||a<=0||!std::isfinite(b)||b<=0)continue;
        all.add(a,b,z);if(used.count(i))adopted.add(a,b,z);
        (z<.7?near:z<1.5?middle:far).add(a,b,z);
    }
    return {{"reference","current_projected_dtof_camera_z_not_independent_ground_truth"},
        {"all",all.json()},{"adopted",adopted.json()},{"near_under_0.7m",near.json()},
        {"middle_0.7_to_1.5m",middle.json()},{"far_over_1.5m",far.json()}};
}

Result ScaleContinuity::apply(Result result,qint64 nowMs,qint64 epoch,qint64 generation,qint64 frameId,bool allowMissing) {
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
        if(!std::isfinite(value)||value<0||(!allowMissing&&value==0)) {reset();result.depth.clear();result.error=QStringLiteral("Invalid temporally scaled depth");return result;}
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

namespace {
constexpr int PortraitWidth=480,PortraitHeight=640;
constexpr int GuardStep=4,GuardWidth=PortraitWidth/GuardStep,GuardHeight=PortraitHeight/GuardStep;
constexpr int ReferenceStep=10,ReferenceWidth=PortraitWidth/ReferenceStep+1,ReferenceHeight=PortraitHeight/ReferenceStep+1;
constexpr double NearLimit=.40,FarLimit=.55;
constexpr int NearSearchRadius=32;
constexpr int NearColumns=(PortraitWidth+NearSearchRadius-1)/NearSearchRadius;
constexpr int NearRows=(PortraitHeight+NearSearchRadius-1)/NearSearchRadius;
constexpr qint64 NoObservation=-10000;

double unitWeight(double value){return std::clamp(value,0.,1.);}
int rounded(double value){return int(std::nearbyint(value));}
using Rgb=std::array<int,3>;
using RgbPatch=std::array<Rgb,5>;

class PortraitRgb {
public:
    explicit PortraitRgb(QByteArrayView planar):data(planar.data()){}
    Rgb pixel(int x,int y) const {
        const auto &view=RvCameraView::Rgb672VgaFp16;
        const auto p=RvCameraView::inputPixel(QPointF(x,y),RvCameraView::Portrait.outputSize());
        constexpr int firstX=16+RvCameraView::PortraitCropX,firstY=12+RvCameraView::PortraitCropY;
        constexpr int lastX=firstX+RvCameraView::PortraitCropWidth-1,lastY=firstY+RvCameraView::PortraitCropHeight-1;
        const double u=std::clamp(p.x(),double(firstX),double(lastX));
        const int left=int(u),right=std::min(left+1,lastX),sourceRow=int(p.y());
        const int top=std::clamp(sourceRow,firstY,lastY),bottom=std::clamp(sourceRow+1,firstY,lastY);
        const int a=int((u-left)*8),b=int((p.y()-sourceRow)*8);
        const int row0=top*view.inputWidth,row1=bottom*view.inputWidth;
        Rgb color;
        for(int c=0;c<3;++c){
            const auto *plane=data+c*view.inputWidth*view.inputHeight;
            const int upper=(8-a)*quint8(plane[row0+left])+a*quint8(plane[row0+right]);
            const int lower=(8-a)*quint8(plane[row1+left])+a*quint8(plane[row1+right]);
            // Match the 3:4 fixed-point RGB resampling used in the offline replay.
            color[c]=((upper*(8-b)/16)+(lower*b/16)+2)/4;
        }
        return color;
    }
    RgbPatch patch(double x,double y) const {
        const int u=rounded(x),v=rounded(y);
        return {pixel(u,v),pixel(u-3,v-3),pixel(u+3,v-3),pixel(u-3,v+3),pixel(u+3,v+3)};
    }
private:
    const char *data;
};

int colorDistance(const Rgb &a,const Rgb &b){
    return std::max({std::abs(a[0]-b[0]),std::abs(a[1]-b[1]),std::abs(a[2]-b[2])});
}
bool samePatch(const RgbPatch &previous,const RgbPatch &current){
    for(size_t i=0;i<previous.size();++i){
        int difference=0;
        for(int c=0;c<3;++c)difference+=std::abs(previous[i][c]-current[i][c]);
        if(difference>3*14)return false;
    }
    return true;
}

// Only distances near the foreground gate matter. Include a full grid cell
// beyond its 58 px maximum so interpolation preserves the useful distances.
void foregroundDistances(const std::vector<int> &seeds,std::vector<float> &distance){
    constexpr int radius=16;
    std::array<int,GuardWidth*GuardHeight> squared;
    squared.fill(radius*radius);
    for(int index:seeds){
        const int x=index%GuardWidth,y=index/GuardWidth;
        for(int v=std::max(0,y-radius);v<=std::min(GuardHeight-1,y+radius);++v){
            const int dy=v-y;
            for(int u=std::max(0,x-radius);u<=std::min(GuardWidth-1,x+radius);++u){
                const int dx=u-x;
                auto &old=squared[v*GuardWidth+u];old=std::min(old,dx*dx+dy*dy);
            }
        }
    }
    distance.resize(squared.size());
    for(size_t i=0;i<squared.size();++i)distance[i]=float(GuardStep*std::sqrt(double(squared[i])));
}

struct WeightedScale {
    std::vector<std::pair<double,double>> ratios;
    double sum=0,weighted=0;
    int votes=0;
    void clear(){ratios.clear();sum=weighted=0;votes=0;}
    void add(double weight,double ratio,double voteThreshold){
        if(weight<=0)return;
        ratios.emplace_back(weight,ratio);sum+=weight;weighted+=weight*ratio;
        votes+=weight>voteThreshold;
    }
    double mean() const {return weighted/sum;}
    double confidence(double minimum,double deviationLimit,double deviationFade) const {
        if(votes<3)return 0;
        const double centre=mean();double deviation=0;
        for(const auto &[weight,ratio]:ratios)deviation+=weight*std::abs(ratio-centre);
        return unitWeight(sum/minimum)*unitWeight((deviationLimit-deviation/sum)/deviationFade);
    }
};

struct FarTrack { double x=0,y=0,z=0; qint64 time=NoObservation; RgbPatch rgb{}; };
struct FarSample {
    const FarTrack *track;
    Rgb color;
    double logModel,logRatio,ageWeight;
    qint64 age;
};
struct BackgroundNode { double z=0,quality=0; qint64 seen=NoObservation; Rgb color{}; };

double confirmedBackground(const std::vector<FarSample> &samples,int x,int y,double raw,const Rgb &color,WeightedScale &fit){
    static const double depthWindow=std::log(1.52),localWindow=std::log(1.25);
    static const double deviationLimit=std::log(1.28),deviationFade=std::log(1.15);
    const double logRaw=std::log(raw);
    bool left=false,right=false,above=false,below=false,close=false;
    int localCount=0;fit.clear();
    for(const auto &s:samples){
        const double dx=s.track->x-x,dy=s.track->y-y,distance=dx*dx+dy*dy;
        if(distance>=155.*155.)continue;
        const double rel=std::abs(logRaw-s.logModel);
        if(rel>=depthWindow)continue;
        const double spatial=1-distance/(155.*155.);
        const double weight=spatial*spatial*(1-rel/depthWindow)
            *unitWeight((70.-colorDistance(color,s.color))/45.)*s.ageWeight;
        fit.add(weight,s.logRatio,.12);
        if(weight>.10&&distance<43.*43.&&s.age<=250&&rel<localWindow){
            ++localCount;close|=distance<25.*25.;
            left|=dx< -2;right|=dx>2;above|=dy< -2;below|=dy>2;
        }
    }
    if(localCount<3||!close||!left||!right||!above||!below||fit.confidence(1.8,deviationLimit,deviationFade)<.56)return 0;
    return raw*std::exp(fit.mean());
}

struct NearSample { double x,y,logModel,logRatio; };
using NearBuckets=std::array<std::vector<NearSample>,NearColumns*NearRows>;
double currentNearCorrection(const NearBuckets &buckets,int x,int y,double raw,double base,WeightedScale &fit){
    static const double window=std::log(1.18),deviationLimit=std::log(1.30),deviationFade=std::log(1.16);
    const double logRaw=std::log(raw);
    const int bx=x/NearSearchRadius,by=y/NearSearchRadius;
    fit.clear();
    for(int row=std::max(0,by-1);row<=std::min(NearRows-1,by+1);++row){
        for(int column=std::max(0,bx-1);column<=std::min(NearColumns-1,bx+1);++column){
            for(const auto &s:buckets[row*NearColumns+column]){
                const double dx=x-s.x,dy=y-s.y,spatial=1-(dx*dx+dy*dy)/(NearSearchRadius*NearSearchRadius);
                if(spatial<=0)continue;
                const double depth=1-std::abs(logRaw-s.logModel)/window;
                if(depth>0)fit.add(spatial*spatial*depth,s.logRatio,.035);
            }
        }
    }
    if(fit.sum<=2.25)return 0;
    const double strength=unitWeight((fit.confidence(3.,deviationLimit,deviationFade)-.75)/.25);
    if(strength<=0)return 0;
    const double measured=std::max(.44,raw*std::exp(fit.mean()));
    return strength*std::min(0.,measured-base);
}
}

struct SplitCorrection::State {
    qint64 epoch=0,generation=0,lastRgbMs=0,previousMeasurementMs=NoObservation;
    std::array<double,48*32> previousDepth{};
    std::array<FarTrack,48*32> far;
    std::vector<float> guard;
    std::vector<BackgroundNode> reference=std::vector<BackgroundNode>(ReferenceWidth*ReferenceHeight);
    bool seenNear=false,seenBoth=false;

    ScaleFit observe(const std::vector<float> &mono,const PortraitRgb &rgb,const RvTofFrame &tof,const Calibration &cal,qint64 now){
        if(tof.samples.isEmpty())return {};
        const auto &view=RvCameraView::Portrait;
        std::array<double,48*32> current{};
        std::array<bool,GuardWidth*GuardHeight> covered{};
        std::vector<int> seeds;
        std::vector<Anchor> near;
        int farCount=0;
        for(int id=0;id<cal.nativeCount();++id){
            const double radial=tof.rangeUq2(id)*.00025;
            if(!tof.valid(id)||!usableNativeRange(tof,id,radial))continue;
            const auto point=projectNativePoint(radial,id,cal,view,tof);
            const double x=point.pixel.x(),y=point.pixel.y(),z=point.cameraZ;
            if(z<=0||x<0||y<0||x>PortraitWidth-1||y>PortraitHeight-1)continue;
            current[id]=z;
            if(z<=.44){
                const int gx=std::min(rounded(x/GuardStep),GuardWidth-1),gy=std::min(rounded(y/GuardStep),GuardHeight-1);
                const int cell=gy*GuardWidth+gx;
                if(!covered[cell]){seeds.push_back(cell);covered[cell]=true;}
            }
            if(x<3||y<3||x>PortraitWidth-4||y>PortraitHeight-4||!awayFromModelEdge(mono,PortraitWidth,int(x),int(y)))continue;
            if(z<=NearLimit){
                near.push_back({x,y,z,std::log(z/sample(mono,PortraitWidth,PortraitHeight,x,y)),id});
            }else if(z>=FarLimit&&previousDepth[id]>=FarLimit&&now-previousMeasurementMs<=200
                     &&std::abs(z-previousDepth[id])<=std::max(.035,.05*z)){
                far[id]={x,y,.75*z+.25*previousDepth[id],now,rgb.patch(x,y)};
                ++farCount;
            }
        }
        previousDepth=current;previousMeasurementMs=now;
        if(!seeds.empty())foregroundDistances(seeds,guard);
        auto fit=fitScale(near,PortraitWidth,PortraitHeight);
        fit.supported=fit.supported&&fit.scale>=.02&&fit.scale<=50;
        seenNear|=fit.supported;
        seenBoth|=seenNear&&farCount>=5;
        return fit;
    }

    std::vector<FarSample> visibleFar(const std::vector<float> &mono,const PortraitRgb &rgb,qint64 now) const {
        std::vector<FarSample> visible;
        for(const auto &track:far){
            const qint64 age=now-track.time;
            if(age>=440)continue;
            const auto patch=rgb.patch(track.x,track.y);
            if(!samePatch(track.rgb,patch))continue;
            const double raw=sample(mono,PortraitWidth,PortraitHeight,track.x,track.y);
            visible.push_back({&track,patch.front(),std::log(raw),std::log(track.z/raw),unitWeight((720.-age)/390.),age});
        }
        return visible;
    }

    void updateBackground(const std::vector<float> &mono,const PortraitRgb &rgb,std::vector<FarSample> &visible,
                          qint64 now,double baseScale){
        double minX=PortraitWidth,minY=PortraitHeight,maxX=0,maxY=0;
        for(const auto &s:visible){
            minX=std::min(minX,s.track->x);maxX=std::max(maxX,s.track->x);
            minY=std::min(minY,s.track->y);maxY=std::max(maxY,s.track->y);
        }
        const bool fitBackground=visible.size()>=35&&maxX-minX>=105&&maxY-minY>=105;
        if(fitBackground&&visible.size()>160){
            std::vector<FarSample> distributed;
            distributed.reserve(160);
            const double step=double(visible.size()-1)/159.;
            for(int i=0;i<159;++i)distributed.push_back(visible[size_t(i*step)]);
            distributed.push_back(visible.back());
            visible=std::move(distributed);
            minX=PortraitWidth;minY=PortraitHeight;maxX=maxY=0;
            for(const auto &s:visible){
                minX=std::min(minX,s.track->x);maxX=std::max(maxX,s.track->x);
                minY=std::min(minY,s.track->y);maxY=std::max(maxY,s.track->y);
            }
        }
        const double agree=std::log(1.17);
        WeightedScale fit;fit.ratios.reserve(visible.size());
        for(int row=0;row<ReferenceHeight;++row)for(int column=0;column<ReferenceWidth;++column){
            const int x=std::min(column*ReferenceStep,PortraitWidth-1),y=std::min(row*ReferenceStep,PortraitHeight-1);
            auto &node=reference[row*ReferenceWidth+column];
            const auto color=rgb.pixel(x,y);
            const bool usable=sample(guard,GuardWidth,GuardHeight,double(x)/GuardStep,double(y)/GuardStep)>=39.;
            if(!usable||now-node.seen>1080||colorDistance(node.color,color)>17){node.quality=0;node.seen=NoObservation;}
            if(!usable||!fitBackground||x<minX-43||x>maxX+43||y<minY-43||y>maxY+43)continue;
            const double raw=mono[y*PortraitWidth+x];
            const double physical=confirmedBackground(visible,x,y,raw,color,fit);
            if(physical<=0)continue;
            if(node.quality==0){
                if(std::abs(std::log(physical/(raw*baseScale)))>agree)continue;
                node.z=physical;node.color=color;
            }else{
                if(std::abs(physical-node.z)>std::max(.05,.12*physical))continue;
                node.z+=.07*(physical-node.z);
            }
            node.seen=now;
            node.quality=std::min(1.,node.quality+.22);
        }
    }

    void applyDepth(Result &base,const std::vector<float> &mono,const ScaleFit &near,size_t farCount,qint64 now,double scale) const {
        std::vector<double> correction(reference.size());
        bool hasBackground=false;
        for(int row=0;row<ReferenceHeight;++row)for(int column=0;column<ReferenceWidth;++column){
            const int index=row*ReferenceWidth+column;
            const auto &node=reference[index];
            if(node.quality<=0)continue;
            const int x=std::min(column*ReferenceStep,PortraitWidth-1),y=std::min(row*ReferenceStep,PortraitHeight-1);
            const double difference=std::max(0.,node.z-mono[y*PortraitWidth+x]*scale);
            const double weight=node.quality*unitWeight((1050.-(now-node.seen))/680.);
            correction[index]=weight*difference*unitWeight((difference-.035)/std::max(.06,.15*node.z));
            hasBackground|=correction[index]>0;
        }
        const bool hasNear=near.supported&&farCount>=45;
        if(!hasBackground&&!hasNear)return;
        NearBuckets buckets;
        double upper=0;
        if(hasNear){
            std::vector<double> models;models.reserve(near.inliers.size());
            for(const auto &point:near.inliers){
                const double raw=sample(mono,PortraitWidth,PortraitHeight,point.x,point.y);
                const int cell=int(point.y/NearSearchRadius)*NearColumns+int(point.x/NearSearchRadius);
                buckets[cell].push_back({point.x,point.y,std::log(raw),point.ratio});
                models.push_back(raw);
            }
            std::sort(models.begin(),models.end());const double rank=.9*(models.size()-1);
            const auto lo=size_t(rank),hi=std::min(lo+1,models.size()-1);
            upper=models[lo]+(rank-lo)*(models[hi]-models[lo]);
        }
        WeightedScale fit;fit.ratios.reserve(near.inliers.size());
        int backgroundPixels=0,nearPixels=0;
        char *bytes=base.depth.data();
        for(int y=0;y<PortraitHeight;++y)for(int x=0;x<PortraitWidth;++x){
            const int index=y*PortraitWidth+x;
            const double raw=mono[index],original=raw*scale;
            const bool possibleNear=hasNear&&original>.44&&original<1.3&&raw<=upper;
            if(!possibleNear&&!hasBackground)continue;
            const double distance=sample(guard,GuardWidth,GuardHeight,double(x)/GuardStep,double(y)/GuardStep);
            double change=0;
            if(possibleNear&&distance<12.)change=currentNearCorrection(buckets,x,y,raw,original,fit);
            else if(hasBackground&&distance>43.)change=unitWeight((distance-43.)/15.)
                *sample(correction,ReferenceWidth,ReferenceHeight,double(x)/ReferenceStep,double(y)/ReferenceStep);
            if(change==0)continue;
            auto bits=qFromLittleEndian<quint32>(bytes+4*index);
            float previous;std::memcpy(&previous,&bits,4);
            const float next=float(previous+change);
            if(!std::isfinite(next)||next<=0||next==previous)continue;
            std::memcpy(&bits,&next,4);qToLittleEndian(bits,bytes+4*index);
            backgroundPixels+=change>0;nearPixels+=change<0;
        }
        base.diagnostics["split_background_pixels"]=backgroundPixels;
        base.diagnostics["split_current_near_pixels"]=nearPixels;
    }
};

SplitCorrection::SplitCorrection()=default;
SplitCorrection::~SplitCorrection()=default;
void SplitCorrection::reset(){state.reset();}

void SplitCorrection::apply(Result &base,const std::vector<float> &mono,QByteArrayView planarRgb,
                            const RvTofFrame &tof,const Calibration &cal,qint64 rgbMs,qint64 epoch,qint64 generation,bool selected){
    if(!base.ok()){reset();return;}
    if(state&&(state->epoch!=epoch||state->generation!=generation||rgbMs<state->lastRgbMs))reset();
    if(!selected&&!state)return;
    if(base.depth.size()!=RvCameraView::Portrait.depthBytes()||mono.size()!=PortraitWidth*PortraitHeight
        ||planarRgb.size()!=RvCameraView::Rgb672VgaFp16.rgbBytes()
        ||(!tof.samples.isEmpty()&&(!cal.valid||cal.columns!=48||cal.rows!=32||!tof.isValidForHostFusion()
                                    ||tof.metadata["tof_reused"]==true))){reset();return;}
    if(!state){state=std::make_unique<State>();state->epoch=epoch;state->generation=generation;}
    state->lastRgbMs=rgbMs;
    const PortraitRgb rgb(planarRgb);
    const auto near=state->observe(mono,rgb,tof,cal,rgbMs);
    if(!state->seenBoth||state->guard.empty())return;
    auto visible=state->visibleFar(mono,rgb,rgbMs);
    const double scale=base.diagnostics["scale"].toDouble(1.);
    state->updateBackground(mono,rgb,visible,rgbMs,scale);
    if(selected)state->applyDepth(base,mono,near,visible.size(),rgbMs,scale);
}
}
