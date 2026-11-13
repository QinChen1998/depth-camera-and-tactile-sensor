#pragma once
#include "RvAssistFusion.h"

// Current-frame IPC payload for the depth viewport and offline worker checks.
namespace RvDepthDiagnostic {
inline constexpr int Pixels=640*480, DepthBytes=Pixels*4, ReasonBytes=1536;
// Fixed-capacity reason slots preserve v1 IPC; native16 uses slots 0..255,
// with the remaining slots explicitly NoMeasurement, never extra ToF samples.
DCTS_DEPTH_INTERNAL QByteArray encode(const std::vector<float> &mono,const RvAssistFusion::Result &result);
DCTS_DEPTH_INTERNAL bool validPayload(const QByteArray &payload,int depthBytes=DepthBytes);

}
