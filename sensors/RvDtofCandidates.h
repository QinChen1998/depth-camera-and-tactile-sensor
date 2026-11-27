#pragma once
#include "RvTofFrame.h"
#include "../tools/rv_capture/Contract.h"
#include <QtCore/QSize>

namespace RvDtofCandidates {
constexpr int NativeHd=RvCaptureContract::NativeHd;
constexpr int Native48=RvCaptureContract::Native48;
constexpr int Native48HighAccuracy=RvCaptureContract::Native48HighAccuracy;
struct SensorDepthPreset {
    int mode;
    int sensorMode;
    int grid;
    bool highAccuracy;
    const char *id;
};
// The board capture mode and sensor -d preset use different numbers.
inline constexpr SensorDepthPreset SensorDepthPresets[] = {
    {RvCaptureContract::Native8, 0, 8, false, "board_native_tof8"},
    {RvCaptureContract::Native8HighAccuracy, 2, 8, true, "board_native_tof8_high_accuracy"},
    {RvCaptureContract::Native16, 5, 16, false, "board_native_tof16"},
    {RvCaptureContract::Native16HighAccuracy, 6, 16, true, "board_native_tof16_high_accuracy"}
};
inline const SensorDepthPreset *sensorDepthPreset(int mode) {
    for (const auto &preset : SensorDepthPresets)
        if (mode == preset.mode) return &preset;
    return nullptr;
}
const SensorDepthPreset *sensorDepthPreset(const QString &scheme);
inline bool isNativeDepth(int mode) {
    return mode == Native48 || mode == Native48HighAccuracy || sensorDepthPreset(mode);
}
inline QSize nativeDepthGrid(int mode) {
    if (const auto *preset = sensorDepthPreset(mode)) return QSize(preset->grid, preset->grid);
    return isNativeDepth(mode) ? QSize(RvCaptureContract::Native48Columns, RvCaptureContract::Native48Rows) : QSize();
}
inline int nativeSensorMode(int mode) {
    if (const auto *preset = sensorDepthPreset(mode)) return preset->sensorMode;
    return RvCaptureContract::sensorMode(mode);
}
uint32_t sdkMode(int mode);
int modeForSdkMode(uint32_t mode);
inline bool supports(int mode){return RvCaptureContract::supports(mode);}
struct Settings {};
inline Settings defaults(int =NativeHd){return {};}
QString name(int mode);
int modeForScheme(const QString &scheme);
}
