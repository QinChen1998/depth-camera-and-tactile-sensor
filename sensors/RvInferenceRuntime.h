#pragma once
#include "../sdk/DepthSdkExports.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <memory>
#include <vector>

namespace RvInferenceRuntime {
inline constexpr auto Int8Sha="7dc50150c210b75e72fac35b45967ef874d85e9a883040223dbd5ebd4e01d63d";
DCTS_DEPTH_INTERNAL bool validBackend(const QString &id);
bool isOpenVino(const QString &id);
QString provider(const QString &id);
DCTS_DEPTH_INTERNAL QString backendName(const QString &id);
QJsonObject identity(const QString &id);
DCTS_DEPTH_INTERNAL bool validGraphMetadata(const QJsonObject &metadata);
QByteArray fileSha256(const QString &path);
QJsonObject verifyCompatibleRuntime();
void checkTensorRtCudaDevice();
DCTS_DEPTH_INTERNAL int writeEnumeration(const QString &reportPath);
}

class RvOpenVinoModel {
public:
    RvOpenVinoModel();
    ~RvOpenVinoModel();
    QJsonObject initialize(const QString &backend,const QString &originalRoot,const QJsonObject &manifest);
    std::vector<float> infer(const QByteArray &rgb);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
