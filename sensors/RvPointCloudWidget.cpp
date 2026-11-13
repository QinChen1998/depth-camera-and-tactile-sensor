#include "RvPointCloudWidget.h"
#include "DepthColorScale.h"
#include "DockableQuick3DWidget.h"
#include "ImageDisplaySettings.h"
#include "RvAssistFusion.h"
#include "RvCalibration.h"
#include "RvCameraView.h"
#include "RvCaptureCalibration.h"
#include "RvSessionController.h"
#include "SceneInstanceBuffer.h"
#include "../RenderingSettings.h"
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int Columns = 48;
constexpr int Rows = 32;
constexpr double MillimetresPerMetre = 1000.0;
constexpr double DefaultDiameter = 2.4;
constexpr auto DiameterKey = "pointCloud/assistedPointDiameterPx";
constexpr auto ColorMinimumKey = "pointCloud/assistedColorMinimumM";
constexpr auto ColorMaximumKey = "pointCloud/assistedColorMaximumM";
using Instance = SceneInstanceBuffer::InstanceTableEntry;

bool capturesAssistedDepth(const RvSessionController &session)
{
    return session.hostHdAssist() && session.assistDepthEnabled()
        && RvCameraView::isHostDepth(session.assistHostModel());
}

bool capturesNative48(const RvSessionController &session)
{
    const int mode = session.dtofCandidateMode();
    return RvCaptureContract::supports(mode)
        && RvCaptureContract::columns(mode) == Columns && RvCaptureContract::rows(mode) == Rows;
}

double savedNumber(const QSettings &settings, const char *key, double fallback, const QDoubleSpinBox &field)
{
    bool ok = false;
    const double value = settings.value(key).toDouble(&ok);
    return ok && std::isfinite(value) && value >= field.minimum() && value <= field.maximum() ? value : fallback;
}
}

RvPointCloudWidget::RvPointCloudWidget(RvSessionController *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_displaySettings(ImageDisplaySettings::shared()),
      m_instances(new SceneInstanceBuffer(this)),
      m_depthInstances(new SceneInstanceBuffer(this)),
      m_view(new DockableQuick3DWidget(this, {"pointInstances", "depthInstances", "depthPointDiameter", "renderSettings",
          "cameraTarget", "cameraDistance", "orbitYaw", "orbitPitch"})),
      m_status(new QLabel(this)), m_reset(new QPushButton(this)), m_depthControls(new QWidget(this)),
      m_legendMinimum(new QLabel(this)), m_legendMaximum(new QLabel(this)),
      m_diameterLabel(new QLabel(m_depthControls)), m_rangeLabel(new QLabel(m_depthControls)),
      m_rangeSeparator(new QLabel(m_depthControls)), m_diameter(new QDoubleSpinBox(m_depthControls)),
      m_colorMinimum(new QDoubleSpinBox(m_depthControls)), m_colorMaximum(new QDoubleSpinBox(m_depthControls)),
      m_applyRange(new QPushButton(m_depthControls))
{
    setObjectName("rvPointCloudWidget");
    m_instances->setObjectName("rvPointCloudInstances");
    m_depthInstances->setObjectName("rvPointCloudDepthInstances");
    setupDisplayControls();
    m_view->setObjectName("rvPointCloudView");
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_view->setClearColor(QColor("#161b22"));
    m_view->setMinimumSize(300, 190);
    m_view->setInitialProperties({{"pointInstances", QVariant::fromValue(m_instances)},
        {"depthInstances", QVariant::fromValue(m_depthInstances)}, {"depthPointDiameter", m_diameter->value()},
        {"renderSettings", QVariant::fromValue(RenderingSettings::instance())}});

    m_status->setObjectName("rvPointCloudStatus");
    m_status->setWordWrap(true);
    m_reset->setObjectName("rvPointCloudReset");
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_status, 1);
    toolbar->addWidget(m_reset);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addLayout(toolbar);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_depthControls);

    auto *gradient = new QLabel(this);
    QImage ramp(128, 1, QImage::Format_RGB32);
    for (int x = 0; x < ramp.width(); ++x)
        ramp.setPixel(x, 0, DepthColorScale::color(float(x + 1), 1, ramp.width()));
    gradient->setPixmap(QPixmap::fromImage(ramp));
    gradient->setFixedSize(96, 7);
    gradient->setScaledContents(true);
    auto *legend = new QHBoxLayout;
    legend->addStretch();
    legend->addWidget(m_legendMinimum);
    legend->addWidget(gradient);
    legend->addWidget(m_legendMaximum);
    layout->addLayout(legend);

    m_expiry.setSingleShot(true);
    m_expiry.setTimerType(Qt::PreciseTimer);
    connect(&m_expiry, &QTimer::timeout, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::tofFrameReady, this, &RvPointCloudWidget::acceptTofFrame);
    connect(m_session, &RvSessionController::assistDepthSnapshotReady, this, &RvPointCloudWidget::acceptAssistedDepth);
    connect(m_session, &RvSessionController::assistDepthStreamCleared, this, &RvPointCloudWidget::clearAssistedDepth);
    connect(m_session, &RvSessionController::streamCleared, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::deviceChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::imagingSwitchStarted, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::dtofCandidateSelectionChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::stateChanged, this, &RvPointCloudWidget::updateStatus);
    connect(&m_depthConversion, &QFutureWatcher<QByteArray>::finished, this, &RvPointCloudWidget::finishDepthConversion);
    connect(m_displaySettings.get(), &ImageDisplaySettings::colorRangeChanged, this, [this] {
        updateColorLegend();
        if (capturesNative48(*m_session))
            presentLatest();
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (auto *scene = m_view->rootObject())
            QMetaObject::invokeMethod(scene, "resetView");
    });
    connect(m_view, &QQuickWidget::statusChanged, this, [this](QQuickWidget::Status status) {
        if (status == QQuickWidget::Ready)
            presentLatest();
        else
            updateStatus();
    });
    retranslate();
}

void RvPointCloudWidget::setupDisplayControls()
{
    m_diameter->setObjectName("rvPointCloudPointDiameter");
    m_diameter->setDecimals(1);
    m_diameter->setRange(1.0, 6.0);
    m_diameter->setSingleStep(0.2);
    m_diameter->setSuffix(QStringLiteral(" px"));
    m_diameter->setKeyboardTracking(false);
    m_diameterLabel->setBuddy(m_diameter);

    m_colorMinimum->setObjectName("rvPointCloudColorMinimum");
    m_colorMaximum->setObjectName("rvPointCloudColorMaximum");
    m_colorMaximum->setSuffix(QStringLiteral(" m"));
    m_applyRange->setObjectName("rvPointCloudColorApply");
    m_rangeLabel->setBuddy(m_colorMinimum);
    for (auto *field : {m_colorMinimum, m_colorMaximum}) {
        field->setDecimals(2);
        field->setRange(ImageDisplaySettings::MinimumColorRangeM, ImageDisplaySettings::MaximumColorRangeM);
        field->setSingleStep(0.1);
    }

    const QSettings settings;
    m_diameter->setValue(savedNumber(settings, DiameterKey, DefaultDiameter, *m_diameter));
    m_colorMinimum->setValue(savedNumber(settings, ColorMinimumKey, m_depthColorMinimum, *m_colorMinimum));
    m_colorMaximum->setValue(savedNumber(settings, ColorMaximumKey, m_depthColorMaximum, *m_colorMaximum));
    if (m_colorMinimum->value() < m_colorMaximum->value()) {
        m_depthColorMinimum = m_colorMinimum->value();
        m_depthColorMaximum = m_colorMaximum->value();
    } else {
        m_colorMinimum->setValue(m_depthColorMinimum);
        m_colorMaximum->setValue(m_depthColorMaximum);
    }

    auto *diameterRow = new QHBoxLayout;
    diameterRow->addWidget(m_diameterLabel);
    diameterRow->addWidget(m_diameter);
    diameterRow->addStretch();
    auto *rangeRow = new QHBoxLayout;
    rangeRow->addWidget(m_rangeLabel);
    rangeRow->addWidget(m_colorMinimum);
    rangeRow->addWidget(m_rangeSeparator);
    rangeRow->addWidget(m_colorMaximum);
    rangeRow->addWidget(m_applyRange);
    rangeRow->addStretch();
    auto *controls = new QVBoxLayout(m_depthControls);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->addLayout(diameterRow);
    controls->addLayout(rangeRow);

    connect(m_diameter, &QDoubleSpinBox::valueChanged, this, [this](double diameter) {
        QSettings().setValue(DiameterKey, diameter);
        if (auto *scene = m_view->rootObject())
            scene->setProperty("depthPointDiameter", diameter);
    });
    const auto updateApply = [this] {
        const double minimum = m_colorMinimum->value();
        const double maximum = m_colorMaximum->value();
        m_applyRange->setEnabled(m_colorMinimum->hasAcceptableInput() && m_colorMaximum->hasAcceptableInput()
            && minimum < maximum
            && (minimum != m_depthColorMinimum || maximum != m_depthColorMaximum));
    };
    for (auto *field : {m_colorMinimum, m_colorMaximum}) {
        connect(field, &QDoubleSpinBox::valueChanged, this, updateApply);
        connect(field->findChild<QLineEdit *>(), &QLineEdit::textChanged, this, updateApply);
    }
    connect(m_applyRange, &QPushButton::clicked, this, &RvPointCloudWidget::applyDepthColorRange);
    updateApply();
}

void RvPointCloudWidget::applyDepthColorRange()
{
    const double minimum = m_colorMinimum->value();
    const double maximum = m_colorMaximum->value();
    if (!m_colorMinimum->hasAcceptableInput() || !m_colorMaximum->hasAcceptableInput() || minimum >= maximum)
        return;
    m_depthColorMinimum = minimum;
    m_depthColorMaximum = maximum;
    QSettings settings;
    settings.setValue(ColorMinimumKey, minimum);
    settings.setValue(ColorMaximumKey, maximum);
    m_applyRange->setEnabled(false);
    updateColorLegend();
    ++m_depthEpoch;
    m_depthDirty = !m_latestDepth.isEmpty();
    startDepthConversion();
}

void RvPointCloudWidget::updateColorLegend()
{
    const bool assisted = capturesAssistedDepth(*m_session);
    const double minimum = assisted ? m_depthColorMinimum : m_displaySettings->colorMinimum();
    const double maximum = assisted ? m_depthColorMaximum : m_displaySettings->colorMaximum();
    m_legendMinimum->setText(QStringLiteral("Z  %1").arg(QLocale().toString(minimum, 'f', 2)));
    m_legendMaximum->setText(QStringLiteral("%1 m").arg(QLocale().toString(maximum, 'f', 2)));
}

void RvPointCloudWidget::setRenderingActive(bool active)
{
    m_renderingActive = active;
    if (!active)
        return;
    if (m_view->source().isEmpty())
        m_view->setSource(QUrl(QStringLiteral("qrc:/qml/RvPointCloudView.qml")));
    else
        presentLatest();
}

void RvPointCloudWidget::acceptTofFrame(const RvTofFrame &frame)
{
    const auto &metadata = frame.metadata;
    const double age = metadata["raw_age_ms"].toDouble(-1);
    if (!capturesNative48(*m_session) || metadata["candidate_mode"] != m_session->dtofCandidateMode()
        || frame.columns() != Columns || frame.rows() != Rows
        || metadata["dtof_fov_correction"] != RvCalibration::Fov
        || !std::isfinite(age) || age < 0 || age >= RvCaptureContract::FreshMs
        || !frame.isValid()
        || !RvCaptureCalibration::valid(metadata["tof_calibration"].toObject(),
                                        metadata["generation"].toString(), RvCalibration::Fov, true))
        return;

    m_latestTof = frame;
    m_expiry.start(std::max(1, RvCaptureContract::FreshMs - int(std::ceil(age))));
    presentLatest();
}

void RvPointCloudWidget::acceptAssistedDepth(const QByteArray &depth, const QJsonObject &metadata)
{
    if (!capturesAssistedDepth(*m_session))
        return;
    const auto model = m_session->assistHostModel();
    const auto &camera = RvCameraView::profile(model);
    if (metadata["assist_host_model"] != model || metadata["depth_unit"] != "camera-Z_m_float32"
        || metadata["width"] != camera.outputWidth || metadata["height"] != camera.outputHeight
        || metadata["depth_K"].toArray() != RvCameraView::intrinsics(camera)
        || metadata["rectify_map_sha256"] != QLatin1String(camera.mapSha)
        || metadata["assist_output_restored_to_calibrated_grid"] != true
        || depth.size() != camera.depthBytes())
        return;

    m_latestDepth = depth;
    m_depthDirty = true;
    startDepthConversion();
}

void RvPointCloudWidget::clearFrames()
{
    m_expiry.stop();
    m_latestTof = {};
    m_instances->setEntries({});
    clearAssistedDepth();
}

void RvPointCloudWidget::clearAssistedDepth()
{
    ++m_depthEpoch;
    m_latestDepth.clear();
    m_depthDirty = false;
    m_hasDepthFrame = false;
    if (m_depthInstances->count())
        m_depthInstances->setTable({});
    updateStatus();
}

void RvPointCloudWidget::presentLatest()
{
    if (!m_renderingActive)
        return;
    if (auto *scene = m_view->rootObject()) {
        scene->setProperty("depthPointDiameter", m_diameter->value());
        if (m_expiry.remainingTime() > 0)
            presentTofFrame();
        startDepthConversion();
    }
    updateStatus();
}

void RvPointCloudWidget::presentTofFrame()
{
    const auto &calibration = RvAssistFusion::calibrationForFov(RvCalibration::Fov);
    QVector<SceneInstanceBuffer::InstanceTableEntry> entries;
    if (calibration.valid) {
        const double minimum = m_displaySettings->colorMinimum();
        const double maximum = m_displaySettings->colorMaximum();
        entries.reserve(Columns * Rows);
        for (int index = 0; index < Columns * Rows; ++index) {
            if (!m_latestTof.valid(index) || !m_latestTof.rangeUq2(index))
                continue;
            const double radiusMm = m_latestTof.distanceMm(index);
            const auto &ray = calibration.cameraRays[index];
            const auto &offset = calibration.translation;
            // Camera X right, Y down, Z forward. Qt looks down -Z with +Y up.
            const QVector3D position(float(radiusMm * ray[0] + MillimetresPerMetre * offset[0]),
                                    -float(radiusMm * ray[1] + MillimetresPerMetre * offset[1]),
                                    -float(radiusMm * ray[2] + MillimetresPerMetre * offset[2]));
            const float depthMm = -position.z();
            const float scale = std::max(3.5f, depthMm * .012f) / 100.f;
            const auto color = QColor::fromRgb(DepthColorScale::color(
                std::max(float(minimum), depthMm / float(MillimetresPerMetre)), minimum, maximum));
            entries.append(SceneInstanceBuffer::entry(position, {scale, scale, scale}, {}, color));
        }
    }
    m_instances->setEntries(entries);
}

void RvPointCloudWidget::startDepthConversion()
{
    if (!m_depthDirty || m_depthConverting || !m_renderingActive || !m_view->rootObject())
        return;

    // Keep one conversion in flight. New frames replace the pending snapshot.
    m_depthDirty = false;
    m_depthConverting = true;
    m_conversionEpoch = m_depthEpoch;
    const auto camera = RvCameraView::profile(m_session->assistHostModel());
    const double minimum = m_depthColorMinimum;
    const double maximum = m_depthColorMaximum;
    m_depthConversion.setFuture(QtConcurrent::run([depth = m_latestDepth, camera, minimum, maximum] {
        return projectDepth(depth, camera, minimum, maximum);
    }));
}

void RvPointCloudWidget::finishDepthConversion()
{
    m_depthConverting = false;
    if (m_conversionEpoch == m_depthEpoch) {
        m_depthInstances->setTable(m_depthConversion.result());
        m_hasDepthFrame = true;
        updateStatus();
    }
    startDepthConversion();
}

QByteArray RvPointCloudWidget::projectDepth(const QByteArray &depth,
    const RvCameraView::Profile &camera, double colorMinimum, double colorMaximum)
{
    QByteArray table(camera.outputWidth * camera.outputHeight * sizeof(Instance), Qt::Uninitialized);
    const char *pixel = depth.constData();
    char *output = table.data();
    const double scale = MillimetresPerMetre / camera.focal;
    Instance point{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {}, {}};
    constexpr float ToColor = 1.f / 255.f;
    for (int row = 0; row < camera.outputHeight; ++row) {
        const double yScale = (camera.cy() - row) * scale;
        for (int column = 0; column < camera.outputWidth; ++column, pixel += sizeof(float)) {
            const auto bits = qFromLittleEndian<quint32>(pixel);
            float metres;
            std::memcpy(&metres, &bits, sizeof(metres));
            if (!std::isfinite(metres) || metres <= 0)
                continue;

            // The rectified depth is already RGB camera Z. Flip Y and Z for Qt.
            const float x = float((column - camera.cx()) * scale * metres);
            const float y = float(yScale * metres);
            const float z = -float(metres * MillimetresPerMetre);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                continue;
            const QRgb color = DepthColorScale::color(metres, colorMinimum, colorMaximum);
            point.row0.setW(x);
            point.row1.setW(y);
            point.row2.setW(z);
            // The unshaded point shader writes the display palette directly.
            point.color = {qRed(color) * ToColor, qGreen(color) * ToColor, qBlue(color) * ToColor, 1.f};
            std::memcpy(output, &point, sizeof(point));
            output += sizeof(point);
        }
    }
    table.resize(output - table.constData());
    return table;
}

void RvPointCloudWidget::updateStatus()
{
    const bool ready = m_view->status() == QQuickWidget::Ready;
    const bool tofAvailable = m_expiry.remainingTime() > 0;
    const bool assisted = capturesAssistedDepth(*m_session);
    if (m_depthControls->isHidden() == assisted) {
        m_depthControls->setVisible(assisted);
        updateColorLegend();
    }
    m_reset->setEnabled(ready && (assisted ? m_depthInstances->count() > 0 : (tofAvailable && m_instances->count() > 0)));
    QString text;
    if (m_view->status() == QQuickWidget::Error) {
        text = tr("3D view unavailable");
    } else if (ready && assisted && m_hasDepthFrame) {
        const auto size = RvCameraView::profile(m_session->assistHostModel()).outputSize();
        text = tr("%1×%2 assisted depth · %3 / %4 points").arg(size.width()).arg(size.height())
            .arg(m_depthInstances->count()).arg(size.width() * size.height());
    } else if (ready && tofAvailable) {
        if (!RvAssistFusion::calibrationForFov(RvCalibration::Fov).valid)
            text = tr("F6 point cloud calibration unavailable");
        else
            text = tr("48×32 ToF · %1 / %2 points").arg(m_instances->count()).arg(Columns * Rows);
    } else if (!assisted && !capturesNative48(*m_session)) {
        text = tr("Select an imaging scheme in Depth Map.");
    } else if (m_session->userStopped()) {
        text = tr("Start capture in Depth Map or Camera raw video.");
    } else if (assisted) {
        text = tr("Waiting for a fresh assisted depth frame...");
    } else {
        text = tr("Waiting for a fresh 48×32 ToF frame...");
    }
    if (text != m_status->text())
        m_status->setText(text);
}

void RvPointCloudWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslate();
}

void RvPointCloudWidget::retranslate()
{
    m_reset->setText(tr("Reset view"));
    m_diameterLabel->setText(tr("Point diameter"));
    m_rangeLabel->setText(tr("Z color range"));
    m_rangeSeparator->setText(QStringLiteral("–"));
    m_colorMinimum->setAccessibleName(tr("Color minimum (m)"));
    m_colorMaximum->setAccessibleName(tr("Color maximum (m)"));
    m_applyRange->setText(tr("Apply"));
    m_view->setAccessibleName(tr("Point cloud view"));
    m_view->setAccessibleDescription(tr("Drag to rotate, middle-drag to pan, and scroll to zoom."));
    updateColorLegend();
    updateStatus();
}
