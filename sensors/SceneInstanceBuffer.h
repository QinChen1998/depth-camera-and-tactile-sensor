#pragma once

#include <QColor>
#include <QObject>
#include <QVector>
#include <QVector3D>
#include <QVector4D>
#include <cstring>
#include <utility>

class SceneInstanceBuffer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY instanceTableChanged)

public:
    struct InstanceTableEntry {
        QVector3D position;
        float diameter;
        QVector4D color;
        QVector3D getPosition() const { return position; }
    };

    explicit SceneInstanceBuffer(QObject *owner) : QObject(owner) {}
    int count() const { return m_count; }
    const QByteArray &table() const { return m_buffer; }
    InstanceTableEntry instance(int index) const {
        Q_ASSERT(index >= 0 && index < count());
        InstanceTableEntry instance;
        std::memcpy(&instance, m_buffer.constData() + qsizetype(index) * sizeof(instance), sizeof(instance));
        return instance;
    }
    QVector3D instancePosition(int index) const { return instance(index).position; }
    QColor instanceColor(int index) const {
        const auto c = instance(index).color;
        return QColor::fromRgbF(c.x(), c.y(), c.z(), c.w());
    }

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
        emit instanceTableChanged();
    }

signals:
    void instanceTableChanged();

private:
    QByteArray m_buffer;
    int m_count = 0;
};
