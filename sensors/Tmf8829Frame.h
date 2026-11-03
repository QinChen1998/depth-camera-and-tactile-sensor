#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <array>

struct Tmf8829Peak
{
    double distanceMm = 0.0;
    quint8 snr = 0;
    quint16 signal = 0;
    bool hasSignal = false;
    bool valid = false;
};

struct Tmf8829Pixel
{
    bool valid = false;
    quint8 peakCount = 0;
    std::array<Tmf8829Peak, 4> peaks{};
    quint16 noise = 0;
    quint16 xtalk = 0;
    bool hasNoise = false;
    bool hasXtalk = false;

    const Tmf8829Peak *nearestValidPeak() const;
};

struct Tmf8829Frame
{
    qint64 timestampNsSinceEpoch = 0;
    qint64 sourceTimestampTicks = 0;
    quint64 frameNumber = 0;
    int width = 0;
    int height = 0;
    QVector<Tmf8829Pixel> pixels;

    static bool isSupportedResolution(int width, int height);
    static double rawQuarterMillimetersToMm(quint16 rawDistance);
    bool isValid(QString *reason = nullptr) const;
    const Tmf8829Pixel *pixelAt(int row, int column) const;
};

Q_DECLARE_METATYPE(Tmf8829Frame)
