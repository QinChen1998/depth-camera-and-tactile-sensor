#pragma once

#include "PointCloudColorScale.h"
#include "RvTofFrame.h"
#include "RvPointCloudCanvas.h"
#include <QTimer>
#include <QVector3D>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class RvSessionController;

class RvPointCloudWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RvPointCloudWidget(RvSessionController *session, QWidget *parent = nullptr);
    void setRenderingActive(bool active);

protected:
    void changeEvent(QEvent *event) override;

private:
    struct NativeFrame {
        QVector<SceneInstanceBuffer::InstanceTableEntry> points;
        QVector<quint32> indices;
    };
    using MeshVertex = RvPointCloudCanvas::Vertex;
    void acceptTofFrame(const RvTofFrame &frame);
    void setPaused(bool paused);
    void clearFrames();
    void presentLatest();
    NativeFrame projectTofFrame(bool mesh) const;
    void presentNativeFrame();
    void colorNativeFrame(NativeFrame &frame) const;
    void setupDisplayControls(QVBoxLayout &layout);
    void applyDepthColorRange();
    void refreshColors();
    void updateColorLegend();
    void updateStatus();
    void retranslate();

    RvSessionController *const m_session;
    SceneInstanceBuffer *const m_instances;
    RvPointCloudCanvas *const m_view;
    QLabel *const m_status;
    QComboBox *const m_displayMode;
    QComboBox *const m_colorMode;
    QPushButton *const m_pause;
    QPushButton *const m_reset;
    QWidget *const m_rangeControls;
    QLabel *const m_colorLegend;
    QLabel *const m_legendMinimum;
    QLabel *const m_legendMaximum;
    QLabel *const m_rangeLabel;
    QLabel *const m_rangeSeparator;
    QDoubleSpinBox *const m_colorMinimum;
    QDoubleSpinBox *const m_colorMaximum;
    QPushButton *const m_applyRange;
    QTimer m_expiry;
    RvTofFrame m_latestTof;
    double m_depthColorMinimum = PointCloudColorScale::DefaultMinimumM;
    double m_depthColorMaximum = PointCloudColorScale::DefaultMaximumM;
    bool m_renderingActive = false;
};
