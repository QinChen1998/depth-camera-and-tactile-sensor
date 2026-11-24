#include "RvPointCloudWidget.h"
#include "RvAssistFusion.h"
#include "RvCalibration.h"
#include "RvCaptureCalibration.h"
#include "RvSessionController.h"
#include "SceneInstanceBuffer.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace {
constexpr int Columns = 48;
constexpr int Rows = 32;
constexpr double MillimetresPerMetre = 1000.0;
constexpr double MaximumColorRangeM = 11.0;
constexpr float MinimumMeshDepthJumpMm = 30.f;
constexpr float RelativeMeshDepthJump = .05f;
// Keep the existing color range settings for board point clouds.
constexpr auto ColorMinimumKey = "pointCloud/assistedColorMinimumM";
constexpr auto ColorMaximumKey = "pointCloud/assistedColorMaximumM";
constexpr auto ColorModeKey = "pointCloud/colorMode";
using Instance = SceneInstanceBuffer::InstanceTableEntry;
enum DisplayMode { Points, Mesh };
enum ColorMode { FixedColor, DynamicColor };

constexpr quint32 MissingVertex = std::numeric_limits<quint32>::max();

QVector<quint32> meshIndices(const std::array<quint32, Columns * Rows> &grid, const QVector<Instance> &points)
{
    QVector<quint32> indices;
    indices.reserve(6 * (Columns - 1) * (Rows - 1));
    const auto depthAt = [&](quint32 index) { return -points[index].getPosition().z(); };
    const auto triangle = [&](quint32 a, quint32 b, quint32 c) {
        if (a == MissingVertex || b == MissingVertex || c == MissingVertex)
            return;
        const auto [low, high] = std::minmax({depthAt(a), depthAt(b), depthAt(c)});
        // Adjacent angular samples can differ in Z even on a sloped surface.
        if (low > 0 && high - low <= std::max(MinimumMeshDepthJumpMm, low * RelativeMeshDepthJump)) {
            indices.append(a);
            indices.append(b);
            indices.append(c);
        }
    };
    for (int row = 0; row < Rows - 1; ++row) {
        for (int column = 0; column < Columns - 1; ++column) {
            const int i = row * Columns + column;
            triangle(grid[i], grid[i + Columns], grid[i + 1]);
            triangle(grid[i + 1], grid[i + Columns], grid[i + Columns + 1]);
        }
    }
    return indices;
}

bool capturesNative48(const RvSessionController &session)
{
    return !session.hostHdAssist() && session.native48ExtendedIntegration()
        && RvDtofCandidates::isNativeDepth(session.dtofCandidateMode());
}

QVector4D displayColor(QRgb color)
{
    constexpr float ToColor = 1.f / 255.f;
    return {qRed(color) * ToColor, qGreen(color) * ToColor, qBlue(color) * ToColor, 1.f};
}

Instance nativePoint(const QVector3D &position)
{
    const float depthMm = -position.z();
    const float diameter = std::max(3.5f, depthMm * .012f);
    return {position, diameter, {}};
}

double savedNumber(const QSettings &settings, const char *key, double fallback, const QDoubleSpinBox &field)
{
    bool ok = false;
    const double value = settings.value(key).toDouble(&ok);
    return ok && std::isfinite(value) && value >= field.minimum() && value <= field.maximum() ? value : fallback;
}
}

RvPointCloudWidget::RvPointCloudWidget(RvSessionController *session, QWidget *parent)
    : QWidget(parent), m_session(session),
      m_instances(new SceneInstanceBuffer(this)),
      m_view(new RvPointCloudCanvas(m_instances, this)),
      m_status(new QLabel(this)), m_displayMode(new QComboBox(this)),
      m_colorMode(new QComboBox(this)), m_pause(new QPushButton(this)),
      m_reset(new QPushButton(this)),
      m_rangeControls(new QWidget(this)), m_colorLegend(new QLabel(this)),
      m_legendMinimum(new QLabel(this)), m_legendMaximum(new QLabel(this)),
      m_rangeLabel(new QLabel(m_rangeControls)), m_rangeSeparator(new QLabel(m_rangeControls)),
      m_colorMinimum(new QDoubleSpinBox(m_rangeControls)), m_colorMaximum(new QDoubleSpinBox(m_rangeControls)),
      m_applyRange(new QPushButton(m_rangeControls))
{
    setObjectName("rvPointCloudWidget");
    m_instances->setObjectName("rvPointCloudInstances");
    m_view->setObjectName("rvPointCloudView");
    m_view->setMinimumSize(300, 190);
    auto viewPolicy = m_view->sizePolicy();
    viewPolicy.setRetainSizeWhenHidden(true);
    m_view->setSizePolicy(viewPolicy);
    m_status->setObjectName("rvPointCloudStatus");
    m_status->setWordWrap(true);
    m_displayMode->setObjectName("rvPointCloudDisplayMode");
    m_displayMode->addItems({QString(), QString()});
    m_pause->setObjectName("rvPointCloudPause");
    m_pause->setCheckable(true);
    m_reset->setObjectName("rvPointCloudReset");
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_status, 1);
    toolbar->addWidget(m_pause);
    toolbar->addWidget(m_displayMode);
    toolbar->addWidget(m_reset);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addLayout(toolbar);
    layout->addWidget(m_view, 1);
    setupDisplayControls(*layout);

    m_colorLegend->setObjectName("rvPointCloudColorLegend");
    m_colorLegend->setFixedSize(96, 7);
    m_colorLegend->setScaledContents(true);
    auto *legend = new QHBoxLayout;
    legend->addStretch();
    legend->addWidget(m_legendMinimum);
    legend->addWidget(m_colorLegend);
    legend->addWidget(m_legendMaximum);
    layout->addLayout(legend);

    m_expiry.setSingleShot(true);
    m_expiry.setTimerType(Qt::PreciseTimer);
    connect(&m_expiry, &QTimer::timeout, this, &RvPointCloudWidget::clearFrames);
    connect(m_pause, &QPushButton::toggled, this, &RvPointCloudWidget::setPaused);
    connect(m_displayMode, &QComboBox::currentIndexChanged, this, &RvPointCloudWidget::presentLatest);
    connect(m_session, &RvSessionController::tofFrameReady, this, &RvPointCloudWidget::acceptTofFrame);
    connect(m_session, &RvSessionController::streamCleared, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::deviceChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::imagingSwitchStarted, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::dtofCandidateSelectionChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::stateChanged, this, &RvPointCloudWidget::updateStatus);
    connect(m_reset, &QPushButton::clicked, m_view, &RvPointCloudCanvas::resetView);
    retranslate();
}

void RvPointCloudWidget::setupDisplayControls(QVBoxLayout &layout)
{
    m_colorMode->setObjectName("rvPointCloudColorMode");
    m_colorMode->addItems({QString(), QString()});

    m_colorMinimum->setObjectName("rvPointCloudColorMinimum");
    m_colorMaximum->setObjectName("rvPointCloudColorMaximum");
    m_colorMaximum->setSuffix(QStringLiteral(" m"));
    m_applyRange->setObjectName("rvPointCloudColorApply");
    m_rangeLabel->setBuddy(m_colorMinimum);
    for (auto *field : {m_colorMinimum, m_colorMaximum}) {
        field->setDecimals(2);
        field->setRange(PointCloudColorScale::DefaultMinimumM, MaximumColorRangeM);
        field->setSingleStep(0.1);
    }

    const QSettings settings;
    m_colorMode->setCurrentIndex(settings.value(ColorModeKey).toString() == "dynamic" ? DynamicColor : FixedColor);
    m_rangeControls->setVisible(m_colorMode->currentIndex() == FixedColor);
    m_colorMinimum->setValue(savedNumber(settings, ColorMinimumKey, m_depthColorMinimum, *m_colorMinimum));
    m_colorMaximum->setValue(savedNumber(settings, ColorMaximumKey, m_depthColorMaximum, *m_colorMaximum));
    if (m_colorMinimum->value() < m_colorMaximum->value()) {
        m_depthColorMinimum = m_colorMinimum->value();
        m_depthColorMaximum = m_colorMaximum->value();
    } else {
        m_colorMinimum->setValue(m_depthColorMinimum);
        m_colorMaximum->setValue(m_depthColorMaximum);
    }

    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(m_colorMode);
    colorRow->addStretch();
    auto *rangeRow = new QHBoxLayout(m_rangeControls);
    rangeRow->setContentsMargins(0, 0, 0, 0);
    rangeRow->addWidget(m_rangeLabel);
    rangeRow->addWidget(m_colorMinimum);
    rangeRow->addWidget(m_rangeSeparator);
    rangeRow->addWidget(m_colorMaximum);
    rangeRow->addWidget(m_applyRange);
    rangeRow->addStretch();
    layout.addLayout(colorRow);
    layout.addWidget(m_rangeControls);

    connect(m_colorMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool dynamic = index == DynamicColor;
        QSettings().setValue(ColorModeKey, dynamic ? QStringLiteral("dynamic") : QStringLiteral("fixed"));
        m_rangeControls->setVisible(!dynamic);
        refreshColors();
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
    refreshColors();
}

void RvPointCloudWidget::refreshColors()
{
    updateColorLegend();
    presentLatest();
}

void RvPointCloudWidget::updateColorLegend()
{
    const bool dynamic = m_colorMode->currentIndex() == DynamicColor;
    if (dynamic) {
        m_legendMinimum->setText(tr("Depth percentile 0%"));
        m_legendMaximum->setText(QStringLiteral("100%"));
    } else {
        m_legendMinimum->setText(QStringLiteral("Z  %1").arg(QLocale().toString(m_depthColorMinimum, 'f', 2)));
        m_legendMaximum->setText(QStringLiteral("%1 m").arg(QLocale().toString(m_depthColorMaximum, 'f', 2)));
    }
    QImage ramp(256, 1, QImage::Format_RGB32);
    for (int x = 0; x < ramp.width(); ++x) {
        const double t = double(x) / (ramp.width() - 1);
        ramp.setPixel(x, 0, dynamic ? PointCloudColorScale::DynamicHistogram::colorAtFraction(t)
                                    : PointCloudColorScale::jet(t));
    }
    m_colorLegend->setPixmap(QPixmap::fromImage(ramp));
}

void RvPointCloudWidget::setRenderingActive(bool active)
{
    m_renderingActive = active;
    presentLatest();
}

void RvPointCloudWidget::setPaused(bool paused)
{
    if (paused)
        clearFrames();
    else
        presentLatest();
}

void RvPointCloudWidget::acceptTofFrame(const RvTofFrame &frame)
{
    if (m_pause->isChecked())
        return;
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

void RvPointCloudWidget::clearFrames()
{
    m_expiry.stop();
    m_latestTof = {};
    m_instances->setEntries({});
    m_view->setMesh({}, {});
    updateStatus();
}

void RvPointCloudWidget::presentLatest()
{
    if (m_renderingActive && !m_pause->isChecked() && capturesNative48(*m_session))
        presentNativeFrame();
    updateStatus();
}

RvPointCloudWidget::NativeFrame RvPointCloudWidget::projectTofFrame(bool mesh) const
{
    const auto &calibration = RvAssistFusion::calibrationForFov(RvCalibration::Fov);
    NativeFrame projected;
    if (!calibration.valid)
        return projected;
    projected.points.reserve(Columns * Rows);
    std::array<quint32, Columns * Rows> grid;
    grid.fill(MissingVertex);
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
        grid[index] = quint32(projected.points.size());
        projected.points.append(nativePoint(position));
    }
    colorNativeFrame(projected);
    if (mesh)
        projected.indices = meshIndices(grid, projected.points);
    return projected;
}

void RvPointCloudWidget::colorNativeFrame(NativeFrame &frame) const
{
    if (frame.points.isEmpty())
        return;
    const auto cameraZ = [](const Instance &point) { return -point.getPosition().z() / MillimetresPerMetre; };
    std::optional<PointCloudColorScale::DynamicHistogram> histogram;
    if (m_colorMode->currentIndex() == DynamicColor)
        histogram.emplace(std::as_const(frame.points), cameraZ);
    for (auto &point : frame.points) {
        const double z = cameraZ(point);
        const auto rgb = histogram ? histogram->color(z)
                                   : PointCloudColorScale::color(z, m_depthColorMinimum, m_depthColorMaximum);
        point.color = displayColor(rgb);
    }
}

void RvPointCloudWidget::presentNativeFrame()
{
    const bool mesh = m_displayMode->currentIndex() == Mesh;
    const auto frame = m_expiry.remainingTime() > 0 ? projectTofFrame(mesh) : NativeFrame{};
    if (!mesh) {
        m_instances->setEntries(frame.points);
        m_view->setMesh({}, {});
        return;
    }
    m_instances->setEntries({});
    if (frame.indices.isEmpty()) {
        m_view->setMesh({}, {});
        return;
    }
    QVector<MeshVertex> vertices;
    vertices.reserve(frame.points.size());
    for (const auto &point : frame.points)
        vertices.append({point.getPosition(), point.color});
    m_view->setMesh(QByteArray(reinterpret_cast<const char *>(vertices.constData()), vertices.size() * sizeof(MeshVertex)),
                    QByteArray(reinterpret_cast<const char *>(frame.indices.constData()), frame.indices.size() * sizeof(quint32)));
}

void RvPointCloudWidget::updateStatus()
{
    const bool paused = m_pause->isChecked();
    const bool hostAssisted = m_session->hostHdAssist();
    const bool native = capturesNative48(*m_session);
    const bool ready = native && !paused && m_renderingActive;
    const bool tofAvailable = m_expiry.remainingTime() > 0;
    const int pointCount = m_instances->count();
    const qsizetype triangleCount = m_view->indexData().size() / (3 * sizeof(quint32));
    const bool meshSelected = m_displayMode->currentIndex() == Mesh;
    const bool showingMesh = triangleCount > 0 || (meshSelected && pointCount == 0);
    m_view->setVisible(!paused && !hostAssisted);
    m_displayMode->setEnabled(ready);
    m_pause->setEnabled(native);
    m_colorMode->setEnabled(native);
    m_rangeControls->setEnabled(native);
    m_pause->setText(paused ? tr("Resume point cloud") : tr("Stop point cloud"));
    m_reset->setEnabled(ready && tofAvailable && (pointCount > 0 || triangleCount > 0));
    QString text;
    if (hostAssisted) {
        text = tr("Point cloud display is currently unavailable for host-assisted inference.");
    } else if (!native) {
        text = tr("Select an imaging scheme in Depth Map.");
    } else if (paused) {
        text = tr("Point cloud stopped");
    } else if (ready && tofAvailable) {
        if (!RvAssistFusion::calibrationForFov(RvCalibration::Fov).valid)
            text = tr("F6 point cloud calibration unavailable");
        else if (showingMesh)
            text = tr("48×32 ToF · %1 triangles").arg(triangleCount);
        else
            text = tr("48×32 ToF · %1 / %2 points").arg(pointCount).arg(Columns * Rows);
    } else if (m_session->userStopped()) {
        text = tr("Start capture in Depth Map or Camera raw video.");
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
    m_displayMode->setItemText(Points, tr("Points"));
    m_displayMode->setItemText(Mesh, tr("Mesh"));
    m_displayMode->setAccessibleName(tr("Point cloud display mode"));
    m_colorMode->setItemText(FixedColor, tr("Fixed Z color range"));
    m_colorMode->setItemText(DynamicColor, tr("Dynamic + histogram equalization"));
    m_colorMode->setAccessibleName(tr("Point cloud color mode"));
    m_colorMode->setItemData(DynamicColor, tr("Equalize depths from 0.10 to 10.00 m in each frame. Colors can vary across frames."), Qt::ToolTipRole);
    m_reset->setText(tr("Reset view"));
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
