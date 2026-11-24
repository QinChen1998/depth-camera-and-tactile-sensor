#include "RvInferenceRuntime.h"
#include "RvCameraView.h"
#include <QRegularExpression>

namespace RvInferenceRuntime {
namespace {
constexpr auto Int8Sha="7dc50150c210b75e72fac35b45967ef874d85e9a883040223dbd5ebd4e01d63d";
bool validBackend(const QString &id){
    return id=="tensorrt"||id=="cpu"||id=="openvino:CPU"||id=="openvino:INT8"
        ||QRegularExpression("^directml:[0-9]{1,2}$").match(id).hasMatch()
        ||QRegularExpression("^openvino:GPU(\\.[0-9]{1,2})?$").match(id).hasMatch();
}
bool isOpenVino(const QString &id){return validBackend(id)&&id.startsWith("openvino:");}
QString backendName(const QString &id){
    if(id=="tensorrt")return "onnxruntime-tensorrt-rtx";
    if(id=="cpu")return "onnxruntime-cpu";
    if(id.startsWith("directml:"))return "onnxruntime-directml";
    return isOpenVino(id)?QStringLiteral("openvino-native"):QString();
}
}

// Identities for recordings created by the retired UniDepth runtime.
QJsonObject identity(const QString &id){
    const auto &p=RvCameraView::Rgb672VgaFp16;
    if(id=="openvino:INT8")return {{"graph_sha256",Int8Sha},{"precision","int8_fp32_decoder"},
        {"source_graph_sha256",p.graphSha},{"conversion","nncf_3.4.0_decoder_128calibration_v1"}};
    return {{"graph_sha256",p.graphSha},{"precision",p.precision},{"source_graph_sha256",p.sourceSha},{"conversion",p.conversion}};
}
bool validGraphMetadata(const QJsonObject &m){
    const auto graph=m["assist_graph_sha256"];
    // Derived graphs carry their backend, source identity and precision.
    if(graph==QLatin1String(RvCameraView::Rgb672VgaFp16.graphSha)&&!m.contains("assist_execution_device"))return true;
    const auto id=m["assist_execution_device"].toString();
    if(!validBackend(id)||m["assist_backend"]!=backendName(id))return false;
    const auto expected=identity(id);
    for(auto it=expected.begin();it!=expected.end();++it)if(m["assist_"+it.key()]!=it.value())return false;
    return true;
}
}
