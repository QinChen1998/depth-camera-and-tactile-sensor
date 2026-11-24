#include "DepthMeasurement.h"
#include "RvCalibration.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <chrono>
#include <stdexcept>

namespace DepthMeasurement {
namespace {
constexpr qint64 Second = 1'000'000'000;
constexpr size_t QueueLimit = 16;

void fail(const QString &detail)
{
    throw std::runtime_error(detail.toUtf8().constData());
}

void append(QFile &file, const QByteArray &data)
{
    if (file.write(data) != data.size())
        fail(file.errorString());
}

bool save(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
}

qint64 nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Job::submit(Frame frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (stopping.load())
            return;
        if (m_queue.size() == QueueLimit) {
            m_reason = QStringLiteral("Disk writer fell behind the incoming frames");
            stopping.store(true);
        } else {
            m_queue.push_back(std::move(frame));
        }
    }
    m_ready.notify_one();
}

void Job::stop(const QString &reason)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (stopping.load())
            return;
        m_reason = reason;
        stopping.store(true);
    }
    m_ready.notify_one();
}

std::optional<Frame> Job::take()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ready.wait_for(lock, std::chrono::milliseconds(200),
                     [this] { return stopping.load() || !m_queue.empty(); });
    if (stopping.load() || m_queue.empty())
        return std::nullopt;
    auto frame = std::move(m_queue.front());
    m_queue.pop_front();
    return frame;
}

QString Job::stopReason() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_reason;
}

Result record(const Options &options, Job &job, const Reader &next)
{
    Result result;
    QJsonObject session;
    QFile payload;
    QFile index;
    qint64 firstTime = 0;
    qint64 lastTime = 0;
    qint64 lastSequence = 0;
    qint64 sequenceGaps = 0;
    qint64 lastArrival = 0;
    qint64 firstArrival = 0;
    qint64 payloadBytes = 0;
    QJsonObject format;
    try {
        if (options.seconds < 30 || options.seconds > 60)
            fail(QStringLiteral("Recording duration must be 30 to 60 seconds"));
        QDir root(options.parentDirectory);
        if (options.parentDirectory.isEmpty() || !root.exists())
            fail(QStringLiteral("Select an existing output folder"));
        const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz'Z'");
        const QString folder = "measurement_" + options.source + "_" + stamp + "_"
            + QUuid::createUuid().toString(QUuid::Id128).left(8);
        if (!root.mkdir(folder))
            fail(QStringLiteral("Cannot create the recording folder"));
        result.directory = root.absoluteFilePath(folder);
        const auto file = [&](const QString &name) { return QDir(result.directory).filePath(name); };
        session = {{"schema", 1}, {"status", "recording"}, {"source", options.source},
                   {"scene", options.scene}, {"requested_seconds", options.seconds},
                   {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                   {"reference_distance_mm", options.measuredDistanceMm > 0
                        ? QJsonValue(options.measuredDistanceMm) : QJsonValue(QJsonValue::Null)},
                   {"depth_file", "depth.bin"}, {"frame_index_file", "frames.jsonl"},
                   {"host_arrival_clock", "steady_clock_ns"}};
        if (!save(file("session.json"), QJsonDocument(session).toJson()))
            fail(QStringLiteral("Cannot write the recording metadata"));
        payload.setFileName(file("depth.bin"));
        index.setFileName(file("frames.jsonl"));
        if (!payload.open(QIODevice::WriteOnly) || !index.open(QIODevice::WriteOnly))
            fail(QStringLiteral("Cannot open the recording files"));

        const qint64 start = nowNs();
        while (!job.stopping.load()) {
            const auto frame = next();
            const auto now = nowNs();
            if (!frame) {
                if (!job.stopping.load() && now - (lastArrival > 0 ? lastArrival : start)
                    > (lastArrival > 0 ? 4 : 35) * Second)
                    fail(QStringLiteral("Timed out waiting for depth frames"));
                continue;
            }
            if (job.stopping.load())
                break;
            const qint64 time = frame->sourceTimeNs;
            if (frame->sequence <= lastSequence || time <= lastTime || frame->depth.isEmpty()
                || frame->hostArrivalNs <= 0)
                fail(QStringLiteral("Invalid or non-monotonic depth frame"));
            if (lastTime && time - lastTime > 4 * Second)
                fail(QStringLiteral("The depth stream was interrupted"));
            if (firstArrival && now - firstArrival > (options.seconds + 15) * Second)
                fail(QStringLiteral("Depth timestamps did not cover the requested duration"));

            if (!firstTime) {
                firstTime = time;
                firstArrival = frame->hostArrivalNs;
                format = frame->format;
                format["frame_bytes"] = frame->depth.size();
                if (format["encoding"] == "rv_uq2_snr_valid") {
                    QFile geometry{QLatin1String(RvCalibration::GeometryResource)};
                    if (!geometry.open(QIODevice::ReadOnly))
                        fail(QStringLiteral("Cannot read the F6 geometry"));
                    const auto data = geometry.readAll();
                    if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != RvCalibration::GeometrySha
                        || !save(file("geometry.bin"), data))
                        fail(QStringLiteral("Cannot save verified F6 geometry"));
                    format["geometry_file"] = "geometry.bin";
                    format["geometry_sha256"] = QLatin1String(RvCalibration::GeometrySha);
                }
                session["format"] = format;
                if (!save(file("session.json"), QJsonDocument(session).toJson()))
                    fail(QStringLiteral("Cannot save the depth format"));
            } else {
                auto current = format;
                current.remove("frame_bytes");
                current.remove("geometry_file");
                current.remove("geometry_sha256");
                if (frame->depth.size() != format["frame_bytes"].toInteger() || frame->format != current)
                    fail(QStringLiteral("The depth profile changed during recording"));
                sequenceGaps += frame->sequence - lastSequence - 1;
            }

            QJsonObject entry{{"sequence", frame->sequence}, {"source_time_ns", time},
                              {"host_arrival_ns", frame->hostArrivalNs}, {"offset", payloadBytes},
                              {"bytes", frame->depth.size()}, {"metadata", frame->metadata}};
            append(payload, frame->depth);
            append(index, QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
            payloadBytes += frame->depth.size();
            lastSequence = frame->sequence;
            lastArrival = frame->hostArrivalNs;
            lastTime = time;
            ++result.frames;
            result.spanNs = time - firstTime;
            job.frames.store(result.frames);
            job.elapsedMs.store(result.spanNs / 1'000'000);
            if (result.spanNs >= options.seconds * Second) {
                result.status = "complete";
                break;
            }
        }
        if (result.status.isEmpty()) {
            result.detail = job.stopReason();
            result.status = result.detail.isEmpty() ? "stopped" : "interrupted";
        }
    } catch (const std::exception &error) {
        result.status = "failed";
        result.detail = QString::fromUtf8(error.what());
    }

    if ((payload.isOpen() && !payload.flush()) || (index.isOpen() && !index.flush())) {
        result.status = "failed";
        result.detail = QStringLiteral("Cannot flush the recording files");
    }
    payload.close();
    index.close();
    if (!result.directory.isEmpty()) {
        session["status"] = result.status;
        session["detail"] = result.detail;
        session["frames"] = result.frames;
        session["span_ns"] = result.spanNs;
        session["depth_bytes"] = payloadBytes;
        session["sequence_gaps"] = sequenceGaps;
        session["ended_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        if (!save(QDir(result.directory).filePath("session.json"), QJsonDocument(session).toJson())) {
            result.status = "failed";
            result.detail = QStringLiteral("Cannot finalize the recording metadata");
        }
    }
    return result;
}
}
