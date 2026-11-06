#pragma once
#include "TactileFrame.h"

namespace tactile {
enum class AutoZeroPhase { Inactive, Collecting, Complete, Failed };
struct AutoZeroResult {
    AutoZeroPhase phase = AutoZeroPhase::Inactive;
    Frame baseline; // Display-only rounded mean, or the latest valid frame at the deadline.
    bool started = false;
    int sampleCount = 0, elapsedMs = 0;
};

// Runs on every native reply in the serial worker, before the latest-frame
// mailbox can coalesce samples. No device writes and no changes to raw frames.
class TactileAutoZero {
public:
    static constexpr int PollIntervalMs = 10, SettleMs = 150;
    static constexpr int EarliestMs = 500, DeadlineMs = 900;
    static constexpr int WindowMs = 300, MinimumSpanMs = 200, MinimumFrames = 6;
    static constexpr int MaximumGapMs = 120;
    static constexpr double MaximumRange = 150, MaximumMeanDrift = 40;
    explicit TactileAutoZero(bool enabled = true);
    void observe(const Frame &frame, qint64 nowMs);
    void advance(qint64 nowMs);
    void cancel();
    bool collecting() const { return m_result.phase == AutoZeroPhase::Collecting; }
    int remainingMs(qint64 nowMs) const;
    const AutoZeroResult &result() const { return m_result; }
private:
    struct Sample { Frame frame; qint64 ms; };
    bool stableMean(Frame &baseline) const;
    AutoZeroResult m_result;
    QVector<Sample> m_window;
    Frame m_last;
    qint64 m_startMs = -1, m_lastMs = -1;
};
}
