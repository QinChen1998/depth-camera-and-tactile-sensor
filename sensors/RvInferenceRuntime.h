#pragma once
#include <QJsonObject>
#include <QString>

namespace RvInferenceRuntime {
QJsonObject identity(const QString &id);
bool validGraphMetadata(const QJsonObject &metadata);
}
