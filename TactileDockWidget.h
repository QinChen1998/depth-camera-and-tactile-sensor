#pragma once
#include <QDockWidget>
#include <QTimer>
#include "sensors/TactileVisualizationWidget.h"
// Owns only visualization lifecycle. No control or model dependencies.
class TactileDockWidget final : public QDockWidget {
public:
    explicit TactileDockWidget(QWidget *parent=nullptr);
    TactileVisualizationWidget *display() const { return m_display; }
    void openSession();
protected:
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;
private:
    TactileVisualizationWidget *m_display=nullptr;
    QTimer m_visibilityTimer;
    bool m_sessionOpen=false, m_contentVisible=false;
};
