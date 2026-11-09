#include "RvInferenceRuntime.h"
#include "RvCameraView.h"
#include "../RuntimeDiscovery.h"
#include "../tools/model_protection/ModelProtectionApi.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLibrary>
#include <QRegularExpression>
#include <QtEndian>
#include <qscopeguard.h>
#include <cmath>
#include <dxgi1_2.h>
#include <intrin.h>

namespace {
QJsonObject json(const QString &path){
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)throw std::runtime_error("Cannot read inference runtime manifest");
    QJsonParseError error;const auto result=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!result.isObject())throw std::runtime_error("Invalid inference runtime manifest");
    return result.object();
}
QString sha(const QString &path){
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot hash inference runtime file");
    QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(&file);return QString::fromLatin1(hash.result().toHex());
}
bool avx2(){
    int info[4]{};__cpuid(info,1);
    if((info[2]&(1<<27))==0||(info[2]&(1<<28))==0||(_xgetbv(0)&6)!=6)return false;
    __cpuidex(info,7,0);return (info[1]&(1<<5))!=0;
}
struct OvLibrary {
    HMODULE module=nullptr;DLL_DIRECTORY_COOKIE directory=nullptr;DctsOpenVinoApi api;
    ov_core_t *core=nullptr;
    ~OvLibrary(){if(core)api.ov_core_free(core);if(module)FreeLibrary(module);if(directory)RemoveDllDirectory(directory);}
    void load(){
        if(!avx2())throw std::runtime_error("This OpenVINO runtime requires AVX2; use the compatible CPU option");
        const auto root=QDir::toNativeSeparators(RuntimeDiscovery::rvCompatibleRoot()+"/openvino");
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        directory=AddDllDirectory(reinterpret_cast<const wchar_t*>(root.utf16()));
        const auto path=root+"/openvino_c.dll";
        module=LoadLibraryExW(reinterpret_cast<const wchar_t*>(path.utf16()),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!module)throw std::runtime_error("Cannot load bundled OpenVINO runtime");
        api.load(module);api.check(api.ov_core_create(&core));
    }
};
}

namespace RvInferenceRuntime {
bool validBackend(const QString &id){
    return id=="tensorrt"||id=="cpu"||id=="openvino:CPU"||id=="openvino:INT8"
        ||QRegularExpression("^directml:[0-9]{1,2}$").match(id).hasMatch()
        ||QRegularExpression("^openvino:GPU(\\.[0-9]{1,2})?$").match(id).hasMatch();
}
bool isOpenVino(const QString &id){return validBackend(id)&&id.startsWith("openvino:");}
QString provider(const QString &id){
    if(id=="tensorrt")return "NvTensorRTRTXExecutionProvider";
    if(id=="cpu")return "CPUExecutionProvider";
    if(id.startsWith("directml:"))return "DmlExecutionProvider";
    if(isOpenVino(id))return "OpenVINO";
    return {};
}
QString backendName(const QString &id){
    if(id=="tensorrt")return "onnxruntime-tensorrt-rtx";
    if(id=="cpu")return "onnxruntime-cpu";
    if(id.startsWith("directml:"))return "onnxruntime-directml";
    return isOpenVino(id)?QStringLiteral("openvino-native"):QString();
}
QJsonObject identity(const QString &id){
    const auto &p=RvCameraView::Rgb672VgaFp16;
    if(id=="openvino:INT8")return {{"graph_sha256",Int8Sha},{"precision","int8_fp32_decoder"},
        {"source_graph_sha256",p.graphSha},{"conversion","nncf_3.4.0_decoder_128calibration_v1"}};
    return {{"graph_sha256",p.graphSha},{"precision",p.precision},{"source_graph_sha256",p.sourceSha},{"conversion",p.conversion}};
}
bool validGraphMetadata(const QJsonObject &m){
    const auto graph=m["assist_graph_sha256"];
    // Preserve original schema 1/2 diagnostics. New derived graphs must carry
    // their explicit backend, source identity and precision contract.
    if(graph==QLatin1String(RvCameraView::Rgb672VgaFp16.graphSha)&&!m.contains("assist_execution_device"))return true;
    const auto id=m["assist_execution_device"].toString();
    if(!validBackend(id)||m["assist_backend"]!=backendName(id))return false;
    const auto expected=identity(id);
    for(auto it=expected.begin();it!=expected.end();++it)if(m["assist_"+it.key()]!=it.value())return false;
    return true;
}
QJsonObject verifyCompatibleRuntime(){
    const QDir root(RuntimeDiscovery::rvCompatibleRoot());const auto m=json(root.filePath("runtime.json"));
    if(m["schema"]!=1||m["source_graph_sha256"]!=QLatin1String(RvCameraView::Rgb672VgaFp16.graphSha)
        ||m["ort_version"]!="1.24.4"||m["openvino_version"]!="2026.4.1"
        ||m["int8"].toObject()["sha256"]!=QLatin1String(Int8Sha))throw std::runtime_error("Compatible runtime identity mismatch");
    const auto files=m["files"].toObject();
    for(const auto *name:{"directml/onnxruntime.dll","directml/DirectML.dll","openvino/openvino_c.dll",
                         "openvino/openvino_intel_cpu_plugin.dll","openvino/openvino_intel_gpu_plugin.dll"})
        if(!files.contains(QLatin1String(name)))throw std::runtime_error("Incomplete compatible inference runtime");
    for(auto it=files.begin();it!=files.end();++it){
        const auto name=it.key();
        if(QDir::isAbsolutePath(name)||name.contains("..")||name.contains('\\')||name.contains(':')
            ||it.value().toString().size()!=64||sha(root.filePath(name))!=it.value().toString())
            throw std::runtime_error("Compatible runtime fingerprint mismatch");
    }
    return m;
}
QJsonArray enumerateCandidates(){
    QJsonArray candidates;
    auto add=[&](const QString &id,const QString &name,const QString &hardware,const QString &key){
        candidates.append(QJsonObject{{"id",id},{"name",name},{"hardware",hardware},{"key",key},{"quantized",id=="openvino:INT8"}});
    };
    const auto library=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(library){
        const auto free=qScopeGuard([&]{FreeLibrary(library);});
        using Create=HRESULT(WINAPI*)(REFIID,void**);
        const auto create=reinterpret_cast<Create>(GetProcAddress(library,"CreateDXGIFactory1"));
        IDXGIFactory1 *factory=nullptr;
        if(create&&SUCCEEDED(create(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))){
            const auto release=qScopeGuard([&]{factory->Release();});
            bool trt=false;
            for(UINT i=0;i<32;++i){
                IDXGIAdapter1 *adapter=nullptr;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
                if(!adapter)continue;DXGI_ADAPTER_DESC1 desc{};
                const bool ok=SUCCEEDED(adapter->GetDesc1(&desc));adapter->Release();
                if(!ok||(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
                const auto name=QString::fromWCharArray(desc.Description);
                const auto key=QString("%1:%2:%3:%4").arg(desc.VendorId).arg(desc.DeviceId).arg(desc.SubSysId).arg(name);
                if(desc.VendorId==0x10de&&!trt){add("tensorrt","TensorRT-RTX",name,"tensorrt:"+key);trt=true;}
                add(QString("directml:%1").arg(i),"DirectML",name,"directml:"+key);
            }
        }
    }
    try {
        verifyCompatibleRuntime();
        OvLibrary ov;ov.load();ov_available_devices_t devices{};
        ov.api.check(ov.api.ov_core_get_available_devices(ov.core,&devices));
        const auto free=qScopeGuard([&]{ov.api.ov_available_devices_free(&devices);});
        for(size_t i=0;i<devices.size;++i){
            const auto device=QString::fromUtf8(devices.devices[i]);
            if(device!="CPU"&&!device.startsWith("GPU"))continue;
            char *full=nullptr;QString name=device;
            if(ov.api.ov_core_get_property(ov.core,devices.devices[i],"FULL_DEVICE_NAME",&full)==OK&&full){
                name=QString::fromUtf8(full);ov.api.ov_free(full);
            }
            // OpenCL enumeration may include non-Intel devices; this backend
            // promises Intel GPU support only. Other vendors use DirectML.
            if(device.startsWith("GPU")&&!name.contains("Intel",Qt::CaseInsensitive))continue;
            add("openvino:"+device,"OpenVINO",name,"openvino:"+device+":"+name);
            if(device=="CPU")add("openvino:INT8","OpenVINO INT8",name,"openvino:int8:"+name);
        }
    } catch(const std::exception &) { /* ORT CPU remains independently usable. */ }
    add("cpu","ONNX Runtime CPU",QCoreApplication::translate("RvInferenceSettings","CPU (compatible)"),"cpu");
    return candidates;
}
int writeEnumeration(const QString &path){
    QFile file(path);if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(QJsonObject{{"schema",1},{"candidates",enumerateCandidates()}}).toJson());return 0;
}
}

struct RvOpenVinoModel::Impl {
    OvLibrary library;QLibrary protection;
    ov_compiled_model_t *compiled=nullptr;ov_infer_request_t *request=nullptr;
    ov_tensor_t *input=nullptr,*output=nullptr;
    ~Impl(){
        auto &a=library.api;if(input)a.ov_tensor_free(input);if(output)a.ov_tensor_free(output);
        if(request)a.ov_infer_request_free(request);if(compiled)a.ov_compiled_model_free(compiled);
    }
};
RvOpenVinoModel::RvOpenVinoModel():d(std::make_unique<Impl>()){}
RvOpenVinoModel::~RvOpenVinoModel()=default;
QJsonObject RvOpenVinoModel::initialize(const QString &backend,const QString &originalRoot,const QJsonObject &manifest){
    const auto compatible=RvInferenceRuntime::verifyCompatibleRuntime();
    const bool ir=backend=="openvino:INT8";
    const QDir root(ir?RuntimeDiscovery::rvCompatibleRoot():originalRoot);
    const auto protection=ir?compatible["int8"].toObject()["model_protection"].toObject():manifest["model_protection"].toObject();
    const auto expected=RvInferenceRuntime::identity(backend)["graph_sha256"].toString().toLatin1();
    const auto device=(ir?QStringLiteral("CPU"):backend.mid(9)).toLatin1();
    QElapsedTimer clock;clock.start();d->library.load();auto &a=d->library.api;
    QJsonObject result;
#ifdef DCTS_INSTALLER_BUILD
    if(protection.isEmpty())throw std::runtime_error("Protected OpenVINO model manifest missing");
#endif
    if(!protection.isEmpty()){
        if(protection["scheme"]!="aes256-gcm-memory-v1"||protection["loader_abi"]!=1
            ||protection["plaintext_sha256"]!=QString::fromLatin1(expected))throw std::runtime_error("Protected OpenVINO model identity mismatch");
        for(const auto *key:{"loader","ciphertext"}){
            const auto name=protection[QLatin1String(key)].toString();
            const auto files=(ir?compatible:manifest)["files"].toObject();
            if(name.isEmpty()||!files.contains(name)||QDir::isAbsolutePath(name)||name.contains("..")||name.contains(':'))
                throw std::runtime_error("Invalid protected OpenVINO file binding");
        }
        d->protection.setFileName(root.filePath(protection["loader"].toString()));
        if(!d->protection.load())throw std::runtime_error(d->protection.errorString().toStdString());
        const auto create=reinterpret_cast<DctsCreateProtectedOpenVinoFn>(d->protection.resolve("DctsCreateProtectedOpenVino"));
        if(!create)throw std::runtime_error("Protected OpenVINO loader API missing");
        DctsModelLoadMetrics metrics{};metrics.struct_bytes=sizeof(metrics);char error[2048]{};
        const auto path=root.filePath(protection["ciphertext"].toString());
        if(create(&a,d->library.core,device.constData(),ir?1:0,reinterpret_cast<const wchar_t*>(path.utf16()),
            expected.constData(),&d->compiled,&metrics,error,sizeof(error)))throw std::runtime_error(error);
        result={{"kind","aes256-gcm-memory-v1"},{"plaintext_file_used",false},{"plaintext_sha256_verified",true},
            {"plaintext_bytes",qint64(metrics.plaintext_bytes)},{"decrypt_ms",metrics.decrypt_ms},
            {"plaintext_clear_ms",metrics.plaintext_clear_ms},{"session_create_ms",metrics.session_create_ms}};
    }else{
        const auto path=root.filePath(ir?compatible["int8"].toObject()["file"].toString():QLatin1String(RvCameraView::Rgb672VgaFp16.graphFile));
        if(sha(path)!=QString::fromLatin1(expected))throw std::runtime_error("OpenVINO graph fingerprint mismatch");
        QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read OpenVINO model");
        auto bytes=file.readAll();
        const auto clear=qScopeGuard([&]{SecureZeroMemory(bytes.data(),size_t(bytes.size()));});
        d->compiled=dctsCompileOpenVino(a,d->library.core,device.constData(),reinterpret_cast<const unsigned char*>(bytes.constData()),size_t(bytes.size()),ir);
        result={{"kind","plaintext-file"},{"plaintext_file_used",true}};
    }
    a.check(a.ov_compiled_model_create_infer_request(d->compiled,&d->request));
    a.check(a.ov_infer_request_get_input_tensor(d->request,&d->input));
    result["provider"]="OpenVINO";result["runtime_version"]="2026.4.1";result["device"]=backend;
    result["initialization_ms"]=clock.nsecsElapsed()/1e6;return result;
}
std::vector<float> RvOpenVinoModel::infer(const QByteArray &rgb){
    auto &a=d->library.api;size_t bytes=0;void *data=nullptr;ov_element_type_e type;
    a.check(a.ov_tensor_get_byte_size(d->input,&bytes));a.check(a.ov_tensor_get_element_type(d->input,&type));
    if(bytes!=size_t(rgb.size())||bytes!=672*504*3||type!=U8)throw std::runtime_error("OpenVINO input contract mismatch");
    a.check(a.ov_tensor_data(d->input,&data));std::memcpy(data,rgb.constData(),bytes);
    a.check(a.ov_infer_request_infer(d->request));
    if(d->output){a.ov_tensor_free(d->output);d->output=nullptr;}
    a.check(a.ov_infer_request_get_output_tensor(d->request,&d->output));
    ov_shape_t shape{};a.check(a.ov_tensor_get_shape(d->output,&shape));
    const auto free=qScopeGuard([&]{a.ov_shape_free(&shape);});
    a.check(a.ov_tensor_get_element_type(d->output,&type));
    if(shape.rank!=4||shape.dims[0]!=1||shape.dims[1]!=1||shape.dims[2]!=480||shape.dims[3]!=640||type!=F32)
        throw std::runtime_error("OpenVINO output contract mismatch");
    a.check(a.ov_tensor_data(d->output,&data));const auto *z=static_cast<const float*>(data);
    std::vector<float> output(z,z+640*480);
    for(float value:output)if(!std::isfinite(value)||value<=0)throw std::runtime_error("Invalid OpenVINO model output");
    return output;
}
