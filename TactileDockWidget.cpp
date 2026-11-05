#include "TactileDockWidget.h"
#include <QCloseEvent>
#include <QAction>
#include <QQuickWidget>
TactileDockWidget::TactileDockWidget(QWidget *parent) : QDockWidget(parent)
{
    setObjectName("tactileDock");
    setFeatures(DockWidgetClosable|DockWidgetMovable|DockWidgetFloatable);
    setAllowedAreas(Qt::AllDockWidgetAreas);
    m_display=new TactileVisualizationWidget(this); m_display->setDockManaged(true); setWidget(m_display);
    // QDockWidget already routes an unchecked action through close(). A second
    // close here would repeat the last-module confirmation after cancellation.
    connect(toggleViewAction(),&QAction::triggered,this,[this](bool visible){if(visible)openSession();});
    m_visibilityTimer.setSingleShot(true); m_visibilityTimer.setInterval(150);
    connect(this,&QDockWidget::visibilityChanged,this,[this](bool visible){m_contentVisible=visible;m_visibilityTimer.start();});
    connect(this,&QDockWidget::topLevelChanged,this,[this]{m_visibilityTimer.start();});
    connect(m_display->findChild<QQuickWidget *>("tactileQuickView"),&QQuickWidget::statusChanged,this,[this](QQuickWidget::Status status){
        if(status==QQuickWidget::Ready) m_visibilityTimer.start();
    });
    connect(&m_visibilityTimer,&QTimer::timeout,this,[this]{
        const bool visible=m_contentVisible && isVisible() && !isMinimized() && !window()->isMinimized();
        // restoreState() shows the dock without invoking its menu action.
        // Visibility starts view refresh only. The normal application requests
        // its one default-port connection separately; offline runs never do.
        if(!m_sessionOpen && visible) {
            m_display->start();
            m_sessionOpen=m_display->running();
        }
        if(m_sessionOpen) m_display->setSuspended(!visible);
    });
}
void TactileDockWidget::openSession()
{
    show(); raise();
    if(!m_sessionOpen) { m_display->start(); m_sessionOpen=m_display->running(); }
    m_visibilityTimer.start();
}
void TactileDockWidget::closeEvent(QCloseEvent *event)
{
    QDockWidget::closeEvent(event);
    if(event->isAccepted()) { m_sessionOpen=false; m_visibilityTimer.stop(); m_display->stop(); }
}
bool TactileDockWidget::event(QEvent *event)
{
    const bool result=QDockWidget::event(event);
    if(event->type()==QEvent::WindowStateChange && m_display) {
        // A known minimized window can suspend immediately. The debounced
        // visibility check still controls recovery and reparent transitions.
        if(isMinimized() || window()->isMinimized())m_display->setSuspended(true);
        m_visibilityTimer.start();
    }
    return result;
}
