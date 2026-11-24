#pragma once

#include <QImage>
#include <QVector3D>
#include <QWidget>

class TactileSceneAdapter;
class QLabel;

class TactileCanvas final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double orbitYaw MEMBER m_yaw)
    Q_PROPERTY(double orbitPitch MEMBER m_pitch)
    Q_PROPERTY(double cameraDistance MEMBER m_distance)
    Q_PROPERTY(QVector3D cameraPan MEMBER m_target)
    Q_PROPERTY(int inspectedPoint READ inspectedPoint WRITE setInspectedPoint)

public:
    explicit TactileCanvas(TactileSceneAdapter *scene, QWidget *parent = nullptr);
    bool ready() const;
    int inspectedPoint() const { return m_inspectedPoint; }
    void setInspectedPoint(int point);
    void resetView();
    void frontView();
    QVector3D project(const QVector3D &metres) const;
    int pickPoint(const QPointF &position) const;
    QImage grabFramebuffer();

signals:
    void painted();

protected:
    void changeEvent(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    void fitModel();
    void fitDots(bool expandOnly);
    void redrawHousing();

    TactileSceneAdapter *const m_scene;
    QLabel *const m_unavailable;
    QImage m_housing;
    QVector3D m_drawnTarget, m_target;
    double m_drawnYaw = 0, m_drawnPitch = 0, m_drawnDistance = 0;
    double m_yaw = -35, m_pitch = -25, m_distance = 65;
    double m_framedGain = -1;
    int m_inspectedPoint = -1;
    bool m_autoFrame = true, m_dotMatrix = false, m_dragged = false;
    Qt::MouseButton m_pressedButton = Qt::NoButton;
    QPointF m_pressPosition, m_previousPosition;
};
