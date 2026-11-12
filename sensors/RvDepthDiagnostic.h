#pragma once
#include "DepthSnapshot.h"
#include "RvAssistFusion.h"
#include <memory>

// One bounded current-frame payload. Nothing is written until an explicit save.
namespace RvDepthDiagnostic {
inline constexpr int Pixels=640*480, DepthBytes=Pixels*4, ReasonBytes=1536;
inline constexpr int PayloadBytes=DepthBytes+2*ReasonBytes;
// Fixed-capacity reason slots preserve v1 IPC; native16 uses slots 0..255,
// with the remaining slots explicitly NoMeasurement, never extra ToF samples.
QByteArray encode(const std::vector<float> &mono,const RvAssistFusion::Result &result);
bool validPayload(const QByteArray &payload,int depthBytes=DepthBytes);
bool matchesPairedNative(const QJsonObject &metadata,const RvTofFrame &native);
struct Snapshot {
    DepthSnapshot output;
    RvTofFrame native;
    QByteArray payload;
    QImage viewport;
    bool valid()const;
    bool save(const QString &parent,QString *result)const;
};
// Explicit ten-second GUI capture. Bounded background writer; no model or
// camera owner. Each packet retains the actual 672x504 calibrated RGB input.
class SequenceRecorder {
public:
    SequenceRecorder();
    ~SequenceRecorder();
    bool start(const QString &parent,QString *error);
    void append(const QByteArray &depth,const QJsonObject &metadata,const QImage &input,
                const RvTofFrame &native,const QByteArray &diagnostic);
    void stop();
    bool finished()const;
    QJsonObject status()const;
private:
    struct State;
    std::unique_ptr<State> state;
};
// Deterministic CPU-only replay of saved monocular output and native ToF.
// Optional explicit comparison directory exports derived depths/images only;
// existing directories and the original snapshot are never overwritten.
int replay(const QString &folder,const QString &reportPath,const QString &comparisonDirectory=QString());
}
