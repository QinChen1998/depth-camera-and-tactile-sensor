#include "RvPointCloudWidget.h"
#include "DockableQuick3DWidget.h"
#include "RvAssistFusion.h"
#include "RvCalibration.h"
#include "RvCameraView.h"
#include "RvCaptureCalibration.h"
#include "RvSessionController.h"
#include "SceneInstanceBuffer.h"
#include "../RenderingSettings.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <QtQuick3D/QQuick3DGeometry>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr int Columns = 48;
constexpr int Rows = 32;
constexpr int AccumulationFrameLimit = 100;
constexpr int AccumulationUpdateMs = 100;
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
using Instance = SceneInstanceBuffer::InstanceTableEntry;
enum DisplayMode { Points, Mesh };

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

template <typename DepthAt>
QVector<quint32> meshIndices(const quint32 *grid, int columns, int rows, DepthAt depthAt)
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
    return RvCaptureContract::supports(mode)
        && RvCaptureContract::columns(mode) == Columns && RvCaptureContract::rows(mode) == Rows;
}

QVector4D displayColor(QRgb color)
{
    constexpr float ToColor = 1.f / 255.f;
    return {qRed(color) * ToColor, qGreen(color) * ToColor, qBlue(color) * ToColor, 1.f};
}

Instance nativePoint(const QVector3D &position, double minimum, double maximum)
{
    const float depthMm = -position.z();
    const float scale = std::max(3.5f, depthMm * .012f) / 100.f;
    const auto color = displayColor(PointCloudColorScale::color(depthMm / MillimetresPerMetre, minimum, maximum));
    return {{scale, 0, 0, position.x()}, {0, scale, 0, position.y()}, {0, 0, scale, position.z()}, color, {}};
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
      m_meshGeometry(new QQuick3DGeometry),
      m_view(new DockableQuick3DWidget(this, {"pointInstances", "depthInstances", "depthPointDiameter", "renderSettings",
          "meshGeometry", "meshVisible", "cameraTarget", "cameraDistance", "orbitYaw", "orbitPitch"})),
      m_status(new QLabel(this)), m_displayMode(new QComboBox(this)),
      m_accumulate(new QPushButton(this)), m_reset(new QPushButton(this)),
      m_diameterControls(new QWidget(this)),
      m_legendMinimum(new QLabel(this)), m_legendMaximum(new QLabel(this)),
      m_diameterLabel(new QLabel(m_diameterControls)), m_rangeLabel(new QLabel(this)),
      m_rangeSeparator(new QLabel(this)), m_diameter(new QDoubleSpinBox(m_diameterControls)),
      m_colorMinimum(new QDoubleSpinBox(this)), m_colorMaximum(new QDoubleSpinBox(this)),
      m_applyRange(new QPushButton(this))
{
    setObjectName("rvPointCloudWidget");
    m_instances->setObjectName("rvPointCloudInstances");
    m_depthInstances->setObjectName("rvPointCloudDepthInstances");
    m_meshGeometry->QObject::setParent(this);
    m_meshGeometry->setObjectName("rvPointCloudMeshGeometry");
    m_meshGeometry->setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
    m_meshGeometry->setStride(sizeof(MeshVertex));
    using Attribute = QQuick3DGeometry::Attribute;
    m_meshGeometry->addAttribute(Attribute::PositionSemantic, offsetof(MeshVertex, position), Attribute::F32Type);
    m_meshGeometry->addAttribute(Attribute::ColorSemantic, offsetof(MeshVertex, color), Attribute::F32Type);
    m_meshGeometry->addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
    m_view->setObjectName("rvPointCloudView");
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_view->setClearColor(QColor("#161b22"));
    m_view->setMinimumSize(300, 190);
    m_status->setObjectName("rvPointCloudStatus");
    m_status->setWordWrap(true);
    m_displayMode->setObjectName("rvPointCloudDisplayMode");
    m_displayMode->addItems({QString(), QString()});
    m_accumulate->setObjectName("rvPointCloudAccumulate");
    m_accumulate->setCheckable(true);
    m_reset->setObjectName("rvPointCloudReset");
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_status, 1);
    toolbar->addWidget(m_displayMode);
    toolbar->addWidget(m_accumulate);
    toolbar->addWidget(m_reset);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addLayout(toolbar);
    layout->addWidget(m_view, 1);
    setupDisplayControls(*layout);
    m_view->setInitialProperties({{"pointInstances", QVariant::fromValue(m_instances)},
        {"depthInstances", QVariant::fromValue(m_depthInstances)}, {"depthPointDiameter", m_diameter->value()},
        {"meshGeometry", QVariant::fromValue(static_cast<QObject *>(m_meshGeometry))},
        {"renderSettings", QVariant::fromValue(RenderingSettings::instance())}});

    auto *gradient = new QLabel(this);
    gradient->setObjectName("rvPointCloudColorLegend");
    QImage ramp(256, 1, QImage::Format_RGB32);
    for (int x = 0; x < ramp.width(); ++x)
        ramp.setPixel(x, 0, PointCloudColorScale::jet(double(x) / (ramp.width() - 1)));
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
    connect(&m_expiry, &QTimer::timeout, this, &RvPointCloudWidget::clearTofFrame);
    m_accumulationUpdate.setSingleShot(true);
    m_accumulationUpdate.setInterval(AccumulationUpdateMs);
    connect(&m_accumulationUpdate, &QTimer::timeout, this, &RvPointCloudWidget::presentLatest);
    connect(m_accumulate, &QPushButton::toggled, this, &RvPointCloudWidget::setAccumulating);
    connect(m_displayMode, &QComboBox::currentIndexChanged, this, &RvPointCloudWidget::changeDisplayMode);
    connect(m_session, &RvSessionController::tofFrameReady, this, &RvPointCloudWidget::acceptTofFrame);
    connect(m_session, &RvSessionController::assistDepthSnapshotReady, this, &RvPointCloudWidget::acceptAssistedDepth);
    connect(m_session, &RvSessionController::assistDepthStreamCleared, this, &RvPointCloudWidget::clearAssistedDepth);
    connect(m_session, &RvSessionController::streamCleared, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::deviceChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::imagingSwitchStarted, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::dtofCandidateSelectionChanged, this, &RvPointCloudWidget::clearFrames);
    connect(m_session, &RvSessionController::stateChanged, this, &RvPointCloudWidget::updateStatus);
    connect(&m_depthConversion, &QFutureWatcher<DepthProjection>::finished, this, &RvPointCloudWidget::finishDepthConversion);
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

void RvPointCloudWidget::setupDisplayControls(QVBoxLayout &layout)
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
        field->setRange(PointCloudColorScale::DefaultMinimumM, MaximumColorRangeM);
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

    auto *diameterRow = new QHBoxLayout(m_diameterControls);
    diameterRow->setContentsMargins(0, 0, 0, 0);
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
    layout.addWidget(m_diameterControls);
    layout.addLayout(rangeRow);

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
    recolorAccumulatedFrames();
    ++m_depthEpoch;
    m_depthDirty = !m_latestDepth.isEmpty();
    presentLatest();
}

void RvPointCloudWidget::updateColorLegend()
{
    m_legendMinimum->setText(QStringLiteral("Z  %1").arg(QLocale().toString(m_depthColorMinimum, 'f', 2)));
    m_legendMaximum->setText(QStringLiteral("%1 m").arg(QLocale().toString(m_depthColorMaximum, 'f', 2)));
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
    if (m_accumulate->isChecked())
        appendAccumulatedFrame();
    else
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
    clearAccumulatedFrames();
    if (!capturesNative48(*m_session)) {
        const QSignalBlocker blocker(m_accumulate);
        m_accumulate->setChecked(false);
    }
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
    if (!m_accumulate->isChecked()) {
        m_instances->setEntries({});
        if (capturesNative48(*m_session))
            publishMesh({});
    }
    updateStatus();
}

void RvPointCloudWidget::presentLatest()
{
    if (!m_renderingActive)
        return;
    if (auto *scene = m_view->rootObject()) {
        scene->setProperty("depthPointDiameter", m_diameter->value());
        if (capturesNative48(*m_session))
            presentNativeFrames();
        startDepthConversion();
    }
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
        projected.points.append(nativePoint(position, m_depthColorMinimum, m_depthColorMaximum));
    }
    if (mesh) {
        const auto &points = std::as_const(projected.points);
        projected.indices = meshIndices(grid.data(), Columns, Rows,
                                        [&](quint32 index) { return -points[index].getPosition().z(); });
    }
    return projected;
}

void RvPointCloudWidget::setAccumulating(bool enabled)
{
    clearAccumulatedFrames();
    if (enabled && m_expiry.remainingTime() > 0)
        appendAccumulatedFrame();
    presentLatest();
}

void RvPointCloudWidget::appendAccumulatedFrame()
{
    if (m_accumulatedFrames.size() == AccumulationFrameLimit)
        m_accumulatedFrames.pop_front();
    m_accumulatedFrames.push_back(projectTofFrame(true));
    // Keep every admitted frame while coalescing uploads to the view.
    if (m_renderingActive && !m_accumulationUpdate.isActive())
        m_accumulationUpdate.start();
}

void RvPointCloudWidget::clearAccumulatedFrames()
{
    m_accumulationUpdate.stop();
    m_accumulatedFrames.clear();
}

void RvPointCloudWidget::recolorAccumulatedFrames()
{
    for (auto &frame : m_accumulatedFrames) {
        for (auto &point : frame.points) {
            const double z = -point.getPosition().z() / MillimetresPerMetre;
            point.color = displayColor(PointCloudColorScale::color(z, m_depthColorMinimum, m_depthColorMaximum));
        }
    }
}

void RvPointCloudWidget::presentNativeFrames()
{
    m_accumulationUpdate.stop();
    const bool accumulating = m_accumulate->isChecked();
    const bool mesh = m_displayMode->currentIndex() == Mesh;
    const bool fresh = m_expiry.remainingTime() > 0;
    if (!mesh && !accumulating) {
        m_instances->setEntries(fresh ? projectTofFrame(false).points : QVector<Instance>{});
        publishMesh({});
        return;
    }
    NativeFrame latest;
    QList<const NativeFrame *> frames;
    if (accumulating) {
        for (const auto &frame : m_accumulatedFrames)
            frames.append(&frame);
    } else if (fresh) {
        latest = projectTofFrame(true);
        frames.append(&latest);
    }

    qsizetype pointCount = 0;
    qsizetype indexCount = 0;
    for (const auto *frame : frames) {
        pointCount += frame->points.size();
        indexCount += frame->indices.size();
    }
    if (!mesh) {
        QByteArray table;
        table.reserve(pointCount * sizeof(Instance));
        for (const auto *frame : frames) {
            if (!frame->points.isEmpty())
                table.append(reinterpret_cast<const char *>(frame->points.constData()), frame->points.size() * sizeof(Instance));
        }
        m_instances->setTable(std::move(table));
        publishMesh({});
        return;
    }

    QVector<MeshVertex> vertices;
    QVector<quint32> indices;
    vertices.reserve(pointCount);
    indices.reserve(indexCount);
    for (const auto *frame : frames) {
        if (frame->indices.isEmpty())
            continue;
        const auto base = quint32(vertices.size());
        for (const auto &point : frame->points)
            vertices.append({point.getPosition(), point.color});
        for (const auto index : frame->indices)
            indices.append(base + index);
    }
    m_instances->setEntries({});
    publishMesh(makeMeshData(vertices, indices));
}

RvPointCloudWidget::MeshData RvPointCloudWidget::makeMeshData(const QVector<MeshVertex> &vertices,
                                                             const QVector<quint32> &indices)
{
    if (indices.isEmpty())
        return {};
    MeshData mesh;
    mesh.minimum = mesh.maximum = vertices.first().position;
    for (const auto &vertex : vertices) {
        const auto &p = vertex.position;
        mesh.minimum = {std::min(mesh.minimum.x(), p.x()), std::min(mesh.minimum.y(), p.y()), std::min(mesh.minimum.z(), p.z())};
        mesh.maximum = {std::max(mesh.maximum.x(), p.x()), std::max(mesh.maximum.y(), p.y()), std::max(mesh.maximum.z(), p.z())};
    }
    mesh.vertices = QByteArray(reinterpret_cast<const char *>(vertices.constData()), vertices.size() * sizeof(MeshVertex));
    mesh.indices = QByteArray(reinterpret_cast<const char *>(indices.constData()), indices.size() * sizeof(quint32));
    return mesh;
}

void RvPointCloudWidget::publishMesh(const MeshData &mesh)
{
    const bool hasFaces = !mesh.indices.isEmpty();
    if (hasFaces || !m_meshGeometry->indexData().isEmpty()) {
        m_meshGeometry->setVertexData(mesh.vertices);
        m_meshGeometry->setIndexData(mesh.indices);
        m_meshGeometry->setBounds(mesh.minimum, mesh.maximum);
        m_meshGeometry->update();
    }
    if (auto *scene = m_view->rootObject())
        scene->setProperty("meshVisible", hasFaces);
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
    const bool mesh = m_displayMode->currentIndex() == Mesh;
    m_depthConversion.setFuture(QtConcurrent::run([depth = m_latestDepth, camera, minimum, maximum, mesh] {
        return projectDepth(depth, camera, minimum, maximum, mesh);
    }));
}

void RvPointCloudWidget::finishDepthConversion()
{
    m_depthConverting = false;
    if (m_conversionEpoch == m_depthEpoch) {
        auto projected = m_depthConversion.result();
        m_depthInstances->setTable(std::move(projected.points));
        publishMesh(projected.mesh);
        m_hasDepthFrame = true;
        updateStatus();
    }
    startDepthConversion();
}

RvPointCloudWidget::DepthProjection RvPointCloudWidget::projectDepth(const QByteArray &depth,
    const RvCameraView::Profile &camera, double colorMinimum, double colorMaximum, bool mesh)
{
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
    Instance point{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {}, {}};
    for (int row = 0; row < rows; ++row) {
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
            const auto color = displayColor(PointCloudColorScale::color(metres, colorMinimum, colorMaximum));
            if (mesh) {
                grid[i] = quint32(vertices.size());
                vertices.append({{x, y, z}, color});
                continue;
            }
            point.row0.setW(x);
            point.row1.setW(y);
            point.row2.setW(z);
            // The unshaded point shader writes the display palette directly.
            point.color = color;
            std::memcpy(output, &point, sizeof(point));
            output += sizeof(point);
        }
    }
    if (mesh) {
        const auto &positions = std::as_const(vertices);
        const auto indices = meshIndices(grid.constData(), columns, rows,
                                         [&](quint32 index) { return -positions[index].position.z(); });
        projected.mesh = makeMeshData(vertices, indices);
    } else {
        projected.points.resize(output - projected.points.constData());
    }
    return projected;
}

void RvPointCloudWidget::updateStatus()
{
    const bool ready = m_view->status() == QQuickWidget::Ready;
    const bool tofAvailable = m_expiry.remainingTime() > 0;
    const bool assisted = capturesAssistedDepth(*m_session);
    if (m_diameterControls->isHidden() == assisted)
        m_diameterControls->setVisible(assisted);
    const bool accumulating = m_accumulate->isChecked();
    const int pointCount = assisted ? m_depthInstances->count() : m_instances->count();
    const qsizetype triangleCount = m_meshGeometry->indexData().size() / (3 * sizeof(quint32));
    const bool native = capturesNative48(*m_session);
    const bool meshSelected = m_displayMode->currentIndex() == Mesh;
    const bool showingMesh = triangleCount > 0 || (meshSelected && pointCount == 0);
    m_displayMode->setEnabled(ready && (native || assisted));
    m_accumulate->setEnabled(ready && native);
    m_diameter->setEnabled(!meshSelected);
    m_reset->setEnabled(ready && (pointCount > 0 || triangleCount > 0) && (accumulating || assisted || tofAvailable));
    QString text;
    if (m_view->status() == QQuickWidget::Error) {
        text = tr("3D view unavailable");
    } else if (ready && accumulating && !m_accumulatedFrames.empty()) {
        text = (showingMesh ? tr("%1 / %2 frames · %3 triangles") : tr("%1 / %2 frames · %3 points"))
            .arg(m_accumulatedFrames.size()).arg(AccumulationFrameLimit).arg(showingMesh ? triangleCount : pointCount);
    } else if (ready && assisted && m_hasDepthFrame) {
        const auto size = RvCameraView::profile(m_session->assistHostModel()).outputSize();
        text = showingMesh ? tr("%1×%2 assisted depth · %3 triangles").arg(size.width()).arg(size.height()).arg(triangleCount)
            : tr("%1×%2 assisted depth · %3 / %4 points").arg(size.width()).arg(size.height())
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
    m_accumulate->setText(tr("Accumulate point cloud"));
    m_accumulate->setToolTip(tr("Overlay the latest 100 native ToF frames in camera coordinates."));
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
