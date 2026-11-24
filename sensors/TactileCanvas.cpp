#include "TactileCanvas.h"
#include "TactileSceneAdapter.h"
#include "TactileSurfaceModel.h"
#include <QMouseEvent>
#include <QLabel>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double HalfFovTangent = .414213562373095;
constexpr float ToMillimetres = 1000;
constexpr float GlyphMillimetres = .1f;
const QColor AxisColors[] = {QColor("#ff525d"), QColor("#48e58b"), QColor("#40baff")};

class TactileProjection {
public:
    TactileProjection(QSize size, QVector3D target, double distance, double pitch, double yaw)
        : m_center(size.width() * .5, size.height() * .5), m_focal(size.height() / (2 * HalfFovTangent)) {
        const auto orbit = QQuaternion::fromEulerAngles(float(pitch), float(yaw), 0);
        m_right = orbit.rotatedVector({1, 0, 0});
        m_up = orbit.rotatedVector({0, 1, 0});
        m_forward = orbit.rotatedVector({0, 0, -1});
        m_eye = target - m_forward * float(distance);
    }

    QVector3D project(const QVector3D &mm) const {
        const auto relative = mm - m_eye;
        const float depth = QVector3D::dotProduct(relative, m_forward);
        if (depth <= 1 || !std::isfinite(depth)) return {};
        const double scale = m_focal / depth;
        return {float(m_center.x() + QVector3D::dotProduct(relative, m_right) * scale),
                float(m_center.y() - QVector3D::dotProduct(relative, m_up) * scale), depth};
    }

    double pixels(double millimetres, float depth) const { return millimetres * m_focal / depth; }

private:
    QPointF m_center;
    double m_focal;
    QVector3D m_eye, m_right, m_up, m_forward;
};

QVector3D pointPosition(const QVariantMap &point, bool dots) {
    return point[dots ? "dotPosition" : "position"].value<QVector3D>() * ToMillimetres;
}

void drawArrow(QPainter &painter, const TactileProjection &projection, const QVector3D &position,
               const QVariantMap &arrow, const QColor &color, double width, double size = 1) {
    if (!arrow["arrowVisible"].toBool()) return;
    const auto tail = projection.project(position);
    const auto direction = arrow["rotation"].value<QQuaternion>().rotatedVector({0, 1, 0});
    const auto end = projection.project(position + direction * float(arrow["length"].toDouble() * size * GlyphMillimetres));
    const auto tip = projection.project(position + direction * float((arrow["length"].toDouble() + 3) * size * GlyphMillimetres));
    if (tail.z() <= 0 || end.z() <= 0 || tip.z() <= 0) return;
    const auto start = tail.toPointF();
    const auto shaftEnd = end.toPointF();
    const auto head = tip.toPointF();
    const auto offset = head - start;
    const double extent = std::hypot(offset.x(), offset.y());
    if (extent < .05) return;
    const double shaft = std::max(.8, projection.pixels(width * size * GlyphMillimetres, end.z()));
    const auto normal = QPointF(-offset.y(), offset.x()) * (shaft / extent);
    painter.setPen(QPen(color, shaft, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(start, shaftEnd);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(QPolygonF{head, shaftEnd + normal, shaftEnd - normal});
}
}

TactileCanvas::TactileCanvas(TactileSceneAdapter *scene, QWidget *parent)
    : QWidget(parent), m_scene(scene), m_unavailable(new QLabel(tr("Device disconnected / data unavailable"), this)) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_unavailable->setObjectName("tactileNoDataMessage");
    m_unavailable->setAlignment(Qt::AlignCenter);
    m_unavailable->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_unavailable->setStyleSheet("color: #d2dce8; background: transparent;");
    m_unavailable->setVisible(!m_scene->valid());
    fitModel();
    connect(m_scene, &TactileSceneAdapter::surfaceChanged, this, [this] {
        m_housing = {};
        m_autoFrame = true;
        fitModel();
        if (m_dotMatrix) fitDots(false);
        setInspectedPoint(-1);
        update();
    });
    connect(m_scene, &TactileSceneAdapter::frameChanged, this, [this] {
        m_unavailable->setVisible(!m_scene->valid());
        if (m_dotMatrix) {
            if (m_framedGain != m_scene->arrowGain()) {
                m_autoFrame = true;
                fitDots(false);
            } else if (m_autoFrame) fitDots(true);
        }
        if (m_inspectedPoint >= m_scene->points().size() || !m_scene->valid()) m_inspectedPoint = -1;
        update();
    });
    connect(m_scene, &TactileSceneAdapter::optionsChanged, this, [this] {
        if (m_dotMatrix != m_scene->dotMatrix()) {
            m_dotMatrix = m_scene->dotMatrix();
            m_autoFrame = true;
            if (m_dotMatrix) fitDots(false);
            else fitModel();
        }
        update();
    });
    connect(m_scene, &TactileSceneAdapter::themeChanged, this, qOverload<>(&QWidget::update));
    connect(m_scene, &TactileSceneAdapter::sourceLabelChanged, this, qOverload<>(&QWidget::update));
}

bool TactileCanvas::ready() const { return m_scene->surfaceModel().valid; }

void TactileCanvas::setInspectedPoint(int point) {
    if (point == m_inspectedPoint) return;
    m_inspectedPoint = point;
    update();
}

void TactileCanvas::fitModel() {
    const auto &model = m_scene->surfaceModel();
    const auto size = model.boundsSize * ToMillimetres;
    const double aspect = double(width()) / std::max(1, height());
    m_target = model.boundsCenter * ToMillimetres;
    m_distance = std::max(20., std::max(double(size.y()), size.x() / std::max(.35, aspect))
        * 1.5 / (2 * HalfFovTangent) + size.z());
}

void TactileCanvas::fitDots(bool expandOnly) {
    if (!m_scene->valid()) return;
    const auto &model = m_scene->surfaceModel();
    const auto center = model.boundsCenter * ToMillimetres;
    const auto half = model.boundsSize * (ToMillimetres * .5f);
    auto low = center - half;
    auto high = center + half;
    const auto dotLow = m_scene->dotBoundsMinimum() * ToMillimetres;
    const auto dotHigh = m_scene->dotBoundsMaximum() * ToMillimetres;
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(low[axis], dotLow[axis] - .35f);
        high[axis] = std::max(high[axis], dotHigh[axis] + .35f);
    }
    if (!expandOnly) m_target = (low + high) * .5f;
    QVector3D extents;
    for (int axis = 0; axis < 3; ++axis)
        extents[axis] = std::max(std::abs(low[axis] - m_target[axis]), std::abs(high[axis] - m_target[axis]));
    const double aspect = std::clamp(double(width()) / std::max(1, height()), .1, 1.);
    const double distance = std::max(20., extents.length() / std::sin(std::atan(HalfFovTangent * aspect)) * 1.08);
    if (!expandOnly || distance > m_distance) m_distance = distance;
    m_framedGain = m_scene->arrowGain();
}

void TactileCanvas::resetView() {
    m_yaw = -35;
    m_pitch = -25;
    m_autoFrame = true;
    fitModel();
    if (m_dotMatrix) fitDots(false);
    update();
}

void TactileCanvas::frontView() {
    resetView();
    m_yaw = m_pitch = 0;
}

QVector3D TactileCanvas::project(const QVector3D &metres) const {
    return TactileProjection(size(), m_target, m_distance, m_pitch, m_yaw).project(metres * ToMillimetres);
}

int TactileCanvas::pickPoint(const QPointF &position) const {
    if (!m_scene->valid() || !rect().contains(position.toPoint())) return -1;
    const TactileProjection projection(size(), m_target, m_distance, m_pitch, m_yaw);
    const auto points = m_scene->points();
    int closest = -1, direct = -1;
    double closestDistance = 12 * 12;
    float front = std::numeric_limits<float>::infinity();
    for (int i = 0; i < points.size(); ++i) {
        const auto point = points[i].toMap();
        const auto centre = projection.project(pointPosition(point, m_dotMatrix));
        if (centre.z() <= 0 || !rect().contains(centre.toPoint())) continue;
        const auto delta = position - centre.toPointF();
        const auto distance = QPointF::dotProduct(delta, delta);
        const auto radius = std::max(1.5, projection.pixels((m_dotMatrix ? 3.5 : point["radius"].toDouble()) * GlyphMillimetres, centre.z()));
        if (distance <= radius * radius && centre.z() < front) { direct = i; front = centre.z(); }
        if (i < m_scene->rawPointCount() && distance < closestDistance) { closest = i; closestDistance = distance; }
    }
    return direct >= 0 ? direct : closest;
}

void TactileCanvas::redrawHousing() {
    const double dpr = devicePixelRatioF();
    const QSize pixels(qRound(width() * dpr), qRound(height() * dpr));
    if (!m_housing.isNull() && m_housing.size() == pixels && m_housing.devicePixelRatio() == dpr
        && m_drawnDistance == m_distance && m_drawnTarget == m_target && m_drawnPitch == m_pitch && m_drawnYaw == m_yaw) return;
    m_drawnTarget = m_target;
    m_drawnDistance = m_distance;
    m_drawnYaw = m_yaw;
    m_drawnPitch = m_pitch;
    m_housing = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
    m_housing.setDevicePixelRatio(dpr);
    m_housing.fill(Qt::transparent);
    if (!ready() || m_housing.isNull()) return;

    const auto &model = m_scene->surfaceModel();
    const TactileProjection projection(size(), m_target, m_distance, m_pitch, m_yaw);
    QVector<QVector3D> projected;
    projected.reserve(model.meshVertices.size());
    for (const auto &vertex : model.meshVertices) projected.append(projection.project(vertex));
    struct Face { quint32 index; float depth; };
    QVector<Face> visible;
    visible.reserve(model.meshIndices.size() / 3);
    for (int i = 0; i < model.meshIndices.size(); i += 3) {
        const auto a = projected[model.meshIndices[i]], b = projected[model.meshIndices[i + 1]], c = projected[model.meshIndices[i + 2]];
        if (a.z() <= 0 || b.z() <= 0 || c.z() <= 0) continue;
        visible.append({quint32(i), a.z() + b.z() + c.z()});
    }
    std::sort(visible.begin(), visible.end(), [](const Face &a, const Face &b) { return a.depth > b.depth; });

    QPainter painter(&m_housing);
    painter.setPen(Qt::NoPen);
    const auto light = QVector3D(-.45f, .55f, .7f).normalized();
    for (const auto face : visible) {
        const auto i = face.index;
        const auto a = model.meshIndices[i], b = model.meshIndices[i + 1], c = model.meshIndices[i + 2];
        const auto normal = QVector3D::normal(model.meshVertices[a], model.meshVertices[b], model.meshVertices[c]);
        const double shade = .52 + .48 * std::abs(QVector3D::dotProduct(normal, light));
        painter.setBrush(QColor(qRound(150 * shade), qRound(165 * shade), qRound(185 * shade)));
        const QPointF triangle[] = {projected[a].toPointF(), projected[b].toPointF(), projected[c].toPointF()};
        painter.drawConvexPolygon(triangle, 3);
    }
}

void TactileCanvas::paintEvent(QPaintEvent *) {
    redrawHousing();
    QPainter painter(this);
    painter.fillRect(rect(), m_scene->background());
    painter.setOpacity(m_scene->modelOpacity());
    painter.drawImage(QPoint(), m_housing);
    painter.setOpacity(1);
    painter.setRenderHint(QPainter::Antialiasing, !m_scene->lowCostRendering());
    const TactileProjection projection(size(), m_target, m_distance, m_pitch, m_yaw);
    const QVector3D origin(-18, -18, 0);
    const QVector3D axes[] = {{3.3f, 0, 0}, {0, 3.3f, 0}, {0, 0, -3.3f}};
    const auto centre = projection.project(origin);
    if (centre.z() > 0) {
        for (int i = 0; i < 3; ++i) {
            const auto end = projection.project(origin + axes[i]);
            if (end.z() <= 0) continue;
            painter.setPen(AxisColors[i]);
            painter.drawLine(centre.toPointF(), end.toPointF());
            painter.drawText(end.toPointF() + QPointF(3, -3), QString(QChar('X' + i)));
        }
    }
    if (m_scene->valid()) {
        const auto points = m_scene->points();
        const auto widthGain = m_scene->arrowWidthGain();
        struct Dot { QVector3D position; double radius; QColor color; };
        QVector<Dot> dots;
        dots.reserve(points.size());
        for (const auto &value : points) {
            const auto point = value.toMap();
            const auto position = pointPosition(point, m_dotMatrix);
            const auto display = projection.project(position);
            if (display.z() <= 0) continue;
            const double radius = m_dotMatrix ? 3.5 : point["radius"].toDouble();
            dots.append({display, std::max(1.5, projection.pixels(radius * GlyphMillimetres, display.z())),
                         point["color"].value<QColor>()});
            if (m_dotMatrix) continue;
            if (m_scene->pointArrows()) drawArrow(painter, projection, position, point, m_scene->textColor(), radius * widthGain);
            if (m_scene->componentArrows()) {
                const auto components = point["components"].toList();
                for (int axis = 0; axis < components.size(); ++axis)
                    drawArrow(painter, projection, position, components[axis].toMap(), AxisColors[axis], 1.75 * widthGain);
            }
        }
        std::sort(dots.begin(), dots.end(), [](const Dot &a, const Dot &b) { return a.position.z() > b.position.z(); });
        painter.setPen(Qt::NoPen);
        for (const auto &dot : dots) {
            painter.setBrush(dot.color);
            painter.drawEllipse(dot.position.toPointF(), dot.radius, dot.radius);
        }
        if (!m_dotMatrix && m_scene->resultantArrow()) {
            const auto resultant = m_scene->resultant();
            drawArrow(painter, projection, pointPosition(resultant, false), resultant, m_scene->accent(),
                      1.75 * widthGain, m_scene->resultantSizeGain());
        }
        if (m_inspectedPoint >= 0 && m_inspectedPoint < points.size()) {
            const auto point = points[m_inspectedPoint].toMap();
            const auto units = point["interpolated"].toBool() ? tr("Interpolated counts")
                : m_scene->sourceLabel().contains("DELTA") ? tr("Delta counts") : tr("Raw counts");
            const auto text = tr("%1\n%2  X %3  Y %4  Z %5").arg(point["id"].toString(), units)
                .arg(point["fx"].toDouble(), 0, 'f', 0).arg(point["fy"].toDouble(), 0, 'f', 0).arg(point["fz"].toDouble(), 0, 'f', 0);
            auto bounds = painter.fontMetrics().boundingRect(rect().adjusted(0, 0, -24, 0), Qt::TextWordWrap, text);
            bounds.adjust(-6, -4, 6, 4);
            bounds.moveBottomLeft(QPoint(6, height() - 6));
            painter.setPen(m_scene->border());
            painter.setBrush(m_scene->background());
            painter.drawRect(bounds);
            painter.setPen(m_scene->textColor());
            painter.drawText(bounds.adjusted(6, 4, -6, -4), Qt::TextWordWrap, text);
        }
    }
    painter.end();
    emit painted();
}

QImage TactileCanvas::grabFramebuffer() { return grab().toImage(); }

void TactileCanvas::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) m_unavailable->setText(tr("Device disconnected / data unavailable"));
}

void TactileCanvas::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    m_unavailable->setGeometry(rect());
    if (!m_autoFrame) return;
    if (m_dotMatrix) fitDots(true);
    else fitModel();
}

void TactileCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) return;
    m_pressedButton = event->button();
    m_pressPosition = m_previousPosition = event->position();
    m_dragged = false;
    event->accept();
}

void TactileCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (m_pressedButton == Qt::NoButton || !(event->buttons() & m_pressedButton)) return;
    const auto delta = event->position() - m_previousPosition;
    const auto displacement = event->position() - m_pressPosition;
    m_previousPosition = event->position();
    m_dragged = m_dragged || QPointF::dotProduct(displacement, displacement) >= 36;
    if (!m_dragged) return;
    m_autoFrame = false;
    if (m_pressedButton == Qt::MiddleButton) {
        const auto orbit = QQuaternion::fromEulerAngles(float(m_pitch), float(m_yaw), 0);
        const double scale = 2 * m_distance * HalfFovTangent / std::max(1, height());
        m_target += orbit.rotatedVector(QVector3D(float(-delta.x() * scale), float(delta.y() * scale), 0));
        for (int axis = 0; axis < 3; ++axis) m_target[axis] = std::clamp(m_target[axis], -80.f, 80.f);
    } else {
        m_yaw -= delta.x() * .45;
        m_pitch = std::clamp(m_pitch - delta.y() * .45, -85., 85.);
    }
    setInspectedPoint(-1);
    update();
}

void TactileCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != m_pressedButton) return;
    m_pressedButton = Qt::NoButton;
    if (!m_dragged && event->button() == Qt::LeftButton) {
        setInspectedPoint(pickPoint(event->position()));
        if (m_inspectedPoint >= 0 && m_inspectedPoint < m_scene->rawPointCount()) m_scene->selectRawPoint(m_inspectedPoint);
    }
}

void TactileCanvas::wheelEvent(QWheelEvent *event) {
    const double steps = event->angleDelta().y() ? event->angleDelta().y() / 120. : event->pixelDelta().y() / 40.;
    if (!steps) return;
    m_autoFrame = false;
    m_distance = std::clamp(m_distance * std::pow(.88, steps), 20., m_dotMatrix ? 5000. : 250.);
    event->accept();
    update();
}
