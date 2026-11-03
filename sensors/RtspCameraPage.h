#pragma once
#include "ImageDisplaySettings.h"
#include "RvSessionController.h"

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

class QLabel;
class QComboBox;
class QPushButton;
class CameraImageCanvas;
class RvSessionController;

// Camera presentation and camera requests. The shared session owns all I/O.
class RtspCameraPage final : public QWidget
{
    Q_OBJECT
public:
    explicit RtspCameraPage(QWidget *parent = nullptr);
    ~RtspCameraPage() override;
    void setSessionController(RvSessionController *session);
    RvSessionController *sessionController() const { return m_session.data(); }
    void stop();
    QImage currentFrameImage() const;
    int displayRotationDegrees() const;
    void setDisplayRotationDegrees(int degrees);

protected:
    void changeEvent(QEvent *event) override;

private:
    void updateSessionUi();
    void retranslateUi();

    QPointer<RvSessionController> m_session;
    QSharedPointer<ImageDisplaySettings> m_imageSettings;
    CameraImageCanvas *m_video = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_reconnectButton = nullptr;
    QLabel *m_videoInfo = nullptr;
    QLabel *m_rotationLabel = nullptr;
    QComboBox *m_rotation = nullptr;
};
