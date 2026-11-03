#pragma once

#include <QtCore/QEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QToolTip>
#include <algorithm>

// A canvas-owned readout has no tooltip timeout. New frames update the same
// label; clearing one canvas cannot dismiss another canvas's readout.
class ImageHoverTip final : public QObject {
public:
    explicit ImageHoverTip(QWidget *canvas):QObject(canvas),m_canvas(canvas),
        m_label(new QLabel(canvas,Qt::ToolTip|Qt::FramelessWindowHint
                           |Qt::WindowDoesNotAcceptFocus|Qt::WindowTransparentForInput)) {
        m_label->setObjectName("imageHoverTip");
        m_label->setAttribute(Qt::WA_ShowWithoutActivating);
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_label->setFocusPolicy(Qt::NoFocus);
        m_label->setTextFormat(Qt::PlainText);
        m_label->setPalette(QToolTip::palette());
        m_label->setFont(QToolTip::font());
        m_label->setBackgroundRole(QPalette::ToolTipBase);
        m_label->setForegroundRole(QPalette::ToolTipText);
        m_label->setAutoFillBackground(true);
        m_label->setFrameStyle(QFrame::Box|QFrame::Plain);
        m_label->setMargin(4);
        qApp->installEventFilter(this);
    }
    ~ImageHoverTip() override {qApp->removeEventFilter(this);delete m_label;}

    bool tracking() const {return m_tracking;}
    QPointF position() const {return m_position;}
    void showText(const QString &text) {
        if(!m_tracking||!m_canvas->isVisible()||text.isEmpty()){hideText();return;}
        m_canvas->setProperty("lastHoverText",text);
        if(m_label->text()!=text){m_label->setText(text);m_label->adjustSize();}
        const QPoint anchor=m_canvas->mapToGlobal(m_position.toPoint());
        QPoint topLeft=anchor+QPoint(16,20);
        if(auto *screen=QGuiApplication::screenAt(anchor)){
            const auto area=screen->availableGeometry();
            if(topLeft.y()+m_label->height()>area.bottom()+1)
                topLeft.setY(anchor.y()-m_label->height()-8);
            topLeft.setX(std::clamp(topLeft.x(),area.left(),std::max(area.left(),area.right()-m_label->width()+1)));
            topLeft.setY(std::clamp(topLeft.y(),area.top(),std::max(area.top(),area.bottom()-m_label->height()+1)));
        }
        if(m_label->pos()!=topLeft)m_label->move(topLeft);
        if(!m_label->isVisible()){
            m_label->setAttribute(Qt::WA_DontShowOnScreen,m_canvas->testAttribute(Qt::WA_DontShowOnScreen)
                                 ||m_canvas->window()->testAttribute(Qt::WA_DontShowOnScreen));
            m_label->show();
        }
    }
    void hideText(){m_label->hide();m_canvas->setProperty("lastHoverText",QString());}
    void clear(){m_tracking=false;hideText();}

protected:
    bool eventFilter(QObject *watched,QEvent *event) override {
        if(watched==m_canvas){
            if(event->type()==QEvent::MouseMove){
                m_position=static_cast<QMouseEvent*>(event)->position();m_tracking=true;
            }else if(event->type()==QEvent::Leave||event->type()==QEvent::Hide
                     ||event->type()==QEvent::Move||event->type()==QEvent::Resize)clear();
            else if(event->type()==QEvent::ToolTip)return true;
        }
        if(event->type()==QEvent::ApplicationDeactivate
           ||(watched==m_canvas->window()&&event->type()==QEvent::WindowDeactivate))clear();
        return false;
    }

private:
    QWidget *m_canvas;
    QLabel *m_label;
    QPointF m_position;
    bool m_tracking=false;
};
