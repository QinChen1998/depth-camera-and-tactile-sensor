#include "RenderingSettings.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSettings>

RenderingSettings *RenderingSettings::instance()
{
    static QPointer<RenderingSettings> settings;
    if (!settings)
        settings = new RenderingSettings(QCoreApplication::instance());
    return settings;
}

RenderingSettings::RenderingSettings(QObject *parent) : QObject(parent)
{
    m_lowCostRendering = QSettings().value("rendering/smoothRendering",true).toBool();
}

void RenderingSettings::setLowCostRendering(bool enabled)
{
    if (m_lowCostRendering == enabled)
        return;
    m_lowCostRendering = enabled;
    QSettings().setValue("rendering/smoothRendering",enabled);
    emit lowCostRenderingChanged();
}
