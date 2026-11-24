#include "RvPointCloudWidget.h"
#include "RvAssistFusion.h"
#include "RvCalibration.h"
#include "RvCameraView.h"
#include "RvCaptureCalibration.h"
#include "RvDepthDiagnostic.h"
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
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace {
constexpr int Columns = 48;
constexpr int Rows = 32;
constexpr double MillimetresPerMetre = 1000.0;
constexpr double DefaultDiameter = 2.4;
constexpr double MaximumColorRangeM = 11.0;
constexpr float MinimumMeshDepthJumpMm = 30.f;
constexpr float RelativeMeshDepthJump = .05f;
constexpr float MinimumOcclusionGapM = .06f;
constexpr float RelativeOcclusionGap = .06f;
constexpr float MinimumSurfaceToleranceM = .015f;
constexpr float RelativeSurfaceTolerance = .012f;
constexpr float MaximumSurfaceSlopeFraction = .75f;
constexpr int SurfaceBaseline = 3;
constexpr int MaxTransitionRadius = 10;
constexpr auto DiameterKey = "pointCloud/assistedPointDiameterPx";
constexpr auto ColorMinimumKey = "pointCloud/assistedColorMinimumM";
constexpr auto ColorMaximumKey = "pointCloud/assistedColorMaximumM";
constexpr auto ColorModeKey = "pointCloud/colorMode";
using Instance = SceneInstanceBuffer::InstanceTableEntry;
enum DisplayMode { Points, Mesh };
enum ColorMode { FixedColor, DynamicColor };

constexpr quint32 MissingVertex = std::numeric_limits<quint32>::max();

bool isDepthTransition(const float *depth, int stride, int before, int after)
{
    const float center = *depth;
    const int available = std::min(before, after);
    for (const int radius : {2, 6, MaxTransitionRadius}) {
        if (available < radius + SurfaceBaseline)
            break;

        const float left = depth[-radius * stride];
        const float right = depth[radius * stride];
        const auto [near, far] = std::minmax(left, right);
        const float gap = far - near;
        const float tolerance = std::max(MinimumSurfaceToleranceM, RelativeSurfaceTolerance * near);
        if (near <= 0 || gap < std::max(MinimumOcclusionGapM, RelativeOcclusionGap * near)
            || center <= near + tolerance || center >= far - tolerance)
            continue;

        const int outerOffset = (radius + SurfaceBaseline) * stride;
        const float outerLeft = depth[-outerOffset];
        const float outerRight = depth[outerOffset];
        if (outerLeft <= 0 || outerRight <= 0)
            continue;

        // Compare the transition with the two outer slopes in inverse Z,
        // which varies linearly along the image of a plane.
        const float leftSlope = 1.f / left - 1.f / outerLeft;
        const float rightSlope = 1.f / right - 1.f / outerRight;
        const float scale = float(radius) / SurfaceBaseline;
        if ((std::abs(leftSlope) + std::abs(rightSlope)) * scale
            > MaximumSurfaceSlopeFraction * std::abs(1.f / left - 1.f / right))
            continue;

        const float atCenterLeft = 1.f / left + scale * leftSlope;
        const float atCenterRight = 1.f / right + scale * rightSlope;
        if (atCenterLeft <= 0 || atCenterRight <= 0)
            continue;
        const auto [inverseNear, inverseFar] = std::minmax(atCenterLeft, atCenterRight);
        if (center > 1.f / inverseFar + tolerance && center < 1.f / inverseNear - tolerance)
            return true;
    }
    return false;
}

template <typename DepthAt, typename IsCanceled>
QVector<quint32> meshIndices(const quint32 *grid, int columns, int rows, DepthAt depthAt, IsCanceled isCanceled)
{
    QVector<quint32> indices;
    indices.reserve(6 * (columns - 1) * (rows - 1));
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
    for (int row = 0; row < rows - 1; ++row) {
        if (isCanceled())
            return {};
        for (int column = 0; column < columns - 1; ++column) {
            const int i = row * columns + column;
            triangle(grid[i], grid[i + columns], grid[i + 1]);
            triangle(grid[i + 1], grid[i + columns], grid[i + columns + 1]);
        }
    }
    return indices;
}

bool capturesAssistedDepth(const RvSessionController &session)
{
    return session.hostHdAssist() && session.assistDepthEnabled()
        && RvCameraView::isHostDepth(session.assistHostModel());
}

bool capturesNative48(const RvSessionController &session)
{
    const int mode = session.dtofCandidateMode();
    return !session.hostHdAssist() && RvCaptureContract::supports(mode)
        && RvCaptureContract::columns(mode) == Columns && RvCaptureContract::rows(mode) == Rows;
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
      m_depthInstances(new SceneInstanceBuffer(this)),
      m_view(new RvPointCloudCanvas(m_instances, m_depthInstances, this)),
      m_status(new QLabel(this)), m_displayMode(new QComboBox(this)),
      m_colorMode(new QComboBox(this)), m_pause(new QPushButton(this)),
      m_reset(new QPushButton(this)),
      m_diameterControls(new QWidget(this)), m_rangeControls(new QWidget(this)), m_colorLegend(new QLabel(this)),
      m_legendMinimum(new QLabel(this)), m_legendMaximum(new QLabel(this)),
      m_diameterLabel(new QLabel(m_diameterControls)), m_rangeLabel(new QLabel(m_rangeControls)),
      m_rangeSeparator(new QLabel(m_rangeControls)), m_diameter(new QDoubleSpinBox(m_diameterControls)),
      m_colorMinimum(new QDoubleSpinBox(m_rangeControls)), m_colorMaximum(new QDoubleSpinBox(m_rangeControls)),
      m_applyRange(new QPushButton(m_rangeControls))
{
    setObjectName("rvPointCloudWidget");
    m_instances->setObjectName("rvPointCloudInstances");
    m_depthInstances->setObjectName("rvPointCloudDepthInstances");
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
    m_view->setDepthPointDiameter(m_diameter->value());

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
    connect(&m_expiry, &QTimer::timeout, this, &RvPointCloudWidget::clearTofFrame);
    connect(m_pause, &QPushButton::toggled, this, &RvPointCloudWidget::setPaused);
    connect(m_displayMode, &QComboBox::currentIndexChanged, this, &RvPointCloudWidget::changeDisplayMode);
    connect(m_session, &RvSessionController::tofFrameReady, this, &RvPointCloudWidget::acceptTofFrame);
    connect(m_session, &RvSessionController::assistDepthSnapshotReady, this, &RvPointCloudWidget::acceptAssistedDepth);
    connect(m_session, &RvSessionController::assistDepthStreamCleared, this, &RvPointCloudWidget::clearAssistedDepth);
    connect(m_session, &RvSessionController::monocularDisplayChanged, this, [this] {
        if (m_pause->isChecked())
            return;
        m_depthInstances->setTable({});
        publishMesh({});
        m_hasDepthFrame = false;
        changeDisplayMode();
    });
    connect(m_session, &RvSessionController::streamCleared, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::deviceChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::imagingSwitchStarted, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::dtofCandidateSelectionChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::stateChanged, this, &RvPointCloudWidget::updateStatus);
    connect(&m_depthConversion, &QFutureWatcher<DepthProjection>::finished, this, &RvPointCloudWidget::finishDepthConversion);
    connect(m_reset, &QPushButton::clicked, m_view, &RvPointCloudCanvas::resetView);
    retranslate();
}

void RvPointCloudWidget::setupDisplayControls(QVBoxLayout &layout)
{
    m_colorMode->setObjectName("rvPointCloudColorMode");
    m_colorMode->addItems({QString(), QString()});

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
        field->setRange(PointCloudColorScale::DefaultMinimumM, MaximumColorRangeM);
        field->setSingleStep(0.1);
    }

    const QSettings settings;
    m_colorMode->setCurrentIndex(settings.value(ColorModeKey).toString() == "dynamic" ? DynamicColor : FixedColor);
    m_rangeControls->setVisible(m_colorMode->currentIndex() == FixedColor);
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

    auto *diameterRow = new QHBoxLayout(m_diameterControls);
    diameterRow->setContentsMargins(0, 0, 0, 0);
    diameterRow->addWidget(m_diameterLabel);
    diameterRow->addWidget(m_diameter);
    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(m_colorMode);
    colorRow->addWidget(m_diameterControls);
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
    connect(m_diameter, &QDoubleSpinBox::valueChanged, this, [this](double diameter) {
        QSettings().setValue(DiameterKey, diameter);
        m_view->setDepthPointDiameter(diameter);
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
    changeDisplayMode();
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
    if (!active || m_pause->isChecked())
        return;
    presentLatest();
}

void RvPointCloudWidget::setPaused(bool paused)
{
    if (paused && m_depthConverting)
        m_depthConversion.cancel();
    m_view->setVisible(!paused);
    if (paused) {
        clearFrames();
    } else {
        setRenderingActive(m_renderingActive);
        updateStatus();
    }
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

void RvPointCloudWidget::acceptAssistedDepth(const QByteArray &depth, const QJsonObject &metadata, const QImage &,
                                             const RvTofFrame &, const QByteArray &diagnostic)
{
    if (m_pause->isChecked() || !capturesAssistedDepth(*m_session))
        return;
    const auto model = m_session->assistHostModel();
    const auto &camera = RvCameraView::outputProfile(model);
    if (metadata["assist_host_model"] != model || metadata["depth_unit"] != "camera-Z_m_float32"
        || !RvCameraView::validOutputGrid(metadata)
        || metadata["depth_K"].toArray() != RvCameraView::intrinsics(camera)
        || metadata["rectify_map_sha256"] != QLatin1String(camera.mapSha)
        || metadata["assist_output_restored_to_calibrated_grid"] != true
        || depth.size() != camera.depthBytes())
        return;

    m_latestDepth = depth;
    m_latestMonocularDepth = RvDepthDiagnostic::monocularDepth(diagnostic, metadata);
    if (m_session->monocularDisplay() && m_latestMonocularDepth.isEmpty()) {
        // A missing predictor in this frame must never leave the old cloud visible.
        ++m_depthEpoch;
        m_depthDirty = false;
        m_hasDepthFrame = false;
        m_depthInstances->setTable({});
        publishMesh({});
        updateStatus();
        return;
    }
    m_depthDirty = true;
    startDepthConversion();
}

void RvPointCloudWidget::clearFrames()
{
    m_expiry.stop();
    m_latestTof = {};
    m_instances->setEntries({});
    publishMesh({});
    clearAssistedDepth();
}

void RvPointCloudWidget::clearAssistedDepth()
{
    ++m_depthEpoch;
    m_latestDepth.clear();
    m_latestMonocularDepth.clear();
    m_depthDirty = false;
    m_hasDepthFrame = false;
    if (m_depthInstances->count())
        m_depthInstances->setTable({});
    if (capturesAssistedDepth(*m_session))
        publishMesh({});
    updateStatus();
}

void RvPointCloudWidget::clearTofFrame()
{
    m_latestTof = {};
    m_instances->setEntries({});
    if (capturesNative48(*m_session))
        publishMesh({});
    updateStatus();
}

void RvPointCloudWidget::presentLatest()
{
    if (!m_renderingActive || m_pause->isChecked())
        return;
    m_view->setAssisted(capturesAssistedDepth(*m_session));
    if (capturesNative48(*m_session)) presentNativeFrame();
    startDepthConversion();
    updateStatus();
}

void RvPointCloudWidget::changeDisplayMode()
{
    if (capturesAssistedDepth(*m_session)) {
        ++m_depthEpoch;
        m_depthDirty = !m_latestDepth.isEmpty();
    }
    presentLatest();
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
    if (mesh) {
        const auto &points = std::as_const(projected.points);
        projected.indices = meshIndices(grid.data(), Columns, Rows,
                                        [&](quint32 index) { return -points[index].getPosition().z(); },
                                        [] { return false; });
    }
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
        publishMesh({});
        return;
    }
    QVector<MeshVertex> vertices;
    vertices.reserve(frame.points.size());
    for (const auto &point : frame.points)
        vertices.append({point.getPosition(), point.color});
    m_instances->setEntries({});
    publishMesh(makeMeshData(vertices, frame.indices));
}

RvPointCloudWidget::MeshData RvPointCloudWidget::makeMeshData(const QVector<MeshVertex> &vertices,
                                                             const QVector<quint32> &indices)
{
    if (indices.isEmpty())
        return {};
    MeshData mesh;
    mesh.vertices = QByteArray(reinterpret_cast<const char *>(vertices.constData()), vertices.size() * sizeof(MeshVertex));
    mesh.indices = QByteArray(reinterpret_cast<const char *>(indices.constData()), indices.size() * sizeof(quint32));
    return mesh;
}

void RvPointCloudWidget::publishMesh(const MeshData &mesh)
{
    m_view->setMesh(mesh.vertices, mesh.indices);
}

void RvPointCloudWidget::startDepthConversion()
{
    if (!m_depthDirty || m_depthConverting || m_pause->isChecked() || !m_renderingActive)
        return;

    const auto &depth = m_session->monocularDisplay() ? m_latestMonocularDepth : m_latestDepth;
    if (depth.isEmpty())
        return;
    // Keep one conversion in flight. New frames replace the pending snapshot.
    m_depthDirty = false;
    m_depthConverting = true;
    m_conversionEpoch = m_depthEpoch;
    const auto camera = RvCameraView::outputProfile(m_session->assistHostModel());
    const double minimum = m_depthColorMinimum;
    const double maximum = m_depthColorMaximum;
    const bool dynamic = m_colorMode->currentIndex() == DynamicColor;
    const bool mesh = m_displayMode->currentIndex() == Mesh;
    m_depthConversion.setFuture(QtConcurrent::run(&RvPointCloudWidget::projectDepth, depth, camera,
                                                  minimum, maximum, dynamic, mesh));
}

void RvPointCloudWidget::finishDepthConversion()
{
    m_depthConverting = false;
    auto future = m_depthConversion.future();
    if (!future.isCanceled() && future.resultCount() > 0) {
        auto projected = future.takeResult();
        if (m_conversionEpoch == m_depthEpoch && !m_pause->isChecked()) {
            m_depthInstances->setTable(std::move(projected.points));
            publishMesh(projected.mesh);
            m_hasDepthFrame = true;
            updateStatus();
        }
    }
    startDepthConversion();
}

void RvPointCloudWidget::projectDepth(QPromise<DepthProjection> &work, const QByteArray &depth,
    const RvCameraView::Profile &camera, double colorMinimum, double colorMaximum, bool dynamic, bool mesh)
{
    if (work.isCanceled())
        return;
    DepthProjection projected;
    const int columns = camera.outputWidth;
    const int rows = camera.outputHeight;
    const int pixels = columns * rows;
    QVector<float> cameraZ;
    cameraZ.resizeForOverwrite(pixels);
    qFromLittleEndian<quint32>(depth.constData(), pixels, cameraZ.data());
    for (float &z : cameraZ) {
        if (!std::isfinite(z) || z <= 0)
            z = 0;
    }
    std::optional<PointCloudColorScale::DynamicHistogram> histogram;
    if (dynamic)
        histogram.emplace(cameraZ, [](float z) { return z; });
    if (work.isCanceled())
        return;
    QVector<MeshVertex> vertices;
    QVector<quint32> grid;
    char *output = nullptr;
    if (mesh) {
        vertices.reserve(pixels);
        grid.fill(MissingVertex, pixels);
    } else {
        projected.points.resizeForOverwrite(pixels * sizeof(Instance));
        output = projected.points.data();
    }
    const double scale = MillimetresPerMetre / camera.focal;
    Instance point{};
    for (int row = 0; row < rows; ++row) {
        if (work.isCanceled())
            return;
        const double yScale = (camera.cy() - row) * scale;
        const int bottom = rows - 1 - row;
        for (int column = 0; column < columns; ++column) {
            const int i = row * columns + column;
            const int right = columns - 1 - column;
            const float *sample = cameraZ.constData() + i;
            const float metres = *sample;
            if (metres <= 0 || isDepthTransition(sample, 1, column, right)
                || isDepthTransition(sample, columns, row, bottom)
                || isDepthTransition(sample, columns + 1, std::min(row, column), std::min(bottom, right))
                || isDepthTransition(sample, columns - 1, std::min(row, right), std::min(bottom, column)))
                continue;

            // The rectified depth is already RGB camera Z. Flip Y and Z for Qt.
            const float x = float((column - camera.cx()) * scale * metres);
            const float y = float(yScale * metres);
            const float z = -float(metres * MillimetresPerMetre);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                continue;
            const auto rgb = histogram ? histogram->color(metres)
                                       : PointCloudColorScale::color(metres, colorMinimum, colorMaximum);
            const auto color = displayColor(rgb);
            if (mesh) {
                grid[i] = quint32(vertices.size());
                vertices.append({{x, y, z}, color});
                continue;
            }
            point.position = {x, y, z};
            // The unshaded point shader writes the display palette directly.
            point.color = color;
            std::memcpy(output, &point, sizeof(point));
            output += sizeof(point);
        }
    }
    if (mesh) {
        const auto &positions = std::as_const(vertices);
        const auto indices = meshIndices(grid.constData(), columns, rows,
                                         [&](quint32 index) { return -positions[index].position.z(); },
                                         [&work] { return work.isCanceled(); });
        if (work.isCanceled())
            return;
        projected.mesh = makeMeshData(vertices, indices);
    } else {
        projected.points.resize(output - projected.points.constData());
    }
    work.addResult(std::move(projected));
}

void RvPointCloudWidget::updateStatus()
{
    const bool paused = m_pause->isChecked();
    const bool ready = !paused && m_renderingActive;
    const bool tofAvailable = m_expiry.remainingTime() > 0;
    const bool assisted = capturesAssistedDepth(*m_session);
    m_view->setAssisted(assisted);
    if (m_diameterControls->isHidden() == assisted)
        m_diameterControls->setVisible(assisted);
    const int pointCount = assisted ? m_depthInstances->count() : m_instances->count();
    const qsizetype triangleCount = m_view->indexData().size() / (3 * sizeof(quint32));
    const bool native = capturesNative48(*m_session);
    const bool meshSelected = m_displayMode->currentIndex() == Mesh;
    const bool showingMesh = triangleCount > 0 || (meshSelected && pointCount == 0);
    m_displayMode->setEnabled(ready && (native || assisted));
    m_diameter->setEnabled(!paused && !meshSelected);
    m_pause->setText(paused ? tr("Resume point cloud") : tr("Stop point cloud"));
    m_reset->setEnabled(ready && (pointCount > 0 || triangleCount > 0) && (assisted || tofAvailable));
    QString text;
    if (paused) {
        text = tr("Point cloud stopped");
    } else if (ready && assisted && m_hasDepthFrame) {
        const auto size = RvCameraView::outputProfile(m_session->assistHostModel()).outputSize();
        const auto label = m_session->monocularDisplay() ? tr("monocular depth") : tr("assisted depth");
        text = showingMesh ? tr("%1×%2 %3 · %4 triangles").arg(size.width()).arg(size.height()).arg(label).arg(triangleCount)
            : tr("%1×%2 %3 · %4 / %5 points").arg(size.width()).arg(size.height()).arg(label)
                .arg(pointCount).arg(size.width() * size.height());
    } else if (ready && tofAvailable) {
        if (!RvAssistFusion::calibrationForFov(RvCalibration::Fov).valid)
            text = tr("F6 point cloud calibration unavailable");
        else if (showingMesh)
            text = tr("48×32 ToF · %1 triangles").arg(triangleCount);
        else
            text = tr("48×32 ToF · %1 / %2 points").arg(pointCount).arg(Columns * Rows);
    } else if (!assisted && !native) {
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
    m_displayMode->setItemText(Points, tr("Points"));
    m_displayMode->setItemText(Mesh, tr("Mesh"));
    m_displayMode->setAccessibleName(tr("Point cloud display mode"));
    m_colorMode->setItemText(FixedColor, tr("Fixed Z color range"));
    m_colorMode->setItemText(DynamicColor, tr("Dynamic + histogram equalization"));
    m_colorMode->setAccessibleName(tr("Point cloud color mode"));
    m_colorMode->setItemData(DynamicColor, tr("Equalize depths from 0.10 to 10.00 m in each frame. Colors can vary across frames."), Qt::ToolTipRole);
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
