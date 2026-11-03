#pragma once
#include "TactileFrame.h"
#include <array>
#include <deque>
#include <cmath>
namespace tactile {
// Display-only gate. Never alters raw acquisition or its diagnostic snapshot.
class TactileDisplayFilter {
public:
    Frame last;
    QString reason;
    quint64 rejected=0;
    void reset() { last={};reason.clear();rejected=0; }
    bool accept(const Frame &frame,double threshold) {
        reason.clear();
        if(!validateFrame(frame))reason="invalid frame";
        else if(!frame.crcOk)reason="CRC mismatch";
        else if(!frame.rangeOk)reason="outside manual range";
        else if(last.valid&&frame.sessionId==last.sessionId&&frame.generation==last.generation) {
            if(frame.sequence<=last.sequence||frame.monotonicNs<=last.monotonicNs)reason="stale frame";
            else for(int f=0;f<frame.fingers.size()&&reason.isEmpty();++f) {
                if(!frame.fingers[f].valid||!last.fingers[f].valid)continue;
                const auto &points=frame.fingers[f].modules[0].points;
                const auto &previous=last.fingers[f].modules[0].points;
                for(int p=0;p<points.size();++p) {
                    const auto a=points[p].force,b=previous[p].force;
                    if(std::abs(a.x-b.x)>threshold||std::abs(a.y-b.y)>threshold||std::abs(a.z-b.z)>threshold) {
                        reason=QString("P%1 jump > %2 counts").arg(p+1).arg(threshold);break;
                    }
                }
            }
        }
        if(!reason.isEmpty()){++rejected;return false;}
        last=frame;return true;
    }
};
inline bool matchingBaseline(const Frame &frame,const Frame &baseline) {
    return frame.valid&&baseline.valid&&frame.sessionId==baseline.sessionId&&frame.generation==baseline.generation;
}
inline Force relativeForce(const Force &raw,const Force &baseline) {
    return {raw.x-baseline.x,raw.y-baseline.y,raw.z-baseline.z};
}
// Display only: apply after subtracting a raw tare baseline. Only X/Y use the
// inclusive deadband; Z always passes through. Acquisition/jump checks stay raw.
inline Force zeroSmallComponents(const Force &value,double threshold) {
    if(!std::isfinite(threshold)||threshold<=0)return value;
    return {std::abs(value.x)<=threshold?0:value.x,std::abs(value.y)<=threshold?0:value.y,value.z};
}
struct HistorySample {
    qint64 timeNs = 0;
    std::array<std::array<Force,2>,3> forces{}; // [sensor type][0 total, 1 module]
    std::array<QVector<Force>,3> pointForces; // Raw measurements only, never interpolated glyphs.
    bool startsSegment = false;
    bool bridgeFromPrevious = false; // Dashed display interpolation, not another measurement.
};
class TactileHistory {
public:
    static constexpr size_t Capacity = 320;
    static constexpr qint64 WindowNs = 10000000000LL;
    static constexpr qint64 BridgeWindowNs = 500000000LL;
    void begin(const QUuid &id, quint64 generation) { m_gate.reset(id,generation); clear(); }
    void clear() { m_samples.clear(); m_break=true; m_bridge=false; }
    void breakSegment(bool allowBridge=false) {
        m_bridge=m_break?(m_bridge&&allowBridge):allowBridge;
        m_break=true;
    }
    bool append(const Frame &frame) {
        if(!m_gate.accept(frame)) return false;
        HistorySample sample; sample.timeNs=frame.monotonicNs; sample.startsSegment=m_break;
        sample.bridgeFromPrevious=m_break&&m_bridge&&!m_samples.empty()
            &&frame.monotonicNs-m_samples.back().timeNs<=BridgeWindowNs;
        m_break=false;m_bridge=false;
        for(int f=0;f<3;++f) { if(!frame.fingers[f].valid)continue; sample.forces[f][0]=frame.fingers[f].resultant;
            for(const auto &point:frame.fingers[f].modules[0].points)sample.pointForces[f].append(point.force);
            for(int m=0;m<1;++m) sample.forces[f][m+1]=frame.fingers[f].modules[m].resultant; }
        m_samples.push_back(sample);
        while(m_samples.size()>Capacity || m_samples.back().timeNs-m_samples.front().timeNs>WindowNs) m_samples.pop_front();
        return true;
    }
    const std::deque<HistorySample> &samples() const { return m_samples; }
private:
    FrameGate m_gate;
    std::deque<HistorySample> m_samples;
    bool m_break=true,m_bridge=false;
};
}
