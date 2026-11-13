#include "RvAssistDepth.h"
#include "RvInferenceRuntime.h"
#include "RvInferenceSettings.h"
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
#include <functional>
#include <fcntl.h>
#include <io.h>
#define NOMINMAX
#include <windows.h>
#ifdef DCTS_TENSORRT_RTX
#include <dxgi.h>
#include <sddl.h>
#include <winioctl.h>
#endif

struct RvAssistWorkerPaths {
    QString program, runtime;
#ifdef DCTS_TENSORRT_RTX
    QString directory;
    QStringList junctions, stagedDirectories;
    bool stagedWorker = false, stagedRuntime = false;

    ~RvAssistWorkerPaths() {
        for(const auto &path:stagedDirectories) QDir(path).removeRecursively();
        for(const auto &junction:junctions) {
            const auto path=QDir::toNativeSeparators(junction).toStdWString();
            RemoveDirectoryW(path.c_str());
        }
        if(!directory.isEmpty()) {
            const auto path=QDir::toNativeSeparators(directory).toStdWString();
            RemoveDirectoryW(path.c_str());
        }
    }

    static bool isAscii(const QString &path) {
        return !path.isEmpty()&&std::all_of(path.begin(),path.end(),[](QChar c){return c.unicode()<128;});
    }

    static QString asciiPath(const QString &path) {
        if(isAscii(path))return path;
        const auto native=QDir::toNativeSeparators(path).toStdWString();
        const DWORD length=GetShortPathNameW(native.c_str(),nullptr,0);
        if(!length)return {};
        std::wstring shortened(length,L'\0');
        const auto written=GetShortPathNameW(native.c_str(),shortened.data(),length);
        if(!written||written>=length)return {};
        const auto result=QDir::fromNativeSeparators(QString::fromWCharArray(shortened.data(),written));
        return isAscii(result)?result:QString();
    }

    bool createPrivateDirectory() {
        HANDLE token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
        const auto closeToken=qScopeGuard([token]{CloseHandle(token);});
        DWORD bytes=0;
        GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
        if(!bytes)return false;
        QByteArray identity(bytes,Qt::Uninitialized);
        if(!GetTokenInformation(token,TokenUser,identity.data(),bytes,&bytes))return false;
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(identity.constData())->User.Sid,&sid))
            return false;
        const auto freeSid=qScopeGuard([sid]{LocalFree(sid);});
        const std::wstring acl=std::wstring(L"D:P(A;OICI;FA;;;")+sid+L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
               acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return false;
        const auto freeDescriptor=qScopeGuard([&]{LocalFree(descriptor);});
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
        for(const auto &candidate:{QDir::tempPath(),qEnvironmentVariable("ProgramData")}) {
            const auto root=asciiPath(candidate);
            if(root.isEmpty()||!QDir::isAbsolutePath(root))continue;
            const auto path=QDir(root).filePath("DctsRtx-"+QUuid::createUuid().toString(QUuid::Id128));
            const auto native=QDir::toNativeSeparators(path).toStdWString();
            if(CreateDirectoryW(native.c_str(),&security)) {
                directory=path;
                return true;
            }
        }
        return false;
    }

    QString aliasDirectory(const QString &target,const QString &name) {
        if(isAscii(target))return target;
        if(directory.isEmpty()&&!createPrivateDirectory())return {};

        const auto alias=QDir(directory).filePath(name);
        const auto link=QDir::toNativeSeparators(alias).toStdWString();
        const auto source=QDir::toNativeSeparators(target).toStdWString();
        if(source.compare(0,2,L"\\\\")==0)return {}; // Junctions target local volumes.
        const auto substitute=L"\\??\\"+source;
        // Mount-point reparse buffers store the substitute and print names as UTF-16.
        struct MountPoint {
            DWORD tag;
            WORD dataLength,reserved,substituteOffset,substituteLength,printOffset,printLength;
            wchar_t names[4096];
        } data{};
        if(substitute.size()+source.size()+2>std::size(data.names)||!CreateDirectoryW(link.c_str(),nullptr))return {};
        junctions.append(alias);
        data.tag=IO_REPARSE_TAG_MOUNT_POINT;
        data.substituteLength=WORD(substitute.size()*sizeof(wchar_t));
        data.printOffset=WORD((substitute.size()+1)*sizeof(wchar_t));
        data.printLength=WORD(source.size()*sizeof(wchar_t));
        data.dataLength=WORD(8+data.printOffset+data.printLength+sizeof(wchar_t));
        std::copy(substitute.begin(),substitute.end(),data.names);
        std::copy(source.begin(),source.end(),data.names+substitute.size()+1);
        const HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return {};
        const auto close=qScopeGuard([handle]{CloseHandle(handle);});
        DWORD returned=0;
        return DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,8+data.dataLength,
                               nullptr,0,&returned,nullptr)?alias:QString();
    }

    static bool readableFile(const QString &path) {
        QFile file(path);
        return QFileInfo(path).isFile()&&file.open(QIODevice::ReadOnly);
    }

    QString readableAsciiDirectory(const QString &source,const QString &alias,const QString &required) {
        const auto shortPath=asciiPath(source);
        if(!shortPath.isEmpty()&&readableFile(QDir(shortPath).filePath(required)))return shortPath;
        const auto target=aliasDirectory(source,alias);
        if(!target.isEmpty()&&readableFile(QDir(target).filePath(required)))return target;
        return {};
    }

    QString stageFiles(const QDir &source,const QString &name,const QStringList &files) {
        if(files.isEmpty()||(directory.isEmpty()&&!createPrivateDirectory()))return {};
        const auto staged=QDir(directory).filePath(name);
        if(!QDir(directory).mkdir(name))return {};
        stagedDirectories.append(staged);
        for(const auto &relative:files){
            if(relative.isEmpty()||QDir::isAbsolutePath(relative)||QDir::cleanPath(relative)!=relative
                ||relative.contains(':')||relative.contains('\\')||relative.split('/').contains(".."))return {};
            const auto from=source.filePath(relative);
            const auto to=QDir(staged).filePath(relative);
            if(!QFileInfo(from).isFile()||!QDir().mkpath(QFileInfo(to).absolutePath())||!QFile::copy(from,to))
                return {};
        }
        return staged;
    }

    bool prepare(const QString &applicationRoot,const QString &runtimeRoot,bool forceStaging) {
        const QDir app(applicationRoot), model(runtimeRoot);
        const auto worker=QStringLiteral("DctsInferenceWorker.exe");
        const auto manifest=QStringLiteral("runtime.json");
        QString appPath,modelPath;
        if(!forceStaging){
            appPath=readableAsciiDirectory(applicationRoot,"app",worker);
            modelPath=readableAsciiDirectory(runtimeRoot,"runtime",manifest);
        }
        if(appPath.isEmpty()){
            auto files=app.entryList({"*.dll"},QDir::Files|QDir::Readable);
            files.prepend(worker);
            appPath=stageFiles(app,"app-files",files);
            stagedWorker=!appPath.isEmpty();
        }
        if(modelPath.isEmpty()){
            QFile source(model.filePath(manifest));
            if(!source.open(QIODevice::ReadOnly)||source.size()>262144)return false;
            const auto runtimeInfo=QJsonDocument::fromJson(source.readAll()).object();
            const auto files=runtimeInfo["files"].toObject();
            if(files.isEmpty())return false;
            modelPath=stageFiles(model,"runtime-files",QStringList{manifest}+files.keys());
            stagedRuntime=!modelPath.isEmpty();
        }
        if(appPath.isEmpty()||modelPath.isEmpty())return false;
        program=QDir(appPath).filePath(worker);
        runtime=modelPath;
        return true;
    }
#endif
};

std::shared_ptr<void> RvAssistDepth::prepareProbeWorker(QString &program,QString &runtime){
    auto paths=std::make_shared<RvAssistWorkerPaths>();
#ifdef DCTS_TENSORRT_RTX
    if(!paths->prepare(RuntimeDiscovery::runtimeDirectory(),RuntimeDiscovery::rvAssistRoot(),false))return {};
#else
    paths->program=RuntimeDiscovery::inferenceWorker();paths->runtime=RuntimeDiscovery::rvAssistRoot();
#endif
    program=paths->program;runtime=paths->runtime;return paths;
}

namespace {
// Three bounded native-ID lists can exceed 16 KiB at dense valid returns.
constexpr int HeaderLimit=65536, PacketLimit=HeaderLimit+2*640*480*4+2*RvDepthDiagnostic::ReasonBytes+12;
#ifdef DCTS_TENSORRT_RTX
constexpr auto HostRuntimeBackend="onnxruntime-tensorrt-rtx";
constexpr auto HostOrtVersion="1.31.1";
constexpr auto HostProviderFile="bin/onnxruntime_providers_nv_tensorrt_rtx.dll";
constexpr auto TensorRtProviderName="NvTensorRTRTXExecutionProvider";
constexpr auto HostResultBackend=HostRuntimeBackend;
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

#else
constexpr auto HostRuntimeBackend="onnxruntime-cuda";
constexpr auto HostResultBackend="onnxruntime-cuda-1.23.2";
constexpr auto HostOrtVersion="1.23.2";
constexpr auto HostProviderFile="bin/onnxruntime_providers_cuda.dll";
constexpr int HostWorkerLoadTimeoutMs=30000;
#endif

QString modelRoot(const QString &model){
    return RuntimeDiscovery::rvAssistModelRoot(model);
}
bool hdSource(const QJsonObject &m,const RvCameraView::Profile &view,const QString &hostModel) {
    const auto integer=[&](const char *key){const double n=m[key].toDouble(-1);return m[key].isDouble()&&n>0&&n<=9007199254740991.0&&std::floor(n)==n;};
    if(m["source_protocol"]!="HDA1"||m["source_transport"]!="rtsp_h265"
        ||m["model"]!="Native"||m["source_rgb_width"]!=2304||m["source_rgb_height"]!=1296
        ||m["rgb_width"]!=view.inputWidth||m["rgb_height"]!=view.inputHeight||!m["dtof_present"].isBool()
        ||m["source_candidate_mode"]!=RvCameraView::captureMode(hostModel)
        ||m["rectify_map_sha256"]!=QLatin1String(view.mapSha)
        ||(QLatin1String(view.name)=="wide"&&m["assist_camera_profile"]!="wide")
        ||!integer("frame_id")||!integer("generation")||!integer("host_decoded_monotonic_ns")
        ||!QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(m["boot_id"].toString()).hasMatch())return false;
    if(m["dtof_present"].toBool()) {
        const double delta=m["pair_delta_ms"].toDouble(1000),bound=m["pair_clock_uncertainty_ms"].toDouble(-1);
        if(m["work_mode"]!="rgb_tof"||!integer("tof_sequence")||!integer("tof_timestamp_ns")||!integer("rgb_timestamp_ns")
            ||!m["tof_reused"].isBool()
            ||!std::isfinite(delta)||!std::isfinite(bound)
            ||bound<0||bound>RvTofFrame::PairClockUncertaintyMs||std::abs(delta)+bound>RvTofFrame::FreshRgbPairBudgetMs
            ||std::abs((m["tof_timestamp_ns"].toDouble()-m["rgb_timestamp_ns"].toDouble())/1e6-delta)>.001
            ||m["pair_clock_source"]!="rtcp_prft_board_realtime_monotonic"
            ||m["source_engine"]!="native-hd-capture-v1"||m["generation_source"]!="host_presentation_epoch"
            ||!QRegularExpression("^[0-9a-f]{32}$").match(m["source_generation"].toString()).hasMatch())return false;
    }else {
        if(m["work_mode"]!="host_rgb")return false;
        for(const auto *key:{"tof_sequence","tof_timestamp_ns","pair_delta_ms","tof_reused","rgb_timestamp_ns"})if(m.contains(key))return false;
    }
    return true;
}
void dropHdTof(QJsonObject &m) {
    m["dtof_present"]=false;m["work_mode"]="host_rgb";
    m["pairing_policy"]="no_current_native_measurement";m["dtof_absent_reason"]="native_measurement_already_dispatched";
    for(const auto *key:{"tof_sequence","tof_timestamp_ns","pair_delta_ms","tof_reused","rgb_timestamp_ns",
            "dtof_width","dtof_height","dtof_bytes","dtof_format","dtof_range_unit","dtof_source","dtof_coordinate",
            "dtof_fov_correction","tof_fov_correction","tof_warning_flags","pair_clock_uncertainty_ms","pair_clock_source",
            RvAssistFusion::NativeProjectionField})m.remove(key);
}
qint64 monotonicMs() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
QJsonObject readJson(const QString &path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>65536)throw std::runtime_error("Native auxiliary runtime manifest missing");
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())throw std::runtime_error("Invalid native auxiliary runtime manifest");
    return doc.object();
}
QByteArray packet(const QJsonObject &json,const QByteArray &body={},const QByteArray &aux={}) {
    const auto header=QJsonDocument(json).toJson(QJsonDocument::Compact);
    const auto headerEnd=12+header.size();
    const auto bodyEnd=headerEnd+body.size();
    QByteArray out(bodyEnd+aux.size(),Qt::Uninitialized);
    auto *data=out.data();
    qToLittleEndian<quint32>(quint32(header.size()),data);
    qToLittleEndian<quint32>(quint32(body.size()),data+4);
    qToLittleEndian<quint32>(quint32(aux.size()),data+8);
    std::memcpy(data+12,header.constData(),size_t(header.size()));
    std::memcpy(data+headerEnd,body.constData(),size_t(body.size()));
    std::memcpy(data+bodyEnd,aux.constData(),size_t(aux.size()));
    return out;
}
QByteArray planarRgb(const QImage &source) {
    const auto image=source.convertToFormat(QImage::Format_RGB888);
    const int width=image.width(),height=image.height(),pixels=width*height;
    QByteArray out(3*pixels,Qt::Uninitialized);
    auto *destination=out.data();
    for(int y=0;y<height;++y){
        const auto *row=image.constScanLine(y);
        auto *red=destination+y*width;
        auto *green=red+pixels;
        auto *blue=green+pixels;
        for(int x=0;x<width;++x){
            red[x]=char(row[3*x]);
            green[x]=char(row[3*x+1]);
            blue[x]=char(row[3*x+2]);
        }
    }
    return out;
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
    QJsonObject modelInfo,storageInfo;
    QString backend=qEnvironmentVariable("RV_INFERENCE_BACKEND","tensorrt");
    QJsonObject executionIdentity() const {return RvInferenceRuntime::identity(backend);}
    bool wide=false;
    QString hostModel;
    const RvCameraView::Profile &view() const {return RvCameraView::profile(hostModel);}
    RvAssistFusion::Calibration calibration;
    NativeModel()=default;
    NativeModel(const NativeModel &)=delete;
    ~NativeModel() {
        if(api) {
            if(session)api->ReleaseSession(session);
            if(memory)api->ReleaseMemoryInfo(memory);
            if(env)api->ReleaseEnv(env);
        }
        protectedLibrary.unload();library.unload();
        if(directory)RemoveDllDirectory(directory);
    }
    void initialize(const QString &root,const QString &selectedHostModel=QString()) {
        if(!RvInferenceRuntime::validBackend(backend))throw std::runtime_error("Unsupported inference backend");
        if(!RvAssistDepth::supportsHostModel(selectedHostModel))
            throw std::runtime_error("Unsupported host model");
        QElapsedTimer initializationClock;initializationClock.start();
#ifdef DCTS_TENSORRT_RTX
        if(backend=="tensorrt")RvInferenceRuntime::checkTensorRtCudaDevice();
#endif
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
                ||i.value().toString().size()!=64||RvInferenceRuntime::fileSha256(dir.filePath(relative))!=i.value().toString().toLatin1())
                throw std::runtime_error("Native auxiliary runtime fingerprint mismatch");
        }
        modelInfo=readJson(dir.filePath("model.json"));
        wide=modelInfo.value("camera_profile")=="wide";
        if(modelInfo["model"]!="UniDepthV2-Small"||!wide||modelInfo["network_profile"]!="rgb672_vga_crop_v1"
            ||!RvCameraView::isUniDepthVga(selectedHostModel))
            throw std::runtime_error("RGB672 VGA model and camera calibration binding mismatch");
        hostModel=selectedHostModel;
        if(protectedModel) {
            if(!RvAssistDepth::supportsHostModel(hostModel)||protection["plaintext_sha256"]!=QLatin1String(view().graphSha))
                throw std::runtime_error("Protected model identity mismatch");
        } else if(!files.contains(QLatin1String(view().graphFile)))throw std::runtime_error("Incomplete native auxiliary runtime closure");
        if(RvCameraView::isUniDepth(hostModel)) {
            if((modelInfo.contains("camera_profile")&&modelInfo["camera_profile"]!=(wide?"wide":"standard"))
                ||modelInfo["weights_sha256"]!=QLatin1String(view().weightsSha)||modelInfo["onnx_sha256"]!=QLatin1String(view().graphSha)
                ||(!protectedModel&&RvInferenceRuntime::fileSha256(dir.filePath(QLatin1String(view().graphFile)))!=view().graphSha)
                ||modelInfo["rectify_sha256"]!=QLatin1String(view().mapSha)
                ||modelInfo["physical_intrinsics_sha256"]!=QLatin1String(RvCalibration::IntrinsicsSha)
                ||modelInfo["rectified_intrinsics_integer_centres"].toArray()!=RvCameraView::intrinsics(view())
                ||(view().resolutionLevel<0?!modelInfo["resolution_level"].isNull():modelInfo["resolution_level"].toInt(-1)!=view().resolutionLevel)
                ||modelInfo["backend"]!=(RvCameraView::isUniDepthVga(hostModel)?HostRuntimeBackend:"onnxruntime-cuda")
                ||modelInfo["input"].toArray()!=QJsonArray{1,3,view().inputHeight,view().inputWidth}
                ||modelInfo["output"].toArray()!=QJsonArray{1,1,view().outputHeight,view().outputWidth}
                ||modelInfo["input_dtype"]!="uint8"||modelInfo["output_dtype"]!="float32"
                ||modelInfo["network_shape"].toArray()!=QJsonArray{view().networkHeight,view().networkWidth}||modelInfo["depth_unit"]!="camera-Z_m_float32")
                throw std::runtime_error("UniDepth model and RV camera calibration binding mismatch");
            if(view().networkProfile&&(modelInfo["network_profile"]!=QLatin1String(view().networkProfile)
                ||modelInfo["rgb_padding_ltrb"].toArray()!=(RvCameraView::isUniDepthVga(hostModel)?QJsonArray{0,0,0,0}:QJsonArray{6,6,6,6})
                ||modelInfo["output_crop_ltrb"].toArray()!=(RvCameraView::isUniDepthVga(hostModel)?QJsonArray{16,12,16,12}:QJsonArray{6,6,6,6})))
                throw std::runtime_error("UniDepth padding and output grid binding mismatch");
            if(view().provisional&&(manifest["validation_status"]!="provisional-numerical-candidate"
                ||modelInfo["provisional"]!=true||modelInfo["candidate_parity_mean_limit"]!=view().parityMeanLimit
                ||modelInfo["candidate_parity_max_limit"]!=view().parityMaxLimit))
                throw std::runtime_error("UniDepth temporary candidate contract mismatch");
            if(modelInfo["precision"]!=QLatin1String(view().precision)||modelInfo["conversion"]!=QLatin1String(view().conversion)
                ||modelInfo["source_onnx_sha256"]!=QLatin1String(view().sourceSha))
                throw std::runtime_error("Unsupported UniDepth precision contract");
        }
        QFile geometry(RvCalibration::GeometryResource);QString error;
        if(!geometry.open(QIODevice::ReadOnly)||!RvAssistFusion::loadCalibration(geometry.readAll(),calibration,error))
            throw std::runtime_error(("Missing or incompatible dToF calibration: "+error).toStdString());
        if(RvInferenceRuntime::isOpenVino(backend)){
            openvino=std::make_unique<RvOpenVinoModel>();
            storageInfo=openvino->initialize(backend,root,manifest);
            return;
        }
        const bool compatibility=backend!="tensorrt";
        if(compatibility)RvInferenceRuntime::verifyCompatibleRuntime();
        const auto libraryRoot=compatibility?RuntimeDiscovery::rvCompatibleRoot()+"/directml":dir.filePath("bin");
        // Restricted DLL search in this disposable native process only.
        if(!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))throw std::runtime_error("Cannot restrict native DLL search");
        directory=AddDllDirectory(reinterpret_cast<const wchar_t *>(libraryRoot.utf16()));
        if(!directory)throw std::runtime_error("Cannot register native auxiliary DLL directory");
#ifdef DCTS_INSTALLER_BUILD
        // RTX v0.5 falls back to memory when its engine staging directory is unavailable.
        // The ciphertext itself is a regular file, so the path below cannot contain temporary files.
        const auto deniedTemp=QDir::toNativeSeparators(dir.filePath("model.dctsm/no-temp"));
        const auto deniedTempPath=reinterpret_cast<const wchar_t *>(deniedTemp.utf16());
        if(!SetEnvironmentVariableW(L"TEMP",deniedTempPath)
           ||!SetEnvironmentVariableW(L"TMP",deniedTempPath)
           ||!SetEnvironmentVariableW(L"CUDA_CACHE_DISABLE",L"1"))
            throw std::runtime_error("Cannot set memory-only engine staging policy");
#endif
        library.setFileName(libraryRoot+"/onnxruntime.dll");
        if(!library.load())throw std::runtime_error(library.errorString().toStdString());
        using GetApi=const OrtApiBase *(ORT_API_CALL *)();
        const auto getApi=reinterpret_cast<GetApi>(library.resolve("OrtGetApiBase"));
        const auto version=compatibility?QStringLiteral("1.24.4"):QString::fromLatin1(HostOrtVersion);
        if(!getApi||QString::fromLatin1(getApi()->GetVersionString())!=version)throw std::runtime_error("ONNX Runtime version mismatch");
        api=getApi()->GetApi(ORT_API_VERSION);if(!api)throw std::runtime_error("ONNX Runtime API unavailable");
        check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"rv-assist",&env));
        OrtSessionOptions *options=nullptr;check(api->CreateSessionOptions(&options));
        const auto release=qScopeGuard([&]{api->ReleaseSessionOptions(options);});
        check(api->SetIntraOpNumThreads(options,compatibility?std::min(8u,std::max(1u,std::thread::hardware_concurrency()/2)):4));
        check(api->SetSessionGraphOptimizationLevel(options,ORT_ENABLE_ALL));
        if(backend.startsWith("directml:")){
            using AppendDml=OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *,int);
            const auto append=reinterpret_cast<AppendDml>(library.resolve("OrtSessionOptionsAppendExecutionProvider_DML"));
            if(!append)throw std::runtime_error("DirectML provider API missing");
            check(api->DisableMemPattern(options));check(api->SetSessionExecutionMode(options,ORT_SEQUENTIAL));
            check(append(options,backend.mid(9).toInt()));
            check(api->AddSessionConfigEntry(options,"session.disable_cpu_ep_fallback","1"));
        }
#ifdef DCTS_TENSORRT_RTX
        if(!compatibility){
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
        }
#else
        OrtCUDAProviderOptions cuda;cuda.device_id=0;cuda.cudnn_conv_algo_search=OrtCudnnConvAlgoSearchHeuristic;
        check(api->SessionOptionsAppendExecutionProvider_CUDA(options,&cuda));
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
#ifdef DCTS_TENSORRT_RTX
        if(!compatibility){
            // TensorRT may build its execution graph on the first Run.
            // Finish that work before the first RGB frame's deadline starts.
            QElapsedTimer warmup;warmup.start();
            infer(QByteArray(view().rgbBytes(),char(127)));
            storageInfo["warmup_ms"]=warmup.nsecsElapsed()/1e6;
        }
        storageInfo["provider"]=RvInferenceRuntime::provider(backend);
        storageInfo["ort_version"]=version;
#endif
        storageInfo["device"]=backend;
        storageInfo["initialization_ms"]=initializationClock.nsecsElapsed()/1e6;
    }
    std::vector<float> infer(const QByteArray &rgb) {
        if(openvino)return openvino->infer(rgb);
        const auto &geometry=view();
        if(rgb.size()!=geometry.rgbBytes())throw std::runtime_error("Invalid auxiliary RGB input");
        OrtValue *input=nullptr,*output=nullptr;
        const auto release=qScopeGuard([&]{if(input)api->ReleaseValue(input);if(output)api->ReleaseValue(output);});
        const int64_t dims[]={1,3,geometry.inputHeight,geometry.inputWidth};
        check(api->CreateTensorWithDataAsOrtValue(memory,const_cast<char *>(rgb.constData()),rgb.size(),dims,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8,&input));
        const char *inputs[]={"rgb"},*outputs[]={"depth"};const OrtValue *values[]={input};
        check(api->Run(session,nullptr,inputs,values,1,outputs,1,&output));
        OrtTensorTypeAndShapeInfo *shape=nullptr;check(api->GetTensorTypeAndShape(output,&shape));
        const auto releaseShape=qScopeGuard([&]{api->ReleaseTensorTypeAndShapeInfo(shape);});
        size_t rank=0;check(api->GetDimensionsCount(shape,&rank));
        int64_t actual[4]{};ONNXTensorElementDataType type;
        if(rank==4)check(api->GetDimensions(shape,actual,4));
        check(api->GetTensorElementType(shape,&type));
        if(rank!=4||actual[0]!=1||actual[1]!=1||actual[2]!=geometry.outputHeight||actual[3]!=geometry.outputWidth
            ||type!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)throw std::runtime_error("Invalid auxiliary depth output shape");
        void *data=nullptr;check(api->GetTensorMutableData(output,&data));
        const auto *z=static_cast<float *>(data);std::vector<float> out(z,z+geometry.depthBytes()/4);
        for(float v:out)if(!std::isfinite(v)||v<=0)throw std::runtime_error("Invalid model output values");
        return out;
    }

private:
    std::unique_ptr<RvOpenVinoModel> openvino;
    void check(OrtStatus *status) {if(status) {const std::string reason=api->GetErrorMessage(status);api->ReleaseStatus(status);throw std::runtime_error(reason);}}
    QLibrary library,protectedLibrary;
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
        if(!h||h>HeaderLimit||b!=672*504*3||(a!=0&&a!=1024&&a!=6144)||!readExact(stdin,header,h)||!readExact(stdin,rgb,b)||!readExact(stdin,tof,a))return AssistRead::Invalid;
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(header,&parse);
        if(parse.error!=QJsonParseError::NoError||!document.isObject())return AssistRead::Invalid;
        request=document.object();return AssistRead::Request;
    };
}
}

RvAssistDepth::RvAssistDepth(QObject *parent,RvInferenceSettings *inference):QObject(parent),m_root(RuntimeDiscovery::rvAssistRoot()),
    m_hostModel(QLatin1String(RvCameraView::UniDepthVgaNative48)),
    m_inference(inference?inference:RvInferenceSettings::instance()) {
    auto *settings=m_inference;
    m_backend=settings->selected();
    connect(settings,&RvInferenceSettings::selectionChanged,this,[this,settings]{
        const auto selected=settings->selected();
        if(selected==m_backend)return;
        m_backend=selected;m_failed=false;m_resultAgeLimitMs=1000;m_inferenceTimeoutMs=1500;
        cancel(tr("Inference device changed; waiting for a new paired frame"));
    });
    connect(settings,&RvInferenceSettings::benchmarkingChanged,this,[this](bool active){
        if(!m_enabled)return;
        if(active)cancel(tr("Host depth inference is paused during benchmarking."));
        else m_failed=false;
    });
    m_timeout.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,&RvAssistDepth::handleTimeout);
    connect(&m_process,&QProcess::readyReadStandardOutput,this,&RvAssistDepth::readOutput);
    connect(&m_process,&QProcess::readyReadStandardError,this,[this]{m_stderr+=m_process.readAllStandardError();m_stderr=m_stderr.right(2048);});
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart)m_workerPaths.reset();
        if(!m_stopping&&!m_destroying)fail(m_process.errorString());
    });
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){
        m_workerPaths.reset();
        if(m_destroying)return;
        if(!m_stopping&&m_process.bytesAvailable()){
            // Prefer the worker's fatal receipt to a generic process-exit
            // error, especially for integrity failures.
            const auto backend=m_backend;
            const QPointer<RvAssistDepth> alive(this);
            readOutput();
            if(!alive||m_failed||m_backend!=backend)return;
        }
        const bool stopping=m_stopping;m_stopping=false;m_ready=false;m_busy=false;m_timeout.stop();
        if(!stopping) {fail(tr("Host assisted depth process exited (%1): %2").arg(code).arg(QString::fromUtf8(m_stderr).right(600)));return;}
        if(m_enabled&&!m_failed)startWorker();
    });
}
RvAssistDepth::~RvAssistDepth() {
    m_destroying=true;m_process.disconnect(this);m_timeout.stop();m_timeout.disconnect(this);
    if(m_process.state()!=QProcess::NotRunning) {

        m_process.kill();m_process.waitForFinished(2000);
    }
}

bool RvAssistDepth::supportsHostModel(const QString &model) {
#if defined(DCTS_TENSORRT_RTX)
    if(RvCameraView::isUniDepthVga(model))return true;
#endif
    return false;
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
    for(auto i=files.begin();i!=files.end();++i)if(!required.contains(i.key()))required.append(i.key());
    for(const auto &relative:required) {
        if(!files.contains(relative)||QDir::isAbsolutePath(relative)||relative.contains("..")||relative.contains('\\'))return false;
        const QFileInfo file(dir.filePath(relative));
        if(!file.isFile()||!file.isReadable()||file.size()<=0)return false;
    }
    return true;
}
void RvAssistDepth::setInputRotationDegrees(int degrees) {
    // The frozen RGB672 VGA graph supports the centred 0/180-degree camera only.
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

    if(m_process.state()!=QProcess::NotRunning) {
        m_stopping=true;
        m_process.kill();
    }
    const QPointer<RvAssistDepth> alive(this);
    if(!m_destroying)emit cleared(reason);
    if(alive)m_cancelling=false;
}
void RvAssistDepth::fail(const QString &reason, bool tryNextBackend)
{
    if(m_failed)return;
    const bool integrity=reason.contains("fingerprint",Qt::CaseInsensitive)||reason.contains("protected",Qt::CaseInsensitive)
        ||reason.contains("AES-GCM",Qt::CaseInsensitive)||reason.contains("identity",Qt::CaseInsensitive)
        ||reason.contains("Cannot hash",Qt::CaseInsensitive);
    if(tryNextBackend&&!integrity&&m_inference->fallback(m_backend,reason))return;
    m_failed=true;
    const QPointer<RvAssistDepth> alive(this);
    cancel(reason);
    if(alive)emit inferenceUnavailable(failureExplanation(reason),reason);
}

QString RvAssistDepth::preflightError(const QString &model,const RvInferenceSettings *inference)
{
#ifdef DCTS_TENSORRT_RTX
    const auto *settings=inference?inference:RvInferenceSettings::instance();
    if(settings->started()&&settings->selected().isEmpty())
        return settings->detecting()?tr("Detecting available inference devices..."):tr("No usable inference device was found. Open Inference Device in Settings to retry.");
    if(!settings->started()&&settings->selected()=="tensorrt"&&!hasNvidiaAdapter())return tr("No NVIDIA GPU was detected. Open Inference Device in Settings to select a CPU or compatible GPU.");
#ifdef DCTS_INSTALLER_BUILD
    if(!hostModelAvailable(model))return tr("The required inference runtime or encrypted model files are missing or invalid.");
#else
    if(!hostModelAvailable(model))return tr("The required host inference files are missing or invalid.");
#endif
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
QByteArray RvAssistDepth::preparePlanarInput(const QImage &image) {
    return planarRgb(image);
}
void RvAssistDepth::submitHd(const QImage &camera,const QJsonObject &source,quint64 epoch,
                             const QByteArray &nativeSamples,const QByteArray &planarInput) {
    if(m_destroying||!m_enabled||m_failed)return;
    const bool paired=source["dtof_present"].toBool();
    auto selected=source;
    selected["assist_host_model"]=m_hostModel;
    const auto &view=RvCameraView::profile(m_hostModel);
    if(camera.size()!=view.inputSize()||(!planarInput.isEmpty()&&planarInput.size()!=view.rgbBytes())
        ||!RvTofFrame::isHostFusionExperiment(source["assist_experiment"])||!hdSource(source,view,m_hostModel)
        ||(paired&&(source["tof_warning_flags"]!=0||source["tof_reused"]!=false))
        ||(paired?!RvTofFrame{nativeSamples,selected}.isValidForHostFusion():!nativeSamples.isEmpty())) {
        cancel(tr("Invalid managed H.265 model input"));return;
    }
    if(paired&&!RvCameraView::supportsNativeGrid(m_hostModel,source["dtof_width"].toInt(),source["dtof_height"].toInt())){
        cancel(tr("Invalid managed H.265 model input"));return;
    }
    if(!m_pending.camera.isNull())++m_dropped;
    // Empty auxiliary bytes mean RGB only. No all-zero synthetic ToF frame.
    m_pending={camera,{nativeSamples,selected},epoch,++m_request,qint64(source["host_decoded_monotonic_ns"].toDouble()/1000000.)};
    m_pending.planarInput=planarInput;
    m_pending.tof.metadata["assist_input_rotation_degrees"]=m_inputRotation;
    m_pending.tof.metadata["assist_diagnostic_capture"]=true;
    m_pending.tof.metadata.insert(RvAssistFusion::NativeProjectionField,RvAssistFusion::defaultNativeProjection(m_pending.tof));
    if(m_process.state()==QProcess::NotRunning)startWorker();
    else if(m_ready&&!m_busy)sendLatest();
}

void RvAssistDepth::startWorker(bool forceStaging) {
    if(m_destroying||!m_enabled||m_failed||m_pending.camera.isNull()||m_process.state()!=QProcess::NotRunning)return;
    const auto *settings=m_inference;
    if(settings->benchmarking())return;
    if(m_backend.isEmpty()){
        if(!settings->detecting())fail(tr("No usable inference device was found. Open Inference Device in Settings to retry."),false);
        return;
    }
    m_root=modelRoot(m_hostModel);
    if(!hostModelAvailable(m_hostModel)) {fail(tr("Selected host model runtime is missing; prepare the optional runtime first"),false);return;}
    m_ready=false;m_busy=false;m_stopping=false;m_buffer.clear();m_stderr.clear();m_token=QUuid::createUuid().toString(QUuid::WithoutBraces);

    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("RV_INFERENCE_BACKEND",m_backend);
    environment.insert("RV_ASSIST_COMPAT_ROOT",RuntimeDiscovery::rvCompatibleRoot());
    for(const auto &key:environment.keys())if(key.startsWith("PYTHON")||key.startsWith("CONDA"))environment.remove(key);
    m_workerPaths.reset();
    auto paths=std::make_unique<RvAssistWorkerPaths>();
#ifdef DCTS_TENSORRT_RTX
    const auto appDirectory=RuntimeDiscovery::runtimeDirectory();
    if(!RvAssistWorkerPaths::readableFile(QDir(appDirectory).filePath("DctsInferenceWorker.exe"))) {
        fail(tr("The TensorRT-RTX worker executable is missing or unreadable in the installation folder."),false);
        return;
    }
    if(!paths->prepare(appDirectory,m_root,forceStaging)) {
        fail(tr("Could not prepare a readable TensorRT-RTX worker and model path."),false);
        return;
    }
#else
    Q_UNUSED(forceStaging);
    paths->program=RuntimeDiscovery::inferenceWorker();
    paths->runtime=m_root;
#endif
    m_process.setProcessEnvironment(environment);
    m_process.setWorkingDirectory(paths->runtime);
    m_process.setProgram(paths->program);
    m_process.setArguments({"--rv-assist-worker",paths->runtime,m_token});
    m_workerPaths=std::move(paths);
    if(RvCameraView::isUniDepthVga(m_hostModel))m_process.setArguments(m_process.arguments()<<m_hostModel);
    m_process.start();m_timeout.start(m_backend=="tensorrt"?HostWorkerLoadTimeoutMs:180000);
    emit statusChanged(tr("Loading the host-assisted inference model..."));
    emit loadingStateChanged(1);
}
bool RvAssistDepth::takePending() {
    if(m_pending.camera.isNull())return false;
    m_current=std::move(m_pending);m_pending={};
    if(monotonicMs()-m_current.receivedMs>1000) {m_current={};emit cleared(tr("Paired frame expired before host inference"));return false;}
    if(!m_current.tof.samples.isEmpty()){
        const auto sequence=quint64(m_current.tof.metadata["tof_sequence"].toInteger());
        if(sequence>m_lastHdTofSequence){
            m_lastHdTofSequence=sequence;
        }else if(sequence==m_lastHdTofSequence){
            m_current.tof.metadata["tof_reused"]=true;
        }else{
            m_current.tof.samples.clear();
            dropHdTof(m_current.tof.metadata);
        }
    }
    return true;
}
void RvAssistDepth::sendLatest() {
    if(!m_enabled||m_failed||!m_ready||m_busy||!takePending())return;
    const QJsonObject request={{"protocol",1},{"type","infer"},{"token",m_token},{"request",double(m_current.request)},
        {"epoch",double(m_current.epoch)},{"metadata",m_current.tof.metadata},{"host_model",m_hostModel}};

    QElapsedTimer serialize;serialize.start();
    const auto planar=m_current.planarInput.isEmpty()?preparePlanarInput(m_current.camera):m_current.planarInput;
    const auto bytes=packet(request,planar,m_current.tof.samples);
    m_current.serializeMs=serialize.nsecsElapsed()/1e6;
    m_busy=true;
    if(m_process.write(bytes)!=bytes.size()) {fail(tr("Cannot send paired frame to host depth process"));return;}
    m_timeout.start(m_inferenceTimeoutMs);
}
void RvAssistDepth::handleTimeout() {

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
    const auto expectedDepth=RvCameraView::outputProfile(m_hostModel).depthBytes();
    while(m_buffer.size()>=12) {
        QElapsedTimer responseClock;responseClock.start();
        const auto size=qFromLittleEndian<quint32>(m_buffer.constData()),body=qFromLittleEndian<quint32>(m_buffer.constData()+4),aux=qFromLittleEndian<quint32>(m_buffer.constData()+8);
        if(!size||size>HeaderLimit||(body!=0&&body!=expectedDepth)
            ||(aux!=0&&(!isUniDepth(m_hostModel)||aux!=expectedDepth+2*RvDepthDiagnostic::ReasonBytes))) {fail(tr("Invalid host depth packet size"));return;}
        const int total=12+int(size+body+aux);if(m_buffer.size()<total)return;
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(m_buffer.mid(12,size),&parse);
        const auto depth=m_buffer.mid(12+size,body),coverage=m_buffer.mid(12+size+body,aux);
        m_buffer.remove(0,total);const auto response=document.object();
        if(parse.error!=QJsonParseError::NoError||!document.isObject()) {fail(tr("Host depth protocol identity mismatch"));return;}
        if(!acceptResponse(response,depth,coverage,responseClock))return;
    }
}

bool RvAssistDepth::acceptResponse(const QJsonObject &response,const QByteArray &depth,const QByteArray &coverage,QElapsedTimer &responseClock) {
    const QPointer<RvAssistDepth> alive(this);
    const auto body=depth.size(),aux=coverage.size();
    if(m_destroying||m_stopping||!m_enabled)return false;
    const auto &view=RvCameraView::profile(m_hostModel);
    const auto expectedDepth=view.depthBytes();
    if(!supportsHostModel(m_hostModel)||(body!=0&&body!=expectedDepth)||(aux!=0&&(!isUniDepth(m_hostModel)||aux!=expectedDepth+2*RvDepthDiagnostic::ReasonBytes)))
        {fail(tr("Invalid host depth packet size"));return false;}
    if(response["protocol"].toInt()!=1||response["token"]!=m_token) {fail(tr("Host depth protocol identity mismatch"));return false;}
    if(response["type"]=="fatal") {fail(response["error"].toString());return false;}
    const auto expectedIdentity=RvInferenceRuntime::identity(m_backend);
    const bool identityMatches=supportsHostModel(m_hostModel)
        &&response["weights_sha256"]==QLatin1String(view.weightsSha)&&response["graph_sha256"]==expectedIdentity["graph_sha256"]
        &&response["precision"]==expectedIdentity["precision"]&&response["source_graph_sha256"]==expectedIdentity["source_graph_sha256"]&&response["conversion"]==expectedIdentity["conversion"]
        &&(m_hostModel=="unidepth"||response["host_model"]==m_hostModel);
    if(response["type"]=="ready"&&!m_ready&&!m_busy&&body==0&&aux==0&&identityMatches) {
#ifdef DCTS_TENSORRT_RTX
        const auto loading=response["model_loading"].toObject();
        if(loading["provider"]!=RvInferenceRuntime::provider(m_backend)||loading["device"]!=m_backend
            ||(m_backend=="tensorrt"&&loading["ort_version"]!=HostOrtVersion)
#ifdef DCTS_INSTALLER_BUILD
            ||loading["plaintext_sha256_verified"]!=true||loading["plaintext_file_used"]!=false
#endif
        ) {
            fail(tr("Host inference worker identity mismatch"));return false;
        }
#endif
        // Processing latency does not relax RGB/ToF capture-time pairing.
        // Keep the original 1 s freshness rule for RTX. Compatibility workers
        // get a bounded budget from an optional benchmark or the default estimate.
        if(m_backend!="tensorrt"){
            const auto measured=m_inference->latencyMs(m_backend);
            m_inferenceTimeoutMs=std::clamp(int(measured*6+2000),10000,120000);
            m_resultAgeLimitMs=std::clamp(int(measured*4+1000),5000,120000);
        }
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
    if(body==expectedDepth&&isUniDepth(m_hostModel)){
        const bool wanted=current.tof.metadata["assist_diagnostic_capture"]==true;
        if(metadata.value("assist_diagnostic_capture")!=current.tof.metadata.value("assist_diagnostic_capture")
           ||(wanted?(metadata["assist_diagnostic_payload"]!="mono_reasons_v1"||!RvDepthDiagnostic::validPayload(coverage,expectedDepth)):aux!=0)){
            fail(tr("Invalid host depth diagnostic payload"));return false;
        }
        if(RvCameraView::isUniDepthVga(m_hostModel)){
            const int nativeCount=RvCameraView::nativeCount(m_hostModel);
            if(metadata["assist_host_model"]!=m_hostModel||(wanted&&metadata["assist_diagnostic_native_count"]!=nativeCount)){
                fail(tr("Host depth result does not match the selected model"));return false;
            }
            for(const auto *key:{"dtof_width","dtof_height","dtof_bytes","dtof_fov_correction","tof_fov_correction"})
                if(metadata.value(key)!=current.tof.metadata.value(key)){fail(tr("Host depth result belongs to another native grid"));return false;}
            if(wanted)for(int i=nativeCount;i<RvDepthDiagnostic::ReasonBytes;++i)
                if(coverage[expectedDepth+i]!=0||coverage[expectedDepth+RvDepthDiagnostic::ReasonBytes+i]!=0){
                    fail(tr("Invalid host depth diagnostic payload"));return false;
                }
        }
    }
    if(current.tof.metadata["source_protocol"]=="HDA1") {
        if(!hdSource(current.tof.metadata,view,m_hostModel)
           ||(!current.tof.samples.isEmpty()&&!current.tof.isValidForHostFusion())){fail(tr("Invalid managed H.265 model input"));return false;}
        for(const auto *key:{"source_protocol","boot_id","source_generation","host_decoded_monotonic_ns","dtof_present",
            "rgb_timestamp_ns","tof_sequence","tof_timestamp_ns","pair_delta_ms","pair_clock_uncertainty_ms","pair_clock_source","tof_reused"})
            if(metadata.value(key)!=current.tof.metadata.value(key)){fail(tr("Host depth result belongs to another H.265 frame"));return false;}
    }
    if(body&&supportsHostModel(m_hostModel)&&(metadata["assist_graph_sha256"]!=expectedIdentity["graph_sha256"]||metadata["assist_precision"]!=expectedIdentity["precision"]
        ||metadata["assist_source_graph_sha256"]!=expectedIdentity["source_graph_sha256"]||metadata["assist_conversion"]!=expectedIdentity["conversion"]
        ||metadata["assist_execution_device"]!=m_backend||metadata["assist_backend"]!=RvInferenceRuntime::backendName(m_backend)
        ||(RvCameraView::isWide(m_hostModel)&&(metadata["assist_camera_profile"]!="wide"||metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha)
            ||metadata["depth_K"].toArray()!=RvCameraView::intrinsics(RvCameraView::outputProfile(m_hostModel))
            ||!RvCameraView::validOutputGrid(metadata)))))
        {fail(tr("Host depth result does not match the selected model"));return false;}
    if(body&&view.provisional&&(metadata["assist_provisional_model"]!=true
        ||metadata["assist_candidate_parity_mean_limit"]!=view.parityMeanLimit))
        {fail(tr("Host depth result does not match the selected model"));return false;}
    if(isUniDepth(m_hostModel)&&current.tof.metadata.contains("assist_output_policy")){
        const auto policy=current.tof.metadata.value("assist_output_policy");
        const auto fov=current.tof.metadata.value("assist_tof_fov_correction");
        if(policy!=QLatin1String(RvAssistFusion::FieldOfViewPolicy)||metadata["assist_output_policy"]!=policy
           ||!fov.isDouble()||(fov!=1&&fov!=2&&fov!=6)||metadata.value("assist_tof_fov_correction")!=fov)
            {fail(tr("Invalid dToF output support contract"));return false;}
        if(body){
            const auto fit=metadata["assist_fusion"].toObject();int positive=0;
            const auto count=fit["fov_pixels"].toDouble(-1);
            if(fit["output_policy"]!=policy||fit["fov_geometry_only"]!=true||!std::isfinite(count)||count<0||count>expectedDepth/4||std::floor(count)!=count)
                {fail(tr("Invalid dToF output support contract"));return false;}
            for(int i=0;i<expectedDepth/4;++i){const quint32 bits=qFromLittleEndian<quint32>(depth.constData()+4*i);float z;std::memcpy(&z,&bits,4);
                if(!std::isfinite(z)||z<0){fail(tr("Invalid dToF output support contract"));return false;}positive+=z>0;}
            if(positive!=count)
                {fail(tr("Invalid dToF output support contract"));return false;}
        }
    }
    if(metadata.value("assist_experiment")!=current.tof.metadata.value("assist_experiment"))
        {fail(QStringLiteral("Host depth experiment identity mismatch"));return false;}
    if(metadata.value(RvAssistFusion::NativeProjectionField)!=current.tof.metadata.value(RvAssistFusion::NativeProjectionField)
       ||(body&&metadata["assist_fusion_revision"]!=QLatin1String(RvAssistFusion::ReuseRgbRevision)))
        {fail(QStringLiteral("Host depth projection identity mismatch"));return false;}
    if((metadata["assist_input_rotation_degrees"]!=current.tof.metadata["assist_input_rotation_degrees"]
        ||metadata["assist_input_rotation_degrees"].toInt(-1)!=m_inputRotation))
        {fail(tr("Host depth result belongs to another input orientation"));return false;}
    if(metadata.value("frame_id")!=current.tof.metadata.value("frame_id")||metadata.value("generation")!=current.tof.metadata.value("generation")
        ||metadata.value("tof_sequence")!=current.tof.metadata.value("tof_sequence")||metadata.value("tof_timestamp_ns")!=current.tof.metadata.value("tof_timestamp_ns"))
        {fail(tr("Host depth result belongs to another RGB/dToF pair"));return false;}
    metadata["assist_frame_age_ms"]=double(age);metadata["assist_pending_dropped"]=double(m_dropped);
    metadata["assist_processing_budget_ms"]=m_resultAgeLimitMs;
    if(age>m_resultAgeLimitMs){emit cleared(tr("Host assisted depth result expired"));}
    else if(body!=expectedDepth||!response["error"].toString().isEmpty()){
        const auto source=response["error"].toString(tr("Invalid host assisted depth result")).toUtf8();
        const auto reason=QCoreApplication::translate("RvAssistFusion",source.constData());
        emit frameRejected(metadata,reason);if(!alive)return false;
        emit cleared(reason);
    }
    // Forward only the native measurement belonging to this validated
    // result. The GUI must never reconstruct it from the newest live ToF.
    else {
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
        auto pairedCamera=RvCameraView::pairedImage(current.camera,metadata);
        if(!current.tof.samples.isEmpty())pairedCamera=pairedCamera.convertToFormat(QImage::Format_RGB888);
        emit resultReady(depth,metadata,pairedCamera,current.epoch,coverage);
        if(!alive)return false;
        if(view.networkProfile)m_lastGuiPublishMs=publishClock.nsecsElapsed()/1e6;
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

    }catch(const std::exception &e) {
        writeResponse({{"protocol",1},{"type","fatal"},{"token",token},{"error",QString::fromUtf8(e.what())}});return 2;
    }
    const auto &view=RvCameraView::outputProfile(model.hostModel);
    const auto executionIdentity=model.executionIdentity();
    const QString hostModel=model.hostModel;
    QJsonObject ready=executionIdentity;
    ready["protocol"]=1;ready["type"]="ready";ready["token"]=token;ready["weights_sha256"]=view.weightsSha;
    ready["host_model"]=hostModel;ready["model_loading"]=model.storageInfo;
    if(!writeResponse(ready))return 3;
    QByteArray rgb,tofBytes;QJsonObject request;
    RvAssistFusion::GlobalRgbContinuity globalRgbContinuity;
    const bool fingerprintModel=qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1";
    int previousInputRotation=-1;
    quint64 lastHdTof=0;
    RvTofFrame lastReusableTof;
    QString hdIdentity;
    for(;;) {
        const auto next=readNext(request,rgb,tofBytes);
        if(next==AssistRead::End)break;
        if(next!=AssistRead::Request||rgb.size()!=view.rgbBytes()||(tofBytes.size()!=0&&tofBytes.size()!=6144&&tofBytes.size()!=1024)
            ||request["protocol"].toInt()!=1||request["token"]!=token||request["type"]!="infer")return 3;
        auto metadata=request["metadata"].toObject();
        QJsonObject response={{"protocol",1},{"type","result"},{"token",token},{"request",request["request"]},{"epoch",request["epoch"]}};
        QByteArray output,coverage;
        try {
            if(!RvTofFrame::isHostFusionExperiment(metadata["assist_experiment"]))
                throw std::runtime_error("Invalid ToF experiment request");
            if(!hdSource(metadata,view,hostModel))throw std::runtime_error("Invalid H.265 clock pairing");
            if(metadata.contains("assist_regional_correction"))throw std::runtime_error("Unsupported regional correction request");
            if(request["host_model"]!=hostModel||metadata["rectify_map_sha256"]!=QLatin1String(view.mapSha))
                throw std::runtime_error("UniDepth camera profile mismatch");
            metadata["assist_host_model"]=hostModel;
            RvTofFrame tof{tofBytes,metadata};
            const bool paired=metadata["dtof_present"].toBool();
            const bool geometric=metadata.value("assist_output_policy")==QLatin1String(RvAssistFusion::FieldOfViewPolicy);
            if(metadata.contains("assist_output_policy")&&!geometric)
                throw std::runtime_error("Invalid dToF output support contract");
            int geometricFov=0;
            if(geometric){
                const auto value=metadata.value("assist_tof_fov_correction");
                if(!model.wide||!value.isDouble()||(value!=1&&value!=2&&value!=6))throw std::runtime_error("Invalid dToF output support contract");
                geometricFov=value.toInt();
                const auto nativeFov=metadata.contains("dtof_fov_correction")?metadata.value("dtof_fov_correction"):metadata.value("tof_fov_correction");
                if(paired&&nativeFov!=value)throw std::runtime_error("Invalid dToF output support contract");
            }
            if((paired&&(!tof.isValidForHostFusion()||metadata["tof_warning_flags"]!=0))
               ||(!paired&&!tofBytes.isEmpty()))throw std::runtime_error("Invalid native dToF or H.265 input");
            if(paired&&!RvCameraView::supportsNativeGrid(hostModel,tof.columns(),tof.rows()))
                throw std::runtime_error("Native grid does not belong to the selected host model");
            const auto expectedProjection=RvAssistFusion::defaultNativeProjection(tof);
            const auto requestedProjection=metadata.value(RvAssistFusion::NativeProjectionField);
            if((!requestedProjection.isUndefined()&&requestedProjection!=expectedProjection)
               ||(paired&&expectedProjection!=QLatin1String(RvAssistFusion::CloseNative48Projection)))
                throw std::runtime_error("Invalid native ToF projection");
            metadata.insert(RvAssistFusion::NativeProjectionField,expectedProjection);
            tof.metadata=metadata;
            if(paired){
                const auto identity=metadata["boot_id"].toString()+metadata["source_generation"].toString()+QString::number(request["epoch"].toInteger());
                const bool newIdentity=identity!=hdIdentity;
                const auto sequence=quint64(metadata["tof_sequence"].toInteger());
                const quint64 previousSequence=newIdentity?0:lastHdTof;
                const bool repeated=sequence==previousSequence;
                if(sequence<previousSequence||metadata["tof_reused"]!=repeated)
                    throw std::runtime_error("Invalid H.265 dToF measurement sequence or reuse flag");
                if(repeated&&(tof.samples!=lastReusableTof.samples
                   ||metadata.value("tof_timestamp_ns")!=lastReusableTof.metadata.value("tof_timestamp_ns")
                   ||metadata.value("dtof_fov_correction")!=lastReusableTof.metadata.value("dtof_fov_correction")
                   ||metadata.value("tof_fov_correction")!=lastReusableTof.metadata.value("tof_fov_correction")))
                    throw std::runtime_error("Reused H.265 dToF measurement identity mismatch");
                if(newIdentity){
                    hdIdentity=identity;
                    globalRgbContinuity.reset();
                }
                lastHdTof=sequence;
                if(!repeated)lastReusableTof=tof;
            }

            QElapsedTimer clock;clock.start();
            QByteArray inferenceRgb=rgb;
            const int inputRotation=metadata["assist_input_rotation_degrees"].toInt(-1);
            {
                if((inputRotation!=0&&inputRotation!=180)||metadata["assist_input_rotation_degrees"].toDouble(-1)!=inputRotation)
                    throw std::runtime_error("Missing or unsupported UniDepth input orientation");
                if(inputRotation!=previousInputRotation){globalRgbContinuity.reset();previousInputRotation=inputRotation;}
                // Rotate each planar channel into the model's upright camera frame.
                // Both centred virtual camera profiles are invariant under 180 degrees. Restore
                // camera-Z to the original sensor grid BEFORE dToF projection.
                // Neither the paired RGB nor calibration/ToF arrays are rotated.
                const int inputPixels=view.inputWidth*view.inputHeight;
                if(inputRotation==180)for(int c=0;c<3;++c)
                    std::reverse(inferenceRgb.begin()+c*inputPixels,inferenceRgb.begin()+(c+1)*inputPixels);
            }
#ifdef DCTS_TENSORRT_RTX
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
            // Raw graph parity is an explicit offline check. Hashing another
            // 1.2 MB every live frame needlessly consumes the RGB frame budget.
            if(fingerprintModel){
                QByteArray modelMonoBytes(int(mono.size())*4,Qt::Uninitialized);
                qToLittleEndian<quint32>(mono.data(),qsizetype(mono.size()),modelMonoBytes.data());
                metadata["assist_model_monocular_sha256"]=QString::fromLatin1(QCryptographicHash::hash(modelMonoBytes,QCryptographicHash::Sha256).toHex());
            }
            mono=RvCameraView::portraitDepth(mono);
            // Reading a missing native field must not insert a null into an
            // RGB-only result: its display geometry uses assist_tof_fov_correction.
            const auto &cal=RvAssistFusion::calibrationForNativeGrid((metadata.contains("dtof_fov_correction")?metadata.value("dtof_fov_correction"):metadata.value("tof_fov_correction")).toInt(1),RvCameraView::nativeColumns(hostModel),RvCameraView::nativeRows(hostModel));
            const auto epoch=request["epoch"].toInteger(),generation=metadata["generation"].toInteger(),frame=metadata["frame_id"].toInteger();
            const auto now=monotonicMs();
            auto fused=paired?RvAssistFusion::fuseGlobalScale(mono,tof,cal,view):
                RvAssistFusion::monocular(mono,"H.265 input has no verified time-paired dToF",view.outputSize());
            fused=globalRgbContinuity.apply(std::move(fused),now,epoch,generation,frame);
            const auto fusionMode=fused.diagnostics["fusion_mode"].toString();
            const auto visible=geometric?RvAssistFusion::limitTofFieldOfView(std::move(fused),
                RvAssistFusion::calibrationForFov(geometricFov),view):std::move(fused);
            metadata["assist_fusion"]=visible.diagnostics;
            if(metadata["assist_diagnostic_capture"]==true){
                coverage=RvDepthDiagnostic::encode(mono,visible);
                metadata["assist_diagnostic_payload"]="mono_reasons_v1";
                metadata["assist_diagnostic_native_count"]=RvCameraView::nativeCount(hostModel);
            }
            metadata["assist_fusion_revision"]=RvAssistFusion::ReuseRgbRevision;
            metadata["assist_inference_ms"]=inferMs;metadata["assist_total_ms"]=clock.nsecsElapsed()/1e6;
            metadata["assist_input_orientation_checked"]=true;metadata["assist_output_restored_to_calibrated_grid"]=true;
            if(!visible.ok())throw std::runtime_error(visible.error.toStdString());
            output=visible.depth;
            metadata["assist_source_model"]=metadata.value("model");
            metadata["assist_source_depth_width"]=metadata.value("width");
            metadata["assist_source_depth_height"]=metadata.value("height");
            const bool monocular=fusionMode=="monocular";
            const bool held=fusionMode=="held_scale";
            metadata["model"]=QString::fromLatin1(view.modelName)+(monocular?" (monocular)":(held?" (held dToF scale)":" + dToF"));
            if(model.wide)metadata["model"]=metadata["model"].toString()+" / wide view";
            metadata["width"]=view.outputWidth;metadata["height"]=view.outputHeight;
            metadata["assist_output_grid"]=RvCameraView::PortraitGrid;
            metadata["assist_native_columns"]=RvCameraView::nativeColumns(hostModel);
            metadata["assist_native_rows"]=RvCameraView::nativeRows(hostModel);
            metadata["assist_capture_mode"]=RvCameraView::captureMode(hostModel);
            metadata["assist_model_depth_size"]=QJsonArray{640,480};
            metadata["assist_model_crop_xywh"]=QJsonArray{140,0,360,480};
            metadata["depth_unit"]="camera-Z_m_float32";metadata["depth_location"]="host";
            metadata["depth_present"]=true;metadata["capture_only"]=false;metadata["inference_location"]="host";
            metadata["depth_method"]=monocular?"monocular_unadjusted":held?"monocular_held_dtof_scale":"portrait_global_rgb";
            metadata["assist_resolution_level"]=view.resolutionLevel<0?QJsonValue():QJsonValue(view.resolutionLevel);
            metadata["depth_K"]=RvCameraView::intrinsics(view);metadata["depth_stride"]=view.outputWidth*4;
            metadata["depth_bytes"]=view.depthBytes();metadata["depth_source"]=monocular?"host_unidepthv2_monocular":(held?"host_unidepthv2_held_scale":"host_unidepthv2_dtof");
            metadata["assist_network_width"]=view.networkWidth;metadata["assist_network_height"]=view.networkHeight;
            if(view.networkProfile)metadata["assist_network_profile"]=view.networkProfile;
            metadata["assist_weights_sha256"]=view.weightsSha;
            metadata["assist_camera_profile"]=view.name;metadata["assist_host_model"]=hostModel;
            for(auto it=executionIdentity.begin();it!=executionIdentity.end();++it)metadata["assist_"+it.key()]=it.value();
            if(view.provisional){metadata["assist_provisional_model"]=true;metadata["assist_candidate_parity_mean_limit"]=view.parityMeanLimit;}
            metadata["assist_backend"]=RvInferenceRuntime::backendName(model.backend);
            metadata["assist_execution_device"]=model.backend;
            metadata["assist_calibration_status"]=monocular?"monocular_not_dtof_corrected":(held?"historical_dtof_scale_only":"usable_initial_fusion_with_limits");
        }catch(const std::exception &e) {output.clear();coverage.clear();response["error"]=QString::fromUtf8(e.what());}
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

int runRvInferenceProbe(const QString &backend,const QString &reportPath) {
    QJsonObject report{{"schema",1},{"backend",backend},{"pass",false},{"hardware_capture_started",false}};
    qputenv("RV_INFERENCE_BACKEND",backend.toUtf8());
    try {
        NativeModel model;model.initialize(RuntimeDiscovery::rvAssistRoot(),QLatin1String(RvCameraView::UniDepthVgaNative48));
        QByteArray rgb(672*504*3,Qt::Uninitialized);
        for(int i=0;i<rgb.size();++i)rgb[i]=char((i*13+i/672)%256);
        const auto first=model.infer(rgb);rgb[0]^=127;
        for(int i=0;i<672*504;++i)rgb[i]=char(255-quint8(rgb[i]));
        const auto second=model.infer(rgb);
        bool changed=first!=second;double total=0,maximum=0;QJsonArray timings;
        for(int i=0;i<3;++i){QElapsedTimer clock;clock.start();const auto depth=model.infer(rgb);
            const double ms=clock.nsecsElapsed()/1e6;timings.append(ms);total+=ms;maximum=std::max(maximum,ms);
            if(depth.size()!=640*480)throw std::runtime_error("Invalid inference probe output");}
        if(!changed)throw std::runtime_error("Inference probe did not respond to changed RGB");
        report["pass"]=true;report["checks"]=5;report["mean_ms"]=total/3;report["maximum_ms"]=maximum;
        report["fps"]=3000/total;report["timings_ms"]=timings;report["loading"]=model.storageInfo;
        report["model_identity"]=model.executionIdentity();report["output_shape"]=QJsonArray{1,1,480,640};
    }catch(const std::exception &error){report["error"]=QString::fromUtf8(error.what());}
    QFile output(reportPath);if(!output.open(QIODevice::WriteOnly))return 3;
    output.write(QJsonDocument(report).toJson());return report["pass"].toBool()?0:1;
}

int runRvAssistDepthTests(const QString &reportPath) {
    // Check the tensor bytes against the original conversion, including row
    // padding, 180 degree orientation, and conversion from another format.
    bool planarMatches=true;
    for(const auto size:{RvCameraView::Rgb672VgaFp16.inputSize(),QSize(7,5)}){
        QImage rgb(size,QImage::Format_RGB888);
        for(int y=0;y<size.height();++y){
            auto *row=rgb.scanLine(y);
            for(int x=0;x<size.width();++x)
                for(int c=0;c<3;++c)
                    row[3*x+c]=uchar((x*3+y*7+c*59)%256);
        }
        for(const auto &image:{rgb,rgb.flipped(Qt::Horizontal|Qt::Vertical),rgb.convertToFormat(QImage::Format_ARGB32)}){
            const auto converted=image.convertToFormat(QImage::Format_RGB888);
            const auto tensor=RvAssistDepth::preparePlanarInput(image);
            const int pixels=size.width()*size.height();
            if(tensor.size()!=3*pixels){planarMatches=false;continue;}
            for(int c=0;c<3;++c)
                for(int y=0;y<size.height();++y)
                    for(int x=0;x<size.width();++x)
                        planarMatches&=quint8(tensor[c*pixels+y*size.width()+x])==converted.constScanLine(y)[3*x+c];
        }
    }
    RvAssistDepth assist;
    const auto &profile=RvCameraView::Portrait;
    QEventLoop loop;
    QJsonObject report;
    const bool testReuse=qEnvironmentVariable("DCTS_TEST_REUSE_TOF")=="1";
    report["prepared_rgb_bytes_match"]=planarMatches;
    QJsonArray statuses,cleared;
    QObject::connect(&assist,&RvAssistDepth::statusChanged,&loop,[&](const QString &message){
        if(statuses.size()<24)statuses.append(message);
    });
    QObject::connect(&assist,&RvAssistDepth::cleared,&loop,[&](const QString &message){
        if(cleared.size()<24)cleared.append(message);
    });
    QObject::connect(&assist,&RvAssistDepth::inferenceUnavailable,&loop,[&](const QString &,const QString &detail){
        report["error"]=detail;
        loop.quit();
    });
    QObject::connect(&assist,&RvAssistDepth::resultReady,&loop,[&](const QByteArray &depth,const QJsonObject &metadata,const QImage &,
                                                               quint64,const QByteArray &){
        if(testReuse&&metadata["tof_reused"]!=true)return;
        report["depth_bytes"]=depth.size();
        report["backend"]=metadata.value("assist_backend");
        report["experiment"]=metadata["assist_experiment"];
        report["tof_reused"]=metadata["tof_reused"];
        report["width"]=metadata.value("width");report["height"]=metadata.value("height");
        report["graph_sha256"]=metadata["assist_graph_sha256"];
        report["first_inference_ms"]=metadata["assist_inference_ms"];
        loop.quit();
    });
    QTimer::singleShot(HostWorkerLoadTimeoutMs+10000,&loop,&QEventLoop::quit);
    assist.setEnabled(true);
    QImage image(profile.inputWidth,profile.inputHeight,QImage::Format_RGB32);
    image.fill(qRgb(88,130,170));
    const auto planar=RvAssistDepth::preparePlanarInput(image);
    QJsonObject source{{"source_protocol","HDA1"},{"source_transport","rtsp_h265"},
        {"model","Native"},{"source_rgb_width",2304},{"source_rgb_height",1296},
        {"rgb_width",profile.inputWidth},{"rgb_height",profile.inputHeight},{"dtof_present",false},{"work_mode","host_rgb"},
        {"assist_experiment","B"},
        {"source_candidate_mode",RvCameraView::captureMode(assist.hostModel())},
        {"rectify_map_sha256",QLatin1String(RvCameraView::Rgb672VgaFp16.mapSha)},{"assist_camera_profile","wide"},
        {"frame_id",1},{"generation",1},{"host_decoded_monotonic_ns",double(monotonicMs())*1000000.},
        {"boot_id","01234567-89ab-cdef-0123-456789abcdef"}};
    QByteArray nativeSamples;
    if(testReuse){
        nativeSamples=QByteArray(6144,0);
        const QJsonObject paired{{"dtof_present",true},{"work_mode","rgb_tof"},
            {"source_generation",QString(32,'a')},{"source_engine","native-hd-capture-v1"},
            {"generation_source","host_presentation_epoch"},{"pairing_policy",RvTofFrame::FreshRgbPairPolicy},
            {"pair_clock_source","rtcp_prft_board_realtime_monotonic"},{"pair_clock_uncertainty_ms",35},
            {"tof_sequence",1},{"tof_timestamp_ns",1000000000.},{"tof_reused",false},{"tof_warning_flags",0},
            {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},{"dtof_fov_correction",6},
            {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
            {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"},
            {"assist_output_policy",RvAssistFusion::FieldOfViewPolicy},{"assist_tof_fov_correction",6}};
        for(auto it=paired.constBegin();it!=paired.constEnd();++it)source[it.key()]=it.value();
    }
    // A real video stream supplies fresh frames while the first GPU request
    // warms up. Keep the production freshness gate, and exercise that stream
    // instead of relying on one cold-start frame surviving the GPU's 1 s gate.
    QTimer frames;
    frames.setInterval(100);
    qint64 frameId=0;
    const auto submit=[&]{
        auto frame=source;
        frame["frame_id"]=++frameId;
        frame["host_decoded_monotonic_ns"]=double(monotonicMs())*1000000.;
        if(testReuse){
            const double delta=-33.*std::min<qint64>(frameId,4);
            frame["pair_delta_ms"]=delta;
            frame["rgb_timestamp_ns"]=1000000000.-delta*1e6;
        }
        assist.submitHd(image,frame,1,nativeSamples,planar);
    };
    QObject::connect(&frames,&QTimer::timeout,&loop,submit);
    QObject::connect(&assist,&RvAssistDepth::loadingStateChanged,&loop,[&](int state){
        if(state!=2)return;
        frames.start();submit();
    });
    assist.m_pending.camera=image;
    const bool forceStaging=qEnvironmentVariable("DCTS_TEST_FORCE_WORKER_STAGING")=="1";
    assist.startWorker(forceStaging);
#ifdef DCTS_TENSORRT_RTX
    if(assist.m_workerPaths){
        report["worker_staged"]=assist.m_workerPaths->stagedWorker;
        report["runtime_staged"]=assist.m_workerPaths->stagedRuntime;
    }
#endif
    report["program"]=assist.m_process.program();
    report["working_directory"]=assist.m_process.workingDirectory();
    if(!report.contains("error"))loop.exec();
    frames.stop();
    report["statuses"]=statuses;report["cleared"]=cleared;report["submitted_frames"]=frameId;
    const bool passed=planarMatches&&report["depth_bytes"].toInt()==profile.depthBytes()
        &&report["width"]==profile.outputWidth&&report["height"]==profile.outputHeight
        &&report["experiment"]=="B"&&(!testReuse||report["tof_reused"]==true)
        &&report["graph_sha256"]==RvInferenceRuntime::identity(assist.m_backend)["graph_sha256"]
        &&report["backend"]==RvInferenceRuntime::backendName(assist.m_backend);
    report["pass"]=passed;
    QFile file(reportPath);
    if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(report).toJson());
    return passed?0:2;
}

