#pragma once
#include "TactileDataSource.h"
#include <QElapsedTimer>
namespace tactile {
class TactileMockSource final : public TactileDataSource {
public:
    void start() override;
    void stop() override;
    void setPaused(bool paused) override;
    SourceState state() const override { return m_state; }
    QUuid sessionId() const override { return m_session; }
    quint64 generation() const override { return m_generation; }
    bool latest(Frame &frame) override;
    void setScenario(Scenario scenario);
    Scenario scenario() const { return m_scenario; }
    // Pure deterministic signal, independent of model state and wall clock.
    static Frame sample(Scenario scenario, double seconds);
private:
    SourceState m_state = SourceState::Stopped;
    Scenario m_scenario = Scenario::PressRelease;
    QUuid m_session;
    quint64 m_generation = 0, m_sequence = 0;
    QElapsedTimer m_clock, m_sessionClock;
    qint64 m_activeNs = 0, m_lastNs = -1;
};
}
