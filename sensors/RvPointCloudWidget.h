#pragma once

#include "PointCloudColorScale.h"
#include "RvTofFrame.h"
#include "SceneInstanceBuffer.h"
#include <QFutureWatcher>
#include <QTimer>
#include <QVector3D>
#include <QWidget>
#include <deque>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QQuick3DGeometry;
class QQuickWidget;
class QVBoxLayout;
class RvSessionController;
namespace RvCameraView { struct Profile; }

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
    struct MeshVertex {
        QVector3D position;
        QVector4D color;
    };
    struct MeshData {
        QByteArray vertices, indices;
        QVector3D minimum, maximum;
    };
    struct DepthProjection {
        QByteArray points;
        MeshData mesh;
    };

    void acceptTofFrame(const RvTofFrame &frame);
    void acceptAssistedDepth(const QByteArray &depth, const QJsonObject &metadata);
    void clearFrames();
    void clearTofFrame();
    void clearAssistedDepth();
    void presentLatest();
    void changeDisplayMode();
    NativeFrame projectTofFrame(bool mesh) const;
    void presentNativeFrames();
    void publishMesh(const MeshData &mesh);
    static MeshData makeMeshData(const QVector<MeshVertex> &vertices, const QVector<quint32> &indices);
    void startDepthConversion();
    void finishDepthConversion();
    static DepthProjection projectDepth(const QByteArray &depth, const RvCameraView::Profile &camera,
                                       double colorMinimum, double colorMaximum, bool mesh);
    void setAccumulating(bool enabled);
    void appendAccumulatedFrame();
    void clearAccumulatedFrames();
    void recolorAccumulatedFrames();
    void setupDisplayControls(QVBoxLayout &layout);
    void applyDepthColorRange();
    void updateColorLegend();
    void updateStatus();
    void retranslate();

    RvSessionController *const m_session;
    SceneInstanceBuffer *const m_instances;
    SceneInstanceBuffer *const m_depthInstances;
    QQuick3DGeometry *const m_meshGeometry;
    QQuickWidget *const m_view;
    QLabel *const m_status;
    QComboBox *const m_displayMode;
    QPushButton *const m_accumulate;
    QPushButton *const m_reset;
    QWidget *const m_diameterControls;
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
    QTimer m_accumulationUpdate;
    std::deque<NativeFrame> m_accumulatedFrames;
    RvTofFrame m_latestTof;
    QByteArray m_latestDepth;
    QFutureWatcher<DepthProjection> m_depthConversion;
    quint64 m_depthEpoch = 0;
    quint64 m_conversionEpoch = 0;
    double m_depthColorMinimum = PointCloudColorScale::DefaultMinimumM;
    double m_depthColorMaximum = PointCloudColorScale::DefaultMaximumM;
    bool m_hasDepthFrame = false;
    bool m_depthDirty = false;
    bool m_depthConverting = false;
    bool m_renderingActive = false;
};
