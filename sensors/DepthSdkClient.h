#pragma once

#include <DepthSdk.h>
#include "RvTofFrame.h"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <optional>

namespace DepthSdkFrames {
RvTofFrame nativeMeasurement(const CxwlDepthFrameEx &frame, const QByteArray &depth, QJsonObject metadata);
}

// Qt presentation adapter. The DLL owns device discovery, configuration and capture.
class DepthSdkClient final : public QObject
{
    Q_OBJECT
public:
    enum class CaptureStatus { Idle, Starting, Running, Stopping, Stopped, Failed, StopFailed };

    explicit DepthSdkClient(const QString &deviceStore, QObject *parent = nullptr);
    ~DepthSdkClient() override;

    bool available() const { return m_manager; }
    bool userStopped() const { return !m_session && !m_request && !m_stopping && m_capture.isEmpty(); }
    bool stopping() const { return m_stopping; }
    bool recovering() const { return m_capture["connection_state"] == "reconnecting"; }
    bool busy() const;
    CaptureStatus captureStatus() const { return m_status; }
    bool captureTransitionPending() const { return m_status == CaptureStatus::Starting || m_status == CaptureStatus::Stopping; }
    QString captureStatusText() const;
    bool automaticConnecting() const { return connectionStage() != CXWL_DEPTH_STAGE_IDLE; }
    int connectionStage() const;
    bool retainingRecoveryDisplay() const { return !m_stopping && m_capture["retaining_display"].toBool(); }
    double videoFps() const { return m_capture["video_fps"].toDouble(); }
    QString currentImagingScheme() const;
    bool setMode(uint32_t mode);
    quint64 stopRevision() const { return m_stopRevision; }
    bool shutdown();

    QJsonArray savedDevices(bool *ok = nullptr) const;
    QString activeDeviceId() const;
    QJsonObject captureDiagnostics() const;
    bool canManageDevices() const { return m_managerState["can_manage"].toBool() && !m_request && !m_stopping && !m_shuttingDown; }
    CxwlDepthResult selectDevice(const QString &id);
    CxwlDepthResult removeDevice(const QString &id);
    CxwlDepthResult maintainBoardFiles(const QString &id, uint32_t operation);
    CxwlDepthResult checkRgbLsc(const QString &id);
    CxwlDepthResult startRgbLscCalibration(const QString &id);
    bool finishUsbGridOffer(const QString &id, bool calibrate);
    CxwlDepthResult recoverDevice(const QString &id);
    CxwlDepthResult connectViaUsb(quint64 checkId = 0);
    void scanUsb();

public slots:
    void start();
    void stop();
    void reconnectNow();

signals:
    void stateChanged();
    void deviceChanged();
    void cameraFrameReady(const QImage &image, quint64 sequence, qint64 decodedNs,
                          quint64 epoch, const QString &generation);
    void streamCleared();
    void depthFrameReady(const QByteArray &depth, const QJsonObject &metadata, const RvTofFrame &native);
    void depthStreamCleared(const QString &reason);
    void recoveryDisplayExpired();
    void imagingSwitchStarted(const QString &scheme, int estimateMs);
    void imagingSwitchFinished();
    void imagingSwitchAborted();
    void usbStatusChanged(const QJsonObject &status);
    void rgbLscChanged(const QJsonObject &status);
    void usbDiscovered(const QJsonObject &status);
    void maintenanceChanged(const QJsonObject &status);
    void deviceRecoveryChanged(const QJsonObject &status);
    void boardFilesRequired(const QString &id);
    void captureError(const QString &reason, const QJsonObject &usb);
    void closeReady();

private:
    friend int runPortSmokeTests(class SensorMainWindow &, const QString &);
    friend int runLiveValidation(class SensorMainWindow &, const QString &);
    friend int runImagingSwitchValidation(class SensorMainWindow &, const QString &);
    friend int runUsbEnrollmentLiveValidation(class SensorMainWindow &, const QString &);

    struct ConnectionRequest {
        uint32_t target;
        quint64 checkId;
        QString deviceId;
    };
    static void CXWL_DEPTH_CALL receiveFrame(void *user, const CxwlDepthFrame *frame);
    static void CXWL_DEPTH_CALL receiveFrameEx(void *user, const CxwlDepthFrameEx *frame);
    void receive(const CxwlDepthFrame &frame, const CxwlDepthFrameEx *quality);
    static void CXWL_DEPTH_CALL captureEvent(void *user, uint32_t event);
    static void CXWL_DEPTH_CALL managerEvent(void *user, uint32_t event);
    CxwlDepthResult requestConnection(ConnectionRequest request);
    CxwlDepthResult connectRequested();
    void beginDisconnect();
    void dispatch();
    bool drain();
    void resetPresentation();
    void refreshManager();
    void resumeRegisteredUsb();
    void rememberCaptureFailure(const char *operation, CxwlDepthResult result, bool fromSdk = true);

    CxwlDepthDeviceManager *m_manager = nullptr;
    CxwlDepthSession *m_session = nullptr;
    QTimer m_dispatch;
    QTimer m_drain;
    std::optional<ConnectionRequest> m_request;
    QJsonObject m_capture, m_managerState;
    QJsonObject m_lastCaptureFailure;
    QByteArray m_depth;
    QJsonObject m_depthMetadata;
    RvTofFrame m_native;
    QImage m_video;
    quint64 m_videoSequence = 0, m_videoEpoch = 0;
    qint64 m_videoTime = 0;
    QString m_videoGeneration;
    QString m_seenAttachment;
    QString m_pendingBoardRepair;
    QString m_resumeUsbDevice;
    CaptureStatus m_status = CaptureStatus::Idle;
    uint32_t m_mode = CXWL_DEPTH_BOARD;
    quint64 m_stopRevision = 0;
    bool m_stopping = false, m_shuttingDown = false;
    bool m_clearDepth = false, m_clearVideo = false, m_devicesDirty = false;
    bool m_maintenanceDirty = false;
    bool m_recoveryDirty = false;
    bool m_rgbLscDirty = false, m_calibrateUsbAfterDisconnect = false;
    bool m_scanning = false, m_suppressNextAttachment = false;
};
