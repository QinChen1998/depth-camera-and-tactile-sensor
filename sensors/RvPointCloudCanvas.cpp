#include "RvPointCloudCanvas.h"
#include <QImage>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QQuaternion>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double Pi = 3.14159265358979323846;
struct ScreenPoint { double x, y, z; QVector4D color; float diameter; };
QRgb rgb(const QVector4D &c) {
    return qRgb(qBound(0, qRound(c.x() * 255), 255), qBound(0, qRound(c.y() * 255), 255),
                qBound(0, qRound(c.z() * 255), 255));
}
}
RvPointCloudCanvas::RvPointCloudCanvas(SceneInstanceBuffer *native, SceneInstanceBuffer *assisted, QWidget *parent)
    : QWidget(parent), m_native(native), m_assisted(assisted) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(300, 190);
    for (auto *source : {m_native, m_assisted})
        connect(source, &SceneInstanceBuffer::instanceTableChanged, this, qOverload<>(&QWidget::update));
}
void RvPointCloudCanvas::setMesh(const QByteArray &vertices, const QByteArray &indices) {
    m_vertices = vertices;
    m_indices = indices;
    update();
}
void RvPointCloudCanvas::setAssisted(bool enabled) {
    if (m_showAssisted == enabled) return;
    m_showAssisted = enabled;
    update();
}
void RvPointCloudCanvas::setDepthPointDiameter(double diameter) {
    if (m_depthPointDiameter == diameter) return;
    m_depthPointDiameter = diameter;
    update();
}
void RvPointCloudCanvas::resetView() {
    m_target = {0, 0, -600}; m_distance = 900; m_yaw = -25; m_pitch = -18; update();
}
QImage RvPointCloudCanvas::renderFrame() const {
    const double dpr = devicePixelRatioF();
    const int w = std::max(1, qRound(width() * dpr)), h = std::max(1, qRound(height() * dpr));
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(QColor("#161b22"));
    if (!hasPoints()) { image.setDevicePixelRatio(dpr); return image; }
    QVector<float> zBuffer(w * h, std::numeric_limits<float>::infinity());
    const double focal = h / (2 * std::tan(Pi / 8));
    const double nearPlane = std::max(1., m_distance / 2000);
    const QQuaternion orbit = QQuaternion::fromEulerAngles(float(m_pitch), float(m_yaw), 0);
    const QVector3D eye = m_target + orbit.rotatedVector({0, 0, float(m_distance)});
    const auto right = orbit.rotatedVector({1, 0, 0});
    const auto up = orbit.rotatedVector({0, 1, 0});
    const auto forward = orbit.rotatedVector({0, 0, -1});
    const auto project = [&](const QVector3D &p) {
        const auto offset = p - eye;
        const double z = QVector3D::dotProduct(offset, forward);
        if (z <= nearPlane) return ScreenPoint{0, 0, z, {}, 0};
        return ScreenPoint{w / 2. + QVector3D::dotProduct(offset, right) * focal / z,
            h / 2. - QVector3D::dotProduct(offset, up) * focal / z, z, {}, 0};
    };
    const auto write = [&](int x, int y, double z, QRgb color) {
        auto &old = zBuffer[y * w + x];
        if (z < old) { old = float(z); reinterpret_cast<QRgb *>(image.scanLine(y))[x] = color; }
    };
    if (!meshVisible()) {
        const auto *points = m_showAssisted ? m_assisted : m_native;
        for (int i = 0; i < points->count(); ++i) {
            const auto point = points->instance(i);
            const auto p = project(point.position);
            if (p.z <= nearPlane) continue;
            const double diameter = m_showAssisted ? m_depthPointDiameter * dpr : point.diameter * focal / p.z;
            const double half = std::max(.5, diameter * .5);
            // Reject off-screen primitives before conversion to bounded integer pixel coordinates.
            if (p.x + half < 0 || p.y + half < 0 || p.x - half >= w || p.y - half >= h) continue;
            const int left = int(std::max(0., std::floor(p.x - half)));
            const int right = int(std::min(double(w - 1), std::ceil(p.x + half)));
            const int top = int(std::max(0., std::floor(p.y - half)));
            const int bottom = int(std::min(double(h - 1), std::ceil(p.y + half)));
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x) write(x, y, p.z, rgb(point.color));
        }
    } else {
        QVector<ScreenPoint> screen;
        const auto vertexCount = m_vertices.size() / sizeof(Vertex);
        screen.reserve(vertexCount);
        for (qsizetype i = 0; i < vertexCount; ++i) {
            Vertex vertex;
            std::memcpy(&vertex, m_vertices.constData() + i * sizeof(vertex), sizeof(vertex));
            auto p = project(vertex.position);
            p.color = vertex.color;
            screen.append(p);
        }
        // Perspective-correct interpolation and a Z buffer preserve occlusion and smooth mesh color.
        for (qsizetype i = 0; i + 3 * sizeof(quint32) <= m_indices.size(); i += 3 * sizeof(quint32)) {
            quint32 triangle[3];
            std::memcpy(triangle, m_indices.constData() + i, sizeof(triangle));
            if (std::max({triangle[0], triangle[1], triangle[2]}) >= screen.size()) continue;
            const auto &a = screen[triangle[0]], &b = screen[triangle[1]], &c = screen[triangle[2]];
            if (a.z <= nearPlane || b.z <= nearPlane || c.z <= nearPlane) continue;
            const double den = (b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
            if (std::abs(den) < 1e-9) continue;
            const double minX = std::min({a.x,b.x,c.x}), maxX = std::max({a.x,b.x,c.x});
            const double minY = std::min({a.y,b.y,c.y}), maxY = std::max({a.y,b.y,c.y});
            if (maxX < 0 || maxY < 0 || minX >= w || minY >= h) continue;
            const int left = int(std::max(0., std::floor(minX))), right = int(std::min(double(w-1),std::ceil(maxX)));
            const int top = int(std::max(0.,std::floor(minY))), bottom = int(std::min(double(h-1),std::ceil(maxY)));
            for (int y=top; y<=bottom; ++y) for (int x=left; x<=right; ++x) {
                const double u = ((b.y-c.y)*(x+.5-c.x)+(c.x-b.x)*(y+.5-c.y))/den;
                const double v = ((c.y-a.y)*(x+.5-c.x)+(a.x-c.x)*(y+.5-c.y))/den;
                const double t = 1-u-v;
                if (std::min({u,v,t}) < -1e-7) continue;
                const double inverseZ = u/a.z+v/b.z+t/c.z;
                const auto color = (a.color*float(u/a.z)+b.color*float(v/b.z)+c.color*float(t/c.z))/float(inverseZ);
                write(x,y,1/inverseZ,rgb(color));
            }
        }
    }
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto origin = project({});
    if (origin.z > nearPlane) {
        const QVector3D axes[] = {{100,0,0},{0,-100,0},{0,0,-100}};
        const QColor colors[] = {QColor("#ce7979"),QColor("#7ca58a"),QColor("#7da7d2")};
        for (int i=0;i<3;++i) {
            const auto tip = project(axes[i]);
            if (tip.z <= nearPlane) continue;
            painter.setPen(QPen(colors[i],std::max(1.,dpr)));
            painter.drawLine(QPointF(origin.x,origin.y),QPointF(tip.x,tip.y));
            painter.drawText(QPointF(tip.x+3*dpr,tip.y-3*dpr),QString(QChar('X'+i)));
        }
    }
    painter.end();
    image.setDevicePixelRatio(dpr);
    return image;
}
QImage RvPointCloudCanvas::grabFramebuffer() const { return renderFrame(); }
void RvPointCloudCanvas::paintEvent(QPaintEvent *) { QPainter(this).drawImage(QPoint(),renderFrame()); }
void RvPointCloudCanvas::mousePressEvent(QMouseEvent *event) { m_previous = event->position(); }
void RvPointCloudCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (!hasPoints() || !(event->buttons() & (Qt::LeftButton|Qt::MiddleButton))) return;
    const auto delta = event->position()-m_previous; m_previous=event->position();
    if (event->buttons() & Qt::MiddleButton) {
        const float scale = float(2*m_distance*std::tan(Pi/8)/std::max(1,height()));
        const auto orbit=QQuaternion::fromEulerAngles(float(m_pitch),float(m_yaw),0);
        m_target += orbit.rotatedVector(QVector3D(float(-delta.x()),float(delta.y()),0)*scale);
    } else { m_yaw -= delta.x()*.45; m_pitch=std::clamp(m_pitch-delta.y()*.45,-85.,85.); }
    update();
}
void RvPointCloudCanvas::wheelEvent(QWheelEvent *event) {
    if (!hasPoints()) return;
    const double steps=event->angleDelta().y() ? event->angleDelta().y()/120. : event->pixelDelta().y()/40.;
    m_distance=std::clamp(m_distance*std::pow(.86,steps),20.,60000.);
    event->accept(); update();
}
