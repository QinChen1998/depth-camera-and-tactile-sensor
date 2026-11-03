#pragma once

#include <QtCore/QObject>
#include <QtCore/QSharedPointer>

// Shared by live depth/video pages; it never owns or starts a data source.
class ImageDisplaySettings final : public QObject
{
    Q_OBJECT

public:
    static QSharedPointer<ImageDisplaySettings> shared();
    int rotationDegrees() const { return m_rotation; }
    void setRotationDegrees(int degrees);
    bool blackOutsideRange() const { return m_blackOutside; }
    void setBlackOutsideRange(bool enabled);
    static constexpr double MinimumColorRangeM = 0.01;
    static constexpr double MaximumColorRangeM = 11;
    double colorMinimum() const { return m_colorMinimum; }
    double colorMaximum() const { return m_colorMaximum; }
    bool setColorRange(double minimum, double maximum);

signals:
    void rotationChanged(int degrees);
    void blackOutsideRangeChanged(bool enabled);
    void colorRangeChanged();

private:
    ImageDisplaySettings();
    int m_rotation = 0;
    bool m_blackOutside = false;
    double m_colorMinimum = MinimumColorRangeM, m_colorMaximum = 3;
};
