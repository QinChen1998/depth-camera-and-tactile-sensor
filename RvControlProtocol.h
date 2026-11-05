#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <stdexcept>

namespace rvcontrol {
constexpr auto Host = "172.32.0.93";
constexpr auto Key = "";
constexpr auto Implementation = "rv-control-cpp-v1";
struct Error : std::runtime_error {
    int code;
    Error(const QString &message, int exitCode = 2) : std::runtime_error(message.toStdString()), code(exitCode) {}
};
struct Configuration {
    int width = 576, depthWidth = 144;
    QString model = "Native", location = "none", method = "none";
    bool keepPreview = false, keepModel = false, recoverFault = false, recoverStreamFault = false;
};
void validate(const Configuration &config);
QString command(const Configuration &config);
qint64 counter(const QJsonValue &value, bool allowString = false);
bool rtspTelemetry(const QJsonObject &service, const QJsonObject &telemetry);
struct Receipt { QString boot, model; qint64 generation; };
Receipt receipt(const QByteArray &output, const Configuration &config);
// Each configure has a new tracker. No old boot/generation/frame floor survives.
class Readiness {
public:
    Readiness(const Receipt &receipt, const Configuration &config) : m_receipt(receipt), m_config(config) {}
    bool observe(const QJsonObject &status);
private:
    Receipt m_receipt;
    Configuration m_config;
    qint64 m_frame = 0, m_retrieved = 0;
    bool m_seen = false;
};
}
