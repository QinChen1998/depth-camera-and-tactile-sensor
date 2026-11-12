#pragma once

#include <QtCore/QObject>
#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <deque>
#include <memory>
#include "RvTofFrame.h"
#include "RvPhotonFrame.h"
#include "RvDtofCandidates.h"
#include "RvDeviceStore.h"

class LanguageManager;
class SensorMainWindow;
class QMediaPlayer;
class QVideoSink;
class QVideoFrame;
class QProcess;
class QUrl;
class QNetworkAccessManager;
class QNetworkReply;
class RvCameraStream;
class RvHdVideoStream;
class RvDtofBoardStream;
class RvAssistDepth;
class RvTunerController;
class LanguageManager;
class SensorMainWindow;
struct RtspVlcBackend;

// One RV device session, independent of either display widget. All public calls
// and signals run on its owner thread; decoding keeps only a bounded mailbox.
class RvSessionController final : public QObject
{
    Q_OBJECT
public:
    enum class AutomaticConnectionStage { Idle, CheckingDevice, PreparingInference, StartingCapture, WaitingForDepth };

    explicit RvSessionController(QObject *parent = nullptr,const QString &deviceStorePath=RvDeviceStore::defaultPath());
    ~RvSessionController() override;
    QString deviceStorePath() const {return m_deviceStorePath;}
    RvDeviceProfile deviceProfile() const {return m_device;}
    void reloadDeviceProfiles();
    int cameraMountRotationDegrees() const {return m_device.cameraMountRotation;}
    QImage cameraInCalibrationFrame(const QImage &image) const;

    // Numeric-only diagnostics; no camera pixels or measurement arrays.
    QJsonObject videoDiagnostics(bool counters = false) const;
    QString streamUrl() const { return m_streamUrl; }
    int cameraWidth() const { return m_config.cameraWidth; }
    QString depthModel() const { return m_config.model; }
    int depthWidth() const { return m_config.depthWidth; }
    QString depthLocation() const { return m_config.location; }
    QString depthMethod() const { return m_config.method; }
    QJsonObject metadata() const { return m_currentMetadata; }
    bool busy() const { return m_tunerExclusive || m_controlPending || m_waitingForSelectedFrame || m_transitioning; }
    AutomaticConnectionStage automaticConnectionStage() const { return m_automaticConnectionStage; }
    bool automaticConnecting() const { return m_automaticConnectionStage != AutomaticConnectionStage::Idle; }
    RvTunerController *tuner() const {return m_tuner;}
    bool userStopped() const { return m_userStopped; }
    bool retainingRecoveryDisplay() const { return !m_destroying && !m_userStopped && m_recoveryDisplayTimer.isActive(); }
    QString stateText() const { return m_stateText; }
    QString detailText() const { return m_detailText; }
    double videoFps() const { return m_videoFps; }
    QString depthInfoText() const { return m_depthInfoText; }
    quint64 presentationEpoch() const {return m_presentationEpoch;}
    bool assistDepthEnabled() const;
    bool hostHdAssist() const {return !m_hostScheme.isEmpty()&&m_dtofMode==RvDtofCandidates::Native48&&m_config.cameraWidth==2304;}
    int assistInputRotationDegrees() const;
    QString assistHostModel() const;
    QString currentImagingScheme() const;
    static constexpr char GlobalRgbImagingScheme[]="host_unidepth_rgb672_vga_fp16_tof48_g";
    static constexpr char ReuseLatestTofImagingScheme[]="host_unidepth_rgb672_vga_fp16_tof48_b";
    static QString imagingHostModel(const QString &scheme);
    QString hostInferencePreflightError() const;
    QString assistExperiment() const;
    QString assistDepthInfoText() const {return m_assistInfo;}
    int assistLoadingState() const {return m_assistLoadingState;}
    bool imagingSwitchActive() const {return m_imagingSwitchActive;}
    QJsonObject imagingSwitchDiagnostics() const;
    bool controlledSource() const;
    int dtofCandidateMode() const {return m_dtofMode;}
    RvDtofCandidates::Settings dtofCandidateSettings() const {return m_dtofSettings;}
    bool setDtofCandidate(int mode,const RvDtofCandidates::Settings& settings);
    bool canSelectDepthWidth(int width) const;
    bool canSelectDepthLocation(const QString &location) const;
    bool canSelectDepthMethod(const QString &method) const;

public slots:
    void setDepthModel(const QString &model);
    void selectImagingScheme(const QString &scheme,const RvDtofCandidates::Settings &settings);
    void setDepthWidth(int width);
    void setDepthLocation(const QString &location);
    void setDepthMethod(const QString &method);
    void setAssistDepthEnabled(bool enabled);
    static int effectiveHostInputRotation(const RvDeviceProfile &device);
    static QString legacyHostInputRotationKey(const RvDeviceProfile &device);
    // Fresh stopped QCore test sessions only; no persisted setting or GUI mode.
    void setDepthConfiguration(int width, const QString &location, const QString &method);
    void setConfiguration(int cameraWidth, const QString &model, int depthWidth,
                          const QString &location, const QString &method);
    void start();
    void startAutomatically(bool verifiedUsbAttachment = false);
    bool automaticStartAllowed(const QString &deviceId) const;
    quint64 stopRevision() const { return m_stopRevision; }
    void deviceUnreachable(const QString &deviceId);
    void stop();
    void reconnectNow();
    void retranslateState();

signals:
    void hostInferenceUnavailable(const QString &explanation,const QString &detail);
    void boardFilesRequired(const RvDeviceProfile &device);
    void deviceChanged();
    void dtofCandidateSelectionChanged();
    void configurationChanged();
    void stateChanged();
    void cameraFrameReady(const QImage &image);
    void streamCleared();
    void recoveryDisplayExpired();
    void depthSnapshotReady(const QByteArray &depth, const QJsonObject &metadata, const QImage &camera, const QByteArray &coverage={});
    void depthFrameReady(const QByteArray &depth, const QJsonObject &metadata);
    void tofFrameReady(const RvTofFrame &frame);
    void photonFrameReady(const RvPhotonFrame &frame);
    void photonStreamCleared(const QString &reason);
    void depthStreamCleared(const QString &reason);
    void assistStateChanged();
    void assistDepthSnapshotReady(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,const RvTofFrame &tof=RvTofFrame(),const QByteArray &diagnostic=QByteArray(),const QImage &inferenceInput=QImage());
    void assistDepthStreamCleared(const QString &reason);
    void imagingSwitchStarted(const QString &scheme,int estimatedMs);
    void imagingSwitchFinished(const QString &scheme,qint64 elapsedMs);
    void imagingSwitchAborted(const QString &reason);

private:
    friend int runPortSmokeTests(SensorMainWindow &,LanguageManager &,const QString &);
    bool testHostHdPreparation();
    friend class RvTunerController;
    friend class RvUsbEnrollment;
    friend int runRvTunerTests(const QString &);
    friend int runPortSmokeTests(SensorMainWindow &, LanguageManager &, const QString &);
    RvTunerController *m_tuner=nullptr;
    bool m_tunerExclusive=false;
    QJsonObject m_captureOwner;
    friend int runRvDtofCandidatesTests(const QString &reportPath);
    friend int runRvAssistDepthTests(const QString &reportPath);
    friend int runRvImagingSwitchTests(const QString &reportPath);
    struct Configuration {
        int cameraWidth = 576;
        QString model = QStringLiteral("Native");
        int depthWidth = 144;
        QString location = QStringLiteral("none");
        QString method = QStringLiteral("none");
    };
    static bool validConfiguration(const Configuration &candidate);
    Configuration withCameraWidth(int width) const;
    Configuration withDepthWidth(int width) const;
    void setAutomaticConnectionStage(AutomaticConnectionStage stage);
    void startImaging(bool automatic);
    void submitConfiguration(const Configuration &candidate, bool previewChange, bool modelChange,
                             bool startAfter = false, bool reconnecting = false, bool automatic = false);
    void syncConfiguration(const QJsonObject &metadata);
    void updateState();
    void rejectConfiguration();
    void failPreviewControl(const QString &reason, bool retry = false);
    void retryConnection();
    void beginConnection(bool resetAttempts);
    void resetQtPlayer();
    bool startVlc(const QUrl &source);
    void presentHostHdVideo();
    void takeHostHdPrepared();
    void observeHostHdClock(const QJsonObject& metadata);
    const RvTofFrame *pairedHostTof(qint64 rgbNs) const;
    bool prepareManagedVlc();
    void pollManagedStatus();
    void stopManagedStatus();
    void stopVlc(bool retainBoardReceiver=false);
    void beginImagingSwitch(const QString &scheme,bool boardRestart);
    bool applyDtofCandidate(int mode,const RvDtofCandidates::Settings &settings);
    void finishImagingSwitch();
    void cancelImagingSwitch(const QString &reason);
    void clearCurrentFrame(const QString &reason);
    void presentVlcFrame();
    bool reconnectHostHdVideo();
    void scheduleReconnect(const QString &reason);
    void handleFrame(const QVideoFrame &frame);
    void setState(const QString &state, const QString &detail);

    Configuration m_config;
    RvDeviceProfile m_device;
    QString m_deviceStorePath,m_deviceError,m_deviceRevision;
    QString m_autoStartAttemptedDevice;
    AutomaticConnectionStage m_automaticConnectionStage = AutomaticConnectionStage::Idle;
    quint64 m_stopRevision=0;
    int m_dtofMode=-1;
    RvDtofCandidates::Settings m_dtofSettings;
    QString m_dtofOptions,m_dtofToken;
    QJsonObject m_dtofReceipt;
    RvDtofBoardStream* m_dtofStream=nullptr;
    QPointer<RvDtofBoardStream> m_retiringDtofStream;
    Configuration m_confirmedConfig;
    bool m_hasConfirmedConfig = false;
    bool m_destroying = false;
    QString m_streamUrl;
    QString m_stateText, m_detailText, m_depthInfoText;
    double m_videoFps = 0;
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_videoSink = nullptr;
    QTimer m_reconnectTimer, m_frameTimer, m_recoveryDisplayTimer;
    QElapsedTimer m_lastFrameClock, m_fpsClock, m_hdFrameClock;
    bool m_hdVideoSeen=false;
    int m_hdVideoReconnects=0;
    int m_reconnectAttempt = 0, m_framesInWindow = 0;
    QString m_lastReconnectReason;
    int m_nextReconnectDelayMs = 1000;
    bool m_userStopped = true;
    RtspVlcBackend *m_vlc = nullptr;
    RvCameraStream *m_nv12 = nullptr;
    RvHdVideoStream *m_hdVideo = nullptr;
    struct HdPreparer;
    std::unique_ptr<HdPreparer> m_hdPreparer;
    quint64 m_hdVideoSequence=0,m_hdOutputSequence=0;
    std::deque<RvTofFrame> m_hdNativeFrames;
    qint64 m_hdRealtimeOffsetNs=0,m_hdBoardSampleNs=0,m_hdSampleReceivedNs=0;
    int m_hdClockSamples=0;
    RvAssistDepth *m_assist = nullptr;
    QString m_confirmedBoot;
    QString m_assistInfo;
    int m_assistLoadingState=0;
    double m_lastAssistFrame = 0;
    QTimer m_assistExpiry,m_nativeDepthExpiry;
    QString m_depthConfigError;
    QProcess *m_previewControl = nullptr;
    QTimer m_controlTimeout, m_switchFrameTimeout;
    QJsonObject m_currentMetadata, m_expectedDepthMetadata;
    QString m_requestedModel;
    int m_requestedPreviewWidth = 0, m_requestedDepthWidth = 144;
    QString m_requestedLocation, m_requestedMethod;
    bool m_controlPending = false, m_waitingForSelectedFrame = false, m_controlTimedOut = false;
    quint64 m_nv12Sequence = 0;
    quint64 m_presentationEpoch = 0;
    bool m_transitioning = false;
    bool m_imagingSwitchActive=false,m_imagingSwitchBoardRestart=false;
    QString m_hostScheme;
    int m_imagingSwitchEstimateMs=0;
    QString m_imagingSwitchScheme;
    QElapsedTimer m_imagingSwitchClock;
    QJsonObject m_lastImagingSwitch;
    QString m_managedVlcDirectory;
    QJsonObject m_expectedCameraStatus;
    bool m_managedRtsp = false, m_rtspStatusReady = false;
    quint64 m_rtspDecodeFloor = 0;
    QNetworkAccessManager *m_statusNetwork = nullptr;
    QPointer<QNetworkReply> m_statusReply;
    QTimer m_statusTimer;
    quint64 m_statusEpoch = 0;
    qint64 m_lastEncodedFrame = 0, m_lastRetrievedFrame = 0;
    QElapsedTimer m_encoderProgressClock, m_connectionClock;
};

