#pragma once
#include <QPointF>
#include <QStringList>
#include <QVector>
#include <QUuid>

namespace tactile {
struct Force {
    double x = 0, y = 0, z = 0; // Magnetic tactile: raw counts; glove: simulated N.
    Force &operator+=(const Force &v) { x += v.x; y += v.y; z += v.z; return *this; }
};
struct PointLayout { QString moduleId, pointId; QPointF position; };
struct Layout {
    QString id = "simulated-five-finger-v1";
    int version = 1;
    QString axes = "SIMULATED: +X right, +Y up, +Z compression";
    QStringList fingerIds;
    QVector<PointLayout> points;
};
const Layout &demoLayout(); // Independent glove legacy layout.
const Layout &sensorLayout(int type = 0);
struct Point { QString id; Force force; bool valid = true; };
struct Module { QString id; QVector<Point> points; Force resultant; bool valid = true; };
struct Finger { QString id; QVector<Module> modules; Force resultant; bool valid = true; };
struct Frame {
    int schemaVersion = 1, layoutVersion = 1;
    QString layoutId = "simulated-tactile-counts-v3", sourceMode = "Mock";
    QUuid sessionId;
    quint64 generation = 0, sequence = 0;
    qint64 monotonicNs = 0;
    bool valid = false;
    int hardwareType = -1; // Serial frames carry only this type; other slots are unavailable.
    bool crcVerified = true, crcOk = true, rangeOk = true;
    QVector<Finger> fingers;
};
// Canonical ID order is part of v1. Rejects malformed frames and inconsistent sums.
bool validateFrame(const Frame &frame, QString *error = nullptr);
bool sameForce(const Force &a, const Force &b);
// Per-session gate also rejects stale generations, repeated sequences and timestamps.
class FrameGate {
public:
    void reset(const QUuid &session, quint64 generation);
    bool accept(const Frame &frame, QString *error = nullptr);
private:
    QUuid m_session;
    quint64 m_generation = 0, m_sequence = 0;
    qint64 m_time = -1;
};
}
