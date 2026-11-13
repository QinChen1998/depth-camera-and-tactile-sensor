#pragma once

#include "RvTofFrame.h"
#include <QFutureWatcher>
#include <QSharedPointer>
#include <QTimer>
#include <QVector3D>
#include <QWidget>

class ImageDisplaySettings;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QQuickWidget;
class RvSessionController;
class SceneInstanceBuffer;
namespace RvCameraView { struct Profile; }

class RvPointCloudWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RvPointCloudWidget(RvSessionController *session, QWidget *parent = nullptr);
    void setRenderingActive(bool active);

protected:
    void changeEvent(QEvent *event) override;

private:
    void acceptTofFrame(const RvTofFrame &frame);
    void acceptAssistedDepth(const QByteArray &depth, const QJsonObject &metadata);
    void clearFrames();
    void clearAssistedDepth();
    void presentLatest();
    void presentTofFrame();
    void startDepthConversion();
    void finishDepthConversion();
    static QByteArray projectDepth(const QByteArray &depth, const RvCameraView::Profile &camera,
                                  double colorMinimum, double colorMaximum);
    void setupDisplayControls();
    void applyDepthColorRange();
    void updateColorLegend();
    void updateStatus();
    void retranslate();

    RvSessionController *const m_session;
    const QSharedPointer<ImageDisplaySettings> m_displaySettings;
    SceneInstanceBuffer *const m_instances;
    SceneInstanceBuffer *const m_depthInstances;
    QQuickWidget *const m_view;
    QLabel *const m_status;
    QPushButton *const m_reset;
    QWidget *const m_depthControls;
    QLabel *const m_legendMinimum;
    QLabel *const m_legendMaximum;
    QLabel *const m_diameterLabel;
    QLabel *const m_rangeLabel;
    QLabel *const m_rangeSeparator;
    QDoubleSpinBox *const m_diameter;
    QDoubleSpinBox *const m_colorMinimum;
    QDoubleSpinBox *const m_colorMaximum;
    QPushButton *const m_applyRange;
    QTimer m_expiry;
    RvTofFrame m_latestTof;
    QByteArray m_latestDepth;
    QFutureWatcher<QByteArray> m_depthConversion;
    quint64 m_depthEpoch = 0;
    quint64 m_conversionEpoch = 0;
    double m_depthColorMinimum = 0.01;
    double m_depthColorMaximum = 3.0;
    bool m_hasDepthFrame = false;
    bool m_depthDirty = false;
    bool m_depthConverting = false;
    bool m_renderingActive = false;
};
