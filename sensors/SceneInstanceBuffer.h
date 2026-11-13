#pragma once

#include <QColor>
#include <QQuaternion>
#include <QVector>
#include <QtQuick3D/QQuick3DInstancing>
#include <cstring>
#include <utility>

class SceneInstanceBuffer final : public QQuick3DInstancing {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY instanceTableChanged)

public:
    explicit SceneInstanceBuffer(QObject *owner) { QObject::setParent(owner); }
    int count() const { return m_count; }

    void setEntries(const QVector<InstanceTableEntry> &entries)
    {
        const qsizetype bytes = entries.size() * sizeof(InstanceTableEntry);
        if (bytes == m_buffer.size() && (!bytes || std::memcmp(entries.constData(), m_buffer.constData(), size_t(bytes)) == 0))
            return;
        setTable(QByteArray(reinterpret_cast<const char *>(entries.constData()), bytes));
    }

    void setTable(QByteArray table)
    {
        Q_ASSERT(table.size() % sizeof(InstanceTableEntry) == 0);
        m_count = int(table.size() / sizeof(InstanceTableEntry));
        m_buffer = std::move(table);
        markDirty();
    }

    static InstanceTableEntry entry(const QVector3D &position, const QVector3D &scale,
                                    const QQuaternion &rotation, const QColor &color)
    {
        return calculateTableEntryFromQuaternion(position, scale, rotation, color);
    }

protected:
    QByteArray getInstanceBuffer(int *count) override
    {
        if (count)
            *count = m_count;
        return m_buffer;
    }

private:
    QByteArray m_buffer;
    int m_count = 0;
};
