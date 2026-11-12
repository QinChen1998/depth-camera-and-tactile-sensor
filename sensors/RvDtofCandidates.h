#pragma once
#include "RvTofFrame.h"
#include "../tools/rv_capture/Contract.h"

namespace RvDtofCandidates {
constexpr int NativeHd=RvCaptureContract::NativeHd;
constexpr int Native48=RvCaptureContract::Native48;
constexpr int Native48HighAccuracy=RvCaptureContract::Native48HighAccuracy;
inline bool isNativeDepth(int mode){return mode==Native48||mode==Native48HighAccuracy;}
inline bool supports(int mode){return RvCaptureContract::supports(mode);}
struct Settings {};
inline Settings defaults(int =NativeHd){return {};}
QString name(int mode);
int modeForScheme(const QString &scheme);
}
