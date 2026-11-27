#include "AppEdition.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
const QJsonObject &productSettings()
{
    static const auto settings = [] {
        QFile manifest(QStringLiteral(":/config/DCTS_APP.json"));
        if (!manifest.open(QIODevice::ReadOnly))
            return QJsonObject{};
        return QJsonDocument::fromJson(manifest.readAll()).object();
    }();
    return settings;
}

const QJsonObject &editionSettings()
{
    static const auto customer = productSettings().value("customer").toObject();
    return AppEdition::Customer ? customer : productSettings();
}
}

QString AppEdition::label()
{
    return Customer ? QStringLiteral("客户版") : QStringLiteral("内测版");
}

QString AppEdition::version()
{
    return productSettings().value("version").toString();
}

const QStringList &AppEdition::imagingSchemes()
{
    static const auto schemes = [] {
        QStringList result;
        const auto ids = editionSettings().value("imaging_schemes").toArray();
        for (const auto &id : ids)
            result.append(id.toString());
        return result;
    }();
    return schemes;
}

bool AppEdition::measurementCaptureEnabled()
{
    return editionSettings().value("measurement_capture").toBool();
}
