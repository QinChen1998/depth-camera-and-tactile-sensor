#include "RvDtofCandidates.h"
#include <DepthSdk.h>

namespace RvDtofCandidates {
namespace {
struct SdkPreset {
    int boardMode;
    uint32_t sdkMode;
};
constexpr SdkPreset SdkPresets[] = {
    {Native48, CXWL_DEPTH_BOARD},
    {Native48HighAccuracy, CXWL_DEPTH_BOARD_HIGH_ACCURACY},
    {RvCaptureContract::Native8, CXWL_DEPTH_BOARD_8X8},
    {RvCaptureContract::Native8HighAccuracy, CXWL_DEPTH_BOARD_8X8_HIGH_ACCURACY},
    {RvCaptureContract::Native16, CXWL_DEPTH_BOARD_16X16},
    {RvCaptureContract::Native16HighAccuracy, CXWL_DEPTH_BOARD_16X16_HIGH_ACCURACY},
};
}

const SensorDepthPreset *sensorDepthPreset(const QString &scheme) {
    for (const auto &preset : SensorDepthPresets)
        if (scheme == QLatin1String(preset.id)) return &preset;
    return nullptr;
}
uint32_t sdkMode(int mode) {
    for (const auto &preset : SdkPresets)
        if (mode == preset.boardMode) return preset.sdkMode;
    return 0;
}
int modeForSdkMode(uint32_t mode) {
    for (const auto &preset : SdkPresets)
        if (mode == preset.sdkMode) return preset.boardMode;
    return -1;
}
QString name(int mode){
    if (const auto *preset = sensorDepthPreset(mode)) return QLatin1String(preset->id);
    return mode==Native48?QStringLiteral("board_native_tof48"):
    mode==Native48HighAccuracy?QStringLiteral("board_native_tof48_high_accuracy"):
    mode==NativeHd?QStringLiteral("native_hd_capture"):QString();}
int modeForScheme(const QString &scheme){
    if (const auto *preset = sensorDepthPreset(scheme)) return preset->mode;
    for(int mode:{Native48,Native48HighAccuracy})if(scheme==name(mode))return mode;
    return -1;
}
}
