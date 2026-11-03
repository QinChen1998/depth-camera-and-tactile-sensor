#include "TactileMockSource.h"
#include <algorithm>
#include <cmath>
namespace tactile {
Frame TactileMockSource::sample(Scenario scenario, double seconds)
{
    Frame f;
    if(!std::isfinite(seconds)||seconds<0 || scenario<Scenario::NoContact || scenario>Scenario::MovingContact) return f;
    f.valid=true;
    // Fixed identity only for pure sample fixtures; live start() supplies a unique identity.
    f.sessionId=QUuid("{6f0c0e93-4838-4c52-a418-4ed876c72327}"); f.generation=1; f.sequence=1;
    for(int i=0;i<3;++i) {
        Finger finger; finger.id=sensorLayout().fingerIds[i];
        const double phase=seconds*1.2+i*0.43;
        for(int m=0;m<1;++m) {
            Module module; module.id=QString("M%1").arg(m+1);
            for(int p=0;p<sensorLayout(i).points.size();++p) {
                Point point; point.id=QString("P%1").arg(p+1);
                double z=0;
                const double gain=(0.35+0.11*i)*(0.7+0.1*(p%4));
                if(scenario==Scenario::PressRelease) z=gain*(0.5+0.5*std::sin(phase));
                if(scenario==Scenario::Tangential) z=gain;
                if(scenario==Scenario::MovingContact) {
                    const double center=1+std::sin(phase);
                    z=gain*std::exp(-4*std::pow(2.0*p/(sensorLayout(i).points.size()-1)-center,2));
                }
                // Keep simulated point amplitudes within 2500 counts while preserving each waveform.
                // Integer raw-count mock only; this scale is not a hardware limit or counts-to-N calibration.
                constexpr double maximumCounts=2500;
                point.force={std::round(maximumCounts*z*0.95*std::sin(phase+p*0.3)),
                    std::round(maximumCounts*z*0.85*std::cos(phase+p*0.5)),std::round(maximumCounts*z)};
                module.points.append(point); module.resultant+=point.force;
            }
            finger.modules.append(module); finger.resultant+=module.resultant;
        }
        f.fingers.append(finger);
    }
    return f;
}
void TactileMockSource::start()
{
    if(m_state!=SourceState::Stopped) return;
    m_session=QUuid::createUuid(); ++m_generation; m_sequence=0; m_activeNs=0; m_lastNs=-1;
    m_clock.start(); m_sessionClock.start(); m_state=SourceState::Running;
}
void TactileMockSource::stop() { m_state=SourceState::Stopped; m_clock.invalidate(); }
void TactileMockSource::setPaused(bool paused)
{
    if(paused && m_state==SourceState::Running) {
        m_activeNs+=m_clock.nsecsElapsed(); m_state=SourceState::Paused;
    } else if(!paused && m_state==SourceState::Paused) { m_clock.restart(); m_state=SourceState::Running; }
}
void TactileMockSource::setScenario(Scenario s)
{
    if(s<Scenario::NoContact || s>Scenario::MovingContact || s==m_scenario) return;
    const auto old=m_state; stop(); m_scenario=s;
    if(old!=SourceState::Stopped) { start(); setPaused(old==SourceState::Paused); }
}
bool TactileMockSource::latest(Frame &f)
{
    if(m_state!=SourceState::Running) return false;
    const qint64 now=m_activeNs+m_clock.nsecsElapsed();
    const qint64 timestamp=m_sessionClock.nsecsElapsed();
    if(timestamp<=m_lastNs) return false;
    f=sample(m_scenario,now/1e9); f.sessionId=m_session; f.generation=m_generation;
    f.sequence=++m_sequence; f.monotonicNs=timestamp; m_lastNs=timestamp;
    return true;
}
}
