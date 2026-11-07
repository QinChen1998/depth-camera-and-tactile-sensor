#include "TactileAutoZero.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tactile {
TactileAutoZero::TactileAutoZero(bool enabled) {
    if(enabled)m_result.phase=AutoZeroPhase::Collecting;
}
int TactileAutoZero::remainingMs(qint64 nowMs) const {
    return collecting()&&m_startMs>=0?int(std::max(qint64(0),DeadlineMs-(nowMs-m_startMs))):-1;
}
void TactileAutoZero::advance(qint64 nowMs) {
    if(!collecting()||m_startMs<0)return;
    m_result.elapsedMs=int(std::max(qint64(0),nowMs-m_startMs));
    if(m_result.elapsedMs>=DeadlineMs) {
        // One automatic single-frame tare at the deadline, using only the
        // latest CRC/range-valid measurement from this session. Explicit
        // cancellation still exits without establishing a zero.
        if(m_last.valid&&m_last.crcOk&&m_last.rangeOk&&validateFrame(m_last)) {
            m_result.baseline=m_last;m_result.sampleCount=1;
            m_result.phase=AutoZeroPhase::Complete;m_window.clear();
        } else cancel();
    }
}
void TactileAutoZero::cancel() {
    if(collecting()){m_result.phase=AutoZeroPhase::Failed;m_window.clear();}
}
void TactileAutoZero::observe(const Frame &frame,qint64 nowMs) {
    if(!collecting())return;
    const bool valid=frame.sourceMode==QStringLiteral("Serial")&&frame.crcOk&&frame.rangeOk&&validateFrame(frame);
    const bool sameSession=!m_last.valid||(frame.sessionId==m_last.sessionId&&frame.generation==m_last.generation
        &&frame.hardwareType==m_last.hardwareType&&frame.sequence>m_last.sequence&&frame.monotonicNs>m_last.monotonicNs);
    if(!valid||!sameSession||nowMs<m_lastMs){advance(nowMs);return;}
    if(m_startMs<0){m_startMs=nowMs;m_result.started=true;}
    if(m_lastMs>=0&&nowMs-m_lastMs>MaximumGapMs)m_window.clear();
    m_last=frame;m_lastMs=nowMs;m_result.elapsedMs=int(nowMs-m_startMs);
    if(m_result.elapsedMs>=SettleMs) {
        m_window.append({frame,nowMs});
        while(!m_window.isEmpty()&&m_window.front().ms<nowMs-WindowMs)m_window.removeFirst();
        m_result.sampleCount=int(m_window.size());
        Frame baseline;
        if(m_result.elapsedMs>=EarliestMs&&m_result.elapsedMs<=DeadlineMs&&stableMean(baseline)) {
            m_result.baseline=std::move(baseline);m_result.phase=AutoZeroPhase::Complete;
            m_window.clear();return;
        }
    }
    advance(nowMs);
}
bool TactileAutoZero::stableMean(Frame &baseline) const {
    const int count=int(m_window.size()),half=count/2;
    if(count<MinimumFrames||m_window.back().ms-m_window.front().ms<MinimumSpanMs)return false;
    baseline=m_window.back().frame;
    auto &finger=baseline.fingers[baseline.hardwareType];auto &module=finger.modules[0];
    module.resultant={};
    for(int point=0;point<module.points.size();++point) {
        double means[3]{};
        for(int axis=0;axis<3;++axis) {
            double low=std::numeric_limits<double>::max(),high=-low,first=0,second=0;
            for(int i=0;i<count;++i) {
                const auto &f=m_window[i].frame.fingers[baseline.hardwareType].modules[0].points[point].force;
                const double value=axis==0?f.x:axis==1?f.y:f.z;
                low=std::min(low,value);high=std::max(high,value);
                (i<half?first:second)+=value;
            }
            if(high-low>MaximumRange||std::abs(first/half-second/(count-half))>MaximumMeanDrift)return false;
            means[axis]=std::round((first+second)/count);
        }
        module.points[point].force={means[0],means[1],means[2]};module.resultant+=module.points[point].force;
    }
    finger.resultant=module.resultant;
    return validateFrame(baseline);
}
}
