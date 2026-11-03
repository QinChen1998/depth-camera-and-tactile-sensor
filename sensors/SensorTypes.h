#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QString>

enum class SensorSourceMode
{
    None,
    Mock,
    OfflinePlayback,
    Hardware
};

enum class SensorConnectionState
{
    Disconnected,
    Connected,
    NoData,
    Error
};

inline QString sensorSourceModeName(SensorSourceMode mode)
{
    switch (mode) {
    case SensorSourceMode::Mock: return QStringLiteral("Mock");
    case SensorSourceMode::OfflinePlayback: return QStringLiteral("Offline Playback");
    case SensorSourceMode::Hardware: return QStringLiteral("Hardware");
    case SensorSourceMode::None: break;
    }
    return QStringLiteral("None");
}

inline QString sensorConnectionStateName(SensorConnectionState state)
{
    switch (state) {
    case SensorConnectionState::Disconnected: return QStringLiteral("Disconnected");
    case SensorConnectionState::Connected: return QStringLiteral("Connected");
    case SensorConnectionState::NoData: return QStringLiteral("No Data");
    case SensorConnectionState::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Error");
}

Q_DECLARE_METATYPE(SensorSourceMode)
Q_DECLARE_METATYPE(SensorConnectionState)
