#include "RtspCameraPage.h"
#include "RvSessionController.h"
#include "ImagePresentation.h"

#include <QtCore/QEvent>
#include <QtCore/QSignalBlocker>
#include <chrono>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

class CameraImageCanvas final : public QLabel
{
public:
    explicit CameraImageCanvas(QWidget *parent) : QLabel(parent) {}
    void setImage(const QImage &image) { m_held=false;setProperty("recoveryFrameHeld",false);m_image=image; update(); }
    QImage image() const { return m_held?QImage():m_image; }
    bool held() const { return m_held; }
    void hold() { if(!m_image.isNull()){m_held=true;setProperty("recoveryFrameHeld",true);update();} }
    void clear() { m_held=false;setProperty("recoveryFrameHeld",false);m_image={}; QLabel::clear(); update(); }
    void setRotation(int degrees) {
        m_rotation = ImagePresentation::normalizedRotation(degrees);
        setProperty("displayRotationDegrees", m_rotation);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#11161d")));
        const QRectF target = ImagePresentation::fittedRect(
            QRectF(rect()).adjusted(8, 8, -8, -8), m_image.size(), m_rotation);
        ImagePresentation::drawImage(painter, target, m_image, m_rotation);
        painter.end();
        if(!m_image.isNull()&&m_image.cacheKey()!=m_lastPaintedKey){
            m_lastPaintedKey=m_image.cacheKey();setProperty("uniquePaintedFrames",++m_uniquePaints);
            if(qEnvironmentVariable("RV_VIDEO_LATENCY_TEST")=="1"){
                const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                setProperty("lastPaintedCacheKey",m_lastPaintedKey);setProperty("lastPaintedNs",qlonglong(ns));
            }
        }
    }
private:
    bool m_held = false;
    qint64 m_lastPaintedKey=0;quint64 m_uniquePaints=0;
    int m_rotation = 0;
    QImage m_image;
};

RtspCameraPage::RtspCameraPage(QWidget *parent) : QWidget(parent)
{
    setObjectName("rtspCameraPage");
    const auto makeRow = [this](const char *name, int height) {
        auto *row = new QWidget(this);
        row->setObjectName(QString::fromLatin1(name));
        row->setFixedHeight(height);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(ImagePresentation::Spacing);
        return layout;
    };
    auto *settingsRow = makeRow("imageSettingsRow", ImagePresentation::ToolbarHeight);
    settingsRow->addStretch();
    m_startButton = new QPushButton(this); m_startButton->setObjectName("rtspCameraStart");
    m_stopButton = new QPushButton(this); m_stopButton->setObjectName("rtspCameraStop");
    m_reconnectButton = new QPushButton(this); m_reconnectButton->setObjectName("rtspCameraReconnect");
    settingsRow->addWidget(m_startButton);
    settingsRow->addWidget(m_stopButton);
    settingsRow->addWidget(m_reconnectButton);

    auto *viewport = new QWidget(this);
    viewport->setObjectName("cameraImageCanvas");
    viewport->setMinimumSize(ImagePresentation::MinimumViewportWidth, ImagePresentation::MinimumViewportHeight);
    viewport->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *viewportLayout = new QVBoxLayout(viewport);
    viewportLayout->setContentsMargins(0, 0, 0, 0);
    viewportLayout->setSpacing(0);
    m_video = new CameraImageCanvas(viewport);
    m_video->setObjectName("rtspCameraLatestFrame");
    m_video->setMinimumSize(ImagePresentation::MinimumViewportWidth, ImagePresentation::MinimumViewportHeight);
    m_video->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    viewportLayout->addWidget(m_video);

    m_imageSettings = ImageDisplaySettings::shared();
    m_video->setRotation(m_imageSettings->rotationDegrees());
    connect(m_imageSettings.data(), &ImageDisplaySettings::rotationChanged, this,
            [this](int degrees) { m_video->setRotation(degrees); });

    auto *statusRow = makeRow("imageStatusRow", ImagePresentation::ToolbarHeight);
    m_videoInfo = new QLabel(statusRow->parentWidget());
    m_videoInfo->setObjectName("rtspCameraVideoInfo");
    statusRow->addWidget(m_videoInfo);
    statusRow->addStretch();
    m_rotationLabel = new QLabel(statusRow->parentWidget());
    m_rotation = new QComboBox(statusRow->parentWidget());
    m_rotation->setObjectName("rvDisplayRotation");
    m_rotationLabel->setBuddy(m_rotation);
    for (const int degrees : {0, 90, 180, 270})
        m_rotation->addItem(QStringLiteral("%1°").arg(degrees), degrees);
    m_rotation->setCurrentIndex(m_rotation->findData(m_imageSettings->rotationDegrees()));
    statusRow->addWidget(m_rotationLabel);
    statusRow->addWidget(m_rotation);
    connect(m_rotation, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) setDisplayRotationDegrees(m_rotation->itemData(index).toInt());
    });
    connect(m_imageSettings.data(), &ImageDisplaySettings::rotationChanged, this, [this](int degrees) {
        const QSignalBlocker blocker(m_rotation);
        m_rotation->setCurrentIndex(m_rotation->findData(degrees));
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ImagePresentation::Margin, ImagePresentation::Margin,
                               ImagePresentation::Margin, ImagePresentation::Margin);
    layout->setSpacing(ImagePresentation::Spacing);
    layout->addWidget(settingsRow->parentWidget());
    layout->addWidget(viewport, 1);
    layout->addWidget(statusRow->parentWidget());

    connect(m_startButton, &QPushButton::clicked, this, [this] {
        if (m_session) { m_session->start(); }
    });
    connect(m_stopButton, &QPushButton::clicked, this, &RtspCameraPage::stop);
    connect(m_reconnectButton, &QPushButton::clicked, this, [this] {
        if (m_session) { m_session->reconnectNow(); }
    });
    retranslateUi();
}

RtspCameraPage::~RtspCameraPage() = default;
QImage RtspCameraPage::currentFrameImage() const { return m_video->image(); }

void RtspCameraPage::setSessionController(RvSessionController *session)
{
    if (m_session == session) return;
    if (m_session) disconnect(m_session, nullptr, this, nullptr);
    m_session = session;
    m_video->clear();
    if (m_session) {
        connect(session, &RvSessionController::configurationChanged, this, &RtspCameraPage::updateSessionUi);
        connect(session, &RvSessionController::stateChanged, this, &RtspCameraPage::updateSessionUi);
        connect(session,&RvSessionController::assistStateChanged,this,&RtspCameraPage::updateSessionUi);
        connect(session,&RvSessionController::deviceChanged,this,&RtspCameraPage::updateSessionUi);
        connect(session, &RvSessionController::cameraFrameReady, this, [this](const QImage &image) {
            const bool sizeChanged = m_video->image().size() != image.size();
            m_video->setImage(image);
            if (sizeChanged) updateSessionUi();
        });
        connect(session, &RvSessionController::streamCleared, this, [this] {
            if(m_session&&m_session->retainingRecoveryDisplay())m_video->hold();else m_video->clear();
            updateSessionUi();
        });
        connect(session, &RvSessionController::recoveryDisplayExpired, this, [this] { if(m_video->held())m_video->clear(); });
        connect(session, &QObject::destroyed, this, [this] {
            m_session = nullptr; m_video->clear(); updateSessionUi();
        });
    }
    updateSessionUi();
}

void RtspCameraPage::stop() { if (m_session) m_session->stop(); }
int RtspCameraPage::displayRotationDegrees() const { return m_imageSettings->rotationDegrees(); }
void RtspCameraPage::setDisplayRotationDegrees(int degrees) { m_imageSettings->setRotationDegrees(degrees); }

void RtspCameraPage::updateSessionUi()
{
    const bool available = !m_session.isNull();
    const bool busy = available && m_session->busy();
    const bool stopped = !available || m_session->userStopped();
    m_startButton->setEnabled(available && !busy && stopped);
    m_stopButton->setEnabled(available && !stopped);
    m_reconnectButton->setEnabled(available && !busy);

    const auto size = m_video->image().size();
    const auto resolution = size.isEmpty() ? QStringLiteral("-")
        : QStringLiteral("%1×%2").arg(size.width()).arg(size.height());
    const double fps = available && !stopped && !size.isEmpty() ? m_session->videoFps() : 0;
    m_videoInfo->setText(tr("Resolution: %1 | FPS: %2")
        .arg(resolution, fps > 0 ? QString::number(fps, 'f', 1) : QStringLiteral("-")));
}

void RtspCameraPage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange) retranslateUi();
}

void RtspCameraPage::retranslateUi()
{
    if (m_session) m_session->retranslateState();
    m_rotationLabel->setText(tr("Display rotation"));
    m_rotation->setToolTip(tr("Rotate the camera video and depth display together."));
    m_startButton->setText(tr("Start")); m_stopButton->setText(tr("Stop"));
    m_reconnectButton->setText(tr("Reconnect now"));
    const QString sharedAction = tr("Start, Stop and Reconnect apply to the shared camera + depth stream.");
    for (auto *button : {m_startButton, m_stopButton, m_reconnectButton}) button->setToolTip(sharedAction);
    const QString recoveryAction = tr("Start or Reconnect resets a latched sensor fault after preserving diagnostics and verifying capture ownership, then waits for fresh camera + depth data.");
    for (auto *button : {m_startButton, m_reconnectButton}) button->setToolTip(recoveryAction);
    updateSessionUi();
}
