#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtWidgets/QComboBox>

// Presentation only: source pixels, metadata and saved snapshots stay unchanged.
namespace ImagePresentation {
inline constexpr int Margin = 6;
inline constexpr int Spacing = 4;
inline constexpr int ToolbarHeight = 28;
inline constexpr int MinimumViewportWidth = 260;
inline constexpr int MinimumViewportHeight = 170;

inline int normalizedRotation(int degrees)
{
    switch (degrees) {
    case 90: case 180: case 270: return degrees;
    default: return 0;
    }
}

inline QSizeF rotatedSize(QSizeF source, int degrees)
{
    const int rotation = normalizedRotation(degrees);
    return rotation == 90 || rotation == 270
        ? QSizeF(source.height(), source.width()) : source;
}

inline QRectF fittedRect(QRectF available, QSizeF source, int degrees)
{
    if (available.isEmpty() || source.isEmpty())
        return {};
    QSizeF target = rotatedSize(source, degrees);
    target.scale(available.size(), Qt::KeepAspectRatio);
    return QRectF(available.center() - QPointF(target.width() / 2, target.height() / 2), target);
}

inline QRectF unrotatedRect(const QRectF &target, int degrees)
{
    const QSizeF size = rotatedSize(target.size(), degrees);
    return QRectF(-size.width() / 2, -size.height() / 2, size.width(), size.height());
}

inline void drawImage(QPainter &painter, const QRectF &target, const QImage &image, int degrees,
                      const QRectF &source = {})
{
    if (image.isNull() || target.isEmpty())
        return;
    painter.save();
    painter.translate(target.center());
    painter.rotate(normalizedRotation(degrees)); // Positive angles are clockwise in widget coordinates.
    painter.drawImage(unrotatedRect(target, degrees), image, source.isEmpty()?QRectF(image.rect()):source);
    painter.restore();
}

// Inverse of the display rotation, used to look up the original depth sample.
inline QPointF sourcePosition(QPointF displayFraction, int degrees)
{
    const double x = displayFraction.x(), y = displayFraction.y();
    switch (normalizedRotation(degrees)) {
    case 90: return {y, 1 - x};
    case 180: return {1 - x, 1 - y};
    case 270: return {1 - y, x};
    default: return displayFraction;
    }
}

inline void retranslateRotationCombo(QComboBox *combo)
{
    combo->setItemText(0, QCoreApplication::translate("ImagePresentation", "0° (Original)"));
    combo->setItemText(1, QCoreApplication::translate("ImagePresentation", "90° clockwise"));
    combo->setItemText(2, QCoreApplication::translate("ImagePresentation", "180°"));
    combo->setItemText(3, QCoreApplication::translate("ImagePresentation", "270° clockwise"));
    combo->setAccessibleName(QCoreApplication::translate("ImagePresentation", "Display rotation"));
    combo->setToolTip(QCoreApplication::translate("ImagePresentation", "Rotate both depth and camera displays on this computer. Source data and saved images keep their original orientation."));
}

inline void populateRotationCombo(QComboBox *combo)
{
    for (int degrees : {0, 90, 180, 270})
        combo->addItem(QString(), degrees);
    retranslateRotationCombo(combo);
}
}
