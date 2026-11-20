#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace DepthMeasurement {
struct Options {
    QString parentDirectory;
    QString source;
    QString scene;
    int seconds = 30;
    double measuredDistanceMm = 0;
};

struct Frame {
    QByteArray depth;
    QJsonObject format;
    QJsonObject metadata;
    qint64 sequence = 0;
    qint64 sourceTimeNs = 0;
    qint64 hostArrivalNs = 0;
};

struct Result {
    QString directory;
    QString status;
    QString detail;
    qint64 frames = 0;
    qint64 spanNs = 0;
};

// RV's producer runs on the GUI thread and never waits for disk writes.
class Job {
public:
    void submit(Frame frame);
    void stop(const QString &reason = {});
    std::optional<Frame> take();
    QString stopReason() const;
    std::atomic_bool stopping{false};
    std::atomic<qint64> frames{0};
    std::atomic<qint64> elapsedMs{0};

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Frame> m_queue;
    QString m_reason;
};

qint64 nowNs();
using Reader = std::function<std::optional<Frame>()>;
Result record(const Options &options, Job &job, const Reader &next);
Result recordGemini(const Options &options, Job &job);
}
