#pragma once
#include "DepthSnapshot.h"
#include "RvAssistFusion.h"

// One bounded current-frame payload. Nothing is written until an explicit save.
namespace RvDepthDiagnostic {
inline constexpr int Pixels=576*324, DepthBytes=Pixels*4, ReasonBytes=1536;
inline constexpr int PayloadBytes=DepthBytes+2*ReasonBytes;
// Fixed-capacity reason slots preserve v1 IPC; native16 uses slots 0..255,
// with the remaining slots explicitly NoMeasurement, never extra ToF samples.
QByteArray encode(const std::vector<float> &mono,const RvAssistFusion::Result &result,int nativeCount=ReasonBytes);
bool validPayload(const QByteArray &payload);
struct Snapshot {
    DepthSnapshot output;
    RvTofFrame native;
    QByteArray payload;
    QImage viewport;
    bool valid()const;
    bool save(const QString &parent,QString *result)const;
};
// Deterministic CPU-only replay of saved monocular output and native ToF.
// Optional explicit comparison directory exports derived depths/images only;
// existing directories and the original snapshot are never overwritten.
int replay(const QString &folder,const QString &reportPath,const QString &comparisonDirectory=QString());
}
