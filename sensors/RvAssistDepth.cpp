#include "RvAssistDepth.h"
#include "RvColorSurface.h"
#include "DepthorV3Geometry.h"
#include "RvDepthDiagnostic.h"
#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include "RvCameraStream.h"
#include "DepthColorScale.h"
#include "../RuntimeDiscovery.h"
#include "../dependency_sources/onnxruntime/onnxruntime_c_api.h"
#include "../tools/model_protection/ModelProtectionApi.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QVector>
#include <QtCore/QJsonDocument>
#include <QtCore/QLibrary>
#include <QtCore/QPointer>
#include <QtCore/QUuid>
#include <QtCore/QtEndian>
#include <QtCore/qscopeguard.h>
#include <QtGui/QPainter>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkProxy>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <fcntl.h>
#include <io.h>
#define NOMINMAX
#include <windows.h>
#ifdef DCTS_INSTALLER_BUILD
#include <dxgi.h>
#endif
#include <bcrypt.h>

namespace {
constexpr int Pixels=576*324, RgbBytes=3*Pixels, DepthBytes=4*Pixels;
// Three bounded native-ID lists can exceed 16 KiB at dense valid returns.
constexpr int HeaderLimit=65536, PacketLimit=HeaderLimit+DepthBytes+RvDepthDiagnostic::PayloadBytes+12;
#ifdef DCTS_INSTALLER_BUILD
constexpr auto HostRuntimeBackend="onnxruntime-tensorrt-rtx";
constexpr auto HostOrtVersion="1.31.1";
constexpr auto HostProviderFile="bin/onnxruntime_providers_nv_tensorrt_rtx.dll";
constexpr auto TensorRtProviderName="NvTensorRTRTXExecutionProvider";
constexpr int HostWorkerLoadTimeoutMs=90000;

bool hasNvidiaAdapter()
{
    HMODULE dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dxgi)return true; // Device enumeration is unavailable. Let the worker diagnose CUDA.
    const auto close=qScopeGuard([dxgi]{FreeLibrary(dxgi);});
    using CreateFactory=HRESULT (WINAPI *)(REFIID,void **);
    const auto create=reinterpret_cast<CreateFactory>(GetProcAddress(dxgi,"CreateDXGIFactory1"));
    IDXGIFactory1 *factory=nullptr;
    if(!create||FAILED(create(__uuidof(IDXGIFactory1),reinterpret_cast<void **>(&factory))))return true;
    const auto release=qScopeGuard([factory]{factory->Release();});
    for(UINT index=0;;++index){
        IDXGIAdapter1 *adapter=nullptr;
        const auto result=factory->EnumAdapters1(index,&adapter);
        if(result==DXGI_ERROR_NOT_FOUND)return false;
        if(FAILED(result))return true;
        DXGI_ADAPTER_DESC1 info{};
        const auto description=adapter->GetDesc1(&info);
        adapter->Release();
        if(SUCCEEDED(description)&&info.VendorId==0x10de&&!(info.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))return true;
    }
}

void checkTensorRtCudaDevice()
{
    HMODULE driver=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!driver)throw std::runtime_error("RTX_DRIVER_MISSING");
    const auto release=qScopeGuard([driver]{FreeLibrary(driver);});
    using Init=int (WINAPI *)(unsigned);
    using Query=int (WINAPI *)(int *);
    using Device=int (WINAPI *)(int *,int);
    using Attribute=int (WINAPI *)(int *,int,int);
    const auto init=reinterpret_cast<Init>(GetProcAddress(driver,"cuInit"));
    const auto version=reinterpret_cast<Query>(GetProcAddress(driver,"cuDriverGetVersion"));
    const auto count=reinterpret_cast<Query>(GetProcAddress(driver,"cuDeviceGetCount"));
    const auto getDevice=reinterpret_cast<Device>(GetProcAddress(driver,"cuDeviceGet"));
    const auto getAttribute=reinterpret_cast<Attribute>(GetProcAddress(driver,"cuDeviceGetAttribute"));
    int cudaVersion=0;
    if(!version||version(&cudaVersion)!=0)throw std::runtime_error("RTX_DRIVER_UNAVAILABLE");
    if(cudaVersion<12090)throw std::runtime_error("RTX_DRIVER_OLD");
    int devices=0;
    if(!init||!count||!getDevice||!getAttribute||init(0)!=0||count(&devices)!=0||devices<=0)
        throw std::runtime_error("RTX_DRIVER_UNAVAILABLE");
    bool capabilityRead=false;
    for(int i=0;i<devices;++i){
        int device=0,major=0;
        if(getDevice(&device,i)==0&&getAttribute(&major,75,device)==0){
            capabilityRead=true;
            if(major>=8)return;
        }
    }
    throw std::runtime_error(capabilityRead?"RTX_GPU_UNSUPPORTED":"RTX_DRIVER_UNAVAILABLE");
}
#else
constexpr auto HostRuntimeBackend="onnxruntime-cuda";
constexpr auto HostOrtVersion="1.23.2";
constexpr auto HostProviderFile="bin/onnxruntime_providers_cuda.dll";
constexpr int HostWorkerLoadTimeoutMs=30000;
#endif

QString modelRoot(const QString &model){
    return RuntimeDiscovery::rvAssistModelRoot(model);
}
bool hdSource(const QJsonObject &m,bool wide=false,bool allowReuse=false) {
    const auto integer=[&](const char *key){const double n=m[key].toDouble(-1);return m[key].isDouble()&&n>0&&n<=9007199254740991.0&&std::floor(n)==n;};
    if(m["source_protocol"]!="HDA1"||m["source_transport"]!="rtsp_h265"
        ||m["model"]!="Native"||m["source_rgb_width"]!=2304||m["source_rgb_height"]!=1296
        ||m["rgb_width"]!=576||m["rgb_height"]!=324||!m["dtof_present"].isBool()
        ||m["rectify_map_sha256"]!=QLatin1String(RvCameraView::profile(wide).mapSha)
        ||(wide&&m["assist_camera_profile"]!="wide")
        ||!integer("frame_id")||!integer("generation")||!integer("host_decoded_monotonic_ns")
        ||!QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(m["boot_id"].toString()).hasMatch())return false;
    if(m["dtof_present"].toBool()) {
        const double delta=m["pair_delta_ms"].toDouble(1000),bound=m["pair_clock_uncertainty_ms"].toDouble(-1);
        if(m["work_mode"]!="rgb_tof"||!integer("tof_sequence")||!integer("tof_timestamp_ns")||!integer("rgb_timestamp_ns")
            ||(m["tof_reused"]!=false&&(!allowReuse||m["tof_reused"]!=true))
            ||!std::isfinite(delta)||!std::isfinite(bound)||bound<0||bound>35||std::abs(delta)+bound>200
            ||(allowReuse&&std::abs(delta)>RvTofFrame::GlobalScaleMaxPairDeltaMs)
            ||std::abs((m["tof_timestamp_ns"].toDouble()-m["rgb_timestamp_ns"].toDouble())/1e6-delta)>.001
            ||m["pair_clock_source"]!="rtcp_prft_board_realtime_monotonic"||m["source_candidate_mode"]!=14
            ||m["source_engine"]!="native-hd-capture-v1"||m["generation_source"]!="host_presentation_epoch"
            ||!QRegularExpression("^[0-9a-f]{32}$").match(m["source_generation"].toString()).hasMatch())return false;
    }else {
        if(m["work_mode"]!="host_rgb")return false;
        for(const auto *key:{"tof_sequence","tof_timestamp_ns","pair_delta_ms","tof_reused","rgb_timestamp_ns"})if(m.contains(key))return false;
    }
    return true;
}
QJsonObject nativeContext(const QJsonObject &m){
    QJsonObject result;
    for(const auto *key:{"boot_id","source_generation","dtof_fov_correction","tof_fov_correction","tof_warning_flags",
                        "dtof_width","dtof_height","dtof_bytes","dtof_format","dtof_range_unit","dtof_source","dtof_coordinate"})
        result[key]=m.value(key);
    return result;
}
void dropHdTof(QJsonObject &m) {
    m["dtof_present"]=false;m["work_mode"]="host_rgb";
    m["pairing_policy"]="no_current_native_measurement";m["dtof_absent_reason"]="native_measurement_already_dispatched";
    for(const auto *key:{"tof_sequence","tof_timestamp_ns","pair_delta_ms","tof_reused","rgb_timestamp_ns",
            "dtof_width","dtof_height","dtof_bytes","dtof_format","dtof_range_unit","dtof_source","dtof_coordinate",
            "dtof_fov_correction","tof_warning_flags","pair_clock_uncertainty_ms","pair_clock_source"})m.remove(key);
}
qint64 monotonicMs() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
QByteArray hashFile(const QString &path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot hash runtime resource");
    const auto check=[](NTSTATUS status) {if(status<0)throw std::runtime_error("Cannot hash runtime resource");};
    // The multi-GB closure must use optimized SHA256 even with Qt's debug DLLs.
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
    const auto releaseAlgorithm=qScopeGuard([&]{BCryptCloseAlgorithmProvider(algorithm,0);});
    ULONG objectSize=0,returned=0;
    check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0));
    if(returned!=sizeof(objectSize)||!objectSize||objectSize>1024*1024)throw std::runtime_error("Invalid SHA256 provider object size");
    QByteArray object(int(objectSize),Qt::Uninitialized),digest(32,Qt::Uninitialized),chunk(1024*1024,Qt::Uninitialized);
    BCRYPT_HASH_HANDLE hash=nullptr;
    check(BCryptCreateHash(algorithm,&hash,reinterpret_cast<PUCHAR>(object.data()),objectSize,nullptr,0,0));
    const auto releaseHash=qScopeGuard([&]{BCryptDestroyHash(hash);});
    qint64 size=0;
    while((size=file.read(chunk.data(),chunk.size()))>0)
        check(BCryptHashData(hash,reinterpret_cast<PUCHAR>(chunk.data()),ULONG(size),0));
    if(size<0)throw std::runtime_error("Cannot read runtime resource for SHA256");
    check(BCryptFinishHash(hash,reinterpret_cast<PUCHAR>(digest.data()),ULONG(digest.size()),0));
    return digest.toHex();
}
QJsonObject readJson(const QString &path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>65536)throw std::runtime_error("Native auxiliary runtime manifest missing");
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())throw std::runtime_error("Invalid native auxiliary runtime manifest");
    return doc.object();
}
QByteArray packet(const QJsonObject &json,const QByteArray &body={},const QByteArray &aux={}) {
    const auto header=QJsonDocument(json).toJson(QJsonDocument::Compact);
    QByteArray out(12,Qt::Uninitialized);
    qToLittleEndian<quint32>(quint32(header.size()),out.data());
    qToLittleEndian<quint32>(quint32(body.size()),out.data()+4);
    qToLittleEndian<quint32>(quint32(aux.size()),out.data()+8);
    return out+header+body+aux;
}
QByteArray planarRgb(const QImage &source) {
    const auto image=source.convertToFormat(QImage::Format_RGB888);QByteArray out(RgbBytes,Qt::Uninitialized);
    for(int y=0;y<324;++y)for(int x=0;x<576;++x)for(int c=0;c<3;++c)out[c*Pixels+y*576+x]=char(image.constScanLine(y)[3*x+c]);
    return out;
}
QImage unpackRgb(const QByteArray &bytes) {
    QImage image(576,324,QImage::Format_RGB888);
    for(int y=0;y<324;++y)for(int x=0;x<576;++x)for(int c=0;c<3;++c)image.scanLine(y)[3*x+c]=quint8(bytes[c*Pixels+y*576+x]);
    return image;
}
// Explicit hardware-review opt-in, one bounded image per worker. All panels
// share one paired frame; no native measurements or float tensors are saved.
bool saveSupportReview(const QString &directory,const QImage &rgb,const std::vector<float> &mono,
                       const RvAssistFusion::Result &fused,const RvAssistFusion::Result &visible,
                       const RvTofFrame &tof,const RvAssistFusion::Calibration &cal,bool wide,
                       const QString &filename=QStringLiteral("tof_support_same_frame.png"),float colourMax=1.f) {
    if(!QDir::isAbsolutePath(directory)||!QDir(directory).exists()||!fused.ok()||!visible.ok())return false;
    const auto painted=[&](const std::vector<float>&z){QImage image(576,324,QImage::Format_RGB32);
        for(int i=0;i<Pixels;++i)image.setPixel(i%576,i/576,DepthColorScale::color(z[i],.2,colourMax));return image;};
    const auto decoded=[](const QByteArray &bytes){std::vector<float> z(Pixels);for(int i=0;i<Pixels;++i){
        const quint32 bits=qFromLittleEndian<quint32>(bytes.constData()+4*i);std::memcpy(&z[i],&bits,4);}return z;};
    const auto after=decoded(fused.depth),limited=decoded(visible.depth);
    auto overlay=rgb.convertToFormat(QImage::Format_RGB32);QPainter points(&overlay);
    std::vector<bool> adopted(1536,false);for(const auto id:fused.diagnostics["used_native_indices"].toArray())adopted[id.toInt()]=true;
    const double focal=RvCameraView::profile(wide).focal;
    for(int i=0;i<tof.sampleCount();++i){const double r=tof.rangeUq2(i)*.00025;if(!tof.valid(i)||tof.snr(i)<6||r<RvAssistFusion::MinimumRadialMetres||r>10)continue;
        const auto &q=cal.cameraRays[i];const double z=r*q[2]+cal.translation[2];if(z<=0)continue;
        const QPointF p(focal*(r*q[0]+cal.translation[0])/z+287.5,focal*(r*q[1]+cal.translation[1])/z+161.5);
        if(p.x()<0||p.x()>=576||p.y()<0||p.y()>=324)continue;
        points.setPen(adopted[i]?Qt::green:Qt::white);points.setBrush(QColor::fromRgb(DepthColorScale::color(float(z),.2,colourMax)));
        points.drawEllipse(p,2.2,2.2);
    }points.end();
    QImage mask(576,324,QImage::Format_RGB32);for(int i=0;i<Pixels;++i)mask.setPixel(i%576,i/576,limited[i]>0?qRgb(255,255,255):qRgb(0,0,0));
    QImage comparison(1152,1056,QImage::Format_RGB32);comparison.fill(Qt::black);QPainter painter(&comparison);
    // This worker owns QCoreApplication, so never access the font database.
    // The opt-in GUI review harness adds captions after loading this image.
    const auto panel=[&](int index,const QImage &image){painter.drawImage((index%2)*576,(index/2)*352+28,image);};
    panel(0,rgb);panel(1,overlay);panel(2,painted(mono));panel(3,painted(after));panel(4,painted(limited));panel(5,mask);
    painter.end();return comparison.save(QDir(directory).filePath(filename));
}
bool protectedModelContract(const QJsonObject &manifest)
{
    const auto protection = manifest["model_protection"].toObject();
    const auto files = manifest["files"].toObject();
    return protection["scheme"] == "aes256-gcm-memory-v1"
        && protection["loader_abi"].toInt() == 1
        && protection["ciphertext"] == "model.dctsm"
        && protection["loader"] == "bin/DctsModelRuntime.dll"
        && files.contains("model.dctsm") && files.contains("bin/DctsModelRuntime.dll");
}

class NativeModel {
public:
    QJsonObject modelInfo,colorDiagnostics,storageInfo;
    const bool profileDepthor=qEnvironmentVariable("RV_DEPTHOR_PERF_PROFILE")=="1";
    QJsonObject depthorTiming;
    bool wide=false;
    QString hostModel;
    const RvCameraView::Profile &view() const {return RvCameraView::profile(hostModel);}
    RvAssistFusion::Calibration calibration;
    NativeModel()=default;
    NativeModel(const NativeModel &)=delete;
    ~NativeModel() {
        if(cudaSync)cudaSync();
        if(api) {
            if(graphBinding)api->ReleaseIoBinding(graphBinding);
            if(graphRun)api->ReleaseRunOptions(graphRun);
            for(auto value:graphValues)if(value)api->ReleaseValue(value);
            // Captured graphs and custom kernels must die before their PTX/domain.
            if(session)api->ReleaseSession(session);
            for(auto pointer:graphDevice)if(pointer&&cudaFree)cudaFree(pointer);
            if(customRegistered&&releaseCustom)releaseCustom();
            if(graphMemory)api->ReleaseMemoryInfo(graphMemory);
            if(memory)api->ReleaseMemoryInfo(memory);if(env)api->ReleaseEnv(env);
        }
        protectedLibrary.unload();customLibrary.unload();cudaLibrary.unload();
        library.unload();if(directory)RemoveDllDirectory(directory);
    }
    void initialize(const QString &root,const QString &selectedHostModel=QString()) {
        QElapsedTimer initializationClock;initializationClock.start();
        const QDir dir(root);const auto manifest=readJson(dir.filePath("runtime.json"));
        if(manifest["schema"].toInt()!=1||manifest["backend"]!=HostRuntimeBackend||manifest["ort_version"]!=HostOrtVersion)
            throw std::runtime_error("Unsupported native auxiliary runtime");
        const auto files=manifest["files"].toObject();
        const bool protectedModel=manifest.contains("model_protection");
        const auto protection=manifest["model_protection"].toObject();
#ifdef DCTS_INSTALLER_BUILD
        if(!protectedModel)throw std::runtime_error("Protected model manifest missing");
#endif
        if(protectedModel&&!protectedModelContract(manifest))
            throw std::runtime_error("Invalid protected model contract");
        const QStringList requiredFiles=
            QStringList{"bin/onnxruntime.dll",HostProviderFile,"bin/onnxruntime_providers_shared.dll","model.json"};
        for(const auto &required:requiredFiles)
            if(!files.contains(required))throw std::runtime_error("Incomplete native auxiliary runtime closure");
        for(auto i=files.begin();i!=files.end();++i) {
            const auto relative=i.key();
            if(QDir::isAbsolutePath(relative)||relative.contains("..")||relative.contains('\\')||!i.value().isString()
                ||i.value().toString().size()!=64||hashFile(dir.filePath(relative))!=i.value().toString().toLatin1())
                throw std::runtime_error("Native auxiliary runtime fingerprint mismatch");
        }
        modelInfo=readJson(dir.filePath("model.json"));
        wide=modelInfo.value("camera_profile")=="wide";
        const auto name=modelInfo["model"].toString();
        if(name=="UniDepthV2-Small")hostModel="unidepth";
        else if(name=="DEPTHOR-Small-V3"){
            hostModel=modelInfo["host_model"].toString();
            if(!DepthorV3::supported(hostModel)||wide)throw std::runtime_error("Unsupported DEPTHOR V3 identity");
        }
        else if(name=="LiteSR-RGB")hostModel="colorsr_rgb";
        else if(name=="LiteSR-NV12")hostModel="colorsr_nv12";
        else throw std::runtime_error("UniDepth model and RV camera calibration binding mismatch");
        if(wide)hostModel+="_wide";
        if(wide&&modelInfo["network_profile"]=="rgb576_pad6_v1")hostModel+=modelInfo["precision"]=="fp16-mixed-trial"?"_576_fp16":"_576";
        if(!selectedHostModel.isEmpty()){
            if(!RvCameraView::isUniDepthNative16(selectedHostModel)||hostModel!="unidepth_wide_576_fp16")
                throw std::runtime_error("Native16 requires the bound RGB576 FP16 model");
            hostModel=selectedHostModel;
        }
        if(protectedModel) {
            if(hostModel!="unidepth_wide_576_fp16_tof16"||protection["plaintext_sha256"]!=QLatin1String(view().graphSha))
                throw std::runtime_error("Protected model identity mismatch");
        } else if(!files.contains(QLatin1String(view().graphFile)))throw std::runtime_error("Incomplete native auxiliary runtime closure");
        if(RvCameraView::isUniDepth(hostModel)) {
            if((modelInfo.contains("camera_profile")&&modelInfo["camera_profile"]!=(wide?"wide":"standard"))
                ||modelInfo["weights_sha256"]!=QLatin1String(view().weightsSha)||modelInfo["onnx_sha256"]!=QLatin1String(view().graphSha)
                ||(!protectedModel&&hashFile(dir.filePath(QLatin1String(view().graphFile)))!=view().graphSha)
                ||modelInfo["rectify_sha256"]!=QLatin1String(view().mapSha)
                ||modelInfo["rectified_intrinsics_integer_centres"].toArray()!=RvCameraView::intrinsics(wide)
                ||(view().resolutionLevel<0?!modelInfo["resolution_level"].isNull():modelInfo["resolution_level"].toInt(-1)!=view().resolutionLevel)
                ||modelInfo["backend"]!="onnxruntime-cuda"
                ||modelInfo["input"].toArray()!=QJsonArray{1,3,324,576}||modelInfo["output"].toArray()!=QJsonArray{1,1,324,576}
                ||modelInfo["input_dtype"]!="uint8"||modelInfo["output_dtype"]!="float32"
                ||modelInfo["network_shape"].toArray()!=QJsonArray{view().networkHeight,view().networkWidth}||modelInfo["depth_unit"]!="camera-Z_m_float32")
                throw std::runtime_error("UniDepth model and RV camera calibration binding mismatch");
            if(view().networkProfile&&(modelInfo["network_profile"]!=QLatin1String(view().networkProfile)
                ||modelInfo["rgb_padding_ltrb"].toArray()!=QJsonArray{6,6,6,6}||modelInfo["output_crop_ltrb"].toArray()!=QJsonArray{6,6,6,6}))
                throw std::runtime_error("UniDepth padding and output grid binding mismatch");
            if(view().provisional&&(manifest["validation_status"]!="provisional-numerical-candidate"
                ||modelInfo["provisional"]!=true||modelInfo["candidate_parity_mean_limit"]!=view().parityMeanLimit
                ||modelInfo["candidate_parity_max_limit"]!=view().parityMaxLimit))
                throw std::runtime_error("UniDepth temporary candidate contract mismatch");
            if(modelInfo["precision"]!=QLatin1String(view().precision)||modelInfo["conversion"]!=QLatin1String(view().conversion)
                ||modelInfo["source_onnx_sha256"]!=QLatin1String(view().sourceSha))
                throw std::runtime_error("Unsupported UniDepth precision contract");
        }
        if(RvCameraView::isColorLiteSR(hostModel)&&(modelInfo["host_model"]!=hostModel||modelInfo["weights_sha256"]!=view().weightsSha||modelInfo["graph_sha256"]!=view().graphSha||hashFile(dir.filePath(view().graphFile))!=view().graphSha||modelInfo["geometry"]!="projected-camera-radial-v1"))throw std::runtime_error("Color LiteSR identity mismatch");
        if(DepthorV3::supported(hostModel)){
            if(modelInfo["weights_sha256"]!=view().weightsSha||modelInfo["onnx_sha256"]!=view().graphSha
                ||hashFile(dir.filePath(view().graphFile))!=view().graphSha||modelInfo["source_onnx_sha256"]!=view().sourceSha
                ||modelInfo["precision"]!=view().precision||modelInfo["conversion"]!=view().conversion
                ||modelInfo["rectify_sha256"]!=view().mapSha||modelInfo["geometry"]!="standard_crop432_to640_camera_z_v1"
                ||modelInfo["input"].toArray()!=QJsonArray{1,3,480,640}||modelInfo["sparse_input"].toArray()!=QJsonArray{1,1,480,640}
                ||modelInfo["output"].toArray()!=QJsonArray{1,1,480,640}||modelInfo["input_dtype"]!="float32"||modelInfo["output_dtype"]!="float32"
                ||modelInfo["depth_unit"]!="camera-Z_m_float32"||modelInfo["backend"]!="onnxruntime-cuda"||modelInfo["v3_old_range_gate_passed"]!=false
                ||modelInfo["maximum_pair_delta_ms"]!=RvCameraView::DepthorMaxPairDeltaMs||modelInfo["allow_native_reuse"]!=true
                ||modelInfo["native_pairing_policy"]!=RvCameraView::DepthorPairingPolicy)
                throw std::runtime_error("DEPTHOR V3 graph, precision or geometry binding mismatch");
            if(cudaGraphs()&&(files["bin/cspn_custom_op.dll"]!=RvCameraView::DepthorCspnDllSha
                ||files["bin/cspn_kernels.ptx"]!=RvCameraView::DepthorCspnPtxSha
                ||modelInfo["custom_op_library"]!="bin/cspn_custom_op.dll"||modelInfo["custom_op_sha256"]!=RvCameraView::DepthorCspnDllSha
                ||modelInfo["custom_op_ptx"]!="bin/cspn_kernels.ptx"||modelInfo["custom_op_ptx_sha256"]!=RvCameraView::DepthorCspnPtxSha
                ||modelInfo["enable_cuda_graph"]!=true||modelInfo["use_tf32"]!=false||modelInfo["fixed_gpu_io_binding"]!=true))
                throw std::runtime_error("DEPTHOR FP32 CSPN/CUDA Graph runtime binding mismatch");
        }
        QFile geometry(":/camera/dtof_m48_geometry.bin");QString error;
        if(!geometry.open(QIODevice::ReadOnly)||!RvAssistFusion::loadCalibration(geometry.readAll(),calibration,error))
            throw std::runtime_error(("Missing or incompatible dToF calibration: "+error).toStdString());
        // Restricted DLL search in this disposable native process only.
        if(!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))throw std::runtime_error("Cannot restrict native DLL search");
        directory=AddDllDirectory(reinterpret_cast<const wchar_t *>(dir.filePath("bin").utf16()));
        if(!directory)throw std::runtime_error("Cannot register native auxiliary DLL directory");
#ifdef DCTS_INSTALLER_BUILD
        checkTensorRtCudaDevice();
        // RTX v0.5 falls back to memory when its engine staging directory is unavailable.
        // The ciphertext itself is a regular file, so the path below cannot contain temporary files.
        const auto deniedTemp=QDir::toNativeSeparators(dir.filePath("model.dctsm/no-temp"));
        const auto deniedTempPath=reinterpret_cast<const wchar_t *>(deniedTemp.utf16());
        if(!SetEnvironmentVariableW(L"TEMP",deniedTempPath)
           ||!SetEnvironmentVariableW(L"TMP",deniedTempPath)
           ||!SetEnvironmentVariableW(L"CUDA_CACHE_DISABLE",L"1"))
            throw std::runtime_error("Cannot set memory-only engine staging policy");
#endif
        library.setFileName(dir.filePath("bin/onnxruntime.dll"));
        if(!library.load())throw std::runtime_error(library.errorString().toStdString());
        using GetApi=const OrtApiBase *(ORT_API_CALL *)();
        const auto getApi=reinterpret_cast<GetApi>(library.resolve("OrtGetApiBase"));
        if(!getApi||QString::fromLatin1(getApi()->GetVersionString())!=HostOrtVersion)throw std::runtime_error("ONNX Runtime version mismatch");
        api=getApi()->GetApi(ORT_API_VERSION);if(!api)throw std::runtime_error("ONNX Runtime API unavailable");
        check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"rv-assist",&env));
        OrtSessionOptions *options=nullptr;check(api->CreateSessionOptions(&options));
        const auto release=qScopeGuard([&]{api->ReleaseSessionOptions(options);});
        check(api->SetIntraOpNumThreads(options,4));check(api->SetSessionGraphOptimizationLevel(options,ORT_ENABLE_ALL));
#ifdef DCTS_INSTALLER_BUILD
        const auto epPath=dir.filePath(HostProviderFile);
        check(api->RegisterExecutionProviderLibrary(env,TensorRtProviderName,
              reinterpret_cast<const wchar_t *>(epPath.utf16())));
        const OrtEpDevice *const *devices=nullptr;
        size_t count=0;
        check(api->GetEpDevices(env,&devices,&count));
        const OrtEpDevice *device=nullptr;
        for(size_t i=0;i<count;++i){
            if(QLatin1String(api->EpDevice_EpName(devices[i]))==TensorRtProviderName){device=devices[i];break;}
        }
        if(!device)throw std::runtime_error("RTX_DEVICE_UNSUPPORTED");
        const char *keys[]={"enable_cuda_graph","nv_dump_subgraphs"};
        const char *values[]={"1","0"};
        check(api->SessionOptionsAppendExecutionProvider_V2(options,env,&device,1,keys,values,2));
        check(api->AddSessionConfigEntry(options,"ep.context_enable","0"));
#else
        if(cudaGraphs()){
            customLibrary.setFileName(dir.filePath("bin/cspn_custom_op.dll"));
            if(!customLibrary.load())throw std::runtime_error(customLibrary.errorString().toStdString());
            using Register=OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *,const OrtApiBase *);
            const auto registerCustom=reinterpret_cast<Register>(customLibrary.resolve("RegisterCustomOps"));
            releaseCustom=reinterpret_cast<decltype(releaseCustom)>(customLibrary.resolve("ReleaseExperimentResources"));
            computeCalls=reinterpret_cast<decltype(computeCalls)>(customLibrary.resolve("GetExperimentComputeCalls"));
            if(!registerCustom||!releaseCustom||!computeCalls)throw std::runtime_error("CSPN custom operator API unavailable");
            customRegistered=true;check(registerCustom(options,getApi()));
            check(api->SetInterOpNumThreads(options,1));check(api->SetSessionExecutionMode(options,ORT_SEQUENTIAL));
        }
        if(DepthorV3::supported(hostModel)||QString::fromLatin1(view().precision)=="fp32"||QString::fromLatin1(view().precision)=="fp32-tf32") {
            // Original FP32 models also keep full-precision CUDA matmul/conv.
            OrtCUDAProviderOptionsV2 *cuda=nullptr;check(api->CreateCUDAProviderOptions(&cuda));
            const auto freeCuda=qScopeGuard([&]{api->ReleaseCUDAProviderOptions(cuda);});
            // Submit one complete option set: UpdateCUDAProviderOptions resets
            // omitted fields, so a second graph-only update would re-enable TF32.
            const char *keys[]={"device_id","cudnn_conv_algo_search","use_tf32","enable_cuda_graph","do_copy_in_default_stream"};
            const char *values[]={"0","HEURISTIC",QString::fromLatin1(view().precision)=="fp32-tf32"?"1":"0","1","1"};
            check(api->UpdateCUDAProviderOptions(cuda,keys,values,cudaGraphs()?5:3));
            if(cudaGraphs()){
                OrtAllocator *allocator=nullptr;char *serialized=nullptr;check(api->GetAllocatorWithDefaultOptions(&allocator));
                check(api->GetCUDAProviderOptionsAsString(cuda,allocator,&serialized));
                const auto freeString=qScopeGuard([&]{allocator->Free(allocator,serialized);});graphProviderOptions=QString::fromUtf8(serialized);
                const auto actualOptions=graphProviderOptions.split(';');
                // ORT 1.23.2 omits enable_cuda_graph from V2 serialization;
                // numerical replay checks verify it via the custom Compute counter.
                if(!actualOptions.contains("use_tf32=0"))
                    throw std::runtime_error("DEPTHOR FP32 TF32-off option was not applied");
            }
            check(api->SessionOptionsAppendExecutionProvider_CUDA_V2(options,cuda));
        } else {
            OrtCUDAProviderOptions cuda;cuda.device_id=0;cuda.cudnn_conv_algo_search=OrtCudnnConvAlgoSearchHeuristic;
            check(api->SessionOptionsAppendExecutionProvider_CUDA(options,&cuda));
        }
#endif
        if(protectedModel) {
            protectedLibrary.setFileName(dir.filePath(protection["loader"].toString()));
            if(!protectedLibrary.load())throw std::runtime_error(protectedLibrary.errorString().toStdString());
            const auto create=reinterpret_cast<DctsCreateProtectedSessionFn>(protectedLibrary.resolve("DctsCreateProtectedSession"));
            if(!create)throw std::runtime_error("Protected model loader API missing");
            DctsModelLoadMetrics metrics{};metrics.struct_bytes=sizeof(metrics);char error[2048]{};
            const auto path=dir.filePath(protection["ciphertext"].toString());
            if(create(api,env,options,reinterpret_cast<const wchar_t *>(path.utf16()),view().graphSha,
                      &session,&metrics,error,sizeof(error)))throw std::runtime_error(error);
            storageInfo={{"kind","aes256-gcm-memory-v1"},{"plaintext_file_used",false},
                {"plaintext_sha256_verified",true},{"plaintext_bytes",qint64(metrics.plaintext_bytes)},
                {"package_read_ms",metrics.read_ms},{"decrypt_ms",metrics.decrypt_ms},{"digest_ms",metrics.digest_ms},
                {"session_create_ms",metrics.session_create_ms},{"plaintext_clear_ms",metrics.plaintext_clear_ms},
                {"loader_total_ms",metrics.total_ms}};
        } else {
            const auto path=dir.filePath(QLatin1String(view().graphFile));QElapsedTimer sessionClock;sessionClock.start();
            check(api->CreateSession(env,reinterpret_cast<const wchar_t *>(path.utf16()),options,&session));
            storageInfo={{"kind","plaintext-file"},{"plaintext_file_used",true},{"session_create_ms",sessionClock.nsecsElapsed()/1e6}};
        }
        check(api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&memory));
        if(cudaGraphs())initializeDepthorGraph(dir);
#ifdef DCTS_INSTALLER_BUILD
        storageInfo["provider"]=TensorRtProviderName;
        storageInfo["ort_version"]=HostOrtVersion;
#endif
        storageInfo["initialization_ms"]=initializationClock.nsecsElapsed()/1e6;
    }
    std::vector<float> infer(const QByteArray &rgb) {
        if(rgb.size()!=RgbBytes)throw std::runtime_error("Invalid auxiliary RGB input");
        OrtValue *input=nullptr,*output=nullptr;
        const auto release=qScopeGuard([&]{if(input)api->ReleaseValue(input);if(output)api->ReleaseValue(output);});
        const int64_t dims[]={1,3,324,576};
        check(api->CreateTensorWithDataAsOrtValue(memory,const_cast<char *>(rgb.constData()),rgb.size(),dims,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8,&input));
        const char *inputs[]={"rgb"},*outputs[]={"depth"};const OrtValue *values[]={input};
        check(api->Run(session,nullptr,inputs,values,1,outputs,1,&output));
        OrtTensorTypeAndShapeInfo *shape=nullptr;check(api->GetTensorTypeAndShape(output,&shape));
        const auto releaseShape=qScopeGuard([&]{api->ReleaseTensorTypeAndShapeInfo(shape);});
        size_t count=0;ONNXTensorElementDataType type;check(api->GetTensorShapeElementCount(shape,&count));check(api->GetTensorElementType(shape,&type));
        if(count!=Pixels||type!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)throw std::runtime_error("Invalid auxiliary depth output shape");
        void *data=nullptr;check(api->GetTensorMutableData(output,&data));
        const auto *z=static_cast<float *>(data);std::vector<float> out(z,z+Pixels);
        for(float v:out)if(!std::isfinite(v)||v<=0)throw std::runtime_error("Invalid model output values");
        return out;
    }

    std::vector<float> inferDepthorNetwork(const std::vector<float> &rgb,const std::vector<float> &sparse){
        QElapsedTimer stage;if(profileDepthor)stage.start();
        if(rgb.size()!=3*DepthorV3::N||sparse.size()!=DepthorV3::N)throw std::runtime_error("Invalid DEPTHOR network input shape");
        if(cudaGraphs()){
            uploadDepthorGraph(rgb,sparse);
            if(profileDepthor){depthorTiming["tensor_setup_ms"]=stage.nsecsElapsed()/1e6;stage.restart();}
            runDepthorGraph(graphRun);
            std::vector<float> result(DepthorV3::N);
            checkedCuda(cudaCopy(result.data(),graphDevice[2],result.size()*sizeof(float),2));
            // Keep the existing CPU-I/O timing endpoint, now including bound H2D.
            if(profileDepthor){depthorTiming["network_run_cpu_io_ms"]=depthorTiming["tensor_setup_ms"].toDouble()+stage.nsecsElapsed()/1e6;stage.restart();}
            for(float z:result)if(!std::isfinite(z)||z<=0)throw std::runtime_error("Invalid DEPTHOR prediction values");
            if(profileDepthor)depthorTiming["output_copy_validation_ms"]=stage.nsecsElapsed()/1e6;
            return result;
        }
        OrtValue *inputs[2]={},*output=nullptr;
        const auto release=qScopeGuard([&]{for(auto value:inputs)if(value)api->ReleaseValue(value);if(output)api->ReleaseValue(output);});
        const int64_t rgbShape[]={1,3,480,640},depthShape[]={1,1,480,640};
        check(api->CreateTensorWithDataAsOrtValue(memory,const_cast<float*>(rgb.data()),rgb.size()*4,rgbShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&inputs[0]));
        check(api->CreateTensorWithDataAsOrtValue(memory,const_cast<float*>(sparse.data()),sparse.size()*4,depthShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&inputs[1]));
        const char *names[]={"rgb","sparse_depth"},*outputs[]={"depth"};const OrtValue *values[]={inputs[0],inputs[1]};
        if(profileDepthor){depthorTiming["tensor_setup_ms"]=stage.nsecsElapsed()/1e6;stage.restart();}
        check(api->Run(session,nullptr,names,values,2,outputs,1,&output));
        if(profileDepthor){depthorTiming["network_run_cpu_io_ms"]=stage.nsecsElapsed()/1e6;stage.restart();}
        OrtTensorTypeAndShapeInfo *shape=nullptr;check(api->GetTensorTypeAndShape(output,&shape));
        const auto freeShape=qScopeGuard([&]{api->ReleaseTensorTypeAndShapeInfo(shape);});
        size_t count=0,dimensions=0;ONNXTensorElementDataType type;int64_t actual[4]{};
        check(api->GetDimensionsCount(shape,&dimensions));check(api->GetTensorShapeElementCount(shape,&count));check(api->GetTensorElementType(shape,&type));
        if(dimensions!=4||count!=DepthorV3::N||type!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)throw std::runtime_error("Invalid DEPTHOR output tensor");
        check(api->GetDimensions(shape,actual,4));
        if(!std::equal(actual,actual+4,depthShape))throw std::runtime_error("DEPTHOR output grid mismatch");
        void *data=nullptr;check(api->GetTensorMutableData(output,&data));
        const auto *p=static_cast<const float*>(data);std::vector<float> result(p,p+count);
        for(float z:result)if(!std::isfinite(z)||z<=0)throw std::runtime_error("Invalid DEPTHOR prediction values");
        if(profileDepthor)depthorTiming["output_copy_validation_ms"]=stage.nsecsElapsed()/1e6;
        return result;
    }
    std::vector<float> inferDepthor(const QByteArray &rgb,const RvTofFrame &tof,int rotation){
        QElapsedTimer stage;if(profileDepthor)stage.start();
        auto image=DepthorV3::prepareRgb(rgb,rotation);
        if(profileDepthor){depthorTiming["rgb_prepare_ms"]=stage.nsecsElapsed()/1e6;stage.restart();}
        auto sparse=DepthorV3::project(tof,rotation,colorDiagnostics);
        if(profileDepthor)depthorTiming["native_projection_ms"]=stage.nsecsElapsed()/1e6;
        auto predicted=inferDepthorNetwork(image,sparse);
        if(profileDepthor)stage.restart();
        auto restored=DepthorV3::restore(predicted,rotation);
        if(profileDepthor)depthorTiming["restore_ms"]=stage.nsecsElapsed()/1e6;
        return restored;
    }

    QJsonObject benchmarkDepthorForward(const std::vector<float> &rgb,const std::vector<float> &sparse,int seconds){
        if(hostModel!="depthor_v3_fp32"&&hostModel!="depthor_v3_fp16")throw std::runtime_error("Forward benchmark requires DEPTHOR V3 FP32 or FP16");
        if(cudaGraphs())return benchmarkDepthorGraph(rgb,sparse,seconds);
        OrtValue *input[2]={},*output=nullptr;
        const auto release=qScopeGuard([&]{for(auto v:input)if(v)api->ReleaseValue(v);if(output)api->ReleaseValue(output);});
        const int64_t rgbShape[]={1,3,480,640},depthShape[]={1,1,480,640};
        check(api->CreateTensorWithDataAsOrtValue(memory,const_cast<float*>(rgb.data()),rgb.size()*4,rgbShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&input[0]));
        check(api->CreateTensorWithDataAsOrtValue(memory,const_cast<float*>(sparse.data()),sparse.size()*4,depthShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&input[1]));
        const char *names[]={"rgb","sparse_depth"},*outputs[]={"depth"};const OrtValue *values[]={input[0],input[1]};
        for(int i=0;i<5;++i){check(api->Run(session,nullptr,names,values,2,outputs,1,&output));api->ReleaseValue(output);output=nullptr;}
        QElapsedTimer total,one;QVector<double> samples;total.start();
        while(total.elapsed()<seconds*1000){one.start();check(api->Run(session,nullptr,names,values,2,outputs,1,&output));
            samples.append(one.nsecsElapsed()/1e6);api->ReleaseValue(output);output=nullptr;}
        const double duration=total.nsecsElapsed()/1e9;std::sort(samples.begin(),samples.end());double sum=0;for(double n:samples)sum+=n;
        QJsonObject result{{"pass",samples.size()>10},{"host_model",hostModel},{"precision",view().precision},{"graph_sha256",view().graphSha},{"seconds",duration},
            {"iterations",samples.size()},{"forward_fps",samples.size()/duration},{"mean_run_ms",sum/samples.size()},
            {"median_run_ms",samples[samples.size()/2]},{"p95_run_ms",samples[int((samples.size()-1)*.95)]},
            {"scope","ORT forward with prepared/reused CPU tensors; includes H2D/D2H and synchronization; excludes image processing, IPC and GUI"},
            {"gpu_kernel_only",false},{"camera_started",false}};
        const auto reference=inferDepthorNetwork(rgb,sparse);
        QLibrary cuda(QFileInfo(library.fileName()).dir().filePath("cudart64_12.dll"));
        using Copy=int (*)(void *,const void *,size_t,int);
        if(!cuda.load())throw std::runtime_error("CUDA runtime unavailable for resident tensor benchmark");
        const auto copy=reinterpret_cast<Copy>(cuda.resolve("cudaMemcpy"));
        if(!copy)throw std::runtime_error("CUDA copy entry unavailable");
        OrtMemoryInfo *deviceInfo=nullptr;OrtAllocator *allocator=nullptr;OrtIoBinding *binding=nullptr;OrtValue *gpu[3]={};
        const auto freeGpu=qScopeGuard([&]{if(binding)api->ReleaseIoBinding(binding);for(auto v:gpu)if(v)api->ReleaseValue(v);
            if(allocator)api->ReleaseAllocator(allocator);if(deviceInfo)api->ReleaseMemoryInfo(deviceInfo);});
        check(api->CreateMemoryInfo("Cuda",OrtDeviceAllocator,0,OrtMemTypeDefault,&deviceInfo));
        check(api->CreateAllocator(session,deviceInfo,&allocator));
        check(api->CreateTensorAsOrtValue(allocator,rgbShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&gpu[0]));
        check(api->CreateTensorAsOrtValue(allocator,depthShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&gpu[1]));
        check(api->CreateTensorAsOrtValue(allocator,depthShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&gpu[2]));
        void *device[3]={};for(int i=0;i<3;++i)check(api->GetTensorMutableData(gpu[i],&device[i]));
        if(copy(device[0],rgb.data(),rgb.size()*4,1)||copy(device[1],sparse.data(),sparse.size()*4,1))throw std::runtime_error("CUDA benchmark input copy failed");
        check(api->CreateIoBinding(session,&binding));check(api->BindInput(binding,"rgb",gpu[0]));check(api->BindInput(binding,"sparse_depth",gpu[1]));check(api->BindOutput(binding,"depth",gpu[2]));
        for(int i=0;i<5;++i){check(api->RunWithBinding(session,nullptr,binding));check(api->SynchronizeBoundOutputs(binding));}
        QVector<double> resident;total.restart();
        while(total.elapsed()<seconds*1000){one.restart();check(api->RunWithBinding(session,nullptr,binding));check(api->SynchronizeBoundOutputs(binding));resident.append(one.nsecsElapsed()/1e6);}
        const double gpuDuration=total.nsecsElapsed()/1e9;std::sort(resident.begin(),resident.end());double gpuSum=0;for(double n:resident)gpuSum+=n;
        std::vector<float> valuesOut(DepthorV3::N);if(copy(valuesOut.data(),device[2],valuesOut.size()*4,2))throw std::runtime_error("CUDA benchmark output copy failed");
        double maximum=0,relative=0;for(int i=0;i<DepthorV3::N;++i){if(!std::isfinite(valuesOut[i]))throw std::runtime_error("Resident forward produced invalid depth");
            const double delta=std::abs(double(valuesOut[i])-reference[i]);maximum=std::max(maximum,delta);relative+=delta/std::max(double(reference[i]),1e-6);}
        const bool parity=maximum<=.001&&relative/DepthorV3::N<=1e-4;
        result["gpu_resident_forward"]=QJsonObject{{"iterations",resident.size()},{"seconds",gpuDuration},{"forward_fps",resident.size()/gpuDuration},
            {"mean_run_ms",gpuSum/resident.size()},{"median_run_ms",resident[resident.size()/2]},{"p95_run_ms",resident[int((resident.size()-1)*.95)]},
            {"maximum_difference_from_cpu_io_m",maximum},{"mean_relative_difference",relative/DepthorV3::N},{"original_1mm_1e4_gate_pass",parity},
            {"bit_identical_to_cpu_io",std::memcmp(valuesOut.data(),reference.data(),valuesOut.size()*4)==0},
            {"scope","Preallocated GPU-resident inputs/output; RunWithBinding plus output synchronization; excludes external H2D/D2H, preprocessing, IPC and GUI"}};
        result["pass"]=samples.size()>10&&resident.size()>10&&parity;return result;
    }

    // Same physical GEO2 rays and extrinsics as UniDepth. Project first;
    // collisions retain the nearest camera-Z. Never resize native ToF indices.
    std::vector<float> inferColor(const QByteArray &rgb,const RvTofFrame &tof,int rotation,bool rawPreview=false) {
        if(!tof.isValid()||tof.columns()!=48||tof.rows()!=32||(rotation!=0&&rotation!=180))throw std::runtime_error("Color LiteSR requires paired native dToF");
        const int fov=(tof.metadata.contains("dtof_fov_correction")?tof.metadata["dtof_fov_correction"]:tof.metadata["tof_fov_correction"]).toInt(-1);
        if(fov!=1&&fov!=2)throw std::runtime_error("Unsupported native calibration");
        const auto &cal=RvAssistFusion::calibrationForFov(fov);
        std::array<RvColorSurface::Point,1536> points{};
        int measured=0;
        for(int i=0;i<1536;++i){
            const double r=tof.rangeUq2(i)*.00025;if(!tof.valid(i)||tof.snr(i)<6||r<.01||r>1.4)continue;
            const auto &q=cal.cameraRays[i];const double x=r*q[0]+cal.translation[0],y=r*q[1]+cal.translation[1],z=r*q[2]+cal.translation[2];
            if(z<=0)continue;
            const double radial=std::sqrt(x*x+y*y+z*z);if(radial<.01||radial>1.4)continue;
            points[i]={(574*x/z+288.)/576,(574*y/z+162.)/324,z,r,true};++measured;
        }
        RvColorSurface::Grid low(48,32),high(256,256),old(48,32);
        for(const auto &p:points)RvColorSurface::point(old,p);
        const int quads=RvColorSurface::raster(points,low,high);
        std::array<float,1536> ranges{};std::array<bool,1536> valid{};
        int oldCount=0,newCount=0;
        for(int y=0;y<32;++y)for(int x=0;x<48;++x){
            const int i=y*48+x;oldCount+=old.z[i]>0;
            const double dx=((x+.5)*576/48-288)/574,dy=((y+.5)*324/32-162)/574;
            ranges[i]=float(low.z[i]*std::sqrt(1+dx*dx+dy*dy));valid[i]=low.z[i]>0&&ranges[i]>=.01&&ranges[i]<=1.4;newCount+=valid[i];
        }
        colorDiagnostics={{"surface_policy","native_connected_projected_surface_v2"},{"native_valid_measurements",measured},
            {"old_point_grid_cells",oldCount},{"surface_grid_cells",newCount},{"connected_quads",quads},
            {"derived_values_are_measurements",false}};
        auto image=unpackRgb(rgb).scaled(256,256,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
        QByteArray inputRgb(3*256*256,Qt::Uninitialized);
        for(int y=0;y<256;++y)for(int x=0;x<256;++x)for(int c=0;c<3;++c)inputRgb[c*65536+y*256+x]=char(image.constScanLine(y)[3*x+c]);
        if(rotation==180){for(int c=0;c<3;++c)std::reverse(inputRgb.begin()+c*65536,inputRgb.begin()+(c+1)*65536);std::reverse(ranges.begin(),ranges.end());std::reverse(valid.begin(),valid.end());}
        OrtValue *in[3]={},*output=nullptr;const auto release=qScopeGuard([&]{for(auto v:in)if(v)api->ReleaseValue(v);if(output)api->ReleaseValue(output);});
        const int64_t rgbShape[]={1,3,256,256},rangeShape[]={1,1,32,48};
        check(api->CreateTensorWithDataAsOrtValue(memory,inputRgb.data(),inputRgb.size(),rgbShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8,&in[0]));
        check(api->CreateTensorWithDataAsOrtValue(memory,ranges.data(),sizeof(ranges),rangeShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&in[1]));
        check(api->CreateTensorWithDataAsOrtValue(memory,valid.data(),sizeof(valid),rangeShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL,&in[2]));
        const char *names[]={"rgb","camera_range","valid"},*outputs[]={rawPreview?"raw_radial":"radial"};const OrtValue *values[]={in[0],in[1],in[2]};
        check(api->Run(session,nullptr,names,values,3,outputs,1,&output));
        OrtTensorTypeAndShapeInfo *shape=nullptr;check(api->GetTensorTypeAndShape(output,&shape));
        const auto freeShape=qScopeGuard([&]{api->ReleaseTensorTypeAndShapeInfo(shape);});
        size_t count=0;ONNXTensorElementDataType type;check(api->GetTensorShapeElementCount(shape,&count));check(api->GetTensorElementType(shape,&type));
        if(count!=65536||type!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)throw std::runtime_error("Invalid Color LiteSR shape");
        void *data=nullptr;check(api->GetTensorMutableData(output,&data));const auto *depth=static_cast<float*>(data);
        std::vector<float> out(Pixels);
        for(int y=0;y<324;++y)for(int x=0;x<576;++x){
            int index=std::min(255,int((y+.5)*256/324))*256+std::min(255,int((x+.5)*256/576));if(rotation==180)index=65535-index;
            const float r=depth[index];if(!rawPreview&&(!std::isfinite(r)||r<0))throw std::runtime_error("Nonfinite Color LiteSR output");
            const double dx=(x-287.5)/574,dy=(y-161.5)/574;
            const int supportIndex=std::min(255,int((y+.5)*256/324))*256+std::min(255,int((x+.5)*256/576));
            out[y*576+x]=rawPreview?r:(high.z[supportIndex]>0?float(r/std::sqrt(1+dx*dx+dy*dy)):0);
        }return out;
    }
    bool cudaGraphs() const {return hostModel=="depthor_v3_fp32";}
    QJsonObject depthorExecutionInfo() const {
        if(!cudaGraphs())return {{"enable_cuda_graph",false}};
        return {{"enable_cuda_graph",true},{"use_tf32",false},{"cuda_provider_options",graphProviderOptions},{"ptx_preloaded_before_capture",true},
            {"fixed_gpu_io_binding",true},{"custom_compute_calls",double(computeCalls())},
            {"rgb_gpu_address",QString::number(quintptr(graphDevice[0]))},{"sparse_gpu_address",QString::number(quintptr(graphDevice[1]))},
            {"depth_gpu_address",QString::number(quintptr(graphDevice[2]))}};
    }
    QJsonObject checkDepthorNumericalCases(const QString &path){
        if(!cudaGraphs()||!QDir::isAbsolutePath(path))throw std::runtime_error("CSPN numerical check requires FP32 and an absolute case manifest");
        const auto cases=readJson(path)["cases"].toArray();
        if(cases.size()<3||cases.size()>256)throw std::runtime_error("Invalid CSPN numerical case count");
        const auto before=depthorExecutionInfo();QJsonArray rows;QStringList inputHashes,outputHashes;bool passed=true;
        const auto load=[](const QJsonValue &path,int count){QFile file(path.toString());
            if(!path.isString()||!QDir::isAbsolutePath(file.fileName())||!file.open(QIODevice::ReadOnly)||file.size()!=count*4)
                throw std::runtime_error("Invalid CSPN numerical fixture");
            auto bytes=file.readAll();if(bytes.size()!=count*4)throw std::runtime_error("Truncated CSPN numerical fixture");
            std::vector<float> values(count);std::memcpy(values.data(),bytes.constData(),bytes.size());
            for(float value:values)if(!std::isfinite(value))throw std::runtime_error("Nonfinite CSPN numerical fixture");return values;};
        const auto digest=[](const std::vector<float> &values){return QString::fromLatin1(QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(float)),QCryptographicHash::Sha256).toHex());};
        for(const auto &value:cases){const auto item=value.toObject();
            const auto rgb=load(item["rgb"],3*DepthorV3::N),sparse=load(item["sparse"],DepthorV3::N),expected=load(item["expected"],DepthorV3::N);
            const auto output=inferDepthorNetwork(rgb,sparse);const bool identical=std::memcmp(output.data(),expected.data(),output.size()*sizeof(float))==0;
            double maximum=0;for(int i=0;i<DepthorV3::N;++i)maximum=std::max(maximum,std::abs(double(output[i])-expected[i]));
            const auto inputHash=digest(rgb)+digest(sparse),outputHash=digest(output);
            if(!inputHashes.contains(inputHash))inputHashes.append(inputHash);if(!outputHashes.contains(outputHash))outputHashes.append(outputHash);
            rows.append(QJsonObject{{"name",item["name"]},{"bitwise_equal",identical},{"max_abs_m",maximum},{"output_sha256",outputHash}});passed&=identical;
        }
        const auto after=depthorExecutionInfo();const bool replay=before["custom_compute_calls"]==after["custom_compute_calls"];
        return {{"pass",passed&&replay&&inputHashes.size()>1&&outputHashes.size()>1},{"cases",rows},
            {"distinct_inputs",inputHashes.size()},{"distinct_outputs",outputHashes.size()},{"custom_compute_not_redispatched",replay},
            {"execution_before",before},{"execution_after",after},{"graph_sha256",view().graphSha},{"camera_started",false}};
    }
private:
    void checkedCuda(int status){if(status)throw std::runtime_error(std::string("DEPTHOR CUDA operation failed: ")+std::to_string(status));}
    void uploadDepthorGraph(const std::vector<float> &rgb,const std::vector<float> &sparse){
        checkedCuda(cudaCopy(graphDevice[0],rgb.data(),rgb.size()*sizeof(float),1));
        checkedCuda(cudaCopy(graphDevice[1],sparse.data(),sparse.size()*sizeof(float),1));
        // Pageable H2D may return after staging, before the final device DMA.
        // Finish both copies before replay on ORT's compute stream.
        checkedCuda(cudaSync());
    }
    void runDepthorGraph(const OrtRunOptions *options){
        check(api->SynchronizeBoundInputs(graphBinding));
        check(api->RunWithBinding(session,options,graphBinding));
        check(api->SynchronizeBoundOutputs(graphBinding));checkedCuda(cudaSync());
    }
    void initializeDepthorGraph(const QDir &dir){
        cudaLibrary.setFileName(dir.filePath("bin/cudart64_12.dll"));
        if(!cudaLibrary.load())throw std::runtime_error("DEPTHOR CUDA runtime unavailable");
        cudaCopy=reinterpret_cast<decltype(cudaCopy)>(cudaLibrary.resolve("cudaMemcpy"));
        cudaSync=reinterpret_cast<decltype(cudaSync)>(cudaLibrary.resolve("cudaDeviceSynchronize"));
        cudaMalloc=reinterpret_cast<decltype(cudaMalloc)>(cudaLibrary.resolve("cudaMalloc"));
        cudaFree=reinterpret_cast<decltype(cudaFree)>(cudaLibrary.resolve("cudaFree"));
        if(!cudaCopy||!cudaSync||!cudaMalloc||!cudaFree)throw std::runtime_error("DEPTHOR CUDA memory API unavailable");
        check(api->CreateMemoryInfo("Cuda",OrtDeviceAllocator,0,OrtMemTypeDefault,&graphMemory));
        const int64_t rgbShape[]={1,3,480,640},depthShape[]={1,1,480,640};
        for(int i=0;i<3;++i){const size_t bytes=size_t(DepthorV3::N)*(i==0?3:1)*sizeof(float);
            checkedCuda(cudaMalloc(&graphDevice[i],bytes));
            check(api->CreateTensorWithDataAsOrtValue(graphMemory,graphDevice[i],bytes,i==0?rgbShape:depthShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&graphValues[i]));}
        check(api->CreateIoBinding(session,&graphBinding));
        check(api->BindInput(graphBinding,"rgb",graphValues[0]));check(api->BindInput(graphBinding,"sparse_depth",graphValues[1]));
        check(api->BindOutput(graphBinding,"depth",graphValues[2]));
        check(api->CreateRunOptions(&graphRun));check(api->AddRunConfigEntry(graphRun,"disable_synchronize_execution_providers","1"));
        OrtRunOptions *warmup=nullptr;check(api->CreateRunOptions(&warmup));
        const auto releaseWarmup=qScopeGuard([&]{api->ReleaseRunOptions(warmup);});
        check(api->AddRunConfigEntry(warmup,"gpu_graph_id","-1"));
        std::vector<float> rgb(3*DepthorV3::N,.5f),sparse(DepthorV3::N,0.f);sparse[DepthorV3::N/2+DepthorV3::W/2]=1.f;
        uploadDepthorGraph(rgb,sparse);
        // PTX is lazy-loaded by the first custom Compute. Run outside capture first.
        runDepthorGraph(warmup);runDepthorGraph(graphRun);
    }
    QJsonObject benchmarkDepthorGraph(const std::vector<float> &rgb,const std::vector<float> &sparse,int seconds){
        std::vector<float> host(DepthorV3::N);uploadDepthorGraph(rgb,sparse);
        for(int i=0;i<5;++i)runDepthorGraph(graphRun);
        QElapsedTimer total,one;QVector<double> samples,resident;double duration[2]={};
        for(int mode=0;mode<2;++mode){total.start();auto &times=mode?resident:samples;
            while(total.elapsed()<seconds*1000){one.start();if(!mode)uploadDepthorGraph(rgb,sparse);runDepthorGraph(graphRun);
                if(!mode)checkedCuda(cudaCopy(host.data(),graphDevice[2],host.size()*sizeof(float),2));times.append(one.nsecsElapsed()/1e6);}
            duration[mode]=total.nsecsElapsed()/1e9;std::sort(times.begin(),times.end());}
        std::vector<float> output(DepthorV3::N);checkedCuda(cudaCopy(output.data(),graphDevice[2],output.size()*sizeof(float),2));
        double maximum=0,relative=0;for(int i=0;i<DepthorV3::N;++i){
            if(!std::isfinite(output[i])||output[i]<=0)throw std::runtime_error("Invalid DEPTHOR graph benchmark output");
            const double delta=std::abs(double(output[i])-host[i]);maximum=std::max(maximum,delta);relative+=delta/std::max(double(host[i]),1e-6);}
        const auto summarize=[](const QVector<double> &times,double seconds){double sum=0;for(double n:times)sum+=n;
            return QJsonObject{{"iterations",times.size()},{"seconds",seconds},{"forward_fps",times.size()/seconds},
                {"mean_run_ms",sum/times.size()},{"median_run_ms",times[times.size()/2]},{"p95_run_ms",times[int((times.size()-1)*.95)]}};};
        auto result=summarize(samples,duration[0]),gpu=summarize(resident,duration[1]);
        const bool parity=maximum<=.001&&relative/DepthorV3::N<=1e-4;
        gpu["maximum_difference_from_cpu_io_m"]=maximum;gpu["mean_relative_difference"]=relative/DepthorV3::N;
        gpu["original_1mm_1e4_gate_pass"]=parity;gpu["bit_identical_to_cpu_io"]=std::memcmp(output.data(),host.data(),output.size()*sizeof(float))==0;
        gpu["scope"]="Persistent GPU I/O; RunWithBinding and synchronization; excludes external H2D/D2H, preprocessing, IPC and GUI";
        result["gpu_resident_forward"]=gpu;result["pass"]=parity&&samples.size()>10&&resident.size()>10;
        result["host_model"]=hostModel;result["precision"]=view().precision;result["graph_sha256"]=view().graphSha;
        result["execution"]=depthorExecutionInfo();result["camera_started"]=false;result["gpu_kernel_only"]=false;
        result["scope"]="Persistent I/O CUDA Graph forward including per-call H2D/D2H and synchronization; excludes preprocessing, IPC and GUI";
        return result;
    }
    void check(OrtStatus *status) {if(status) {const std::string reason=api->GetErrorMessage(status);api->ReleaseStatus(status);throw std::runtime_error(reason);}}
    QLibrary library,customLibrary,cudaLibrary,protectedLibrary;
    QString graphProviderOptions;
    void (ORT_API_CALL *releaseCustom)()=nullptr;
    unsigned long long (ORT_API_CALL *computeCalls)()=nullptr;
    bool customRegistered=false;
    int (*cudaCopy)(void *,const void *,size_t,int)=nullptr;
    int (*cudaSync)()=nullptr;
    int (*cudaMalloc)(void **,size_t)=nullptr;
    int (*cudaFree)(void *)=nullptr;
    void *graphDevice[3]={};OrtValue *graphValues[3]={};
    OrtIoBinding *graphBinding=nullptr;OrtRunOptions *graphRun=nullptr;OrtMemoryInfo *graphMemory=nullptr;
    DLL_DIRECTORY_COOKIE directory=nullptr;
    const OrtApi *api=nullptr;
    OrtEnv *env=nullptr;OrtSession *session=nullptr;OrtMemoryInfo *memory=nullptr;
};
bool readExact(FILE *file,QByteArray &bytes,int size) {
    bytes.resize(size);int done=0;
    while(done<size) {const auto n=std::fread(bytes.data()+done,1,size-done,file);if(!n)return false;done+=int(n);}return true;
}
bool writePacket(const QJsonObject &json,const QByteArray &bytes={},const QByteArray &aux={}) {
    const auto out=packet(json,bytes,aux);return std::fwrite(out.constData(),1,out.size(),stdout)==size_t(out.size())&&std::fflush(stdout)==0;
}
enum class AssistRead {End,Request,Invalid};
using AssistReader=std::function<AssistRead(QJsonObject&,QByteArray&,QByteArray&)>;
using AssistWriter=std::function<bool(const QJsonObject&,const QByteArray&,const QByteArray&)>;
int runAssistEngine(const QString&,const QString&,const AssistReader&,const AssistWriter&,const QString & = QString());
AssistReader stdioAssistReader(){
    return [prefix=QByteArray{},header=QByteArray{}](QJsonObject &request,QByteArray &rgb,QByteArray &tof) mutable {
        if(!readExact(stdin,prefix,12))return AssistRead::End;
        const auto h=qFromLittleEndian<quint32>(prefix.constData()),b=qFromLittleEndian<quint32>(prefix.constData()+4),a=qFromLittleEndian<quint32>(prefix.constData()+8);
        if(!h||h>HeaderLimit||b!=RgbBytes||(a!=0&&a!=256&&a!=1024&&a!=6144)||!readExact(stdin,header,h)||!readExact(stdin,rgb,b)||!readExact(stdin,tof,a))return AssistRead::Invalid;
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(header,&parse);
        if(parse.error!=QJsonParseError::NoError||!document.isObject())return AssistRead::Invalid;
        request=document.object();return AssistRead::Request;
    };
}
}

// Only the disposable, explicitly enabled QCore headless test uses this
// transport. The same engine runs on its owning thread; one job is bounded.
struct RvAssistDirectTransport {
    struct Job {QJsonObject request;QImage camera;QByteArray native;};
    std::mutex mutex;std::condition_variable available;std::optional<Job> pending;
    bool stopping=false;std::thread worker;
    using Post=std::function<void(const QJsonObject&,const QByteArray&,const QByteArray&)>;
    RvAssistDirectTransport(const QString &root,const QString &token,Post post):worker([this,root,token,post=std::move(post)]{
        const auto read=[this](QJsonObject &request,QByteArray &rgb,QByteArray &native){
            Job job;
            {std::unique_lock<std::mutex> lock(mutex);available.wait(lock,[this]{return stopping||pending.has_value();});
             if(stopping)return AssistRead::End;job=std::move(*pending);pending.reset();}
            QElapsedTimer prepare;prepare.start();rgb=planarRgb(job.camera);native=std::move(job.native);request=std::move(job.request);
            auto metadata=request["metadata"].toObject();metadata["assist_direct_rgb_prepare_ms"]=prepare.nsecsElapsed()/1e6;request["metadata"]=metadata;
            {std::lock_guard<std::mutex> lock(mutex);if(stopping)return AssistRead::End;}
            return AssistRead::Request;
        };
        const auto write=[this,&post](const QJsonObject &response,const QByteArray &depth,const QByteArray &diagnostic){
            {std::lock_guard<std::mutex> lock(mutex);if(stopping)return false;}
            post(response,depth,diagnostic);return true;
        };
        const int code=runAssistEngine(root,token,read,write);
        if(code)write({{"protocol",1},{"type","fatal"},{"token",token},{"error","In-process host inference exited"}},{},{});
    }){}
    ~RvAssistDirectTransport(){
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;pending.reset();}available.notify_one();
        if(worker.joinable())worker.join();
    }
    bool submit(const QJsonObject &request,const QImage &camera,const QByteArray &native){
        {std::lock_guard<std::mutex> lock(mutex);if(stopping||pending)return false;pending=Job{request,camera,native};}
        available.notify_one();return true;
    }
};

RvAssistDepth::RvAssistDepth(QObject *parent):QObject(parent),m_root(RuntimeDiscovery::rvAssistRoot()) {
    if(!validExperiment(m_experiment))m_experiment=QStringLiteral("H");
    m_timeout.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,&RvAssistDepth::handleTimeout);
    connect(&m_process,&QProcess::readyReadStandardOutput,this,&RvAssistDepth::readOutput);
    connect(&m_process,&QProcess::readyReadStandardError,this,[this]{m_stderr+=m_process.readAllStandardError();m_stderr=m_stderr.right(2048);});
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError){if(!m_stopping&&!m_destroying)fail(m_process.errorString());});
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){
        if(m_destroying)return;
        const bool stopping=m_stopping;m_stopping=false;m_ready=false;m_busy=false;m_timeout.stop();
        if(!stopping) {fail(tr("Host assisted depth process exited (%1): %2").arg(code).arg(QString::fromUtf8(m_stderr).right(600)));return;}
        if(m_enabled&&!m_failed)startWorker();
    });
}
RvAssistDepth::~RvAssistDepth() {
    m_destroying=true;m_process.disconnect(this);m_timeout.stop();m_timeout.disconnect(this);
    m_direct.reset();
    if(m_process.state()!=QProcess::NotRunning) {
        if(m_workerCspn){m_process.closeWriteChannel();if(m_process.waitForFinished(2000))return;}
        m_process.kill();m_process.waitForFinished(2000);
    }
}

bool RvAssistDepth::supportsHostModel(const QString &model) {
    return model==QStringLiteral("unidepth_wide_576_fp16_tof16");
}
bool RvAssistDepth::isUniDepth(const QString &model){return RvCameraView::isUniDepth(model);}
bool RvAssistDepth::hostModelAvailable(const QString &model) {
    if(!supportsHostModel(model))return false;
    const QString root=modelRoot(model);
    if(root.isEmpty())return false;
    const QDir dir(root);
    QFile manifest(dir.filePath("runtime.json"));
    if(!manifest.open(QIODevice::ReadOnly)||manifest.size()<=0||manifest.size()>262144)return false;
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(manifest.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())return false;
    const auto info=document.object();
    if(info["schema"].toInt()!=1)return false;
    const auto files=info["files"].toObject();
    QStringList required;
    if(info["backend"]!=HostRuntimeBackend||info["ort_version"]!=HostOrtVersion)return false;
    required={"model.json","bin/onnxruntime.dll","bin/onnxruntime_providers_shared.dll",HostProviderFile};
#ifdef DCTS_INSTALLER_BUILD
    if(!info.contains("model_protection"))return false;
#endif
    if(info.contains("model_protection")) {
        const auto protection=info["model_protection"].toObject();
        if(!protectedModelContract(info)
            ||protection["plaintext_sha256"]!=QLatin1String(RvCameraView::profile(model).graphSha))return false;
        required.append({"model.dctsm","bin/DctsModelRuntime.dll"});
    } else required.append(QLatin1String(RvCameraView::profile(model).graphFile));
    if(model=="depthor_v3_fp32")required.append({"bin/cspn_custom_op.dll","bin/cspn_kernels.ptx"});
    for(auto i=files.begin();i!=files.end();++i)if(!required.contains(i.key()))required.append(i.key());
    for(const auto &relative:required) {
        if(!files.contains(relative)||QDir::isAbsolutePath(relative)||relative.contains("..")||relative.contains('\\'))return false;
        const QFileInfo file(dir.filePath(relative));
        if(!file.isFile()||!file.isReadable()||file.size()<=0)return false;
    }
    return true;
}
bool RvAssistDepth::setHostModel(const QString &model) {
    if(m_directMode&&model!="unidepth_wide_576_fp16")return false;
    if(!supportsHostModel(model))return false;
    if(model==m_hostModel)return true;
    m_hostModel=model;m_failed=false;
    cancel(tr("Host model changed; waiting for a new paired frame"));return true;
}

bool RvAssistDepth::setExperiment(const QString &mode) {
    if(m_directMode&&mode!="G")return false;
    if(m_destroying||m_cancelling||!isUniDepth(m_hostModel)||!validExperiment(mode))return false;
    if(mode==m_experiment)return true;
    m_experiment=mode;m_failed=false;
    // Cancels both mailboxes and the worker's global scale history. A new
    // worker is launched only after a fresh selected input arrives.
    cancel(tr("Host depth correction mode changed; waiting for a new measurement"));
    return true;
}

bool RvAssistDepth::enableHeadlessFp16GTestMode(){
    const auto *app=QCoreApplication::instance();
    if(!app||app->inherits("QGuiApplication")||m_destroying||m_cancelling||m_request||m_direct
        ||m_process.state()!=QProcess::NotRunning||m_hostModel!="unidepth_wide_576_fp16"||m_experiment!="G")return false;
    m_directMode=true;return true;
}

void RvAssistDepth::setColorRawPreview(bool enabled) {
    if(m_destroying||m_cancelling||!RvCameraView::isColorLiteSR(m_hostModel)||m_colorRawPreview==enabled)return;
    m_colorRawPreview=enabled;m_failed=false;
    cancel(tr("Depth display changed; waiting for a new paired frame"));
}

void RvAssistDepth::setInputRotationDegrees(int degrees) {
    // The frozen 576x324 graph supports the centred 0/180-degree camera only.
    if((degrees!=0&&degrees!=180) || degrees==m_inputRotation)return;
    m_inputRotation=degrees;cancel(tr("Host inference input orientation changed; waiting for a new paired frame"));
}

void RvAssistDepth::setEnabled(bool enabled) {
    if(m_destroying||m_enabled==enabled)return;
    m_enabled=enabled;m_failed=false;
    if(!enabled)cancel(tr("Host assisted depth is disabled"));
    // Launch only when a selected, paired frame is received, never on startup.
}
void RvAssistDepth::cancel(const QString &reason) {
    if(m_cancelling)return;
    m_cancelling=true;
    emit loadingStateChanged(0);
    m_pending={};m_current={};m_busy=false;m_ready=false;m_buffer.clear();m_timeout.stop();m_lastHdTofSequence=0;m_lastGuiPublishMs=0;
    m_lastHdTofTimestampNs=0;m_lastHdTofSamples.clear();m_lastHdTofContext={};
    if(m_directMode){m_token.clear();m_direct.reset();}
    if(m_process.state()!=QProcess::NotRunning) {
        m_stopping=true;
        if(m_workerCspn){
            // Results are revoked above immediately. EOF lets the FP32 worker
            // destroy its Session/graphs before releasing the custom PTX/DLL.
            m_process.closeWriteChannel();const auto token=m_token;
            QTimer::singleShot(2000,this,[this,token]{if(m_stopping&&m_token==token&&m_process.state()!=QProcess::NotRunning)m_process.kill();});
        }else m_process.kill();
    }
    const QPointer<RvAssistDepth> alive(this);
    if(!m_destroying)emit cleared(reason);
    if(alive)m_cancelling=false;
}
void RvAssistDepth::fail(const QString &reason)
{
    if(m_failed)return;
    m_failed=true;
    const QPointer<RvAssistDepth> alive(this);
    cancel(reason);
    if(alive)emit inferenceUnavailable(failureExplanation(reason),reason);
}

QString RvAssistDepth::preflightError(const QString &model)
{
#ifdef DCTS_INSTALLER_BUILD
    if(!hasNvidiaAdapter())return tr("No NVIDIA GPU was detected. This version requires a supported NVIDIA RTX GPU.");
    if(!hostModelAvailable(model))return tr("The required inference runtime or encrypted model files are missing or invalid.");
#else
    Q_UNUSED(model);
#endif
    return {};
}

QString RvAssistDepth::failureExplanation(const QString &reason)
{
    if(reason=="RTX_DRIVER_MISSING")return tr("The NVIDIA CUDA driver is missing or could not be loaded.");
    if(reason=="RTX_DRIVER_OLD")return tr("The NVIDIA driver is too old for the bundled CUDA 12.9 runtime. Update the driver.");
    if(reason=="RTX_DRIVER_UNAVAILABLE")return tr("The NVIDIA driver could not initialize a CUDA device. Check the driver and whether the GPU is enabled.");
    if(reason=="RTX_GPU_UNSUPPORTED")return tr("The detected NVIDIA GPU architecture is unsupported. TensorRT-RTX requires a supported RTX GPU from the Ampere generation or later.");
    if(reason=="RTX_DEVICE_UNSUPPORTED")return tr("TensorRT-RTX did not accept any GPU. Check the GPU model, driver and Windows environment.");
    if(reason.contains("out of memory",Qt::CaseInsensitive)||reason.contains("CUDA_ERROR_OUT_OF_MEMORY",Qt::CaseInsensitive))
        return tr("There is insufficient GPU memory to run the model. Close other GPU-intensive applications and try again.");
    if(reason.contains("fingerprint",Qt::CaseInsensitive)||reason.contains("protected model",Qt::CaseInsensitive)
        ||reason.contains("AES-GCM",Qt::CaseInsensitive)||reason.contains("Cannot hash",Qt::CaseInsensitive))
        return tr("The encrypted model or a runtime file failed integrity verification. Reinstall the application.");
    if(reason.contains("timed out",Qt::CaseInsensitive)||reason==tr("Host assisted depth timed out; disable and enable it to retry"))
        return tr("Model initialization or inference timed out.");
    if(reason.contains("runtime",Qt::CaseInsensitive)||reason.contains(".dll",Qt::CaseInsensitive)
        ||reason.contains("library",Qt::CaseInsensitive)||reason.contains("module",Qt::CaseInsensitive))
        return tr("A required inference runtime component could not be loaded or initialized.");
    return tr("The host inference worker failed to initialize or run the model.");
}
void RvAssistDepth::submit(const QImage &camera,const RvTofFrame &tof,quint64 epoch) {
    if(!m_enabled||m_failed||m_destroying)return;
    if(reusesNativeMeasurements()){cancel(tr("Invalid managed H.265 model input"));return;}
    if(RvCameraView::isUniDepthNative16(m_hostModel)||camera.size()!=QSize(576,324)||!tof.isValid()
       ||(RvCameraView::isWide(m_hostModel)&&tof.metadata["rectify_map_sha256"]!=QLatin1String(RvCameraView::Wide.mapSha))) {cancel(tr("No paired 576×324 camera and native dToF frame"));return;}
    if(!m_pending.camera.isNull())++m_dropped;
    m_pending={camera,tof,epoch,++m_request,monotonicMs()-qint64(std::ceil(tof.metadata["host_receive_to_poll_ms"].toDouble()))};
    m_pending.tof.metadata["assist_input_rotation_degrees"]=m_inputRotation;
    if(RvCameraView::isColorLiteSR(m_hostModel))m_pending.tof.metadata["color_raw_preview"]=colorRawPreview();
    if(isUniDepth(m_hostModel))m_pending.tof.metadata["assist_diagnostic_capture"]=!m_directMode;

    if(!m_direct&&m_process.state()==QProcess::NotRunning)startWorker();
    else if(m_ready&&!m_busy)sendLatest();
}
void RvAssistDepth::submitHd(const QImage &camera,const QJsonObject &source,quint64 epoch,const QByteArray &nativeSamples) {
    if(m_destroying||!m_enabled||m_failed)return;
    const bool paired=source["dtof_present"].toBool();
    const bool reuse=reusesNativeMeasurements();auto selected=source;if(reuse)selected["assist_experiment"]="H";
    if(camera.size()!=QSize(576,324)||!hdSource(source,RvCameraView::isWide(m_hostModel),DepthorV3::supported(m_hostModel)||reuse)
        ||(reuse&&paired&&!RvTofFrame{nativeSamples,selected}.isValidForHostFusion())
        ||(paired?!RvTofFrame{nativeSamples,source}.isValid(DepthorV3::supported(m_hostModel)||reuse):!nativeSamples.isEmpty())) {
        cancel(tr("Invalid managed H.265 model input"));return;
    }
    if(paired&&!RvCameraView::supportsNativeGrid(m_hostModel,source["dtof_width"].toInt(),source["dtof_height"].toInt())){
        cancel(tr("Invalid managed H.265 model input"));return;
    }
    // A/G/H retain video-cadence prediction. B and strict-support
    // diagnostic require a current paired measurement.
    const bool supported=source["assist_output_policy"]==QLatin1String(RvAssistFusion::SupportPolicy);
    if((!isUniDepth(m_hostModel)||supported||tofDriven())&&!paired)return;
    if(!m_pending.camera.isNull()){++m_dropped;m_dropReasons["pending_superseded"]=m_dropReasons["pending_superseded"].toInt()+1;}
    // Empty auxiliary bytes mean RGB only. No all-zero synthetic ToF frame.
    m_pending={camera,{nativeSamples,source},epoch,++m_request,qint64(source["host_decoded_monotonic_ns"].toDouble()/1000000.)};
    if(RvCameraView::isUniDepthNative16(m_hostModel))m_pending.tof.metadata["assist_host_model"]=m_hostModel;
    m_pending.tof.metadata["assist_input_rotation_degrees"]=m_inputRotation;
    if(RvCameraView::isColorLiteSR(m_hostModel))m_pending.tof.metadata["color_raw_preview"]=colorRawPreview();
    if(isUniDepth(m_hostModel))m_pending.tof.metadata["assist_experiment"]=m_experiment;
    if(isUniDepth(m_hostModel))m_pending.tof.metadata["assist_diagnostic_capture"]=!m_directMode;
    if(m_directMode)m_pending.tof.metadata["assist_transport"]="in_process_headless_test";
    if(tofDriven()||DepthorV3::supported(m_hostModel)||reuse)m_pending.tof.metadata["host_pair_enqueued_ms"]=double(monotonicMs());
    if(!m_direct&&m_process.state()==QProcess::NotRunning)startWorker();
    else if(m_ready&&!m_busy)sendLatest();
}

void RvAssistDepth::startWorker() {
    if(m_destroying||!m_enabled||m_failed||m_pending.camera.isNull()||m_direct||m_process.state()!=QProcess::NotRunning)return;
    m_root=modelRoot(m_hostModel);
    if(!hostModelAvailable(m_hostModel)) {fail(tr("Selected host model runtime is missing; prepare the optional runtime first"));return;}
    m_ready=false;m_busy=false;m_stopping=false;m_buffer.clear();m_stderr.clear();m_token=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(m_directMode){
        const auto token=m_token;
        m_timeout.start(30000);
        m_direct=std::make_unique<RvAssistDirectTransport>(m_root,token,[this,token](const QJsonObject &response,const QByteArray &depth,const QByteArray &diagnostic){
            QMetaObject::invokeMethod(this,[this,token,response,depth,diagnostic]{
                if(m_destroying||!m_enabled||m_failed||m_stopping||!m_directMode||m_token!=token)return;
                QElapsedTimer clock;clock.start();acceptResponse(response,depth,diagnostic,clock);
            },Qt::QueuedConnection);
        });
        emit statusChanged(tr("Loading the host-assisted inference model on the GPU..."));emit loadingStateChanged(1);return;
    }
    auto environment=QProcessEnvironment::systemEnvironment();
    for(const auto &key:environment.keys())if(key.startsWith("PYTHON")||key.startsWith("CONDA"))environment.remove(key);
    m_process.setProcessEnvironment(environment);m_process.setWorkingDirectory(m_root);
    m_process.setProgram(QCoreApplication::applicationFilePath());
    m_process.setArguments({"--rv-assist-worker",m_root,m_token});
    if(RvCameraView::isUniDepthNative16(m_hostModel))m_process.setArguments(m_process.arguments()<<m_hostModel);
    m_workerCspn=m_hostModel=="depthor_v3_fp32";
    m_process.start();m_timeout.start(HostWorkerLoadTimeoutMs);
    emit statusChanged(tr("Loading the host-assisted inference model on the GPU..."));
    emit loadingStateChanged(1);
}
bool RvAssistDepth::takePending() {
    if(m_pending.camera.isNull())return false;
    m_current=std::move(m_pending);m_pending={};
    if(monotonicMs()-m_current.receivedMs>1000) {m_dropReasons["pending_expired"]=m_dropReasons["pending_expired"].toInt()+1;m_current={};emit cleared(tr("Paired frame expired before host inference"));return false;}
    if((tofDriven()||DepthorV3::supported(m_hostModel)||(reusesNativeMeasurements()&&m_current.tof.metadata["dtof_present"]==true))
       &&m_current.tof.metadata["measurement_age_at_pair_ms"].toDouble()+monotonicMs()-m_current.tof.metadata["host_pair_enqueued_ms"].toDouble()>1000){
        m_dropReasons["measurement_expired"]=m_dropReasons["measurement_expired"].toInt()+1;m_current={};return false;}
    if(m_current.tof.metadata["source_protocol"]=="HDA1"&&!m_current.tof.samples.isEmpty()) {
        const auto seq=m_current.tof.metadata["tof_sequence"].toInteger();
        if(DepthorV3::supported(m_hostModel)||reusesNativeMeasurements()){
            auto &meta=m_current.tof.metadata;const auto timestamp=meta["tof_timestamp_ns"].toInteger();
            const auto context=nativeContext(meta);
            const bool reused=quint64(seq)==m_lastHdTofSequence;
            if(quint64(seq)<m_lastHdTofSequence||(reused&&(timestamp!=m_lastHdTofTimestampNs||m_current.tof.samples!=m_lastHdTofSamples||context!=m_lastHdTofContext))
                ||(!reused&&m_lastHdTofSequence&&timestamp<=m_lastHdTofTimestampNs)){
                m_dropReasons["native_identity_regressed"]=m_dropReasons["native_identity_regressed"].toInt()+1;m_current={};return false;
            }
            meta["tof_reused"]=reused;
            if(!hdSource(meta,RvCameraView::isWide(m_hostModel),true)
               ||(reusesNativeMeasurements()&&!m_current.tof.isValidForHostFusion())){m_current={};return false;}
            m_lastHdTofSequence=quint64(seq);m_lastHdTofTimestampNs=timestamp;m_lastHdTofSamples=m_current.tof.samples;m_lastHdTofContext=context;
        }
        else if(quint64(seq)<=m_lastHdTofSequence){
            if(!isUniDepth(m_hostModel)||tofDriven()||m_current.tof.metadata.value("assist_output_policy")==QLatin1String(RvAssistFusion::SupportPolicy)){m_dropReasons["duplicate_measurement"]=m_dropReasons["duplicate_measurement"].toInt()+1;m_current={};return false;}
            m_current.tof.samples.clear();dropHdTof(m_current.tof.metadata);
        }
        else m_lastHdTofSequence=quint64(seq);
    }
    return true;
}
void RvAssistDepth::sendLatest() {
    if(!m_enabled||m_failed||!m_ready||m_busy||!takePending())return;
    const QJsonObject request={{"protocol",1},{"type","infer"},{"token",m_token},{"request",double(m_current.request)},
        {"epoch",double(m_current.epoch)},{"metadata",m_current.tof.metadata},{"host_model",m_hostModel}};
    if(m_directMode){
        m_current.serializeMs=0;m_busy=true;
        if(!m_direct||!m_direct->submit(request,m_current.camera,m_current.tof.samples)){fail(tr("Cannot submit headless inference frame"));return;}
        m_timeout.start(1500);return;
    }
    QElapsedTimer serialize;serialize.start();
    const auto bytes=packet(request,planarRgb(m_current.camera),m_current.tof.samples);
    m_current.serializeMs=serialize.nsecsElapsed()/1e6;
    m_busy=true;
    if(m_process.write(bytes)!=bytes.size()) {fail(tr("Cannot send paired frame to host depth process"));return;}
    m_timeout.start(1500);
}
void RvAssistDepth::handleTimeout() {
    if(m_directMode){fail(tr("Host assisted depth timed out; disable and enable it to retry"));return;}
    // A busy GUI can deliver the expired timer before the already completed
    // worker's readyRead notification. Drain available IPC first; readOutput
    // still rejects old results and may start a NEW request/deadline.
    const QPointer<RvAssistDepth> alive(this);
    const auto token=m_token;
    if(m_process.state()==QProcess::Running&&m_process.bytesAvailable()==0)m_process.waitForReadyRead(0);
    if(!alive)return;
    readOutput();
    if(!alive||m_destroying||m_stopping||m_failed||!m_enabled||m_token!=token)return;
    if(m_timeout.isActive()||(m_ready&&!m_busy))return;
    fail(tr("Host assisted depth timed out; disable and enable it to retry"));
}
void RvAssistDepth::readOutput() {
    const auto chunk=m_process.readAllStandardOutput();if(m_destroying||m_stopping||!m_enabled)return;
    if(!supportsHostModel(m_hostModel)){fail(tr("Unsupported host model"));return;}
    m_buffer+=chunk;if(m_buffer.size()>2*PacketLimit) {fail(tr("Host depth protocol exceeded its bound"));return;}
    const QPointer<RvAssistDepth> alive(this);
    while(m_buffer.size()>=12) {
        QElapsedTimer responseClock;responseClock.start();
        const auto size=qFromLittleEndian<quint32>(m_buffer.constData()),body=qFromLittleEndian<quint32>(m_buffer.constData()+4),aux=qFromLittleEndian<quint32>(m_buffer.constData()+8);
        if(!size||size>HeaderLimit||(body!=0&&body!=DepthBytes)||(aux!=0&&(!isUniDepth(m_hostModel)||aux!=RvDepthDiagnostic::PayloadBytes))) {fail(tr("Invalid host depth packet size"));return;}
        const int total=12+int(size+body+aux);if(m_buffer.size()<total)return;
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(m_buffer.mid(12,size),&parse);
        const auto depth=m_buffer.mid(12+size,body),coverage=m_buffer.mid(12+size+body,aux);m_buffer.remove(0,total);const auto response=document.object();
        if(parse.error!=QJsonParseError::NoError||!document.isObject()) {fail(tr("Host depth protocol identity mismatch"));return;}
        if(!acceptResponse(response,depth,coverage,responseClock))return;
    }
}

bool RvAssistDepth::acceptResponse(const QJsonObject &response,const QByteArray &depth,const QByteArray &coverage,QElapsedTimer &responseClock) {
    const QPointer<RvAssistDepth> alive(this);
    const auto body=depth.size(),aux=coverage.size();
    if(m_destroying||m_stopping||!m_enabled)return false;
    if(!supportsHostModel(m_hostModel)||(body!=0&&body!=DepthBytes)||(aux!=0&&(!isUniDepth(m_hostModel)||aux!=RvDepthDiagnostic::PayloadBytes)))
        {fail(tr("Invalid host depth packet size"));return false;}
    if(response["protocol"].toInt()!=1||response["token"]!=m_token) {fail(tr("Host depth protocol identity mismatch"));return false;}
    if(response["type"]=="fatal") {fail(response["error"].toString());return false;}
    const auto &view=RvCameraView::profile(m_hostModel);
    const bool identityMatches=supportsHostModel(m_hostModel)
        &&response["weights_sha256"]==QLatin1String(view.weightsSha)&&response["graph_sha256"]==QLatin1String(view.graphSha)
        &&response["precision"]==QLatin1String(view.precision)&&response["source_graph_sha256"]==QLatin1String(view.sourceSha)&&response["conversion"]==QLatin1String(view.conversion)
        &&(m_hostModel=="unidepth"||response["host_model"]==m_hostModel);
    if(response["type"]=="ready"&&!m_ready&&!m_busy&&body==0&&aux==0&&identityMatches) {
#ifdef DCTS_INSTALLER_BUILD
        const auto loading=response["model_loading"].toObject();
        if(loading["provider"]!=TensorRtProviderName||loading["plaintext_sha256_verified"]!=true
            ||loading["plaintext_file_used"]!=false) {
            fail(tr("Host inference worker identity mismatch"));return false;
        }
#endif
        m_timeout.stop();m_ready=true;emit loadingStateChanged(2);
        emit statusChanged(tr("Host model ready; waiting for input"));
        if(!alive||!m_enabled||m_stopping)return false;
        sendLatest();if(!alive)return false;return true;
    }
    if(response["type"]!="result"||!m_busy||response["request"].toDouble()!=double(m_current.request)
        ||response["epoch"].toDouble()!=double(m_current.epoch)) {fail(tr("Unexpected host depth result"));return false;}
    const auto current=m_current;m_current={};m_busy=false;m_timeout.stop();
    const qint64 age=monotonicMs()-current.receivedMs;
    auto metadata=response["metadata"].toObject();
    if(body==DepthBytes&&isUniDepth(m_hostModel)){
        const bool wanted=current.tof.metadata["assist_diagnostic_capture"]==true;
        if(metadata.value("assist_diagnostic_capture")!=current.tof.metadata.value("assist_diagnostic_capture")
           ||(wanted?(metadata["assist_diagnostic_payload"]!="mono_reasons_v1"||!RvDepthDiagnostic::validPayload(coverage)):aux!=0)){
            fail(tr("Invalid host depth diagnostic payload"));return false;
        }
        if(RvCameraView::isUniDepthNative16(m_hostModel)){
            if(metadata["assist_host_model"]!=m_hostModel||(wanted&&metadata["assist_diagnostic_native_count"]!=256)){
                fail(tr("Host depth result does not match the selected model"));return false;
            }
            for(const auto *key:{"dtof_width","dtof_height","dtof_bytes","dtof_fov_correction","tof_fov_correction"})
                if(metadata.value(key)!=current.tof.metadata.value(key)){fail(tr("Host depth result belongs to another native grid"));return false;}
            if(wanted)for(int i=256;i<RvDepthDiagnostic::ReasonBytes;++i)
                if(coverage[RvDepthDiagnostic::DepthBytes+i]!=0||coverage[RvDepthDiagnostic::DepthBytes+RvDepthDiagnostic::ReasonBytes+i]!=0){
                    fail(tr("Invalid host depth diagnostic payload"));return false;
                }
        }
    }
    if(current.tof.metadata["source_protocol"]=="HDA1") {
        if(!hdSource(current.tof.metadata,RvCameraView::isWide(m_hostModel),DepthorV3::supported(m_hostModel)||reusesNativeMeasurements())
           ||(reusesNativeMeasurements()&&!current.tof.samples.isEmpty()&&!current.tof.isValidForHostFusion())){fail(tr("Invalid managed H.265 model input"));return false;}
        for(const auto *key:{"source_protocol","boot_id","source_generation","host_decoded_monotonic_ns","dtof_present",
            "rgb_timestamp_ns","tof_sequence","tof_timestamp_ns","pair_delta_ms","pair_clock_uncertainty_ms","pair_clock_source","tof_reused"})
            if(metadata.value(key)!=current.tof.metadata.value(key)){fail(tr("Host depth result belongs to another H.265 frame"));return false;}
    }
    if(body&&supportsHostModel(m_hostModel)&&(metadata["assist_graph_sha256"]!=QLatin1String(view.graphSha)||metadata["assist_precision"]!=QLatin1String(view.precision)
        ||metadata["assist_source_graph_sha256"]!=QLatin1String(view.sourceSha)||metadata["assist_conversion"]!=QLatin1String(view.conversion)
        ||(RvCameraView::isWide(m_hostModel)&&(metadata["assist_camera_profile"]!="wide"||metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha)
            ||metadata["depth_K"].toArray()!=RvCameraView::intrinsics(true)))))
        {fail(tr("Host depth result does not match the selected model"));return false;}
    if(body&&view.provisional&&(metadata["assist_provisional_model"]!=true
        ||metadata["assist_candidate_parity_mean_limit"]!=view.parityMeanLimit))
        {fail(tr("Host depth result does not match the selected model"));return false;}
    if(isUniDepth(m_hostModel)&&current.tof.metadata.contains("assist_output_policy")){
        const auto policy=current.tof.metadata.value("assist_output_policy");
        const bool geometric=policy==QLatin1String(RvAssistFusion::FieldOfViewPolicy);
        if(metadata["assist_output_policy"]!=current.tof.metadata["assist_output_policy"]
           ||(!geometric&&policy!=QLatin1String(RvAssistFusion::SupportPolicy))||(!geometric&&current.tof.samples.isEmpty()))
            {fail(tr("Invalid dToF output support contract"));return false;}
        if(geometric){
            const auto fov=current.tof.metadata.value("assist_tof_fov_correction");
            if(!RvCameraView::isWide(m_hostModel)||!fov.isDouble()||(fov!=1&&fov!=2)||metadata.value("assist_tof_fov_correction")!=fov)
                {fail(tr("Invalid dToF output support contract"));return false;}
        }
        if(body){
            const auto fit=metadata["assist_fusion"].toObject();int positive=0;
            const auto count=fit[geometric?"fov_pixels":"support_pixels"].toDouble(-1);
            if(fit["output_policy"]!=policy||(geometric&&fit["fov_geometry_only"]!=true)||!std::isfinite(count)||count<0||count>Pixels||std::floor(count)!=count)
                {fail(tr("Invalid dToF output support contract"));return false;}
            for(int i=0;i<Pixels;++i){const quint32 bits=qFromLittleEndian<quint32>(depth.constData()+4*i);float z;std::memcpy(&z,&bits,4);
                if(!std::isfinite(z)||z<0){fail(tr("Invalid dToF output support contract"));return false;}positive+=z>0;}
            if(positive!=count||(!geometric&&positive>0&&(!fit["dtof_corrected"].toBool()||current.tof.samples.isEmpty())))
                {fail(tr("Invalid dToF output support contract"));return false;}
        }
    }
    if(RvCameraView::isColorLiteSR(m_hostModel)&&(metadata.value("color_raw_preview")!=current.tof.metadata.value("color_raw_preview")||metadata["color_raw_preview"].toBool()!=colorRawPreview()))
        {fail(QStringLiteral("Color LiteSR display identity mismatch"));return false;}
    if(metadata.value("assist_experiment")!=current.tof.metadata.value("assist_experiment"))
        {fail(QStringLiteral("Host depth experiment identity mismatch"));return false;}
    if((metadata["assist_input_rotation_degrees"]!=current.tof.metadata["assist_input_rotation_degrees"]
        ||metadata["assist_input_rotation_degrees"].toInt(-1)!=m_inputRotation))
        {fail(tr("Host depth result belongs to another input orientation"));return false;}
    if(metadata.value("frame_id")!=current.tof.metadata.value("frame_id")||metadata.value("generation")!=current.tof.metadata.value("generation")
        ||metadata.value("tof_sequence")!=current.tof.metadata.value("tof_sequence")||metadata.value("tof_timestamp_ns")!=current.tof.metadata.value("tof_timestamp_ns"))
        {fail(tr("Host depth result belongs to another RGB/dToF pair"));return false;}
    metadata["assist_frame_age_ms"]=double(age);metadata["assist_pending_dropped"]=double(m_dropped);
    const double measurementAge=current.tof.metadata["measurement_age_at_pair_ms"].toDouble()+monotonicMs()-current.tof.metadata["host_pair_enqueued_ms"].toDouble(monotonicMs());
    const bool checkMeasurementAge=tofDriven()||DepthorV3::supported(m_hostModel)||(reusesNativeMeasurements()&&!current.tof.samples.isEmpty());
    if(checkMeasurementAge){metadata["assist_measurement_age_ms"]=measurementAge;metadata["assist_drop_reasons"]=m_dropReasons;}
    if(age>1000||(checkMeasurementAge&&measurementAge>1000)){m_dropReasons["result_expired"]=m_dropReasons["result_expired"].toInt()+1;emit cleared(tr("Host assisted depth result expired"));}
    else if(body!=DepthBytes||!response["error"].toString().isEmpty()){
        const auto source=response["error"].toString(tr("Invalid host assisted depth result")).toUtf8();
        const auto reason=QCoreApplication::translate("RvAssistFusion",source.constData());
        emit frameRejected(metadata,reason);if(!alive)return false;
        emit cleared(reason);
    }
    // Forward only the native measurement belonging to this validated
    // result. The GUI must never reconstruct it from the newest live ToF.
    else {
        const bool profile=DepthorV3::supported(m_hostModel)&&qEnvironmentVariable("RV_DEPTHOR_PERF_PROFILE")=="1";
        if(profile){metadata["assist_parent_validation_ms"]=responseClock.nsecsElapsed()/1e6;
            metadata["assist_parent_serialize_ms"]=current.serializeMs;metadata["assist_previous_gui_publish_ms"]=m_lastGuiPublishMs;
            metadata["assist_parent_ready_age_ms"]=double(monotonicMs()-current.receivedMs);}
        if(view.networkProfile){
            metadata["assist_parent_validation_ms"]=responseClock.nsecsElapsed()/1e6;
            metadata["assist_previous_gui_publish_ms"]=m_lastGuiPublishMs;
            // Validation is complete. Start the next GPU job while direct
            // GUI observers consume this immutable paired result. The
            // original models retain their ordering and transport.
            const auto token=m_token,model=m_hostModel;
            sendLatest();if(!alive)return false;
            if(m_process.bytesToWrite())m_process.waitForBytesWritten(0);
            if(!alive||!m_enabled||m_stopping||m_failed||m_token!=token||m_hostModel!=model)return false;
        }
        QElapsedTimer publishClock;publishClock.start();
        emit resultReady(depth,metadata,current.camera,current.epoch,current.tof,coverage);
        if(!alive)return false;
        if(view.networkProfile||profile)m_lastGuiPublishMs=publishClock.nsecsElapsed()/1e6;
    }
    if(!alive)return false;
    sendLatest();if(!alive||!m_enabled||m_stopping)return false;
    return true;
}

namespace {
int runAssistEngine(const QString &root,const QString &token,const AssistReader &readNext,const AssistWriter &writer,const QString &selectedHostModel) {
    const auto writeResponse=[&](const QJsonObject &json,const QByteArray &bytes=QByteArray(),const QByteArray &aux=QByteArray()){return writer(json,bytes,aux);};
    NativeModel model;
    try {
        model.initialize(root,selectedHostModel);
        if(DepthorV3::supported(model.hostModel)) {
            // CUDA's first Run can exceed a live frame's 1000 ms age limit.
            // Finish allocation/kernel setup inside the loading deadline,
            // before ready. This synthetic prediction is never published.
            std::vector<float> sparse(DepthorV3::N,0.f);
            sparse[(DepthorV3::H/2)*DepthorV3::W+DepthorV3::W/2]=1.f;
            model.inferDepthorNetwork(std::vector<float>(3*DepthorV3::N,.5f),sparse);
        }
    }catch(const std::exception &e) {
        writeResponse({{"protocol",1},{"type","fatal"},{"token",token},{"error",QString::fromUtf8(e.what())}});return 2;
    }
    const auto &view=model.view();
    const QString hostModel=model.hostModel;
    if(!writeResponse({{"protocol",1},{"type","ready"},{"token",token},{"weights_sha256",view.weightsSha},{"graph_sha256",view.graphSha},
        {"precision",view.precision},{"source_graph_sha256",view.sourceSha},{"conversion",view.conversion},{"host_model",hostModel},{"model_loading",model.storageInfo}}))return 3;
    QByteArray rgb,tofBytes;QJsonObject request;
    RvAssistFusion::ScaleContinuity continuity;
    RvAssistFusion::ScaleContinuity compareLegacyContinuity;
    int previousInputRotation=-1;
    quint64 lastHdTof=0;qint64 lastHdTofTimestampNs=0;QByteArray lastHdNative;QJsonObject lastHdNativeContext;QString hdIdentity;
    int reviewPairs=0;
    for(;;) {
        const auto next=readNext(request,rgb,tofBytes);
        if(next==AssistRead::End)break;
        if(next!=AssistRead::Request||rgb.size()!=RgbBytes||(tofBytes.size()!=0&&tofBytes.size()!=6144&&!(DepthorV3::supported(hostModel)&&tofBytes.size()==256)&&!(RvCameraView::isUniDepthNative16(hostModel)&&tofBytes.size()==1024))
            ||request["protocol"].toInt()!=1||request["token"]!=token||request["type"]!="infer")return 3;
        auto metadata=request["metadata"].toObject();
        QJsonObject response={{"protocol",1},{"type","result"},{"token",token},{"request",request["request"]},{"epoch",request["epoch"]}};
        QByteArray output,coverage;
        try {
            const QString experiment=metadata.value("assist_experiment").toString("A");
            const bool reuse=RvCameraView::isUniDepth(hostModel)&&experiment=="H";
            const bool hd=hdSource(metadata,model.wide,DepthorV3::supported(hostModel)||reuse);
            if(request["host_model"].toString("unidepth")!=hostModel||metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha))
                throw std::runtime_error("UniDepth camera profile mismatch");
            if(RvCameraView::isUniDepthNative16(hostModel))metadata["assist_host_model"]=hostModel;
            const RvTofFrame tof{tofBytes,metadata};
            if(metadata["source_protocol"]=="HDA1"&&!hd)throw std::runtime_error("Invalid H.265 clock pairing");
            const bool paired=hd?metadata["dtof_present"].toBool():true;
            if(RvCameraView::isUniDepthNative16(hostModel)&&!hd)throw std::runtime_error("Native16 requires managed H.265 capture");
            if(!RvAssistDepth::validExperiment(experiment)||(experiment=="B"&&(!hd||!paired))||(reuse&&!hd))
                throw std::runtime_error("Invalid ToF experiment request");
            const bool supported=metadata.value("assist_output_policy")==QLatin1String(RvAssistFusion::SupportPolicy);
            const bool geometric=metadata.value("assist_output_policy")==QLatin1String(RvAssistFusion::FieldOfViewPolicy);
            if(metadata.contains("assist_output_policy")&&((!supported&&!geometric)||(supported&&!paired)))
                throw std::runtime_error("Invalid dToF output support contract");
            int geometricFov=0;
            if(geometric){
                const auto value=metadata.value("assist_tof_fov_correction");
                if(!model.wide||!value.isDouble()||(value!=1&&value!=2))throw std::runtime_error("Invalid dToF output support contract");
                geometricFov=value.toInt();
                const auto nativeFov=metadata.contains("dtof_fov_correction")?metadata.value("dtof_fov_correction"):metadata.value("tof_fov_correction");
                if(paired&&nativeFov!=value)throw std::runtime_error("Invalid dToF output support contract");
            }
            if((paired&&!(reuse?tof.isValidForHostFusion():tof.isValid(DepthorV3::supported(hostModel))))||(!paired&&!tofBytes.isEmpty()))throw std::runtime_error("Invalid native dToF or H.265 input");
            if(paired&&!RvCameraView::supportsNativeGrid(hostModel,tof.columns(),tof.rows()))
                throw std::runtime_error("Native grid does not belong to the selected host model");
            if(hd&&paired){
                const auto identity=metadata["boot_id"].toString()+metadata["source_generation"].toString()+QString::number(request["epoch"].toInteger());
                if(identity!=hdIdentity){hdIdentity=identity;lastHdTof=0;lastHdTofTimestampNs=0;lastHdNative.clear();lastHdNativeContext={};continuity.reset();compareLegacyContinuity.reset();}
                const auto seq=quint64(metadata["tof_sequence"].toInteger());
                if(DepthorV3::supported(hostModel)||reuse){
                    const auto timestamp=metadata["tof_timestamp_ns"].toInteger();const bool reused=seq==lastHdTof;
                    const auto context=nativeContext(metadata);
                    if(seq<lastHdTof||metadata["tof_reused"]!=QJsonValue(reused)
                        ||(reused&&(timestamp!=lastHdTofTimestampNs||tofBytes!=lastHdNative||context!=lastHdNativeContext))
                        ||(!reused&&lastHdTof&&timestamp<=lastHdTofTimestampNs))throw std::runtime_error(DepthorV3::supported(hostModel)?"DEPTHOR native measurement identity or reuse flag mismatch":"UniDepth H native measurement identity or reuse flag mismatch");
                    lastHdTofTimestampNs=timestamp;lastHdNative=tofBytes;lastHdNativeContext=context;
                }else if(seq<=lastHdTof)throw std::runtime_error("H.265 dToF measurement reused");
                lastHdTof=seq;
            }

            if(DepthorV3::supported(hostModel)) {
                if(!paired||!hd||std::abs(metadata["pair_delta_ms"].toDouble(1000))>RvCameraView::DepthorMaxPairDeltaMs||experiment!="A"
                    ||metadata.contains("assist_output_policy")||metadata.contains("color_raw_preview"))
                    throw std::runtime_error("DEPTHOR V3 requires the latest RGB/dToF pair within 100 ms");
                const auto warning=metadata.value("tof_warning_flags");
                if(!warning.isDouble()||(warning!=0&&!(warning==8&&metadata["warning_policy"]=="observe-0x08")))
                    throw std::runtime_error("DEPTHOR native sensor warning rejected");
                const auto rotationValue=metadata.value("assist_input_rotation_degrees");
                if(!rotationValue.isDouble()||(rotationValue!=0&&rotationValue!=180))throw std::runtime_error("Invalid DEPTHOR input orientation");
                QElapsedTimer timer;timer.start();
                const auto z=model.inferDepthor(rgb,tof,rotationValue.toInt());
                QElapsedTimer pack;if(model.profileDepthor)pack.start();
                output.resize(DepthBytes);int count=0;
                for(int i=0;i<Pixels;++i){quint32 bits;std::memcpy(&bits,&z[i],4);qToLittleEndian(bits,output.data()+4*i);count+=z[i]>0;}
                if(model.profileDepthor){model.depthorTiming["pack_ms"]=pack.nsecsElapsed()/1e6;metadata["assist_depthor_timing"]=model.depthorTiming;}
                metadata["model"]="DEPTHOR Small V3 ("+QString::fromLatin1(view.precision)+")";
                metadata["width"]=576;metadata["height"]=324;metadata["depth_stride"]=576*4;metadata["depth_bytes"]=DepthBytes;
                metadata["depth_unit"]="camera-Z_m_float32";metadata["depth_location"]="host";metadata["inference_location"]="host";
                metadata["depth_present"]=true;metadata["capture_only"]=false;metadata["depth_K"]=RvCameraView::intrinsics(false);
                metadata["depth_source"]="host_depthor_v3";metadata["depth_method"]="absolute_camera_z_no_postfusion";
                metadata["assist_host_model"]=hostModel;metadata["assist_camera_profile"]="standard";
                metadata["assist_network_width"]=640;metadata["assist_network_height"]=480;
                metadata["assist_graph_sha256"]=view.graphSha;metadata["assist_weights_sha256"]=view.weightsSha;
                metadata["assist_source_graph_sha256"]=view.sourceSha;metadata["assist_precision"]=view.precision;metadata["assist_conversion"]=view.conversion;
                metadata["assist_backend"]="onnxruntime-cuda-1.23.2";
                metadata["assist_inference_ms"]=timer.nsecsElapsed()/1e6;metadata["assist_total_ms"]=timer.nsecsElapsed()/1e6;
                metadata["assist_input_orientation_checked"]=true;metadata["assist_output_restored_to_calibrated_grid"]=true;
                metadata["assist_pairing_policy"]=RvCameraView::DepthorPairingPolicy;metadata["assist_maximum_pair_delta_ms"]=RvCameraView::DepthorMaxPairDeltaMs;
                metadata["assist_source_model"]="Native";metadata["assist_source_depth_width"]=0;metadata["assist_source_depth_height"]=0;
                metadata["assist_calibration_status"]="device_projection_adapted_dense_accuracy_unverified";
                metadata["v3_old_range_gate_passed"]=false;metadata["physical_accuracy_verified"]=false;
                metadata["depthor_projection"]=model.colorDiagnostics;
                metadata["assist_fusion"]=QJsonObject{{"output_pixels",count},{"fusion_mode","depthor_v3"},{"dtof_conditioned",true},{"dtof_corrected",false}};
                response["metadata"]=metadata;if(!writeResponse(response,output))return 3;continue;
            }

            if(RvCameraView::isColorLiteSR(hostModel)) {
                if(!paired||std::abs(metadata["pair_delta_ms"].toDouble(1000))>20)throw std::runtime_error("Color LiteSR requires a current RGB/dToF pair within 20 ms");
                QElapsedTimer timer;timer.start();
                const bool rawPreview=metadata["color_raw_preview"].toBool();
                const auto z=model.inferColor(rgb,tof,metadata["assist_input_rotation_degrees"].toInt(-1),rawPreview);
                output.resize(DepthBytes);int count=0;
                for(int i=0;i<Pixels;++i){quint32 bits;std::memcpy(&bits,&z[i],4);qToLittleEndian(bits,output.data()+4*i);count+=z[i]>0;}
                metadata["model"]=QString::fromLatin1(view.modelName);metadata["width"]=576;metadata["height"]=324;
                metadata["depth_unit"]="camera-Z_m_float32";metadata["depth_location"]="host";metadata["depth_present"]=true;metadata["capture_only"]=false;
                metadata["depth_stride"]=576*4;metadata["depth_bytes"]=DepthBytes;metadata["depth_K"]=RvCameraView::intrinsics(false);
                metadata["depth_source"]="host_litesr_color_projected";metadata["depth_method"]="projected_camera_radial_v1";
                if(rawPreview){metadata["depth_unit"]="camera_radial_m_float32";metadata["depth_method"]="raw_network_inverse_normalized";}
                metadata["assist_host_model"]=hostModel;metadata["assist_graph_sha256"]=view.graphSha;metadata["assist_weights_sha256"]=view.weightsSha;
                metadata["assist_source_graph_sha256"]=view.sourceSha;metadata["assist_precision"]=view.precision;metadata["assist_conversion"]=view.conversion;
                metadata["assist_camera_profile"]="standard";metadata["assist_network_width"]=256;metadata["assist_network_height"]=256;
                metadata["assist_inference_ms"]=timer.nsecsElapsed()/1e6;metadata["assist_total_ms"]=timer.nsecsElapsed()/1e6;
                metadata["assist_calibration_status"]="projected_real_geometry_accuracy_unverified";metadata["inference_location"]="host";
                metadata["assist_fusion"]=QJsonObject{{"output_pixels",count},{"fusion_mode",rawPreview?"color_litesr_raw":"color_litesr"},{"dtof_corrected",false}};
                metadata["color_surface"]=model.colorDiagnostics;
                metadata["assist_input_orientation_checked"]=true;metadata["assist_output_restored_to_calibrated_grid"]=true;
                response["metadata"]=metadata;if(!writeResponse(response,output))return 3;continue;
            }
            QElapsedTimer clock;clock.start();
            QByteArray inferenceRgb=rgb;
            const int inputRotation=metadata["assist_input_rotation_degrees"].toInt(-1);
            {
                if((inputRotation!=0&&inputRotation!=180)||metadata["assist_input_rotation_degrees"].toDouble(-1)!=inputRotation)
                    throw std::runtime_error("Missing or unsupported UniDepth input orientation");
                if(inputRotation!=previousInputRotation){continuity.reset();compareLegacyContinuity.reset();previousInputRotation=inputRotation;}
                // Rotate each planar channel into the model's upright camera frame.
                // Both centred virtual camera profiles are invariant under 180 degrees. Restore
                // camera-Z to the original sensor grid BEFORE dToF projection.
                // Neither the paired RGB nor calibration/ToF arrays are rotated.
                if(inputRotation==180)for(int c=0;c<3;++c)
                    std::reverse(inferenceRgb.begin()+c*Pixels,inferenceRgb.begin()+(c+1)*Pixels);
            }
#ifdef DCTS_INSTALLER_BUILD
            decltype(model.infer(inferenceRgb)) mono;
            try {
                mono=model.infer(inferenceRgb);
            } catch(const std::exception &error) {
                writeResponse({{"protocol",1},{"type","fatal"},{"token",token},
                               {"error",QString::fromUtf8(error.what())}}, {}, {});
                return 3;
            }
#else
            auto mono=model.infer(inferenceRgb);
#endif
            if(inputRotation==180)std::reverse(mono.begin(),mono.end());
            const double inferMs=clock.nsecsElapsed()/1e6;
            // Reading a missing native field must not insert a null into an
            // RGB-only result: its display geometry uses assist_tof_fov_correction.
            const auto &cal=RvAssistFusion::calibrationForNativeGrid((metadata.contains("dtof_fov_correction")?metadata.value("dtof_fov_correction"):metadata.value("tof_fov_correction")).toInt(1),RvCameraView::isUniDepthNative16(hostModel)?16:48,RvCameraView::isUniDepthNative16(hostModel)?16:32);
            const bool globalOnly=experiment=="G"||reuse;
            const bool fastGlobal=RvCameraView::runtimeModel(hostModel)=="unidepth_wide_576_fp16"&&globalOnly;
            const bool compare=qEnvironmentVariable("RV_UNIDEPTH_COMPARE")=="1";
            const bool supportAudit=qEnvironmentVariable("RV_UNIDEPTH_SUPPORT_AUDIT")=="1";
            const auto reviewDirectory=qEnvironmentVariable("RV_HOST_HD_REVIEW_DIRECTORY");
            // Explicit comparisons/reviews still need RGB and the unmasked
            // result. Normal FP16+G only publishes the final immutable result.
            const bool retainFusion=!fastGlobal||compare||supportAudit||!reviewDirectory.isEmpty();
            const auto camera=retainFusion?unpackRgb(rgb):QImage{};
            const auto epoch=request["epoch"].toInteger(),generation=metadata["generation"].toInteger(),frame=metadata["frame_id"].toInteger();
            const auto measurementMs=metadata.value("tof_timestamp_ns").toInteger()/1000000;
            auto fused=experiment=="B"?RvAssistFusion::fuseRecovering(mono,camera,tof,cal,model.wide,&continuity,measurementMs,epoch,generation,frame):
                continuity.apply(!paired?RvAssistFusion::monocular(mono,"H.265 input has no verified time-paired dToF"):
                    (globalOnly?(fastGlobal?RvAssistFusion::fuseGlobalScale(mono,QSize(576,324),tof,cal,model.wide):
                        RvAssistFusion::fuseGlobalScale(mono,camera,tof,cal,model.wide)):
                        RvAssistFusion::fuse(mono,camera,tof,cal,model.wide)),monotonicMs(),epoch,generation,frame);
            // Same restored tensor and same native pair; scalar-only shadow
            // comparisons do not publish or retain extra images/measurements.
            if(paired&&compare){
                const auto old=compareLegacyContinuity.apply(RvAssistFusion::fuse(mono,camera,tof,cal,model.wide),measurementMs,epoch,generation,frame);
                const auto candidate=RvAssistFusion::fuseRecovering(mono,camera,tof,cal,model.wide);
                auto train=tof;QJsonArray heldIds;
                for(int i=0;i<tof.sampleCount();++i)if(((i%tof.columns()/std::max(1,tof.columns()/8))+(i/tof.columns()/std::max(1,tof.rows()/8)))%4==0){train.samples[4*i+3]=0;heldIds.append(i);}
                const auto heldOld=RvAssistFusion::fuse(mono,camera,train,cal,model.wide);
                const auto heldNew=RvAssistFusion::fuseRecovering(mono,camera,train,cal,model.wide);
                auto heldOnly=tof;for(int i=0;i<tof.sampleCount();++i)if(((i%tof.columns()/std::max(1,tof.columns()/8))+(i/tof.columns()/std::max(1,tof.rows()/8)))%4!=0)heldOnly.samples[4*i+3]=0;
                metadata["assist_comparison"]=QJsonObject{
                    {"reference","same_input_sensor_agreement_not_independent_ground_truth"},
                    {"A",RvAssistFusion::agreement(mono,old,tof,cal,model.wide)["all"]},
                    {"B",RvAssistFusion::agreement(mono,candidate,tof,cal,model.wide)["all"]},
                    {"A_spatial_holdout",RvAssistFusion::agreement(mono,heldOld,heldOnly,cal,model.wide)["all"]},
                    {"B_spatial_holdout",RvAssistFusion::agreement(mono,heldNew,heldOnly,cal,model.wide)["all"]}};
                if(reviewPairs==19&&!qEnvironmentVariable("RV_HOST_HD_REVIEW_DIRECTORY").isEmpty())
                    saveSupportReview(qEnvironmentVariable("RV_HOST_HD_REVIEW_DIRECTORY"),camera,mono,old,candidate,tof,cal,model.wide,"experiment_same_frame.png",4.f);
            }
            const auto fusionMode=fused.diagnostics["fusion_mode"].toString();
            const auto visible=supported?RvAssistFusion::limitTofSupport(fused,tof,RvAssistFusion::calibrationForNativeGrid(
                (metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"]).toInt(1),RvCameraView::isUniDepthNative16(hostModel)?16:48,RvCameraView::isUniDepthNative16(hostModel)?16:32),model.wide):
                (geometric?RvAssistFusion::limitTofFieldOfView(retainFusion?fused:std::move(fused),RvAssistFusion::calibrationForNativeGrid(geometricFov,RvCameraView::isUniDepthNative16(hostModel)?16:48,RvCameraView::isUniDepthNative16(hostModel)?16:32),model.wide):fused);
            metadata["assist_fusion"]=visible.diagnostics;
            if(metadata["assist_diagnostic_capture"]==true){
                coverage=RvDepthDiagnostic::encode(mono,visible,RvCameraView::isUniDepthNative16(hostModel)?256:1536);
                metadata["assist_diagnostic_payload"]="mono_reasons_v1";
                if(RvCameraView::isUniDepthNative16(hostModel))metadata["assist_diagnostic_native_count"]=256;
            }
            metadata["assist_fusion_revision"]=reuse?"global_scale_latest_native_rgb_v2":(experiment=="G"?"global_scale_rgb_v2":(experiment=="B"?"local_recovery_v3":"legacy_v3"));
            if(paired&&supportAudit)
                metadata["assist_support_agreement"]=RvAssistFusion::agreement(mono,fused,tof,RvAssistFusion::calibrationForNativeGrid(
                    (metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"]).toInt(1),RvCameraView::isUniDepthNative16(hostModel)?16:48,RvCameraView::isUniDepthNative16(hostModel)?16:32),model.wide);
            metadata["assist_inference_ms"]=inferMs;metadata["assist_total_ms"]=clock.nsecsElapsed()/1e6;
            metadata["assist_input_orientation_checked"]=true;metadata["assist_output_restored_to_calibrated_grid"]=true;
            if(!visible.ok())throw std::runtime_error(visible.error.toStdString());
            if(paired&&supportAudit&&!reviewDirectory.isEmpty()&&++reviewPairs==20
               &&!saveSupportReview(reviewDirectory,unpackRgb(rgb),mono,fused,visible,tof,RvAssistFusion::calibrationForNativeGrid(
                   (metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"]).toInt(1),RvCameraView::isUniDepthNative16(hostModel)?16:48,RvCameraView::isUniDepthNative16(hostModel)?16:32),model.wide))
                metadata["assist_review_error"]="Failed to save same-frame support comparison";
            output=visible.depth;
            metadata["assist_source_model"]=metadata.value("model");
            metadata["assist_source_depth_width"]=metadata.value("width");
            metadata["assist_source_depth_height"]=metadata.value("height");
            const bool monocular=fusionMode=="monocular";
            const bool held=fusionMode=="held_scale";
            metadata["model"]=QString::fromLatin1(view.modelName)+(monocular?" (monocular)":(held?" (held dToF scale)":" + dToF"));
            if(model.wide)metadata["model"]=metadata["model"].toString()+" / wide view";
            metadata["width"]=576;metadata["height"]=324;
            metadata["depth_unit"]="camera-Z_m_float32";metadata["depth_location"]="host";
            metadata["depth_present"]=true;metadata["capture_only"]=false;metadata["inference_location"]="host";
            metadata["depth_method"]=monocular?"monocular_unadjusted":(held?"monocular_held_dtof_scale":"oriented_robust_scale_logfield_temporal_v3");
            metadata["assist_resolution_level"]=view.resolutionLevel<0?QJsonValue():QJsonValue(view.resolutionLevel);
            if(visible.diagnostics["recovery_applied"]==true)metadata["depth_method"]="oriented_legacy_plus_local_recovery_v1";
            if(globalOnly)metadata["depth_method"]=monocular?"monocular_unadjusted":(held?"monocular_held_dtof_scale":reuse?"oriented_global_scale_latest_native_v2":"oriented_global_scale_temporal_v2");
            metadata["depth_K"]=RvCameraView::intrinsics(model.wide);metadata["depth_stride"]=576*4;
            metadata["depth_bytes"]=DepthBytes;metadata["depth_source"]=monocular?"host_unidepthv2_monocular":(held?"host_unidepthv2_held_scale":"host_unidepthv2_dtof");
            metadata["assist_network_width"]=view.networkWidth;metadata["assist_network_height"]=view.networkHeight;
            if(view.networkProfile)metadata["assist_network_profile"]=view.networkProfile;
            metadata["assist_weights_sha256"]=view.weightsSha;metadata["assist_graph_sha256"]=view.graphSha;
            metadata["assist_camera_profile"]=view.name;metadata["assist_host_model"]=hostModel;
            metadata["assist_precision"]=view.precision;metadata["assist_conversion"]=view.conversion;metadata["assist_source_graph_sha256"]=view.sourceSha;
            if(view.provisional){metadata["assist_provisional_model"]=true;metadata["assist_candidate_parity_mean_limit"]=view.parityMeanLimit;}
#ifdef DCTS_INSTALLER_BUILD
            metadata["assist_backend"]="onnxruntime-tensorrt-rtx";
#else
            metadata["assist_backend"]="onnxruntime-cuda-1.23.2";
#endif
            metadata["assist_calibration_status"]=monocular?"monocular_not_dtof_corrected":(held?"historical_dtof_scale_only":"usable_initial_fusion_with_limits");
        }catch(const std::exception &e) {continuity.reset();output.clear();coverage.clear();response["error"]=QString::fromUtf8(e.what());}
        response["metadata"]=metadata;if(!writeResponse(response,output,coverage))return 3;
    }
    return 0;
}
}

int runRvAssistWorker(const QString &root,const QString &token,const QString &hostModel) {
    if(!RvAssistDepth::supportsHostModel(hostModel))return 3;
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    return runAssistEngine(root,token,stdioAssistReader(),[](const auto &json,const auto &bytes,const auto &aux){return writePacket(json,bytes,aux);},hostModel);
}

// Synthetic test adapter only. Real optimized sessions never read/write these
// packets; they submit shared images and receive typed responses in memory.
int runRvAssistDirectTransportTest(const QString &root,const QString &token){
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    std::mutex mutex;std::condition_variable changed;bool ready=false,failed=false;int completed=0;
    RvAssistDirectTransport direct(root,token,[&](const QJsonObject &response,const QByteArray &depth,const QByteArray &diagnostic){
        std::lock_guard<std::mutex> lock(mutex);
        if(response["type"]=="ready"&&response["host_model"]!="unidepth_wide_576_fp16"){
            failed=true;writePacket({{"protocol",1},{"type","fatal"},{"token",token},{"error","Direct test only accepts RGB576 FP16"}});
        }else{
            if(!writePacket(response,depth,diagnostic))failed=true;
            ready|=response["type"]=="ready";failed|=response["type"]=="fatal";completed+=response["type"]=="result";
        }
        changed.notify_one();
    });
    {std::unique_lock<std::mutex> lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(90),[&]{return ready||failed;})||failed)return 2;}
    auto read=stdioAssistReader();QJsonObject request;QByteArray rgb,native;int submitted=0;
    for(;;){
        const auto next=read(request,rgb,native);if(next==AssistRead::End)return 0;if(next!=AssistRead::Request)return 3;
        if(!direct.submit(request,unpackRgb(rgb),native))return 3;++submitted;
        std::unique_lock<std::mutex> lock(mutex);
        if(!changed.wait_for(lock,std::chrono::seconds(90),[&]{return failed||completed>=submitted;})||failed)return 2;
    }
}

int runRvAssistRuntimeTest(const QString &root,const QString &reportPath) {
    QJsonObject report;bool passed=false;
    try {
        NativeModel model;QElapsedTimer clock;clock.start();model.initialize(root);report["load_ms"]=clock.nsecsElapsed()/1e6;

        if(DepthorV3::supported(model.hostModel)) {
            const auto numericalCases=qEnvironmentVariable("RV_DEPTHOR_NUMERICAL_CASES");
            if(!numericalCases.isEmpty()){
                const auto result=model.checkDepthorNumericalCases(numericalCases);QFile file(reportPath);
                if(!file.open(QIODevice::WriteOnly))return 3;file.write(QJsonDocument(result).toJson());return result["pass"].toBool()?0:2;
            }
            const auto floats=[&](const char *name,int size){QFile f(QDir(root).filePath(name));
                if(!f.open(QIODevice::ReadOnly)||f.size()!=size*4)throw std::runtime_error("DEPTHOR synthetic fixture missing");
                auto bytes=f.readAll();std::vector<float> values(size);std::memcpy(values.data(),bytes.constData(),bytes.size());return values;};
            auto rgb=floats("parity_rgb.f32",3*DepthorV3::N),sparse=floats("parity_sparse.f32",DepthorV3::N),expected=floats("parity_depth.f32",DepthorV3::N);
            const int forwardSeconds=std::clamp(qEnvironmentVariableIntValue("RV_DEPTHOR_PURE_BENCH_SECONDS"),0,30);
            if(forwardSeconds){const auto benchmark=model.benchmarkDepthorForward(rgb,sparse,forwardSeconds);
                QFile file(reportPath);if(!file.open(QIODevice::WriteOnly))return 3;file.write(QJsonDocument(benchmark).toJson());return benchmark["pass"].toBool()?0:2;}
            QJsonArray timings;std::vector<float> predicted;
            for(int repeat=0;repeat<4;++repeat){clock.restart();predicted=model.inferDepthorNetwork(rgb,sparse);if(repeat)timings.append(clock.nsecsElapsed()/1e6);}
            double mean=0,maxAbs=0,meanRel=0,maxRel=0;
            for(int i=0;i<DepthorV3::N;++i){const double delta=std::abs(double(predicted[i])-expected[i]),relative=delta/std::max(double(expected[i]),1e-6);
                mean+=delta;meanRel+=relative;maxAbs=std::max(maxAbs,delta);maxRel=std::max(maxRel,relative);}
            report["mean_abs_m"]=mean/DepthorV3::N;report["max_abs_m"]=maxAbs;report["mean_relative_error"]=meanRel/DepthorV3::N;report["max_relative_error"]=maxRel;
            report["original_fp32_1mm_1e4_gate_pass"]=maxAbs<=.001&&meanRel/DepthorV3::N<=1e-4;
            report["inference_ms"]=timings;report["precision"]=model.view().precision;report["graph_sha256"]=model.view().graphSha;
            QFile output(QFileInfo(reportPath).absolutePath()+"/"+QFileInfo(reportPath).completeBaseName()+"_depth.f32");
            if(!output.open(QIODevice::WriteOnly)||output.write(reinterpret_cast<const char*>(predicted.data()),predicted.size()*4)!=qint64(predicted.size()*4))throw std::runtime_error("Cannot save DEPTHOR synthetic result");
            RvTofFrame tof;tof.samples=QByteArray(6144,0);
            tof.metadata=QJsonObject{{"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},
                {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
                {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"},
                {"work_mode","rgb_tof"},{"frame_id",1},{"generation",1},{"tof_sequence",1},{"tof_timestamp_ns",1},{"tof_reused",false}};
            QJsonArray cases;
            for(int fov:{1,2})for(int rotation:{0,180}){
                tof.metadata["dtof_fov_correction"]=fov;
                for(int i=0;i<1536;++i){qToLittleEndian<quint16>(2400,tof.samples.data()+4*i);tof.samples[4*i+2]=20;tof.samples[4*i+3]=1;}
                auto z=model.inferDepthor(QByteArray(RgbBytes,char(128)),tof,rotation);int count=0;
                for(int y=0;y<324;++y)for(int x=0;x<576;++x){const auto value=z[y*576+x];if(!std::isfinite(value)||value<0||((x<72||x>=504)?value!=0:value<=0))throw std::runtime_error("DEPTHOR crop/restoration regression");count+=value>0;}
                tof.samples.fill(0);bool rejected=false;try{model.inferDepthor(QByteArray(RgbBytes,char(128)),tof,rotation);}catch(...){rejected=true;}
                if(!rejected)throw std::runtime_error("Empty DEPTHOR native input was accepted");
                cases.append(QJsonObject{{"fov",fov},{"rotation",rotation},{"positive_pixels",count},{"empty_input_rejected",rejected}});
            }
            report["geometry_cases"]=cases;report["host_model"]=model.hostModel;report["physical_accuracy_verified"]=false;
            report["execution"]=model.depthorExecutionInfo();
            report["v3_old_range_gate_passed"]=false;report["pass"]=true;
            QFile file(reportPath);if(!file.open(QIODevice::WriteOnly))return 3;file.write(QJsonDocument(report).toJson());return 0;
        }

        if(RvCameraView::isColorLiteSR(model.hostModel)) {
            RvColorSurface::regression();report["surface_regressions_passed"]=true;
            RvTofFrame tof;tof.samples=QByteArray(6144,0);
            tof.metadata=QJsonObject{{"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},
                {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
                {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"},
                {"work_mode","rgb_tof"},{"frame_id",1},{"generation",1},{"tof_sequence",1},{"tof_timestamp_ns",1},{"tof_reused",false}};
            const QByteArray rgb(RgbBytes,char(128));QJsonArray cases;
            for(int fov:{1,2})for(int rotation:{0,180}) {
                tof.metadata["dtof_fov_correction"]=fov;
                for(int i=0;i<1536;++i){qToLittleEndian<quint16>(2400,tof.samples.data()+4*i);tof.samples[4*i+2]=20;tof.samples[4*i+3]=1;}
                clock.restart();auto z=model.inferColor(rgb,tof,rotation);int count=0;double low=10,high=0;
                for(float v:z){if(!std::isfinite(v)||v<0||v>1.4)throw std::runtime_error("Invalid synthetic color result");if(v>0){++count;low=std::min(low,double(v));high=std::max(high,double(v));}}
                if(count==0)throw std::runtime_error("Projected synthetic plane is empty");
                cases.append(QJsonObject{{"fov",fov},{"rotation",rotation},{"positive_pixels",count},{"surface",model.colorDiagnostics},{"minimum_z",low},{"maximum_z",high},{"inference_ms",clock.nsecsElapsed()/1e6}});
                const auto filtered=z;
                const auto raw=model.inferColor(rgb,tof,rotation,true);
                int revealed=0;
                for(int y=0;y<324;++y)for(int x=0;x<576;++x){const int i=y*576+x;
                    if(!std::isfinite(raw[i]))throw std::runtime_error("Raw synthetic prediction is not finite");
                    if(filtered[i]==0&&raw[i]!=0)++revealed;
                    const double dx=(x-287.5)/574,dy=(y-161.5)/574;
                    if(filtered[i]>0&&std::abs(raw[i]/std::sqrt(1+dx*dx+dy*dy)-filtered[i])>2e-5)throw std::runtime_error("Raw and filtered outputs disagree on supported pixels");}
                if(!revealed)throw std::runtime_error("Raw prediction still has an output mask");
                if(model.inferColor(rgb,tof,rotation)!=filtered)throw std::runtime_error("Raw preview changed filtered result");
                report["raw_preview_regressions_passed"]=true;
                tof.samples.fill(0);z=model.inferColor(rgb,tof,rotation);for(float v:z)if(v!=0)throw std::runtime_error("Empty native input emitted depth");
            }
            report["synthetic_geometry_cases"]=cases;report["host_model"]=model.hostModel;report["pass"]=true;
            report["actual_model_output_hw"]=QJsonArray{256,256};report["presentation_hw"]=QJsonArray{324,576};
            report["physical_accuracy_verified"]=false;QFile file(reportPath);if(!file.open(QIODevice::WriteOnly))return 3;
            file.write(QJsonDocument(report).toJson());return 0;
        }
        QFile input(QDir(root).filePath("parity_rgb.bin")),reference(QDir(root).filePath("parity_depth.bin"));
        if(!input.open(QIODevice::ReadOnly)||input.size()!=RgbBytes||!reference.open(QIODevice::ReadOnly)||reference.size()!=DepthBytes)throw std::runtime_error("Synthetic parity fixtures missing");
        const auto rgb=input.readAll(),ref=reference.readAll();QJsonArray times;double mean=0,maximum=0;
        for(int i=0;i<6;++i) {
            clock.restart();const auto z=model.infer(rgb);times.append(clock.nsecsElapsed()/1e6);mean=0;maximum=0;
            for(int j=0;j<Pixels;++j) {float expected;std::memcpy(&expected,ref.constData()+4*j,4);const double error=std::abs(z[j]-expected)/std::max(double(expected),1e-6);mean+=error;maximum=std::max(maximum,error);}
            mean/=Pixels;
        }
        report["mean_relative_error"]=mean;report["max_relative_error"]=maximum;report["inference_ms"]=times;
        report["backend"]="onnxruntime-cuda-1.23.2";report["graph_sha256"]=model.view().graphSha;
        report["camera_profile"]=model.view().name;
        report["precision"]=model.view().precision;report["conversion"]=model.view().conversion;report["source_graph_sha256"]=model.view().sourceSha;
        report["strict_original_parity_pass"]=mean<.001&&maximum<.01;
        report["provisional_model"]=model.view().provisional;report["mean_relative_error_limit"]=model.view().parityMeanLimit;
        report["max_relative_error_limit"]=model.view().parityMaxLimit;
        passed=(mean<model.view().parityMeanLimit&&maximum<model.view().parityMaxLimit);
    }catch(const std::exception &e) {report["error"]=QString::fromUtf8(e.what());}
    report["pass"]=passed;QFile file(reportPath);if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(report).toJson());return passed?0:2;
}

int runRvAssistLiveTest(const QString &reportPath,const QString &hostModel) {
    if(!RvAssistDepth::supportsHostModel(hostModel))return 3;
    // Explicit, read-only diagnostic: one CAN8 client, no configure/deploy/SSH,
    // no recording of images, depths, native samples or model tensors.
    QEventLoop loop;QElapsedTimer clock;clock.start();
    QNetworkAccessManager network;network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest statusRequest(QUrl("http://172.32.0.93:8837/status"));statusRequest.setTransferTimeout(3000);
    auto *statusReply=network.get(statusRequest);QEventLoop statusLoop;
    QObject::connect(statusReply,&QNetworkReply::finished,&statusLoop,&QEventLoop::quit);statusLoop.exec();
    const auto statusBody=statusReply->readAll();const auto status=QJsonDocument::fromJson(statusBody).object();
    const auto service=status["service"].toObject(),telemetry=status["telemetry"].toObject();
    const bool available=statusReply->error()==QNetworkReply::NoError&&service["state"]=="running"
        &&service["rgb_width"].toInt()==576&&telemetry["client_connected"].isBool()&&!telemetry["client_connected"].toBool();
    const auto expectedGeneration=service["generation"].toVariant().toLongLong();statusReply->deleteLater();
    if(!available||expectedGeneration<=0){
        QFile report(reportPath);if(!report.open(QIODevice::WriteOnly))return 3;
        report.write(QJsonDocument(QJsonObject{{"pass",false},{"error","Live test requires an idle running 576 camera service; no connection was opened"}}).toJson());return 2;
    }
    RvCameraStream stream;stream.host="172.32.0.93";stream.port=8831;
    RvAssistDepth assist;assist.setHostModel(hostModel);assist.setEnabled(true);
    QJsonArray results,errors,rejected;QJsonObject source;quint64 last=0;int packets=0,rejectedCount=0;bool sourceChanged=false;
    QObject::connect(&assist,&RvAssistDepth::resultReady,&loop,[&](const QByteArray &depth,const QJsonObject &meta,const QImage &,quint64){
        int finite=0;double low=1e30,high=0;
        for(int i=0;i<depth.size()/4;++i){float z;std::memcpy(&z,depth.constData()+4*i,4);if(std::isfinite(z)&&z>0){++finite;low=std::min(low,double(z));high=std::max(high,double(z));}}
        results.append(QJsonObject{{"frame_id",meta["frame_id"]},{"generation",meta["generation"]},{"tof_sequence",meta["tof_sequence"]},
            {"pair_delta_ms",meta["pair_delta_ms"]},{"inference_ms",meta["assist_inference_ms"]},{"total_ms",meta["assist_total_ms"]},
            {"age_ms",meta["assist_frame_age_ms"]},{"fusion",meta["assist_fusion"]},{"finite_positive_pixels",finite},
            {"width",meta["width"]},{"height",meta["height"]},{"depth_K",meta["depth_K"]},{"minimum_z_m",low},{"maximum_z_m",high},
            {"host_model",meta["assist_host_model"]},{"input_rotation_degrees",meta["assist_input_rotation_degrees"]},
            {"orientation_checked",meta["assist_input_orientation_checked"]},{"output_restored",meta["assist_output_restored_to_calibrated_grid"]},{"valid_roi",meta["assist_valid_roi_xywh"]}});
        if(results.size()>=8)loop.quit();
    });
    QObject::connect(&assist,&RvAssistDepth::cleared,&loop,[&](const QString &error){if(errors.size()<12&&!errors.contains(error))errors.append(error);});
    QObject::connect(&assist,&RvAssistDepth::frameRejected,&loop,[&](const QJsonObject &meta,const QString &reason){
        ++rejectedCount;
        if(rejected.size()<24)rejected.append(QJsonObject{{"frame_id",meta["frame_id"]},{"reason",reason},{"fusion",meta["assist_fusion"]}});
    });
    QTimer poll;poll.setInterval(15);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&]{
        const auto snapshot=stream.snapshot();if(last==snapshot.sequence)return;last=snapshot.sequence;
        if(snapshot.image.isNull()) {if(errors.size()<12&&!errors.contains(snapshot.status))errors.append(snapshot.status);return;}
        if(snapshot.metadata["generation"].toVariant().toLongLong()!=expectedGeneration){sourceChanged=true;errors.append("Board generation changed during read-only test");loop.quit();return;}
        ++packets;if(source.isEmpty()) {
            for(const auto *key:{"protocol","fusion_protocol","generation","model","rgb_width","rgb_height","width","height","dtof_width","dtof_height",
                    "dtof_format","dtof_coordinate","dtof_range_unit","rectify_map_sha256","tof_filter","tof_filter_raw_preserved"})source[key]=snapshot.metadata[key];
            source["rectified_rgb888_bytes"]=snapshot.image.width()*snapshot.image.height()*3;
            source["wire_nv12_bytes"]=snapshot.image.width()*snapshot.image.height()*3/2;source["native_tof_bytes"]=snapshot.tof.samples.size();
        }
        auto tof=snapshot.tof;tof.metadata=snapshot.metadata;assist.submit(snapshot.image,tof,1);
    });
    QTimer timeout;timeout.setSingleShot(true);QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    stream.start();poll.start();timeout.start(hostModel=="unidepth"?40000:100000);loop.exec();poll.stop();timeout.stop();
    assist.setEnabled(false);stream.requestInterruption();stream.wait();
    const bool passed=!sourceChanged&&packets>=3&&results.size()>=3;
    const QJsonObject report={{"pass",passed},{"selected_host_model",hostModel},{"source",source},{"packets",packets},{"results",results},{"errors",errors},{"rejected_examples",rejected},{"rejected_inferences",rejectedCount},
        {"elapsed_ms",clock.elapsed()},{"camera_or_depth_recorded",false},{"board_configuration_changed",false},
        {"accuracy_ground_truth_available",false},{"calibration_status","usable_initial_fusion_with_limits"}};
    QFile file(reportPath);if(!file.open(QIODevice::WriteOnly))return 3;file.write(QJsonDocument(report).toJson());return passed?0:2;
}
