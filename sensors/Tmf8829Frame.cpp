#include "Tmf8829Frame.h"

#include <cmath>
#include <limits>

const Tmf8829Peak *Tmf8829Pixel::nearestValidPeak() const
{
    const Tmf8829Peak *nearest = nullptr;
    const int count = qMin<int>(peakCount, static_cast<int>(peaks.size()));
    for (int index = 0; index < count; ++index) {
        const Tmf8829Peak &peak = peaks[static_cast<size_t>(index)];
        if (!peak.valid || !std::isfinite(peak.distanceMm))
            continue;
        if (!nearest || peak.distanceMm < nearest->distanceMm)
            nearest = &peak;
    }
    return nearest;
}

bool Tmf8829Frame::isSupportedResolution(int frameWidth, int frameHeight)
{
    return (frameWidth == 8 && frameHeight == 8)
        || (frameWidth == 16 && frameHeight == 16)
        || (frameWidth == 32 && frameHeight == 32)
        || (frameWidth == 48 && frameHeight == 32);
}

double Tmf8829Frame::rawQuarterMillimetersToMm(quint16 rawDistance)
{
    // This conversion belongs only at a raw hardware-frame adapter boundary.
    // Official JSON loggers already write peaks[].distance in millimetres.
    return static_cast<double>(rawDistance) * 0.25;
}

bool Tmf8829Frame::isValid(QString *reason) const
{
    const auto fail = [reason](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };
    if (!isSupportedResolution(width, height))
        return fail(QStringLiteral("unsupported TMF8829 resolution"));
    if (pixels.size() != width * height)
        return fail(QStringLiteral("TMF8829 pixel count does not match resolution"));
    for (const Tmf8829Pixel &pixel : pixels) {
        if (pixel.peakCount > pixel.peaks.size())
            return fail(QStringLiteral("TMF8829 pixel has more than four peaks"));
        if (!pixel.valid)
            continue;
        if (!pixel.nearestValidPeak())
            return fail(QStringLiteral("valid TMF8829 pixel has no valid peak"));
        for (int index = 0; index < pixel.peakCount; ++index) {
            const Tmf8829Peak &peak = pixel.peaks[static_cast<size_t>(index)];
            if (peak.valid && (!std::isfinite(peak.distanceMm)
                               || peak.distanceMm < 0.0)) {
                return fail(QStringLiteral("TMF8829 peak distance is invalid"));
            }
        }
    }
    if (reason)
        reason->clear();
    return true;
}

const Tmf8829Pixel *Tmf8829Frame::pixelAt(int row, int column) const
{
    if (row < 0 || row >= height || column < 0 || column >= width)
        return nullptr;
    const int index = row * width + column;
    return index >= 0 && index < pixels.size() ? &pixels.at(index) : nullptr;
}
