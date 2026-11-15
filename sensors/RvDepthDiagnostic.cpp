#include "RvDepthDiagnostic.h"
#include "RvInferenceRuntime.h"
#include "RvCameraView.h"
#include "DepthColorScale.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <QtGui/QPainter>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace RvDepthDiagnostic {
namespace {
std::vector<float> decode(const QByteArray &bytes){
    std::vector<float> out(bytes.size()/4);
    for(int i=0;i<int(out.size());++i){const auto bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);std::memcpy(&out[i],&bits,4);}return out;
}
QByteArray floats(const std::vector<float> &values){
    QByteArray out(int(values.size())*4,0);for(int i=0;i<int(values.size());++i){quint32 bits;std::memcpy(&bits,&values[i],4);qToLittleEndian(bits,out.data()+4*i);}return out;
}
QByteArray digest(const QByteArray &data){return QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex();}
double sample(const std::vector<float> &z,double u,double v,const QSize &size){
    const int width=size.width(),height=size.height();
    if(z.size()!=Pixels||u<0||u>width-1||v<0||v>height-1)return 0;
    const int x=int(u),y=int(v);double value=0;
    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const double w=(dx?u-x:1-u+x)*(dy?v-y:1-v+y);
        const float a=z[std::min(y+dy,height-1)*width+std::min(x+dx,width-1)];if(w>0&&a<=0)return 0;value+=w*a;}return value;
}
QImage paint(const std::vector<float> &z,const QSize &size,double lo,double hi,bool black){
    QImage image(size,QImage::Format_RGB32);for(int y=0;y<size.height();++y){auto *p=reinterpret_cast<QRgb*>(image.scanLine(y));
        for(int x=0;x<size.width();++x)p[x]=DepthColorScale::color(z[y*size.width()+x],lo,hi,true,black);}return image;
}
const RvCameraView::Profile &frameProfile(const QJsonObject &metadata) {
    return RvCameraView::isPortrait(metadata)?RvCameraView::Portrait:RvCameraView::Rgb672VgaFp16;
}
bool frameGridValid(const QJsonObject &metadata) {
    const auto model=metadata["assist_host_model"].toString();
    if(model==QLatin1String(RvCameraView::LegacyNative16Model))
        return !metadata.contains("assist_output_grid")&&metadata["width"]==640&&metadata["height"]==480;
    return RvCameraView::isUniDepth(model)&&RvCameraView::validOutputGrid(metadata);
}
bool write(const QString &path,const QByteArray &bytes){QSaveFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size()&&f.commit();}
bool png(const QString &path,const QImage &image){QSaveFile f(path);return f.open(QIODevice::WriteOnly)&&image.save(&f,"PNG")&&f.commit();}
}
QByteArray encode(const std::vector<float> &mono,const RvAssistFusion::Result &result,int nativeCount){
    if((mono.size()!=Pixels&&mono.size()!=640*480)||(nativeCount!=ReasonBytes&&nativeCount!=256))return {};
    const int depthBytes=int(mono.size())*4;
    QByteArray out(depthBytes+2*ReasonBytes,Qt::Uninitialized);auto *data=out.data();
    qToLittleEndian<quint32>(mono.data(),qsizetype(mono.size()),data);
    const auto reasons=[&](const QByteArray &source,int offset){
        std::memset(data+offset,RvAssistFusion::NoMeasurement,ReasonBytes);
        if(source.size()==ReasonBytes||source.size()==nativeCount)std::memcpy(data+offset,source.constData(),nativeCount);
    };
    reasons(result.globalReasons,depthBytes);reasons(result.localReasons,depthBytes+ReasonBytes);return out;
}
bool validPayload(const QByteArray &p,int depthBytes){
    if((depthBytes!=DepthBytes&&depthBytes!=640*480*4)||p.size()!=depthBytes+2*ReasonBytes)return false;
    const auto *data=p.constData();
    for(int i=0;i<depthBytes/4;++i){const auto bits=qFromLittleEndian<quint32>(data+4*i);float z;std::memcpy(&z,&bits,4);
        if(!std::isfinite(z)||z<=0)return false;}
    for(int i=depthBytes;i<p.size();++i)if(quint8(data[i])>RvAssistFusion::AlreadySupported)return false;
    return true;
}
QByteArray monocularDepth(const QByteArray &payload,const QJsonObject &metadata){
    if(!RvCameraView::validOutputGrid(metadata)
        ||metadata["assist_diagnostic_payload"]!="mono_reasons_v1"
        ||payload.size()!=PayloadBytes)return {};
    return payload.first(DepthBytes);
}
bool Snapshot::valid()const{
    const auto &m=output.metadata;
    const auto &profile=frameProfile(m);
    if(!frameGridValid(m)||!validPayload(payload)||output.metresFloat32LE.size()!=DepthBytes
       ||output.cameraImage.size()!=profile.outputSize()||m["depth_unit"]!="camera-Z_m_float32")return false;
    if(profile.provisional&&(m["assist_provisional_model"]!=true||m["assist_candidate_parity_mean_limit"]!=profile.parityMeanLimit))return false;
    if(m["rectify_map_sha256"]!=QLatin1String(profile.mapSha)||!RvInferenceRuntime::validGraphMetadata(m)
       ||m["depth_K"].toArray()!=RvCameraView::intrinsics(profile)
       ||m["assist_output_restored_to_calibrated_grid"]!=true
       ||(m["assist_input_rotation_degrees"]!=0&&m["assist_input_rotation_degrees"]!=180))return false;
    for(float z:decode(output.metresFloat32LE))if(!std::isfinite(z)||z<0)return false;
    const bool legacy=m["assist_host_model"]==QLatin1String(RvCameraView::LegacyNative16Model);
    if(m["assist_diagnostic_native_count"]!=(legacy?256:ReasonBytes))return false;
    if(legacy){
        for(int i=256;i<ReasonBytes;++i)if(payload[DepthBytes+i]!=0||payload[DepthBytes+ReasonBytes+i]!=0)return false;
    }
    if(native.samples.isEmpty())return m["dtof_present"]==false;
    if(!native.isValidForHostFusion()||native.columns()!=(legacy?16:48)||native.rows()!=(legacy?16:32))return false;
    for(const auto *key:{"frame_id","generation","source_generation","boot_id","tof_sequence","tof_timestamp_ns","tof_reused","pairing_policy","rectify_map_sha256","dtof_fov_correction","tof_fov_correction",
                        "pair_delta_ms","pair_clock_uncertainty_ms","rgb_timestamp_ns","source_protocol",RvAssistFusion::NativeProjectionField})
        if(m.value(key)!=native.metadata.value(key))return false;
    return true;
}
bool Snapshot::save(const QString &parent,QString *result)const{
    const auto fail=[&](const QString &s){if(result)*result=s;return false;};
    if(!valid())return fail("No complete, matching UniDepth diagnostic snapshot");
    if(!QDir().mkpath(parent))return fail("Cannot create diagnostic directory");
    QTemporaryDir stage(QDir(parent).filePath(".host-depth-diagnostic-XXXXXX"));if(!stage.isValid())return fail("Cannot stage diagnostic snapshot");
    const auto &m=output.metadata;const auto fit=m["assist_fusion"].toObject();
    auto mono=decode(payload.left(DepthBytes)),final=decode(output.metresFloat32LE),base=mono;
    const double scale=fit["scale"].toDouble(1);for(auto &z:base)z=float(z*scale);
    const auto &view=frameProfile(m);
    const auto size=view.outputSize();
    const int fov=(m.contains("dtof_fov_correction")?m["dtof_fov_correction"]:m["tof_fov_correction"]).toInt(m["assist_tof_fov_correction"].toInt(-1));
    const auto &cal=RvAssistFusion::calibrationForNativeGrid(fov,native.columns(),native.rows());if(!native.samples.isEmpty()&&!cal.valid)return fail("Unknown diagnostic FOV");
    const double lo=m["display_minimum_m"].toDouble(0),hi=m["display_maximum_m"].toDouble(3);const bool black=m["display_out_of_range_black"].toBool();
    auto rgb=output.cameraImage.convertToFormat(QImage::Format_RGB32);QPainter painter(&rgb);painter.setRenderHint(QPainter::Antialiasing);
    QByteArray csv="native_id,row,column,valid,snr,radial_m,u,v,tof_camera_z_m,mono_z_m,global_z_m,final_z_m,final_minus_tof_m,global_reason,local_reason\n";
    for(int n=0;n<native.sampleCount();++n){
        const char g=payload[DepthBytes+n],l=payload[DepthBytes+ReasonBytes+n];
        const double radius=native.distanceMm(n)/1000.;
        const auto point=RvAssistFusion::projectNativePoint(radius,n,cal,view,native);
        const double u=point.pixel.x(),v=point.pixel.y(),z=point.cameraZ;
        const double f=sample(final,u,v,size);
        QStringList row{QString::number(n),QString::number(n/native.columns()),QString::number(n%native.columns()),QString::number(native.valid(n)),QString::number(native.snr(n)),
            QString::number(radius,'g',12),QString::number(u,'g',12),QString::number(v,'g',12),QString::number(z,'g',12),
            QString::number(sample(mono,u,v,size),'g',12),QString::number(sample(base,u,v,size),'g',12),QString::number(f,'g',12),
            f>0?QString::number(f-z,'g',12):QString(),RvAssistFusion::pointReasonName(g),RvAssistFusion::pointReasonName(l)};
        csv+=row.join(',').toUtf8()+'\n';
        if(native.valid(n)&&u>=0&&u<size.width()&&v>=0&&v<size.height()){
            QColor color=l==RvAssistFusion::Adopted?QColor("#ff62ed"):g==RvAssistFusion::Adopted?QColor("#ffe344"):
                l==RvAssistFusion::Boundary?QColor("#ff8800"):(g==RvAssistFusion::GlobalOutlier||l==RvAssistFusion::LocalConflict)?QColor("#ff3333"):QColor("#bbbbbb");
            painter.setBrush(color);painter.setPen(QPen(Qt::black,.6));painter.drawEllipse(QPointF(u+.5,v+.5),1.8,1.8);
        }
    }
    painter.end();
    const auto inference=m["assist_input_rotation_degrees"]==180?output.cameraImage.transformed(QTransform().rotate(180)):output.cameraImage;
    QJsonObject manifest{{"schema",2},{"kind","unidepth_diagnostic"},{"captured_utc",output.capturedUtc.toString(Qt::ISODateWithMs)},
        {"saved_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"metadata",m},{"native_metadata",native.metadata},
        {"reference","same paired RGB and ToF; adopted agreement is not independent ground truth"},
        {"comparison_scale","Recorded applied global scale"},
        {"depth_encoding",QStringLiteral("%1x%2 row-major float32 little-endian camera-Z metres").arg(size.width()).arg(size.height())},
        {"tof_encoding",QString::number(native.sampleCount())+" native IDs; each: range_uq2 uint16 LE (0.25 mm), SNR uint8, valid uint8; no rotation or resampling"},
        {"reasons_encoding","uint8 global[1536] followed by local[1536]; names are in points.csv"},
        {"coordinates","Metric arrays and paired RGB use the same calibrated grid. The oriented RGB crop follows model input orientation."},
        {"image_legend","paired_points.png: magenta local, yellow global, orange boundary rejected, red global ratio/local conflict, grey other rejected; native pixel centers, no snapping"}};
    manifest["reason_active_native_ids"]=m["assist_diagnostic_native_count"];
    QJsonObject hashes;const auto data=[&](const char *name,const QByteArray &b){hashes[name]=QString::fromLatin1(digest(b));return write(stage.filePath(name),b);};
    if(!data("mono.f32",payload.left(DepthBytes))||!data("global.f32",floats(base))||!data("depth.f32",output.metresFloat32LE)
       ||!data("tof.bin",native.samples)||!data("reasons.bin",payload.mid(DepthBytes))||!data("points.csv",csv)
       ||!png(stage.filePath("paired_rgb.png"),output.cameraImage)||!png(stage.filePath("paired_points.png"),rgb)||!png(stage.filePath("inference_rgb.png"),inference)
       ||!png(stage.filePath("mono.png"),paint(mono,size,lo,hi,black))||!png(stage.filePath("global.png"),paint(base,size,lo,hi,black))
       ||!png(stage.filePath("depth.png"),paint(final,size,lo,hi,black))||(!viewport.isNull()&&!png(stage.filePath("viewport.png"),viewport)))return fail("Cannot write diagnostic files");
    for(const auto *name:{"paired_rgb.png","paired_points.png","inference_rgb.png","mono.png","global.png","depth.png","viewport.png"}){
        QFile f(stage.filePath(name));if(f.open(QIODevice::ReadOnly))hashes[name]=QString::fromLatin1(digest(f.readAll()));}
    QFile geometry(fov==6?RvCalibration::GeometryResource:fov==2?":/camera/dtof_m48_geometry_f2.bin":":/camera/dtof_m48_geometry.bin");
    if(geometry.open(QIODevice::ReadOnly))manifest["geometry_sha256"]=QString::fromLatin1(digest(geometry.readAll()));
    manifest["files_sha256"]=hashes;
    if(!write(stage.filePath("manifest.json"),QJsonDocument(manifest).toJson()))return fail("Cannot write diagnostic manifest");
    const auto name="host_depth_"+output.capturedUtc.toString("yyyyMMdd_HHmmss_zzz")+'_'+QUuid::createUuid().toString(QUuid::Id128).left(8);
    const auto destination=QDir(parent).filePath(name);
    if(!QDir().rename(stage.path(),destination))return fail("Cannot publish diagnostic snapshot");
    stage.setAutoRemove(false);if(result)*result=destination;return true;
}
int replay(const QString &folder,const QString &reportPath,const QString &comparisonDirectory){
    QJsonObject report{{"pass",false},{"input",folder},{"hardware_started",false}};
    try{
        const auto require=[](bool ok,const char *s){if(!ok)throw std::runtime_error(s);};
        QFile f(QDir(folder).filePath("manifest.json"));require(f.open(QIODevice::ReadOnly)&&f.size()<262144,"Invalid diagnostic manifest");
        const auto manifest=QJsonDocument::fromJson(f.readAll()).object();require((manifest["schema"]==1||manifest["schema"]==2)&&manifest["kind"]=="unidepth_diagnostic","Unsupported diagnostic schema");
        const auto load=[&](const char *name,int size){QFile file(QDir(folder).filePath(name));require(file.open(QIODevice::ReadOnly)&&file.size()==size,"Invalid diagnostic binary length");
            const auto bytes=file.readAll();require(manifest["files_sha256"].toObject()[name]==QString::fromLatin1(digest(bytes)),"Diagnostic fingerprint mismatch");return bytes;};
        const auto m=manifest["metadata"].toObject();require(frameGridValid(m),"Unknown diagnostic model or grid");
        const auto &profile=frameProfile(m);
        require(m["rectify_map_sha256"]==QLatin1String(profile.mapSha)&&RvInferenceRuntime::validGraphMetadata(m),"Diagnostic model or view mismatch");
        const auto nativeMeta=manifest["native_metadata"].toObject();RvTofFrame tof{load("tof.bin",nativeMeta["dtof_bytes"].toInt()),nativeMeta};require(tof.isValidForHostFusion(),"Replay requires a paired native measurement");
        const auto monoBytes=load("mono.f32",DepthBytes),savedBytes=load("depth.f32",DepthBytes),reasons=load("reasons.bin",2*ReasonBytes);
        const auto mono=decode(monoBytes),saved=decode(savedBytes);
        QFile imageFile(QDir(folder).filePath("paired_rgb.png"));require(imageFile.open(QIODevice::ReadOnly)&&imageFile.size()<4*1024*1024,"Invalid paired RGB file");
        const auto imageBytes=imageFile.readAll();require(manifest["files_sha256"].toObject()["paired_rgb.png"]==QString::fromLatin1(digest(imageBytes)),"Paired RGB fingerprint mismatch");
        const QImage rgb=QImage::fromData(imageBytes);require(rgb.size()==profile.outputSize(),"Invalid paired RGB");
        Snapshot snapshot{{savedBytes,{},rgb,m,QDateTime::currentDateTimeUtc()},tof,monoBytes+reasons,{}};require(snapshot.valid(),"Diagnostic measurement identity mismatch");
        const int fov=(m.contains("dtof_fov_correction")?m["dtof_fov_correction"]:m["tof_fov_correction"]).toInt(-1);const auto &cal=RvAssistFusion::calibrationForNativeGrid(fov,tof.columns(),tof.rows());
        QFile geometry(fov==6?RvCalibration::GeometryResource:fov==2?":/camera/dtof_m48_geometry_f2.bin":":/camera/dtof_m48_geometry.bin");require(geometry.open(QIODevice::ReadOnly)&&manifest["geometry_sha256"]==QString::fromLatin1(digest(geometry.readAll())),"Diagnostic calibration mismatch");
        const auto mode=m["assist_experiment"].toString();
        const bool current=RvCameraView::isPortrait(m);
        require(m["assist_fusion_revision"]!=QLatin1String(RvAssistFusion::SplitCorrectionRevision),
                "Split G replay requires the preceding RGB and ToF frame sequence");
        const auto revision=current?RvAssistFusion::portraitRevision(mode):"global_scale_latest_native_rgb_v2";
        require((current?RvTofFrame::isHostFusionExperiment(mode):mode=="H")&&m["assist_fusion_revision"]==QLatin1String(revision),
                "Unsupported recorded correction revision");
        auto repaired=RvAssistFusion::fuseGlobalScale(mono,tof,cal,profile);
        require(repaired.ok(),"Recorded global scale fit rejected");
        const double recorded=m["assist_fusion"].toObject()["scale"].toDouble(0);
        const double measured=repaired.diagnostics["scale"].toDouble(0);
        require(std::isfinite(recorded)&&recorded>0&&std::isfinite(measured)&&measured>0,"Invalid recorded scale");
        auto values=decode(repaired.depth);
        for(auto &v:values)v=float(v*(recorded/measured));
        repaired.depth=floats(values);repaired.diagnostics["scale"]=recorded;
        const auto policy=m["assist_output_policy"];
        if(policy==QLatin1String(RvAssistFusion::FieldOfViewPolicy))
            repaired=RvAssistFusion::limitTofFieldOfView(std::move(repaired),cal,profile);
        else if(policy==QLatin1String(RvAssistFusion::SupportPolicy))
            repaired=RvAssistFusion::limitTofSupport(std::move(repaired),tof,cal,profile);
        else require(policy.isUndefined(),"Unknown recorded output policy");
        require(repaired.ok(),"Invalid recorded output geometry");
        const auto candidate=decode(repaired.depth);double difference=0,maximum=0;
        for(int i=0;i<Pixels;++i){const auto d=std::abs(double(candidate[i])-saved[i]);difference+=d;maximum=std::max(maximum,d);}
        report["replayed_correction"]=mode;report["replayed_fusion_revision"]=m["assist_fusion_revision"];
        report["repaired_fusion"]=repaired.diagnostics;
        report["repaired_agreement"]=RvAssistFusion::agreement(mono,repaired,tof,cal,profile);
        report["repaired_vs_saved_mean_m"]=difference/Pixels;report["repaired_vs_saved_max_m"]=maximum;
        if(!comparisonDirectory.isEmpty()){
            const auto output=QDir::cleanPath(QFileInfo(comparisonDirectory).absoluteFilePath());
            const auto source=QDir::cleanPath(QFileInfo(folder).canonicalFilePath());
            require(output.compare(source,Qt::CaseInsensitive)!=0&&!output.startsWith(source+"/",Qt::CaseInsensitive)
                    &&!QFileInfo::exists(output),"Comparison directory must be new and outside the original snapshot");
            require(QDir().mkpath(output),"Cannot create comparison directory");const QDir dir(output);
            const double lo=m["display_minimum_m"].toDouble(0),hi=m["display_maximum_m"].toDouble(3);
            const bool black=m["display_out_of_range_black"].toBool();
            require(write(dir.filePath("current_depth.f32"),repaired.depth)
                    &&write(dir.filePath("current_reasons.bin"),repaired.globalReasons+repaired.localReasons)
                    &&png(dir.filePath("current_depth.png"),paint(candidate,profile.outputSize(),lo,hi,black)),"Cannot export comparison depth");
            report["comparison_directory"]=output;
        }
        report["pass"]=true;
    }catch(const std::exception &e){report["error"]=QString::fromUtf8(e.what());}
    return write(reportPath,QJsonDocument(report).toJson())?(report["pass"].toBool()?0:2):3;
}
}
