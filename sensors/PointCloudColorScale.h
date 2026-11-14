#pragma once

#include <QtGui/qrgb.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace PointCloudColorScale {
inline constexpr double DefaultMinimumM = 0.01;
inline constexpr double DefaultMaximumM = 3.0;

inline QRgb jet(double normalized)
{
    // Orbbec Viewer samples the Jet ramps at 256 positions and interpolates
    // between samples before converting to RGB8.
    static const auto samples = [] {
        std::array<std::array<float, 3>, 256> table{};
        for (int i = 0; i < int(table.size()); ++i) {
            const double t = double(i) / 255.0;
            for (int channel = 0; channel < 3; ++channel)
                table[i][channel] = float(std::clamp(1.5 - std::abs(4.0 * t - (3 - channel)), 0.0, 1.0));
        }
        return table;
    }();
    const float index = float(std::clamp(normalized, 0.0, 1.0) * 255.0);
    const int lower = int(index);
    const int upper = std::min(lower + 1, 255);
    const float weight = index - lower;
    const auto channel = [&](int i) {
        return int(255.f * (samples[lower][i] + weight * (samples[upper][i] - samples[lower][i])));
    };
    return qRgb(channel(0), channel(1), channel(2));
}

inline QRgb color(double cameraZ, double minimum = DefaultMinimumM, double maximum = DefaultMaximumM)
{
    if (!std::isfinite(cameraZ) || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
        return qRgb(0, 0, 0);
    return jet((cameraZ - minimum) / (maximum - minimum));
}
}
