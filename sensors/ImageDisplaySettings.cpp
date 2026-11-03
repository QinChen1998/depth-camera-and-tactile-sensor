#include "ImageDisplaySettings.h"
#include "ImagePresentation.h"

#include <QtCore/QSettings>
#include <QtCore/QWeakPointer>
#include <algorithm>
#include <cmath>

namespace {
constexpr auto RotationKey = "imageDisplay/rotationDegrees";
constexpr auto BlackKey = "imageDisplay/blackOutsideRange";
constexpr auto MinimumKey = "imageDisplay/colorMinimumM";
constexpr auto MaximumKey = "imageDisplay/colorMaximumM";
bool validRange(double minimum, double maximum)
{
    return std::isfinite(minimum) && std::isfinite(maximum) && minimum >= ImageDisplaySettings::MinimumColorRangeM
        && maximum <= ImageDisplaySettings::MaximumColorRangeM && minimum < maximum;
}
}

QSharedPointer<ImageDisplaySettings> ImageDisplaySettings::shared()
{
    // Pages are created on the GUI thread. Let the last page release the model
    // so a later session reloads persisted settings instead of a stale cache.
    static QWeakPointer<ImageDisplaySettings> weak;
    auto settings = weak.toStrongRef();
    if (!settings) {
        settings.reset(new ImageDisplaySettings);
        weak = settings;
    }
    return settings;
}

ImageDisplaySettings::ImageDisplaySettings()
{
    QSettings settings;
    const QVariant legacy = settings.value("depth/displayRotationDegrees",
        settings.value("camera/displayRotationDegrees", 0));
    m_rotation = ImagePresentation::normalizedRotation(settings.value(RotationKey, legacy).toInt());
    settings.setValue(RotationKey, m_rotation);
    m_blackOutside=settings.value(BlackKey,settings.value("rvDepth/blackOutsideRange",false)).toBool();
    settings.setValue(BlackKey,m_blackOutside);
    const auto loadRange = [&](const char *minimumKey, const char *maximumKey) {
        if (!settings.contains(minimumKey) || !settings.contains(maximumKey)) return false;
        bool minimumOk = false, maximumOk = false;
        const double storedMinimum = settings.value(minimumKey).toDouble(&minimumOk);
        const double storedMaximum = settings.value(maximumKey).toDouble(&maximumOk);
        if (!minimumOk || !maximumOk || !std::isfinite(storedMinimum) || !std::isfinite(storedMaximum)) return false;
        const double minimum = std::max(storedMinimum, MinimumColorRangeM);
        const double maximum = std::min(storedMaximum, MaximumColorRangeM);
        if (!validRange(minimum, maximum)) return false;
        m_colorMinimum = minimum; m_colorMaximum = maximum;
        return true;
    };
    // Prefer the depth palette when migrating two formerly independent ranges.
    if (!loadRange(MinimumKey, MaximumKey)
        && !loadRange("rvDepth/minimumM", "rvDepth/maximumM"))
        loadRange("dtof/displayMinimumM", "dtof/displayMaximumM");
    settings.setValue(MinimumKey, m_colorMinimum);
    settings.setValue(MaximumKey, m_colorMaximum);
}

bool ImageDisplaySettings::setColorRange(double minimum, double maximum)
{
    if (!validRange(minimum, maximum)) return false;
    if (minimum == m_colorMinimum && maximum == m_colorMaximum) return true;
    m_colorMinimum = minimum; m_colorMaximum = maximum;
    QSettings settings;
    settings.setValue(MinimumKey, minimum);
    settings.setValue(MaximumKey, maximum);
    emit colorRangeChanged();
    return true;
}
void ImageDisplaySettings::setRotationDegrees(int degrees)
{
    const int rotation = ImagePresentation::normalizedRotation(degrees);
    if (rotation == m_rotation)
        return;
    m_rotation = rotation;
    QSettings().setValue(RotationKey, rotation);
    emit rotationChanged(rotation);
}

void ImageDisplaySettings::setBlackOutsideRange(bool enabled)
{
    if(enabled==m_blackOutside)return;
    m_blackOutside=enabled;QSettings().setValue(BlackKey,enabled);
    emit blackOutsideRangeChanged(enabled);
}
