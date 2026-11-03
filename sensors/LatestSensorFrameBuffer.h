#pragma once

#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>

// A producer writes directly to this slot from its acquisition thread.  The
// GUI polls it at its own refresh rate, so a 120 fps producer never posts 120
// queued GUI events.  Frame copies are cheap when their Qt containers are
// implicitly shared.
template <typename Frame>
class LatestSensorFrameBuffer
{
public:
    void publish(const Frame &frame)
    {
        QMutexLocker locker(&m_mutex);
        m_latest = frame;
        ++m_generation;
        m_hasFrame = true;
    }

    bool takeLatest(Frame *frame, quint64 *generation = nullptr,
                    quint64 *droppedSinceLastTake = nullptr)
    {
        if (!frame)
            return false;
        QMutexLocker locker(&m_mutex);
        if (!m_hasFrame || m_generation == m_consumedGeneration)
            return false;
        if (droppedSinceLastTake) {
            *droppedSinceLastTake = m_generation > m_consumedGeneration + 1
                ? m_generation - m_consumedGeneration - 1 : 0;
        }
        *frame = m_latest;
        m_consumedGeneration = m_generation;
        if (generation)
            *generation = m_generation;
        return true;
    }

    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_hasFrame = false;
        m_generation = 0;
        m_consumedGeneration = 0;
        m_latest = Frame();
    }

    quint64 publishedCount() const
    {
        QMutexLocker locker(&m_mutex);
        return m_generation;
    }

private:
    mutable QMutex m_mutex;
    Frame m_latest;
    quint64 m_generation = 0;
    quint64 m_consumedGeneration = 0;
    bool m_hasFrame = false;
};
