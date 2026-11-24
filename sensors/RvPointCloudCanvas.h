#pragma once
#include "SceneInstanceBuffer.h"
#include <QWidget>
#include <QVector3D>
#include <QVector4D>
#include <QVector>

/* Qt Widgets raster renderer. No Qt Quick 3D, QML or GPL-only module. */
class RvPointCloudCanvas final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QVector3D cameraTarget MEMBER m_target)
    Q_PROPERTY(double cameraDistance MEMBER m_distance)
    Q_PROPERTY(double orbitYaw MEMBER m_yaw)
    Q_PROPERTY(double orbitPitch MEMBER m_pitch)
    Q_PROPERTY(bool meshVisible READ meshVisible)
    Q_PROPERTY(double depthPointDiameter READ depthPointDiameter WRITE setDepthPointDiameter)
public:
    struct Vertex { QVector3D position; QVector4D color; };
    explicit RvPointCloudCanvas(SceneInstanceBuffer *native, SceneInstanceBuffer *assisted, QWidget *parent = nullptr);
    void setMesh(const QByteArray &vertices, const QByteArray &indices);
    void setAssisted(bool enabled);
    void resetView();
    void setDepthPointDiameter(double diameter);
    double depthPointDiameter() const { return m_depthPointDiameter; }
    bool meshVisible() const { return !m_indices.isEmpty(); }
    const QByteArray &vertexData() const { return m_vertices; }
    const QByteArray &indexData() const { return m_indices; }
    int stride() const { return sizeof(Vertex); }
    QImage grabFramebuffer() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
private:
    QImage renderFrame() const;
    bool hasPoints() const { return m_native->count() || m_assisted->count() || meshVisible(); }
    SceneInstanceBuffer *const m_native;
    SceneInstanceBuffer *const m_assisted;
    QByteArray m_vertices, m_indices;
    QVector3D m_target{0, 0, -600};
    double m_distance = 900, m_yaw = -25, m_pitch = -18;
    double m_depthPointDiameter = 2.4;
    bool m_showAssisted = false;
    QPointF m_previous;
};
