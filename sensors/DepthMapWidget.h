#pragma once
class QSpinBox;

#include "LatestSensorFrameBuffer.h"
#include "SensorTypes.h"
#include "Tmf8829Frame.h"
#include "DepthSnapshot.h"
#include "ImageDisplaySettings.h"
#include "RvSessionController.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QSharedPointer>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QPushButton;
class QCheckBox;
class RvDepthView;
class QJsonObject;
class QComboBox;
class QLabel;
class QTimer;
class QSlider;
class DepthMapCanvas;
class QTabWidget;
class QProgressBar;

class DepthMapWidget : public QWidget
{
    Q_OBJECT

public:
    enum class DisplayMode { Depth, Snr, Signal, Noise, Xtalk };

    explicit DepthMapWidget(QWidget *parent = nullptr);
    ~DepthMapWidget() override;
    void setFrameBuffer(
        const QSharedPointer<LatestSensorFrameBuffer<Tmf8829Frame>> &buffer);
    void setSourceState(SensorSourceMode mode, SensorConnectionState state,
                        const QString &detail);
    void setSelectedSourceMode(SensorSourceMode mode);
    QString statusText() const;
    void setRvDepthMode(bool enabled=true);
    void setSessionController(RvSessionController *controller);
    RvSessionController *sessionController() const;
    void acceptRvDepth(const QByteArray &depth,const QJsonObject &metadata);
    void clearRvDepth(const QString &reason);
    void acceptRvSnapshot(const QByteArray &depth, const QJsonObject &metadata, const QImage &camera);
    void acceptAssistSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,const RvTofFrame &tof=RvTofFrame(),const QByteArray &diagnostic=QByteArray());
    void clearAssistDepth(const QString &reason);
    void acceptLiteSrDepth(const QByteArray &depth,const QJsonObject &metadata,const QByteArray &coverage={});
    void clearLiteSrDepth(const QString &reason);
    DepthSnapshot snapshot() const;
    SensorSourceMode selectedSourceMode() const { return m_sourceMode; }
    int selectedWidth() const;
    int selectedHeight() const;
    int displayRotationDegrees() const;
    void setDisplayRotationDegrees(int degrees);


    static QImage renderHeatmap(const Tmf8829Frame &frame, DisplayMode mode,
                                double *minimum = nullptr,
                                double *maximum = nullptr);

signals:
    void sourceModeRequested(SensorSourceMode mode);
    void mockResolutionRequested(int width, int height);
    void pausedChanged(bool paused);

protected:
    void changeEvent(QEvent *event) override;

private:
    friend int runRvImagingSwitchTests(const QString &reportPath);
    friend int runRvAssistDepthTests(const QString &reportPath);
    friend int runDepthorFp32PerformanceTests(const QString &reportPath);
    void beginImagingSwitchUi(const QString &scheme,int estimatedMs);
    void finishImagingSwitchUi();
    void cancelImagingSwitchUi();
    void advanceImagingSwitchUi();
    bool eventFilter(QObject *watched,QEvent *event) override;
    void clearCandidate(const QString& reason);
    void holdCandidate();
    RvDepthView* m_candidateCanvas=nullptr;
    QComboBox *m_colorOutput=nullptr;
    void acceptCandidate(const RvDtofCandidates::Result& result);
    DepthSnapshot m_candidateSnapshot;
    void saveCurrentFrame();
    QPushButton *m_saveButton = nullptr;
    QCheckBox *m_saveCamera = nullptr;
    QCheckBox *m_blackOutsideRange = nullptr;
    QLabel *m_colorMinimumLabel = nullptr, *m_colorMaximumLabel = nullptr;
    QPushButton *m_applyColorRange = nullptr;
    DepthSnapshot m_snapshot;
    DepthSnapshot m_assistSnapshot,m_liteSrSnapshot;
    RvDepthView *m_liteSrCanvas=nullptr;
    QSpinBox *m_liteSrWhite=nullptr;
    QLabel *m_liteSrWhiteCaption=nullptr;
    QComboBox *m_liteSrOrientation=nullptr;
    QPushButton *m_liteSrRepairButton=nullptr;
    QCheckBox *m_boardLiteSrHostContour=nullptr;
    void editLiteSrRepairSettings();
    void pollLatestFrame();
    void renderCurrentFrame();
    void retranslateUi();
    void syncSessionControls();
    QStringList m_sessionControlsKey;
    void syncDisplayColors();
    void updateRvPresentation();

    QSharedPointer<LatestSensorFrameBuffer<Tmf8829Frame>> m_buffer;
    QSharedPointer<ImageDisplaySettings> m_imageSettings;
    DepthMapCanvas *m_canvas = nullptr;
    RvDepthView *m_rvCanvas = nullptr;
    RvDepthView *m_assistCanvas = nullptr;
    QTabWidget *m_depthTabs = nullptr;
    QWidget *m_switchOverlay=nullptr;
    QLabel *m_switchTitle=nullptr,*m_switchDetail=nullptr;
    QProgressBar *m_switchProgress=nullptr;
    QTimer *m_switchUiTimer=nullptr;
    QElapsedTimer m_switchUiClock,m_switchFinishClock;
    int m_switchEstimatedMs=10000,m_switchFinishFrom=0;
    bool m_switchUiActive=false,m_switchUiFinishing=false;
    QString m_switchUiScheme;
    bool m_rvMode = false;
    QPointer<RvSessionController> m_session;
    QWidget *m_rvControls = nullptr;
    QLabel *m_rvModelCaption = nullptr;
    QLabel *m_rvResolutionCaption = nullptr;
    QComboBox *m_rvModel = nullptr;
    QPushButton *m_rvStart = nullptr, *m_rvStop = nullptr, *m_rvReconnect = nullptr;
    QComboBox *m_rvResolution = nullptr;
    QCheckBox *m_rvOnBoard = nullptr;
    QComboBox *m_rvMethod = nullptr;
    QWidget *m_sourceControls = nullptr;

    QLabel *m_statusLabel = nullptr;
    QLabel *m_infoLabel = nullptr;
    QLabel *m_sourceCaption = nullptr;
    QLabel *m_resolutionCaption = nullptr;
    QLabel *m_displayCaption = nullptr;
    QComboBox *m_sourceCombo = nullptr;
    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_displayCombo = nullptr;
    QCheckBox *m_pauseCheck = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QElapsedTimer m_fpsTimer;
    int m_framesSinceFpsUpdate = 0;
    double m_fps = 0.0;
    quint64 m_droppedFrames = 0;
    SensorSourceMode m_sourceMode = SensorSourceMode::None;
    SensorConnectionState m_connectionState = SensorConnectionState::NoData;
    QString m_statusDetail;
    Tmf8829Frame m_currentFrame;
};
