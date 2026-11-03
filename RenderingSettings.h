#pragma once

#include <QObject>

// User preference shared by the application's renderers.
class RenderingSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool lowCostRendering READ lowCostRendering WRITE setLowCostRendering
               NOTIFY lowCostRenderingChanged)
public:
    static RenderingSettings *instance();
    bool lowCostRendering() const { return m_lowCostRendering; }
    void setLowCostRendering(bool enabled);
signals:
    void lowCostRenderingChanged();
private:
    explicit RenderingSettings(QObject *parent);
    bool m_lowCostRendering = true;
};
