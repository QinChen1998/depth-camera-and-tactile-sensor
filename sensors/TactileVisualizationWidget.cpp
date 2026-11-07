#include "TactileVisualizationWidget.h"
#include "../UiStyle.h"
#include "../RenderingSettings.h"
#include <QtCore/QFile>
#include "TactileSceneAdapter.h"
#include "TactileSurfaceModel.h"
#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlError>
#include <QCheckBox>
#include <QSlider>
#include <QTableWidget>
#include <QHeaderView>
#include <QButtonGroup>
#include <QComboBox>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QEvent>
#include <QWheelEvent>
#include <QHelpEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolTip>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QVariantAnimation>
#include <QEnterEvent>
#include <functional>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QApplication>
#include <QSettings>
#include <QMessageBox>
#include <QStyleOptionSpinBox>
#include <QStyle>

namespace {
constexpr qint64 TrustedHoldMs=1000;
constexpr qint64 ArrowTransitionMs=100;
qint64 tactileDisplayNowMs() { QElapsedTimer now;now.start();return now.msecsSinceReference(); }
QSettings tactilePreferences() {
    return QSettings(QSettings::IniFormat,QSettings::UserScope,
        QCoreApplication::organizationName(),QCoreApplication::applicationName());
}

// Keep each existing settings row independent; only the space between its
// caption and editor grows when the dock is widened.
QWidget *tactileSettingRow(QWidget *control,bool withUnit=false) {
    auto *row=new QWidget;row->setProperty("tactileSettingRow",true);
    auto *layout=new QHBoxLayout(row);layout->setContentsMargins(0,6,0,6);layout->setSpacing(8);
    auto *label=new QLabel;label->setObjectName(control->objectName()+"Label");label->setBuddy(control);
    layout->addWidget(label);layout->addStretch();layout->addWidget(control,0,Qt::AlignRight|Qt::AlignVCenter);
    if(withUnit) {
        auto *unit=new QLabel;unit->setObjectName(control->objectName()+"Unit");unit->setProperty("tactileUnit",true);
        layout->addWidget(unit);
    }
    return row;
}
void tactileSettingCaption(QWidget *control,const QString &text,const QString &unit={}) {
    auto *row=control->parentWidget();
    row->findChild<QLabel *>(control->objectName()+"Label")->setText(text);
    if(auto *units=row->findChild<QLabel *>(control->objectName()+"Unit")) {
        units->setText(unit);
        units->setFixedWidth(qMax(units->fontMetrics().horizontalAdvance("counts/frame"),units->fontMetrics().horizontalAdvance(unit)));
    }
    control->setAccessibleName(unit.isEmpty()?text:text+" ("+unit+")");
}

QToolButton *tactileMinimizeButton(QVBoxLayout *layout,QPushButton *toggle,const QString &name) {
    auto *button=new QToolButton;button->setObjectName(name);
    button->setAutoRaise(true);button->setFixedSize(28,24);button->setCursor(Qt::PointingHandCursor);
    button->setIcon(button->style()->standardIcon(QStyle::SP_TitleBarMinButton));
    auto *row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->addStretch();row->addWidget(button);
    layout->addLayout(row);
    QObject::connect(button,&QToolButton::clicked,toggle,[toggle]{toggle->setChecked(false);});
    return button;
}

class TactileSwitch final : public QPushButton {
public:
    TactileSwitch() {
        setCheckable(true);setFixedSize(36,24);setCursor(Qt::PointingHandCursor);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        const auto group=isEnabled()?QPalette::Active:QPalette::Disabled;
        QColor track=isChecked()?palette().color(group,QPalette::Highlight):QColor(palette().color(QPalette::Base).lightness()<128?"#454950":"#e7e7e7");
        if(!isEnabled())track.setAlpha(100);
        if(underMouse()&&isEnabled())track=track.darker(106);
        p.setPen(Qt::NoPen);p.setBrush(track);p.drawRoundedRect(QRectF(2,2,32,20),10,10);
        p.setBrush(palette().color(group,QPalette::Base).lightness()>128?Qt::white:palette().color(group,QPalette::Text));
        p.drawEllipse(QRectF(isChecked()?17:5,5,14,14));
        if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(palette().color(group,QPalette::Highlight),1,Qt::DotLine));p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),12,12);}
    }
};

class TactileOptionWheelGuard final : public QObject {
public:
    explicit TactileOptionWheelGuard(QScrollArea *scroll):QObject(scroll),m_scroll(scroll) {}
protected:
    bool eventFilter(QObject *target,QEvent *event) override {
        if(event->type()==QEvent::Wheel) {
            // Route option wheels to the page explicitly. Popup wheels are
            // consumed locally so they cannot edit or scroll the option list.
            auto *control=qobject_cast<QWidget *>(target);
            auto *wheel=static_cast<QWheelEvent *>(event);event->accept();
            if(control&&control->window()==m_scroll->window()) {
                QWheelEvent pageWheel(m_scroll->viewport()->mapFromGlobal(wheel->globalPosition()),wheel->globalPosition(),
                    wheel->pixelDelta(),wheel->angleDelta(),wheel->buttons(),wheel->modifiers(),wheel->phase(),wheel->inverted());
                QApplication::sendEvent(m_scroll->viewport(),&pageWheel);
            }
            return true;
        }
        return QObject::eventFilter(target,event);
    }
private:
    QScrollArea *m_scroll;
};
}

class TactileSettingsCombo : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent *event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::Text),1));
        const QPointF center(width()-14,height()/2.0);
        QPainterPath chevron;chevron.moveTo(center+QPointF(-3,-1.5));chevron.lineTo(center+QPointF(0,1.5));chevron.lineTo(center+QPointF(3,-1.5));painter.drawPath(chevron);
    }
};

template<class Spin> class TactileSettingsSpin final : public Spin {
public:
    using Spin::Spin;
protected:
    void paintEvent(QPaintEvent *event) override {
        Spin::paintEvent(event);
        QStyleOptionSpinBox option;this->initStyleOption(&option);
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const auto arrow=[&](QStyle::SubControl button,bool up,bool canStep) {
            const auto area=this->style()->subControlRect(QStyle::CC_SpinBox,&option,button,this);
            if(area.isEmpty())return;
            painter.setPen(QPen(this->palette().color(this->isEnabled()&&canStep?QPalette::Active:QPalette::Disabled,QPalette::Text),1));
            const QPointF center=QRectF(area).center();const qreal direction=up?-1:1;
            QPainterPath path;path.moveTo(center+QPointF(-3,-1.5*direction));path.lineTo(center+QPointF(0,1.5*direction));path.lineTo(center+QPointF(3,-1.5*direction));painter.drawPath(path);
        };
        arrow(QStyle::SC_SpinBoxUp,true,option.stepEnabled.testFlag(QAbstractSpinBox::StepUpEnabled));
        arrow(QStyle::SC_SpinBoxDown,false,option.stepEnabled.testFlag(QAbstractSpinBox::StepDownEnabled));
    }
};

class TactilePortComboBox final : public TactileSettingsCombo {
public:
    explicit TactilePortComboBox(std::function<void()> refresh) : m_refresh(std::move(refresh)) {}
    void showPopup() override { m_refresh();QComboBox::showPopup(); }
private:
    std::function<void()> m_refresh;
};

class TactileFoldButton final : public QToolButton {
public:
    TactileFoldButton() {setCheckable(true);setFixedSize(32,32);setCursor(Qt::PointingHandCursor);}
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        const auto group=isEnabled()?QPalette::Active:QPalette::Disabled;
        p.setPen(Qt::NoPen);p.setBrush(palette().color(group,isDown()?QPalette::Mid:underMouse()?QPalette::Midlight:QPalette::AlternateBase));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),11,11);
        p.translate(width()/2.0,height()/2.0);
        p.setPen(QPen(palette().color(group,QPalette::Text),1.4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        QPainterPath corners;
        if(isChecked()) {
            corners.moveTo(-6,-1);corners.lineTo(-6,-6);corners.lineTo(-1,-6);
            corners.moveTo(1,6);corners.lineTo(6,6);corners.lineTo(6,1);
        } else {
            corners.moveTo(-6,-1);corners.lineTo(-1,-1);corners.lineTo(-1,-6);
            corners.moveTo(1,6);corners.lineTo(1,1);corners.lineTo(6,1);
        }
        p.drawPath(corners);
        if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(palette().color(group,QPalette::Highlight),1,Qt::DotLine));p.drawRoundedRect(QRectF(-13,-13,26,26),9,9);}
    }
};

// Local display-only buttons. Animate the paint, never the layout or selection data.
class TactileChoiceButton final : public QPushButton {
public:
    explicit TactileChoiceButton(int finger=-1,QWidget *parent=nullptr):QPushButton(parent),m_finger(finger) {
        setCheckable(true);setCursor(Qt::PointingHandCursor);
        setSizePolicy(finger<0?QSizePolicy::Expanding:QSizePolicy::Fixed,QSizePolicy::Fixed);
        if (finger < 0) setMinimumSize(60, 32);
        m_animation.setDuration(150);m_animation.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &v){m_emphasis=v.toReal();update();});
        connect(this,&QPushButton::toggled,this,[this]{animate();});
    }
    QSize sizeHint() const override
    {
        if (m_finger < 0) return QSize(fontMetrics().horizontalAdvance(text()) + 30, 34);
        auto bold = font();
        bold.setBold(true);
        const QFontMetrics metrics(bold);
        int captionWidth = 69;
        for (const auto &word : text().split(' '))
            captionWidth = qMax(captionWidth, metrics.horizontalAdvance(word) + 2);
        return QSize(captionWidth + 8, 84 + captionHeight(captionWidth));
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void enterEvent(QEnterEvent *e) override {m_hovered=true;animate();QPushButton::enterEvent(e);}
    void leaveEvent(QEvent *e) override {m_hovered=false;animate();QPushButton::leaveEvent(e);}
    void hideEvent(QHideEvent *e) override {m_animation.stop();m_hovered=false;m_emphasis=isChecked()?1:0;QPushButton::hideEvent(e);}
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        const auto group=isEnabled()?QPalette::Active:QPalette::Disabled;
        const auto base=palette().color(group,QPalette::Base),accent=palette().color(group,QPalette::Highlight);
        auto blend=[](QColor a,QColor b,qreal t){return QColor::fromRgbF(a.redF()*(1-t)+b.redF()*t,a.greenF()*(1-t)+b.greenF()*t,a.blueF()*(1-t)+b.blueF()*t);};
        const QRectF card=QRectF(rect()).adjusted(2,2, -2,-2).translated(0,isDown()?1:0);
        p.setBrush(blend(base,accent,0.06+0.15*m_emphasis));
        p.setPen(QPen(blend(palette().color(group,QPalette::Mid),accent,m_emphasis),isChecked()?1.6:1));
        p.drawRoundedRect(card,m_finger<0?card.height()/2:16,m_finger<0?card.height()/2:16);
        if(m_finger>=0){
            p.save();p.translate(width()/2.0,48-m_emphasis*2);p.scale(1+m_emphasis*.08,1+m_emphasis*.08);
            const QColor neutral=blend(palette().color(group,QPalette::Text),base,.55);
            p.setPen(Qt::NoPen);p.setBrush(accent);
            for(const auto &point:tactile::sensorLayout(m_finger).points)
                p.drawEllipse(QPointF((point.position.x()-.5)*65,(point.position.y()-.5)*65),2.5,2.5);
            p.restore();
        }
        auto f=font();f.setBold(isChecked());p.setFont(f);p.setPen(palette().color(group,QPalette::Text));
        const int caption = m_finger < 0 ? 0 : captionHeight(width() - 8);
        p.drawText(m_finger<0?card:QRectF(4,height()-5-caption,width()-8,caption),Qt::AlignCenter|Qt::TextWordWrap,text());
        if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(accent,1,Qt::DotLine));p.drawRoundedRect(card.adjusted(3,3,-3,-3),12,12);}
    }
private:
    int captionHeight(int width) const
    {
        auto bold = font();
        bold.setBold(true);
        const QFontMetrics metrics(bold);
        return qMax(32, metrics.boundingRect(QRect(0, 0, width, 0), Qt::TextWordWrap, text()).height() + 4);
    }
    void animate(){const qreal target=isChecked()?1:(m_hovered?.55:0);m_animation.stop();if(!isVisible()){m_emphasis=target;update();return;}m_animation.setStartValue(m_emphasis);m_animation.setEndValue(target);m_animation.start();}
    int m_finger;
    bool m_hovered=false;
    qreal m_emphasis=0;
    QVariantAnimation m_animation;
};

class TactileQuickWidget final : public QQuickWidget {
public:
    using QQuickWidget::QQuickWidget;
    std::function<void()> renderWindowChanged;
protected:
    bool event(QEvent *event) override {
        if(event->type()!=QEvent::WindowChangeInternal || !rootObject())return QQuickWidget::event(event);
        // Qt 6.11.1 replaces the offscreen QQuickWindow when docking. Its
        // Quick 3D scene manager cannot safely migrate the old spatial nodes.
        // Recreate this small display scene only at this boundary, preserving
        // camera/UI state and the existing adapter, source and history.
        const auto url=source();QVariantMap state;
        for(const auto *name:{"display","orbitYaw","orbitPitch","cameraDistance","cameraPan","inspectedPoint"})state[name]=rootObject()->property(name);
        setSource({});
        const bool handled=QQuickWidget::event(event);
        setInitialProperties(state);setSource(url);
        if(renderWindowChanged)renderWindowChanged();
        return handled;
    }
};

class TactileScale final : public QWidget {
public:
    explicit TactileScale(QWidget *parent=nullptr):QWidget(parent){setFixedHeight(32);}
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); const QRectF bar(8,2,width()-16,8);
        QLinearGradient gradient(bar.topLeft(),bar.topRight());
        for(int i=0;i<=10;++i)gradient.setColorAt(i/10.0,TactileSceneAdapter::forceColor(TactileSceneAdapter::PressureColorMaximum*i/10.0));
        painter.fillRect(bar,gradient);painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(8,12,width()-16,18),Qt::AlignLeft,"0");
        painter.drawText(QRectF(8,12,width()-16,18),Qt::AlignCenter,"Fz / counts");
        painter.drawText(QRectF(8,12,width()-16,18),Qt::AlignRight,QString::number(TactileSceneAdapter::PressureColorMaximum));
    }
};

class TactileCurveCanvas final : public QWidget {
public:
    explicit TactileCurveCanvas(TactileVisualizationWidget *owner) : QWidget(owner), m_owner(owner)
    { setObjectName("tactileCurves"); setMinimumSize(250,170); setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding); }
protected:
    void paintEvent(QPaintEvent *) override {
        m_owner->recordPaint();
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),palette().brush(QPalette::Base));
        const auto &samples=m_owner->history().samples();
        const qint64 end=samples.empty()?0:samples.back().timeNs;
        const int finger=m_owner->selectedFinger(), point=m_owner->selectedPoint();
        const auto rawCount=tactile::sensorLayout(finger).points.size();
        const auto baseline=m_owner->displayBaselineForce(point);
        const double noise=m_owner->noiseDisplayThreshold();
        struct PlotSample { const tactile::HistorySample *sample; tactile::Force force; };
        QVector<PlotSample> plot;
        plot.reserve(qsizetype(samples.size()));
        double low=0,high=0;
        for(const auto &sample:samples) {
            if(m_owner->awaitingDisplayZero())break;
            const auto &raw=sample.pointForces[finger];
            if(raw.size()!=rawCount){plot.append(PlotSample{nullptr,{}});continue;}
            const auto f=tactile::zeroSmallComponents(
                tactile::relativeForce(m_owner->displayPointForce(raw,point),baseline),noise);
            plot.append(PlotSample{&sample,f});
            low=std::min({low,f.x,f.y,f.z});high=std::max({high,f.x,f.y,f.z});
        }
        const double span=std::max(400.0,high-low);
        const double magnitude=std::pow(10.0,std::floor(std::log10(span/4)));
        const double tick=std::ceil(span*1.15/4/magnitude)*magnitude;
        low=std::floor((low-span*.05)/tick)*tick;
        high=std::ceil((high+span*.05)/tick)*tick;
        const auto number=[](double value){return std::abs(value)>=1000?QString::number(value/1000,'g',4)+"k":QString::number(value,'f',0);};
        const int margin=std::max(fontMetrics().horizontalAdvance(number(low)),fontMetrics().horizontalAdvance(number(high)))+12;
        const QRectF area(margin,32,width()-margin-12,height()-60);
        const QColor colors[]={QColor("#d94b56"),QColor("#229b61"),QColor("#298bd0")};
        for(int axis=0;axis<3;++axis) {
            const int x=margin+axis*(width()-margin-8)/3;
            p.setPen(QPen(colors[axis],2));p.drawLine(x,14,x+16,14);
            p.setPen(palette().color(QPalette::Text));p.drawText(QRectF(x+21,4,32,20),Qt::AlignLeft,QString("%1").arg(QString("XYZ").mid(axis,1)));
        }
        p.setPen(palette().color(QPalette::Mid));p.drawRect(area);
        for(double value=low;value<=high+tick*.01;value+=tick) {
            const double y=area.bottom()-(value-low)/(high-low)*area.height();
            p.setPen(QPen(palette().color(QPalette::Mid),value==0?1.1:.5,value==0?Qt::SolidLine:Qt::DotLine));p.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));
            p.setPen(palette().color(QPalette::Text));p.drawText(QRectF(0,y-9,margin-6,18),Qt::AlignRight|Qt::AlignVCenter,number(value));
        }
        for(int sec=-10;sec<=0;sec+=2) {
            const double x=area.right()+sec/10.0*area.width();
            p.setPen(QPen(palette().color(QPalette::Mid),.5,Qt::DotLine));p.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));
            p.setPen(palette().color(QPalette::Text));p.drawText(QRectF(x-12,area.bottom()+3,24,18),Qt::AlignCenter,QString::number(sec));
        }
        p.save();p.setClipRect(area);
        for(int axis=0;axis<3;++axis) {
            p.setPen(QPen(colors[axis],1.8));QPainterPath path,bridges;bool first=true;QPointF previous;
            for(const auto &[sample,f]:plot) {
                if(!sample){first=true;continue;}
                const double value=axis==0?f.x:axis==1?f.y:f.z;
                const QPointF point(area.right()+double(sample->timeNs-end)/tactile::TactileHistory::WindowNs*area.width(),area.bottom()-(value-low)/(high-low)*area.height());
                if(!first&&sample->startsSegment&&sample->bridgeFromPrevious){bridges.moveTo(previous);bridges.lineTo(point);}
                if(first||sample->startsSegment)path.moveTo(point);else path.lineTo(point);first=false;previous=point;
            }
            p.drawPath(path);
            p.setPen(QPen(colors[axis],1.4,Qt::DashLine));p.drawPath(bridges);
        }
        p.restore();
    }
private:
    TactileVisualizationWidget *m_owner;
};

TactileVisualizationWidget::TactileVisualizationWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("tactileVisualization"); resize(900,620);
    auto *root=new QVBoxLayout(this);root->setContentsMargins(8,6,8,6);root->setSpacing(4);
    auto *scroll=new QScrollArea;scroll->setObjectName("tactilePlotsScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);root->addWidget(scroll,1);
    auto *body=new QWidget;body->setObjectName("tactileSettingsBody");scroll->setWidget(body);
    updateSettingsStyle();
    auto *bodyLayout=new QVBoxLayout(body);bodyLayout->setContentsMargins(0,0,0,0);bodyLayout->setSizeConstraint(QLayout::SetMinimumSize);
    createSerialControls(bodyLayout);
    m_displayControls=new QWidget;m_displayControls->setObjectName("tactileDisplayControls");
    m_displayControls->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);
    auto *displayLayout=new QVBoxLayout(m_displayControls);displayLayout->setContentsMargins(16,8,16,8);displayLayout->setSpacing(0);
    m_minimizeDisplay=tactileMinimizeButton(displayLayout,m_displaySettings,"tactileMinimizeDisplay");
    bodyLayout->addWidget(m_displayControls);m_displayControls->hide();
    connect(m_displaySettings,&QPushButton::toggled,m_displayControls,&QWidget::setVisible);
    m_splitter=new QSplitter;m_splitter->setObjectName("tactileContentSplitter");bodyLayout->addWidget(m_splitter,1);
    auto *left=new QWidget;left->setObjectName("tactileScenePanel");auto *leftLayout=new QVBoxLayout(left);leftLayout->setContentsMargins(0,0,0,0);leftLayout->setSpacing(5);
    m_adapter=new TactileSceneAdapter(this);m_adapter->setPalette(palette());
    auto *renderSettings=RenderingSettings::instance();
    m_adapter->setLowCostRendering(renderSettings->lowCostRendering());
    connect(renderSettings,&RenderingSettings::lowCostRenderingChanged,this,[this,renderSettings]{
        m_adapter->setLowCostRendering(renderSettings->lowCostRendering());
    });
    auto *quick=new TactileQuickWidget(left);m_canvas=quick;m_canvas->setObjectName("tactileQuickView");m_canvas->setMinimumSize(260,260);m_canvas->setResizeMode(QQuickWidget::SizeRootObjectToView);m_canvas->setFocusPolicy(Qt::StrongFocus);
    m_canvas->setInitialProperties({{"display",QVariant::fromValue(m_adapter)}});m_canvas->setClearColor(m_adapter->background());
    m_viewError=new QLabel;m_viewError->setObjectName("tactileViewError");m_viewError->setWordWrap(true);m_viewError->hide();leftLayout->addWidget(m_viewError);
    connect(m_canvas,&QQuickWidget::statusChanged,this,[this](QQuickWidget::Status status){
        if(status==QQuickWidget::Error){QStringList errors;for(const auto &error:m_canvas->errors())errors<<error.toString();m_viewError->setText(tr("3D view unavailable: %1").arg(errors.join("\n")));m_viewError->show();stop();}
        else if(status==QQuickWidget::Ready){m_viewError->hide();if(m_defaultConnectRequested)connectDefaultSerial();}
    });
    auto *forceScale=new TactileScale;forceScale->setObjectName("tactileForceScale");
    leftLayout->addWidget(m_canvas,1);leftLayout->addWidget(forceScale);
    m_splitter->addWidget(left);
    auto *right=new QWidget;right->setObjectName("tactileRightPanel");auto *rightLayout=new QHBoxLayout(right);rightLayout->setContentsMargins(4,0,0,0);rightLayout->setSpacing(10);
    m_fingerBox=new QWidget;m_fingerBox->setObjectName("tactileSensorChoices");auto *fingerLayout=new QVBoxLayout(m_fingerBox);fingerLayout->setContentsMargins(2,0,2,0);fingerLayout->setSpacing(6);
    m_fingers=new QButtonGroup(this);
    m_foldSensors=new TactileFoldButton;m_foldSensors->setObjectName("tactileFoldSensors");fingerLayout->addWidget(m_foldSensors,0,Qt::AlignHCenter);
    for(int i=0;i<3;++i){auto *b=new TactileChoiceButton(i);b->setObjectName(QString("finger%1").arg(i));m_fingers->addButton(b,i);fingerLayout->addWidget(b);}
    fingerLayout->addStretch(1);m_fingers->button(0)->setChecked(true);m_fingerBox->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred);rightLayout->addWidget(m_fingerBox);
    auto *sideScroll=new QScrollArea;sideScroll->setObjectName("tactileDetailsScroll");sideScroll->setWidgetResizable(true);sideScroll->setFrameShape(QFrame::NoFrame);
    rightLayout->addWidget(sideScroll,1);
    auto *column=new QWidget;column->setObjectName("tactileSettingsAndReadings");sideScroll->setWidget(column);
    auto *columnLayout=new QVBoxLayout(column);columnLayout->setContentsMargins(0,0,0,0);columnLayout->setSpacing(7);columnLayout->setSizeConstraint(QLayout::SetMinimumSize);columnLayout->setAlignment(Qt::AlignTop);
    columnLayout->addWidget(m_serialControls);columnLayout->addWidget(m_displayControls);
    auto *detail=new QWidget;detail->setObjectName("tactileResultPanel");auto *detailLayout=new QVBoxLayout(detail);detailLayout->setSizeConstraint(QLayout::SetMinimumSize);detailLayout->setContentsMargins(0,0,0,0);detailLayout->setSpacing(7);columnLayout->addWidget(detail,1);
    auto *readings=new QWidget;readings->setObjectName("tactileReadingsPanel");
    auto *readingsLayout=new QVBoxLayout(readings);readingsLayout->setContentsMargins(0,0,0,0);readingsLayout->setSpacing(7);
    detailLayout->addWidget(readings,1);
    m_chartLabel=new QLabel;readingsLayout->addWidget(m_chartLabel);
    m_curves=new TactileCurveCanvas(this);readingsLayout->addWidget(m_curves,1);
    auto *valuesRow=new QHBoxLayout;
    for(int i=0;i<3;++i){m_values[i]=new QLabel;m_values[i]->setObjectName(QString("force%1").arg(i));auto f=m_values[i]->font();f.setBold(true);m_values[i]->setFont(f);m_values[i]->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);valuesRow->addWidget(m_values[i],1);}readingsLayout->addLayout(valuesRow);
    m_tableLabel=new QLabel;m_tableLabel->setWordWrap(true);readingsLayout->addWidget(m_tableLabel);
    m_pointTable=new QTableWidget(0,4);m_pointTable->setObjectName("tactilePointValues");
    m_pointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);m_pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);m_pointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pointTable->setAlternatingRowColors(true);m_pointTable->setSortingEnabled(false);
    m_pointTable->verticalHeader()->hide();m_pointTable->verticalHeader()->setDefaultSectionSize(22);
    m_pointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pointTable->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Fixed);
    m_pointTable->setMinimumHeight(145);readingsLayout->addWidget(m_pointTable,1);
    m_displayMode=new TactileSettingsCombo;m_displayMode->setObjectName("tactileDisplayMode");
    m_displayMode->addItem(QString(),"xyz");m_displayMode->addItem(QString(),"dots");
    displayLayout->addWidget(tactileSettingRow(m_displayMode));
    m_reverseZArrows=new TactileSwitch;m_reverseZArrows->setObjectName("tactileReverseZArrows");
    displayLayout->addWidget(tactileSettingRow(m_reverseZArrows));
    connect(m_reverseZArrows,&QPushButton::toggled,this,[this](bool reversed){m_adapter->setReverseZArrows(reversed);updateValues();});
    m_jumpThreshold=new TactileSettingsSpin<QSpinBox>;m_jumpThreshold->setObjectName("tactileJumpThreshold");
    m_jumpThreshold->setRange(1,65535);m_jumpThreshold->setValue(5000);m_jumpThreshold->setSingleStep(500);
    displayLayout->addWidget(tactileSettingRow(m_jumpThreshold,true));
    m_noiseEnabled=new TactileSwitch;m_noiseEnabled->setObjectName("tactileNoiseEnabled");
    displayLayout->addWidget(tactileSettingRow(m_noiseEnabled));
    m_noiseThreshold=new TactileSettingsSpin<QSpinBox>;m_noiseThreshold->setObjectName("tactileNoiseThreshold");
    m_noiseThreshold->setRange(1,5000);m_noiseThreshold->setValue(150);m_noiseThreshold->setSingleStep(10);
    m_noiseThreshold->setEnabled(false);displayLayout->addWidget(tactileSettingRow(m_noiseThreshold,true));
    const auto noiseChanged=[this]{m_arrowFrom={};m_arrowTransition.invalidate();updateValues();};
    connect(m_noiseEnabled,&QPushButton::toggled,this,[this,noiseChanged](bool enabled){m_noiseThreshold->setEnabled(enabled);noiseChanged();});
    connect(m_noiseThreshold,&QSpinBox::valueChanged,this,noiseChanged);
    m_arrowGain=new TactileSettingsSpin<QDoubleSpinBox>;m_arrowGain->setObjectName("tactileArrowGain");
    m_arrowGain->setRange(0.1,100);m_arrowGain->setDecimals(1);m_arrowGain->setSingleStep(0.5);
    m_arrowGain->setValue(1);displayLayout->addWidget(tactileSettingRow(m_arrowGain,true));
    connect(m_arrowGain,&QDoubleSpinBox::valueChanged,this,[this](double gain){m_adapter->setArrowGain(gain);updateValues();});
    m_arrowWidthGain=new TactileSettingsSpin<QDoubleSpinBox>;m_arrowWidthGain->setObjectName("tactileArrowWidthGain");
    m_arrowWidthGain->setRange(0.1,10);m_arrowWidthGain->setDecimals(1);m_arrowWidthGain->setSingleStep(0.1);
    m_arrowWidthGain->setValue(1);displayLayout->addWidget(tactileSettingRow(m_arrowWidthGain,true));
    connect(m_arrowWidthGain,&QDoubleSpinBox::valueChanged,m_adapter,&TactileSceneAdapter::setArrowWidthGain);
    m_resultantSizeGain=new TactileSettingsSpin<QDoubleSpinBox>;m_resultantSizeGain->setObjectName("tactileResultantSizeGain");
    m_resultantSizeGain->setRange(0.1,10);m_resultantSizeGain->setDecimals(1);m_resultantSizeGain->setSingleStep(0.1);
    m_resultantSizeGain->setValue(1);displayLayout->addWidget(tactileSettingRow(m_resultantSizeGain,true));
    connect(m_resultantSizeGain,&QDoubleSpinBox::valueChanged,m_adapter,&TactileSceneAdapter::setResultantSizeGain);
    m_filterStatus=new QLabel;m_filterStatus->setObjectName("tactileFilterStatus");
    m_filterStatus->setWordWrap(false);
    m_filterStatus->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    m_filterStatus->setFixedHeight(m_filterStatus->fontMetrics().height()+4);detailLayout->addWidget(m_filterStatus);
    connect(m_pointTable,&QTableWidget::currentCellChanged,this,[this](int row,int,int,int){
        selectPoint(row);
    });
    connect(m_adapter,&TactileSceneAdapter::rawPointSelected,this,&TactileVisualizationWidget::selectPoint);
    m_density=new TactileSettingsCombo;m_density->setObjectName("tactileDensity");
    for(int size:{2,3,4})m_density->addItem(QString(),size);
    displayLayout->addWidget(tactileSettingRow(m_density));
    connect(m_density,&QComboBox::currentIndexChanged,this,[this](int index){
        m_adapter->setGridSize(m_density->itemData(index).toInt());
        if(m_canvas->rootObject())m_canvas->rootObject()->setProperty("inspectedPoint",-1);
        updateValues();
    });
    auto *opacityField=new QWidget;auto *opacityRow=new QHBoxLayout(opacityField);opacityRow->setContentsMargins(0,8,0,8);
    m_opacityLabel=new QLabel;
    m_opacitySlider=new QSlider(Qt::Horizontal);m_opacitySlider->setObjectName("tactileModelOpacity");
    m_opacitySlider->setRange(0,100);m_opacitySlider->setValue(qRound(m_adapter->modelOpacity()*100));
    m_opacityValue=new QLabel(QString::number(m_opacitySlider->value())+"%");m_opacityValue->setMinimumWidth(40);
    m_opacityLabel->setBuddy(m_opacitySlider);
    opacityRow->addWidget(m_opacityLabel);opacityRow->addWidget(m_opacitySlider,1);opacityRow->addWidget(m_opacityValue);
    displayLayout->addWidget(opacityField);
    connect(m_opacitySlider,&QSlider::valueChanged,this,[this](int value){m_adapter->setModelOpacity(value/100.0);m_opacityValue->setText(QString::number(value)+"%");});
    m_pointsToggle=new TactileSwitch;m_pointsToggle->setObjectName("pointArrowsToggle");m_pointsToggle->setChecked(false);
    m_resultantToggle=new TactileSwitch;m_resultantToggle->setObjectName("resultantArrowToggle");m_resultantToggle->setChecked(true);
    m_componentsToggle=new TactileSwitch;m_componentsToggle->setObjectName("componentArrowsToggle");m_componentsToggle->setChecked(true);
    auto *arrowControls=new QHBoxLayout;arrowControls->setSpacing(24);
    arrowControls->addWidget(tactileSettingRow(m_resultantToggle),1);arrowControls->addWidget(tactileSettingRow(m_componentsToggle),1);displayLayout->addLayout(arrowControls);
    auto *displayOptions=new QHBoxLayout;displayOptions->setSpacing(24);displayOptions->addWidget(tactileSettingRow(m_pointsToggle),1);
    m_statusToggle=new TactileSwitch;m_statusToggle->setObjectName("tactileShowStatus");
    m_adapter->setCommunicationStatusVisible(false);
    displayOptions->addWidget(tactileSettingRow(m_statusToggle),1);displayLayout->addLayout(displayOptions);
    m_hideReadingsToggle=new TactileSwitch;m_hideReadingsToggle->setObjectName("tactileHideReadings");
    displayLayout->addWidget(tactileSettingRow(m_hideReadingsToggle));
    connect(m_hideReadingsToggle,&QPushButton::toggled,detail,[this,readings](bool hidden){
        if(hidden){saveSplitterLayout();m_readingsTimer.stop();m_curveTimer.stop();}
        readings->setVisible(!hidden);
        updateRightPanelLayout();
        if(!hidden)updatePanel();
    });
    connect(m_statusToggle,&QPushButton::toggled,this,[this](bool visible){
        m_adapter->setCommunicationStatusVisible(visible);updatePanelStatus();updateSerialStatus();
    });
    m_splitter->addWidget(right);m_splitter->setChildrenCollapsible(false);m_splitter->setStretchFactor(0,1);m_splitter->setStretchFactor(1,1);m_splitter->setSizes({440,440});
    connect(m_fingers,&QButtonGroup::idClicked,this,[this](int id){m_finger=id;if(m_canvas->rootObject())m_canvas->rootObject()->setProperty("inspectedPoint",-1);updateValues();});
    connect(m_pointsToggle,&QPushButton::toggled,m_adapter,&TactileSceneAdapter::setPointArrows);connect(m_resultantToggle,&QPushButton::toggled,m_adapter,&TactileSceneAdapter::setResultantArrow);
    connect(m_componentsToggle,&QPushButton::toggled,m_adapter,&TactileSceneAdapter::setComponentArrows);
    connect(m_displayMode,&QComboBox::currentIndexChanged,this,[this]{
        const bool dots=m_displayMode->currentData().toString()=="dots";
        m_adapter->setDotMatrix(dots);
        for(QWidget *control:QList<QWidget *>{m_arrowWidthGain,m_resultantSizeGain,m_pointsToggle,m_resultantToggle,m_componentsToggle})control->setEnabled(!dots);
    });
    connect(m_resetView,&QPushButton::clicked,this,[this]{if(m_canvas->rootObject())QMetaObject::invokeMethod(m_canvas->rootObject(),"resetView");});
    connect(m_frontView,&QPushButton::clicked,this,[this]{if(m_canvas->rootObject())QMetaObject::invokeMethod(m_canvas->rootObject(),"frontView");});
    // QQuickWidget uses the GUI render loop; count completed offscreen renders,
    // deduplicating the data serial. This does not measure screen presentation.
    quick->renderWindowChanged=[this]{connect(m_canvas->quickWindow(),&QQuickWindow::afterRendering,this,[this]{recordView();});};
    quick->renderWindowChanged();
    if (QFile::exists(":/qml/TactileView.qml")) m_canvas->setSource(QUrl("qrc:/qml/TactileView.qml"));
    m_timer.setTimerType(Qt::PreciseTimer);m_timer.setInterval(33);connect(&m_timer,&QTimer::timeout,this,&TactileVisualizationWidget::refresh);
    m_readingsTimer.setSingleShot(true);m_readingsTimer.setInterval(100);
    connect(&m_readingsTimer,&QTimer::timeout,this,&TactileVisualizationWidget::updateReadings);
    m_curveTimer.setSingleShot(true);m_curveTimer.setInterval(66);
    connect(&m_curveTimer,&QTimer::timeout,this,&TactileVisualizationWidget::updateCurve);
    connect(m_connectionSettings,&QPushButton::toggled,this,&TactileVisualizationWidget::updateRightPanelLayout);
    connect(m_displaySettings,&QPushButton::toggled,this,&TactileVisualizationWidget::updateRightPanelLayout);
    connect(m_foldSensors,&QToolButton::toggled,this,[this]{updateSensorChoices();updateRightPanelLayout();});
    retranslate();restoreDisplaySettings();restoreSerialSettings();
    // Panel visibility is session-local and user-controlled after these startup defaults.
    m_connectionSettings->setChecked(true);m_displaySettings->setChecked(false);
    updateRightPanelLayout();updateValues();
    connect(m_jumpThreshold,&QSpinBox::valueChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_noiseEnabled,&QPushButton::toggled,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_reverseZArrows,&QPushButton::toggled,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_noiseThreshold,&QSpinBox::valueChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_arrowGain,&QDoubleSpinBox::valueChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_arrowWidthGain,&QDoubleSpinBox::valueChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_resultantSizeGain,&QDoubleSpinBox::valueChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_density,&QComboBox::currentIndexChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_displayMode,&QComboBox::currentIndexChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_opacitySlider,&QSlider::valueChanged,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_pointsToggle,&QPushButton::toggled,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_resultantToggle,&QPushButton::toggled,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_componentsToggle,&QPushButton::toggled,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_hideReadingsToggle,&QPushButton::toggled,this,&TactileVisualizationWidget::saveDisplaySettings);
    connect(m_splitter,&QSplitter::splitterMoved,this,&TactileVisualizationWidget::saveSplitterLayout);
    // Wheel scrolling is navigation, never an option edit, even with focus.
    // Include popup views and editable children; keep page scrollbars
    // and the Quick 3D camera outside this local guard.
    auto *wheelGuard=new TactileOptionWheelGuard(sideScroll);
    for(auto *combo:findChildren<QComboBox *>()) {
        combo->installEventFilter(wheelGuard);
        if(combo->lineEdit())combo->lineEdit()->installEventFilter(wheelGuard);
        combo->view()->installEventFilter(wheelGuard);combo->view()->viewport()->installEventFilter(wheelGuard);
    }
    for(auto *spin:findChildren<QAbstractSpinBox *>()) {
        spin->installEventFilter(wheelGuard);
        for(auto *edit:spin->findChildren<QLineEdit *>())edit->installEventFilter(wheelGuard);
    }
    for(auto *slider:findChildren<QSlider *>())slider->installEventFilter(wheelGuard);
}
TactileVisualizationWidget::~TactileVisualizationWidget() {saveSplitterLayout();m_initialResponseTimer.stop();m_portScanTimer.stop();m_timer.stop();m_serial.stop();delete m_canvas;m_canvas=nullptr;}
void TactileVisualizationWidget::updateSensorChoices()
{
    const bool collapsed=m_foldSensors->isChecked();
    for(auto *button:m_fingers->buttons())button->setVisible(!collapsed);
    const auto action=collapsed?tr("Expand sensor choices"):tr("Collapse sensor choices");
    m_foldSensors->setText(action);m_foldSensors->setToolTip(action);m_foldSensors->setAccessibleName(action);
    m_fingerBox->layout()->activate();
}
void TactileVisualizationWidget::updateRightPanelLayout()
{
    const bool hidden=m_hideReadingsToggle->isChecked();
    const bool compact=hidden&&!m_connectionSettings->isChecked()&&!m_displaySettings->isChecked();
    auto *right=findChild<QWidget *>("tactileRightPanel");
    auto *sideScroll=findChild<QScrollArea *>("tactileDetailsScroll");
    findChild<QWidget *>("tactileResultPanel")->setVisible(!hidden);
    sideScroll->setVisible(!compact);
    if(compact)right->setFixedWidth(m_fingerBox->sizeHint().width()+right->layout()->contentsMargins().left()+right->layout()->contentsMargins().right());
    else {right->setMinimumWidth(0);right->setMaximumWidth(QWIDGETSIZE_MAX);}
    right->layout()->activate();
    if(m_splitterReady)restoreSplitterLayout();
}
void TactileVisualizationWidget::saveSplitterLayout()
{
    if(!m_splitterReady||m_restoringSplitter||findChild<QWidget *>("tactileResultPanel")->isHidden())return;
    const auto sizes=m_splitter->sizes();
    if(sizes.size()!=2||sizes[0]<=0||sizes[1]<=0)return;
    auto settings=tactilePreferences();settings.beginGroup("tactile/layout");
    settings.setValue(m_splitter->orientation()==Qt::Horizontal?"horizontal":"vertical",m_splitter->saveState());settings.sync();
}
void TactileVisualizationWidget::restoreSplitterLayout()
{
    if(m_splitter->width()<=0||m_splitter->height()<=0)return;
    m_restoringSplitter=true;const QSignalBlocker blocker(m_splitter);
    const bool compact=m_hideReadingsToggle->isChecked()&&!m_connectionSettings->isChecked()&&!m_displaySettings->isChecked();
    const auto orientation=!compact&&width()<640?Qt::Vertical:Qt::Horizontal;
    auto settings=tactilePreferences();settings.beginGroup("tactile/layout");
    const auto state=settings.value(orientation==Qt::Horizontal?"horizontal":"vertical").toByteArray();
    bool restored=!compact&&!state.isEmpty()&&state.size()<=4096&&m_splitter->restoreState(state);
    restored=restored&&m_splitter->orientation()==orientation;
    m_splitter->setOrientation(orientation);m_splitter->setChildrenCollapsible(false);
    for(int i=0;i<2;++i)m_splitter->setCollapsible(i,false);
    const auto sizes=m_splitter->sizes();
    if(compact) {
        const int choicesWidth=m_splitter->widget(1)->maximumWidth();
        m_splitter->setSizes({qMax(1,m_splitter->width()-m_splitter->handleWidth()-choicesWidth),choicesWidth});
    } else if(!restored||sizes.size()!=2||sizes[0]<=0||sizes[1]<=0)m_splitter->setSizes({440,440});
    m_splitterReady=true;m_restoringSplitter=false;
}
void TactileVisualizationWidget::restoreDisplaySettings()
{
    auto settings=tactilePreferences();settings.beginGroup("tactile/display");
    const auto number=[&](const char *key,double fallback,double low,double high) {
        bool ok=false;const double value=settings.value(key,fallback).toDouble(&ok);
        return ok&&std::isfinite(value)&&value>=low&&value<=high?value:fallback;
    };
    const auto flag=[&](const char *key,bool fallback) {
        const auto text=settings.value(key,fallback).toString().trimmed().toLower();
        return text=="true"||text=="1"?true:text=="false"||text=="0"?false:fallback;
    };
    m_jumpThreshold->setValue(int(number("jumpThreshold",5000,1,65535)));
    m_noiseThreshold->setValue(int(number("noiseThreshold",150,1,5000)));
    m_noiseEnabled->setChecked(flag("noiseEnabled",true));
    m_reverseZArrows->setChecked(flag("reverseZArrows",false));
    m_arrowGain->setValue(number("arrowGain",1,.1,100));
    m_arrowWidthGain->setValue(number("arrowWidthGain",1,.1,10));
    m_resultantSizeGain->setValue(number("resultantSizeGain",1,.1,10));
    const int mode=m_displayMode->findData(settings.value("visualization","xyz").toString());
    m_displayMode->setCurrentIndex(mode<0?0:mode);
    const int grid=m_density->findData(settings.value("outputGridSize",settings.value("gridSize",3)).toInt());
    m_density->setCurrentIndex(grid<0?m_density->findData(3):grid);
    m_opacitySlider->setValue(int(number("modelOpacity",45,0,100)));
    m_pointsToggle->setChecked(flag("pointArrows",false));
    m_resultantToggle->setChecked(flag("resultantArrow",true));
    m_componentsToggle->setChecked(flag("componentArrows",true));
    m_hideReadingsToggle->setChecked(flag("hideReadings",false));
}
void TactileVisualizationWidget::saveDisplaySettings()
{
    auto settings=tactilePreferences();settings.beginGroup("tactile/display");
    settings.setValue("jumpThreshold",m_jumpThreshold->value());settings.setValue("arrowGain",m_arrowGain->value());
    settings.setValue("noiseEnabled",m_noiseEnabled->isChecked());settings.setValue("noiseThreshold",m_noiseThreshold->value());
    settings.setValue("reverseZArrows",m_reverseZArrows->isChecked());
    settings.setValue("arrowWidthGain",m_arrowWidthGain->value());
    settings.setValue("resultantSizeGain",m_resultantSizeGain->value());
    settings.setValue("visualization",m_displayMode->currentData());
    settings.setValue("outputGridSize",m_density->currentData());
    settings.setValue("modelOpacity",m_opacitySlider->value());
    settings.setValue("pointArrows",m_pointsToggle->isChecked());settings.setValue("resultantArrow",m_resultantToggle->isChecked());
    settings.setValue("componentArrows",m_componentsToggle->isChecked());
    settings.setValue("hideReadings",m_hideReadingsToggle->isChecked());
    settings.sync();
}
void TactileVisualizationWidget::start()
{
    if(m_canvas->status()!=QQuickWidget::Ready || !isVisible() || m_started) return;
    m_started=true;m_suspended=false;
    m_timer.start();refresh();
}
void TactileVisualizationWidget::stop()
{
    cancelSerialRetry();
    m_defaultConnectRequested=false;m_initialResponseTimer.stop();m_waitingForFirstHardwareFrame=false;
    if(auto *notice=findChild<QMessageBox *>("tactileSaveReminder"))notice->close();
    m_configTimer.stop();m_configAwaitClose=false;
    if(m_serial.configBusy())setDeviceConfigStatus(tr("Configuration interrupted; device state may have changed. Reconnect and read parameters before another write."));
    m_verifyingAddress=false;
    m_started=false;m_timer.stop();m_serial.stop();m_displayFilter.reset();m_liveFrame={};m_serialSample={};m_serialHistory.clear();
    clearHardwareDisplay();m_heldTableFrame={};
    m_tareFrame={};m_autoZeroResult={};m_frame={};updateValues();
}
void TactileVisualizationWidget::setSuspended(bool suspended)
{
    if(m_suspended==suspended) return;
    m_suspended=suspended;
    m_serial.setPaused(suspended);m_serialHistory.breakSegment();m_liveFrame={};clearHardwareDisplay();m_heldTableFrame={};
    if(running()) m_timer.start(); else m_timer.stop(); updateValues();
}
bool TactileVisualizationWidget::running() const { return m_started&&!m_suspended; }
void TactileVisualizationWidget::receiveSerialSample(const tactile::SerialSample &received)
{
        if(m_waitingForFirstHardwareFrame&&received.modelIdentified&&!m_serial.stopping()
            &&received.frame.sessionId==m_serial.sessionId()&&received.frame.generation==m_serial.generation()
            &&received.settings.port==m_connectedSettings.port&&received.settings.baud==m_connectedSettings.baud
            &&received.settings.type>=0&&received.settings.type<3&&received.frame.hardwareType==received.settings.type) {
            const bool changed=m_finger!=received.settings.type||m_hardwareType!=received.settings.type;
            m_connectedSettings.type=received.settings.type;m_connectedSettings.slave=received.settings.slave;
            m_hardwareType=received.settings.type;m_finger=received.settings.type;
            const QSignalBlocker blocker(m_slave);m_slave->setValue(received.settings.slave);
            m_fingers->button(m_finger)->setChecked(true);
            if(changed){
                if(m_canvas->rootObject())m_canvas->rootObject()->setProperty("inspectedPoint",-1);
                updateValues();
            }
        }
        if(m_trustedReceivedMs>=0&&!hardwareDisplayFrame(false).valid){clearHardwareDisplay();m_serialHistory.breakSegment();}
        if(received.requests>m_serialSample.requests+1)m_serialHistory.breakSegment(true);
        if(!received.portOpen&&!received.error.isEmpty()) {
            if(m_serialSample.portOpen)m_removedSerialPort=m_connectedSettings.port;
            m_serialSample=received;
            disconnectSerial();return;
        }
        m_serialSample=received;
        if(!received.portOpen||received.verifyCrc!=m_crc->isChecked()||m_suspended) {
            m_liveFrame={};m_heldTableFrame={};clearHardwareDisplay();m_serialHistory.breakSegment();return;
        }
        if(received.accepted&&tactile::validateFrame(received.frame)) {
            const auto previous=hardwareDisplayFrame(true);
            m_liveFrame=received.frame; // Table and diagnostics retain the current raw frame.
            if(received.frame.crcOk)m_heldTableFrame=received.frame;
            // Startup transients cannot pin the jump gate to a pre-settling
            // value. The native multi-frame stability gate admits the zero.
            if(m_waitingForFirstHardwareFrame)m_displayFilter.reset();
            if(m_displayFilter.accept(received.frame,m_jumpThreshold->value())) {
                if(m_serialHistory.append(received.frame)) {
                    m_trustedReceivedMs=received.receivedMs;
                    m_arrowFrom=tactile::matchingBaseline(previous,received.frame)?previous:tactile::Frame{};
                    if(m_arrowFrom.valid)m_arrowTransition.start();else m_arrowTransition.invalidate();
                } else {m_liveFrame={};m_heldTableFrame={};m_displayFilter.reset();clearHardwareDisplay();m_serialHistory.breakSegment();}
            } else m_serialHistory.breakSegment(true);
        } else {
            m_liveFrame={};m_serialHistory.breakSegment(true);
            if(received.error!=QStringLiteral("CRC mismatch"))m_heldTableFrame={};
            m_displayFilter.reason=received.error.isEmpty()?QStringLiteral("invalid frame"):received.error;
            if(received.requests)++m_displayFilter.rejected;
        }
        finishInitialSerialConnection(received);
}
void TactileVisualizationWidget::refresh()
{
    bool liveChanged=false;
    tactile::SerialSample received;
    if(m_serial.takeLatest(received)) {
        receiveSerialSample(received);liveChanged=true;
    }
    QElapsedTimer now;now.start();
    bool expired=false;
    if(m_trustedReceivedMs>=0&&(!m_serial.isRunning()||!hardwareDisplayFrame(false).valid)) {
        clearHardwareDisplay();m_serialHistory.breakSegment();expired=true;
    }
    if((m_liveFrame.valid||m_heldTableFrame.valid)&&(!m_serial.isRunning()||now.msecsSinceReference()-m_serialSample.receivedMs>m_timeout->value()+m_interval->value()+500)) {
        m_liveFrame={};m_heldTableFrame={};m_serialHistory.breakSegment();expired=true;
    }
    refreshDisplay(liveChanged||expired);
    if(expired&&m_finger==m_hardwareType)updatePanel();
}
void TactileVisualizationWidget::refreshDisplay(bool liveChanged)
{
    selectFrame();
    if(!liveChanged){
        if(m_arrowTransition.isValid()) {
            m_adapter->publish(arrowFrame(),m_finger,false,0,&m_tareFrame,noiseDisplayThreshold());
            if(m_arrowTransition.elapsed()>=ArrowTransitionMs){m_arrowTransition.invalidate();m_arrowFrom={};}
        }
        updateSerialStatus();return;
    }
    ++m_refreshCount;
    m_metrics.sampleMs=tactile::metricNowMs();
    if(m_metrics.enabled)m_metrics.source.record(m_refreshCount,m_metrics.sampleMs);
    m_adapter->publish(arrowFrame(),m_finger,false,0,&m_tareFrame,noiseDisplayThreshold());
    // Coalesce new data independently for the list and plot. Neither timer
    // controls sampling, history, or the 3D transition cadence.
    if(!m_hideReadingsToggle->isChecked()) {
        if(!m_readingsTimer.isActive())m_readingsTimer.start();
        if(!m_curveTimer.isActive())m_curveTimer.start();
    }
    updatePanelStatus();
    updateSerialStatus();
}
tactile::Force TactileVisualizationWidget::selectedForce() const
{
    if(!m_frame.valid) return {};
    return m_frame.fingers[m_finger].resultant;
}
tactile::Force TactileVisualizationWidget::chartForce() const
{
    const auto view=visualFrame();
    return view.valid?curveForce(sampleTactileDisplayPoint(view.fingers[m_finger].modules[0].points,
        m_finger,m_density->currentData().toInt(),selectedPoint()),selectedPoint()):tactile::Force{};
}
int TactileVisualizationWidget::displayPointCount() const {
    return tactileDisplayPointCount(m_finger,m_density->currentData().toInt());
}
tactile::Force TactileVisualizationWidget::displayPointForce(const QVector<tactile::Force> &raw,int point) const {
    return sampleTactileDisplayPoint(raw,m_finger,m_density->currentData().toInt(),point);
}
tactile::Frame TactileVisualizationWidget::visualFrame() const
{
    if(m_finger!=m_hardwareType)return {};
    if(awaitingDisplayZero())return {};
    return hardwareDisplayFrame(false);
}
tactile::Frame TactileVisualizationWidget::tableFrame() const
{
    if(m_finger!=m_hardwareType)return {};
    if(m_liveFrame.valid)return m_liveFrame;
    if(m_crc->isChecked()&&m_serialSample.portOpen&&!m_suspended
        &&m_serialSample.error==QStringLiteral("CRC mismatch"))return m_heldTableFrame;
    return {};
}
tactile::Frame TactileVisualizationWidget::arrowFrame() const
{
    if(awaitingDisplayZero())return {};
    return m_finger==m_hardwareType?hardwareDisplayFrame(true):tactile::Frame{};
}
bool TactileVisualizationWidget::awaitingDisplayZero() const
{
    return m_finger==m_hardwareType&&(m_waitingForFirstHardwareFrame
        ||(m_autoZeroResult.phase==tactile::AutoZeroPhase::Failed&&!hasDisplayBaseline()));
}
void TactileVisualizationWidget::clearHardwareDisplay()
{
    m_trustedReceivedMs=-1;m_arrowFrom={};m_arrowTransition.invalidate();
}
tactile::Frame TactileVisualizationWidget::hardwareDisplayFrame(bool interpolate) const
{
    const auto age=tactileDisplayNowMs()-m_trustedReceivedMs;
    if(m_trustedReceivedMs<0||age<0||age>TrustedHoldMs||m_suspended)return {};
    auto display=m_displayFilter.last;
    if(!interpolate||!m_arrowTransition.isValid()||!tactile::matchingBaseline(display,m_arrowFrom))return display;
    const double amount=std::clamp(double(m_arrowTransition.elapsed())/ArrowTransitionMs,0.0,1.0);
    if(amount>=1)return display;
    // These rounded display-only vectors never enter raw tables, history or the rejection gate.
    for(int f=0;f<display.fingers.size();++f) {
        auto &finger=display.fingers[f];if(!finger.valid)continue;
        finger.resultant={};
        for(int m=0;m<finger.modules.size();++m) {
            auto &module=finger.modules[m];module.resultant={};
            for(int p=0;p<module.points.size();++p) {
                auto &force=module.points[p].force;const auto from=m_arrowFrom.fingers[f].modules[m].points[p].force;
                force={std::round(from.x+(force.x-from.x)*amount),std::round(from.y+(force.y-from.y)*amount),std::round(from.z+(force.z-from.z)*amount)};
                module.resultant+=force;
            }
            finger.resultant+=module.resultant;
        }
    }
    return display;
}
bool TactileVisualizationWidget::hasDisplayBaseline() const
{
    if(m_finger!=m_hardwareType)return false;
    const auto &reference=m_heldTableFrame.valid?m_heldTableFrame:m_displayFilter.last;
    return tactile::matchingBaseline(reference,m_tareFrame)&&m_tareFrame.fingers[m_finger].valid;
}
tactile::Force TactileVisualizationWidget::curveForce(const tactile::Force &raw,int point) const
{
    return tactile::zeroSmallComponents(tactile::relativeForce(raw,displayBaselineForce(point)),noiseDisplayThreshold());
}
tactile::Force TactileVisualizationWidget::displayBaselineForce(int point) const
{
    return hasDisplayBaseline()?sampleTactileDisplayPoint(m_tareFrame.fingers[m_finger].modules[0].points,
        m_finger,m_density->currentData().toInt(),point):tactile::Force{};
}
double TactileVisualizationWidget::noiseDisplayThreshold() const
{
    return m_finger==m_hardwareType&&m_noiseEnabled->isChecked()?m_noiseThreshold->value():0;
}
void TactileVisualizationWidget::setDisplayZero()
{
    if(m_waitingForFirstHardwareFrame)return;
    auto baseline=tableFrame();
    if(m_finger==m_hardwareType&&!baseline.crcOk)baseline=m_heldTableFrame;
    if(!tactile::validateFrame(baseline)||(m_finger==m_hardwareType&&!baseline.crcOk))return;
    m_autoZeroResult={};applyDisplayZero(baseline);
}
void TactileVisualizationWidget::applyDisplayZero(const tactile::Frame &baseline)
{
    m_tareFrame=baseline;
    m_arrowTransition.invalidate();m_arrowFrom={};
    m_serialHistory.begin(baseline.sessionId,baseline.generation);
    if(tactile::matchingBaseline(baseline,m_displayFilter.last)&&baseline.sequence<=m_displayFilter.last.sequence)
        m_serialHistory.append(m_displayFilter.last); // An averaged zero is never a raw history sample.
    updateValues();
}
void TactileVisualizationWidget::updateValues()
{
    m_readingsBaseline.clear();
    updateDisplayPointOptions();
    selectFrame();updateSerialStatus();
    m_adapter->publish(arrowFrame(),m_finger,false,0,&m_tareFrame,noiseDisplayThreshold());
    updatePanel();
}
void TactileVisualizationWidget::selectPoint(int point)
{
    if(point<0||point>=displayPointCount())return;
    m_selectedPoints[m_finger]=point;
    updateReadings();
    updateCurve();
    if(!m_hideReadingsToggle->isChecked())m_pointTable->scrollToItem(m_pointTable->item(point,0));
}
void TactileVisualizationWidget::updateReadings()
{
    m_readingsTimer.stop();
    if(m_hideReadingsToggle->isChecked())return;
    const QSignalBlocker tableBlock(m_pointTable);
    const auto displayed=tableFrame();
    const bool tableHeld=m_finger==m_hardwareType&&!m_liveFrame.valid&&displayed.valid;
    m_tableLabel->setText(tr("Zero-point delta per output · counts"));
    m_chartLabel->setText((hasDisplayBaseline()?tr("P%1 · delta counts · time (s)"):
        tr("P%1 · displayed counts · time (s)")).arg(selectedPoint()+1));
    const auto v=chartForce(); const double values[]={v.x,v.y,v.z};
    const bool visualValid=visualFrame().valid;
    for(int i=0;i<3;++i) m_values[i]->setText(QString("%1 %2").arg(QString("XYZ").mid(i,1),visualValid?QString::number(values[i],'f',0):QString("--")));
    const int rows=displayPointCount();
    if(m_pointTable->rowCount()!=rows) {
        m_pointTable->setRowCount(rows);
        for(int row=0;row<rows;++row)for(int col=0;col<4;++col) {
            if(!m_pointTable->item(row,col))m_pointTable->setItem(row,col,new QTableWidgetItem);
            m_pointTable->item(row,col)->setTextAlignment(col==0?Qt::AlignCenter:Qt::AlignRight|Qt::AlignVCenter);
            if(col==0)m_pointTable->item(row,col)->setText(QString("P%1").arg(row+1));
        }
    }
    const bool tared=tactile::matchingBaseline(displayed,m_tareFrame)&&m_tareFrame.fingers[m_finger].valid;
    const int grid=m_density->currentData().toInt();
    if(tared&&m_readingsBaseline.isEmpty()) {
        m_readingsBaseline.reserve(rows);
        for(int row=0;row<rows;++row)m_readingsBaseline.append(
            sampleTactileDisplayPoint(m_tareFrame.fingers[m_finger].modules[0].points,m_finger,grid,row));
    }
    const auto measuredCount=tactile::sensorLayout(m_finger).points.size();
    const auto measuredTip=tr("Measured point"), interpolatedTip=tr("Interpolated output point");
    const bool rangeWarning=displayed.sourceMode=="Serial"&&displayed.valid&&(!displayed.crcOk||!displayed.rangeOk||(!tableHeld&&!m_displayFilter.reason.isEmpty()));
    const QBrush background=rangeWarning?QBrush(QColor("#ffd58a")):QBrush();
    const QBrush foreground=rangeWarning?QBrush(QColor("#302000")):QBrush();
    const double noise=noiseDisplayThreshold();
    for(int row=0;row<rows;++row) {
        m_pointTable->item(row,0)->setToolTip(row<measuredCount?measuredTip:interpolatedTip);
        tactile::Force difference;
        if(tared) {
            const auto force=sampleTactileDisplayPoint(displayed.fingers[m_finger].modules[0].points,m_finger,grid,row);
            difference=tactile::zeroSmallComponents(tactile::relativeForce(force,m_readingsBaseline[row]),noise);
        }
        const double deltas[]={difference.x,difference.y,difference.z};
        for(int axis=0;axis<3;++axis) {
            const QString delta=tared?QString::number(deltas[axis],'f',0):QString("--");
            auto *cell=m_pointTable->item(row,axis+1);
            if(cell->text()!=delta){cell->setText(delta);cell->setToolTip(delta);}
            cell->setBackground(background);cell->setForeground(foreground);
        }
    }
    const auto selectedRows=m_pointTable->selectionModel()->selectedRows();
    if(m_pointTable->currentRow()!=selectedPoint()||selectedRows.size()!=1||selectedRows.front().row()!=selectedPoint())
        m_pointTable->setCurrentCell(selectedPoint(),0,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
}
void TactileVisualizationWidget::updateCurve()
{
    m_curveTimer.stop();
    if(!m_hideReadingsToggle->isChecked())m_curves->update();
}
void TactileVisualizationWidget::updateDisplayPointOptions()
{
    for(int i=0;i<m_density->count();++i)
        m_density->setItemText(i,tr("%1 tactile output points").arg(tactileDisplayPointCount(m_finger,m_density->itemData(i).toInt())));
}
void TactileVisualizationWidget::updatePanel()
{
    updateReadings();
    updateCurve();
    updatePanelStatus();
}
void TactileVisualizationWidget::updatePanelStatus()
{
    const QString filterText=m_finger==m_hardwareType&&m_displayFilter.reason!=QStringLiteral("CRC mismatch")
        ?m_displayFilter.reason:QString();
    m_filterStatus->setVisible(m_statusToggle->isChecked()&&!filterText.isEmpty());
    m_filterStatus->setToolTip(filterText);
    m_filterStatus->setText(m_filterStatus->fontMetrics().elidedText(filterText,Qt::ElideRight,m_filterStatus->width()));
}
void TactileVisualizationWidget::hideEvent(QHideEvent *e) {
    m_portScanTimer.stop();
    m_missingPortScans=0;
    saveSplitterLayout();
    if(!m_dockManaged) {if(isMinimized())setSuspended(true);else stop();}
    QWidget::hideEvent(e);
}
void TactileVisualizationWidget::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refreshSerialPorts();
    if(m_connectionSettings->isChecked()||m_serial.isRunning()||m_autoConnectEnabled)m_portScanTimer.start();
    if(!m_splitterReady)QTimer::singleShot(0,this,[this]{if(isVisible()&&!m_splitterReady)restoreSplitterLayout();});
    if(!m_dockManaged&&!isMinimized())setSuspended(false);
    if(m_defaultConnectRequested)QTimer::singleShot(0,this,&TactileVisualizationWidget::connectDefaultSerial);
}
void TactileVisualizationWidget::closeEvent(QCloseEvent *e) { saveSplitterLayout();stop(); QWidget::closeEvent(e); }
void TactileVisualizationWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    const bool compact=m_hideReadingsToggle->isChecked()&&!m_connectionSettings->isChecked()&&!m_displaySettings->isChecked();
    const auto orientation=!compact&&width()<640?Qt::Vertical:Qt::Horizontal;
    // Dragged sizes were saved by splitterMoved; the old axis may already be squeezed by this resize.
    if(m_splitter->orientation()!=orientation){m_splitter->setOrientation(orientation);if(m_splitterReady)restoreSplitterLayout();}
}
void TactileVisualizationWidget::changeEvent(QEvent *e) {
    QWidget::changeEvent(e);
    if(e->type()==QEvent::LanguageChange){retranslate();if(m_canvas->engine())m_canvas->engine()->retranslate();updateValues();}
    if(e->type()==QEvent::FontChange && m_fingerBox)
        QTimer::singleShot(0,this,&TactileVisualizationWidget::updateRightPanelLayout);
    if(e->type()==QEvent::PaletteChange){updateSettingsStyle();m_adapter->setPalette(palette());m_canvas->setClearColor(m_adapter->background());}
}
void TactileVisualizationWidget::updateSettingsStyle()
{
    auto *body=findChild<QWidget *>("tactileSettingsBody");
    if(!body)return;
    const auto appearance=palette();
    const bool dark=appearance.color(QPalette::Base).lightness()<128;
    // Re-polishing the owner itself can discard an explicit external palette.
    // Keep this stylesheet inside the scrolling content and preserve the owner.
    const auto settingsStyle=QString::fromLatin1(R"(
        QWidget#tactileDisplayControls, QWidget#tactileSerialControls {
            background: %4; border: 1px solid %1; border-radius: 12px;
        }
        QWidget[tactileSettingRow="true"] { border: 0; border-bottom: 1px solid %1; }
        QLabel[tactileUnit="true"] { color: %2; }
        QComboBox[tactileCompactControl="true"], QAbstractSpinBox[tactileCompactControl="true"] {
            background: %4; color: %6; border: 1px solid %1; border-radius: 8px; padding: 3px 8px;
        }
        QComboBox[tactileCompactControl="true"]::drop-down { border: 0; background: transparent; width: 24px; }
        QComboBox[tactileCompactControl="true"]::down-arrow { image: none; width: 0; height: 0; }
        QComboBox[tactileCompactControl="true"] QLineEdit { background: transparent; border: 0; padding: 0; }
        QAbstractSpinBox[tactileCompactControl="true"] { padding-right: 24px; }
        QAbstractSpinBox[tactileCompactControl="true"]::up-button {
            subcontrol-origin: border; subcontrol-position: top right; width: 20px; height: 14px;
            background: transparent; border: 0; border-left: 1px solid %1; border-top-right-radius: 7px;
        }
        QAbstractSpinBox[tactileCompactControl="true"]::down-button {
            subcontrol-origin: border; subcontrol-position: bottom right; width: 20px; height: 14px;
            background: transparent; border: 0; border-left: 1px solid %1; border-bottom-right-radius: 7px;
        }
        QAbstractSpinBox[tactileCompactControl="true"]::up-button:hover,
        QAbstractSpinBox[tactileCompactControl="true"]::down-button:hover { background: %3; }
        QComboBox[tactileCompactControl="true"]:focus, QAbstractSpinBox[tactileCompactControl="true"]:focus {
            border-color: %5;
        }
        QComboBox[tactileCompactControl="true"]:disabled, QAbstractSpinBox[tactileCompactControl="true"]:disabled {
            color: %2; background: %3;
        }
        QPushButton[tactileCompactControl="true"] {
            background: %7; border: 1px solid transparent; border-radius: 8px; padding: 3px 8px;
        }
        QPushButton[tactileCompactControl="true"]:hover { background: %8; }
        QPushButton[tactileCompactControl="true"]:pressed { background: %9; }
        QPushButton[tactileCompactControl="true"]:checked { border-color: %5; }
        QPushButton[tactileCompactControl="true"]:focus { border-color: %5; }
        QPushButton[tactileCompactControl="true"]:disabled { color: %2; }
    )").arg(dark?"#454950":"#e7e7e7",dark?"#999da5":"#858585",dark?"#30343b":"#f5f5f5",
        appearance.color(QPalette::Base).name(),appearance.color(QPalette::Highlight).name(),appearance.color(QPalette::Text).name(),
        UiStyle::actionButtonColor(appearance,UiStyle::ButtonState::Normal).name(),
        UiStyle::actionButtonColor(appearance,UiStyle::ButtonState::Hovered).name(),
        UiStyle::actionButtonColor(appearance,UiStyle::ButtonState::Pressed).name());
    // Dock transitions may report an unchanged palette. Avoid a full widget
    // re-polish in that path, which can stall frame delivery and focus handling.
    const bool restyle=body->styleSheet()!=settingsStyle;
    if(!restyle&&body->palette()==appearance)return;
    if(restyle)body->setStyleSheet(settingsStyle);
    body->setPalette(appearance);
    // Styled descendants do not inherit subsequent palette changes by default.
    for(auto *control:body->findChildren<QWidget *>())control->setPalette(appearance);
}
void TactileVisualizationWidget::retranslate()
{
    setWindowTitle(tr("Tactile visualization"));
    m_curves->setToolTip(tr("Dashed line: interpolation across missing samples (up to 500 ms). Arrows interpolate trusted values over 100 ms."));
    const QStringList fingers={tr("Fingertip tactile"),tr("Terminal tactile"),tr("Ultrathin tactile")};
    for(int i=0;i<3;++i)m_fingers->button(i)->setText(fingers[i]);
    updateSensorChoices();

    m_chartLabel->setText((hasDisplayBaseline()?tr("P%1 · delta counts · time (s)"):tr("P%1 · displayed counts · time (s)")).arg(selectedPoint()+1));
    m_tareButton->setText(tr("Set delta zero point"));
    tactileSettingCaption(m_displayMode,tr("Display mode"));
    m_displayMode->setItemText(0,tr("XYZ axes"));m_displayMode->setItemText(1,tr("Dot matrix"));
    m_displayMode->setAccessibleName(tr("Display mode"));
    m_displayMode->setToolTip(tr("Point colors show Z pressure from 0 to 2500 counts; higher values keep the maximum color. Dot matrix spheres rise outward with positive Z only. X/Y do not move the dots; zero or negative Z leaves them on the surface."));
    tactileSettingCaption(m_arrowGain,tr("Display gain"),tr("x"));
    m_arrowGain->setToolTip(tr("Scale arrow lengths or dot lift from 0.1x to 100x. Dot lift scales linearly with positive Z and gain; arrow lengths remain capped. Raw data, delta and curves stay unchanged."));
    tactileSettingCaption(m_arrowWidthGain,tr("Arrow thickness gain"),tr("x"));
    m_arrowWidthGain->setToolTip(tr("Scale the shaft and head widths of all force arrows from 0.1x to 10x. Length uses its own gain; 1x restores the original thickness."));
    tactileSettingCaption(m_resultantSizeGain,tr("Resultant arrow size gain"),tr("x"));
    m_resultantSizeGain->setToolTip(tr("Scale the entire resultant arrow from 0.1x to 10x around its base, including the shaft and head. 1x uses the current display and thickness gains."));
    tactileSettingCaption(m_jumpThreshold,tr("Display jump limit"),tr("counts/frame"));
    m_jumpThreshold->setToolTip(tr("Hardware only: compare every axis with the last displayed raw frame. Reject the whole display frame above this limit; CRC/range errors are always skipped. Current readings remain in the list, following the display noise threshold. Increase the limit if a real force step is intentionally large."));
    m_noiseEnabled->setText(tr("Suppress small readings (hardware only)"));
    m_reverseZArrows->setText(tr("Reverse Z arrow direction"));
    tactileSettingCaption(m_noiseEnabled,tr("Suppress small readings (hardware only)"));
    tactileSettingCaption(m_reverseZArrows,tr("Reverse Z arrow direction"));
    m_reverseZArrows->setToolTip(tr("Reverse only the Z direction of force arrows, including the Z component of resultants. Raw readings, tables, curves, point colors and dot displacement are unchanged."));
    tactileSettingCaption(m_noiseThreshold,tr("Noise threshold"),tr("counts"));
    const auto noiseHelp=tr("Hardware display: X/Y components at or below this absolute threshold become zero in the model, chart and list. Z always passes through, whether suppression is on or off. Tare is subtracted first. Default: 150 counts.");
    m_noiseEnabled->setToolTip(noiseHelp);m_noiseThreshold->setToolTip(noiseHelp);
    m_tareButton->setToolTip(tr("Use the latest CRC-valid values, or the previous values held in the table after a CRC error. Without valid data, the zero point stays unchanged."));
    m_tableLabel->setText(tr("Zero-point delta per output · counts"));
    m_pointTable->setHorizontalHeaderLabels({tr("Point"),QStringLiteral("X"),QStringLiteral("Y"),QStringLiteral("Z")});
    const QFontMetrics cellMetrics(m_pointTable->font()),headerMetrics(m_pointTable->horizontalHeader()->font());
    m_pointTable->setColumnWidth(0,qMax(cellMetrics.horizontalAdvance("P136"),headerMetrics.horizontalAdvance(tr("Point")))+18);
    m_pointTable->setToolTip(tr("Only the difference from the zero baseline is shown. X/Y follow the noise threshold; Z is never noise-suppressed. -- means no valid zero baseline or data. Added points use surface interpolation."));
    m_crc->setToolTip(tr("Off accepts CRC-mismatched data for diagnosis. Length, ID and function checks remain active; range warnings do not alter raw values."));
    tactileSettingCaption(m_density,tr("Display method"));
    updateDisplayPointOptions();
    m_density->setItemData(0,tr("Measured points"),Qt::ToolTipRole);
    m_density->setItemData(1,tr("Measured points plus surface interpolation"),Qt::ToolTipRole);
    m_density->setItemData(2,tr("Measured points plus surface interpolation"),Qt::ToolTipRole);
    m_density->setToolTip(tr("Choose the output point count shared by the model, chart and list."));
    m_pointsToggle->setText(tr("Point forces"));m_resultantToggle->setText(tr("Resultant"));
    tactileSettingCaption(m_pointsToggle,tr("Point forces"));
    m_componentsToggle->setText(tr("XYZ components"));
    tactileSettingCaption(m_resultantToggle,tr("Resultant"));
    tactileSettingCaption(m_componentsToggle,tr("XYZ components"));
    m_opacityLabel->setText(tr("Model visibility"));
    m_opacitySlider->setAccessibleName(tr("Model visibility"));
    m_opacitySlider->setToolTip(tr("0% hidden; 100% opaque. Lower visibility reveals pressure inside the model."));
    m_componentsToggle->setToolTip(tr("X red / Y green / Z blue; no Z display threshold. Z points into the surface."));
    m_resetView->setText(tr("Reset view"));m_frontView->setText(tr("Front view"));
    m_canvas->setToolTip(QString());
    m_connectionSettings->setText(tr("Connection settings"));m_connect->setText(tr("Connect sensor"));
    m_displaySettings->setText(tr("Display settings"));m_statusToggle->setText(tr("Show communication status"));
    m_minimizeConnection->setToolTip(tr("Minimize connection settings"));
    m_minimizeConnection->setAccessibleName(m_minimizeConnection->toolTip());
    m_minimizeDisplay->setToolTip(tr("Minimize display settings"));
    m_minimizeDisplay->setAccessibleName(m_minimizeDisplay->toolTip());
    m_hideReadingsToggle->setText(tr("Hide chart and values list"));
    tactileSettingCaption(m_statusToggle,tr("Show communication status"));
    tactileSettingCaption(m_hideReadingsToggle,tr("Hide chart and values list"));
    m_disconnect->setText(tr("Disconnect"));
    m_scanPorts->setText(tr("Refresh ports"));
    m_crc->setText(tr("Verify CRC"));
    m_port->setToolTip(tr("Serial port"));
    tactileSettingCaption(m_port,tr("Serial port"));
    tactileSettingCaption(m_baud,tr("Baud rate"));tactileSettingCaption(m_slave,tr("Slave ID"));
    m_slave->setToolTip(tr("0 is the vendor public address: single-device bus only. New device IDs must be 1-127; 0 cannot be written."));
    m_baud->setToolTip(tr("Connect with the current settings first. While connected, edit these values and click Write and save to change the sensor."));
    m_writeAddress->setText(tr("Write and save"));
    m_writeAddress->setToolTip(tr("Save changed ID and baud together, then power-cycle the sensor once."));
    tactileSettingCaption(m_timeout,tr("Response timeout"),tr("ms"));
    tactileSettingCaption(m_interval,tr("Request gap"),tr("ms"));
    m_interval->setToolTip(tr("Delay after each completed serial request. Changes apply during acquisition; response time is additional. The display refreshes separately."));

    // Resolve widths from the current language, never from the dock width or
    // a changing port list. Language changes may resize controls; window changes cannot.
    for(auto *combo:{m_port,m_baud,m_displayMode,m_density}) {
        int width=combo==m_port?112:96;
        if(combo!=m_port)for(int i=0;i<combo->count();++i)width=qMax(width,combo->fontMetrics().horizontalAdvance(combo->itemText(i))+48);
        combo->setProperty("tactileCompactControl",true);combo->setFixedSize(width,30);
    }
    for(auto *spin:findChildren<QAbstractSpinBox *>()) {
        spin->setProperty("tactileCompactControl",true);spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        spin->setFixedSize(96,30);
    }
    for(auto *button:findChildren<QPushButton *>()) {
        if(m_fingers->buttons().contains(button)||dynamic_cast<TactileSwitch *>(button))continue;
        button->setProperty("tactileCompactControl",true);
        button->setFixedSize(button->fontMetrics().horizontalAdvance(button->text())+20,28);
    }
    m_disconnect->setFixedWidth(qMax(m_disconnect->width(),m_disconnect->fontMetrics().horizontalAdvance(tr("Disconnecting..."))+20));
}

void TactileVisualizationWidget::createSerialControls(QVBoxLayout *root)
{
    auto *actions=new QHBoxLayout;
    m_connectionSettings=new QPushButton; m_connectionSettings->setCheckable(true);
    m_connectionSettings->setObjectName("tactileConnectionSettings");
    m_displaySettings=new QPushButton;m_displaySettings->setCheckable(true);m_displaySettings->setObjectName("tactileDisplaySettings");
    m_resetView=new QPushButton;m_resetView->setObjectName("resetTactileView");m_frontView=new QPushButton;m_frontView->setObjectName("frontTactileView");
    m_tareButton=new QPushButton;m_tareButton->setObjectName("tactileTare");
    connect(m_tareButton,&QPushButton::clicked,this,&TactileVisualizationWidget::setDisplayZero);
    m_crc=new QCheckBox;m_crc->setObjectName("tactileVerifyCrc");m_crc->setChecked(true);
    for(auto *button:{m_connectionSettings,m_displaySettings,m_tareButton,m_resetView,m_frontView})
        actions->addWidget(button);
    actions->addStretch();actions->addWidget(m_crc);root->addLayout(actions);
    m_zeroStatus=new QLabel;m_zeroStatus->setObjectName("tactileZeroStatus");
    m_zeroStatus->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    m_zeroStatus->setFixedHeight(m_zeroStatus->fontMetrics().height()+4);root->addWidget(m_zeroStatus);
    m_serialStatus=new QLabel;m_serialStatus->setObjectName("tactileSerialStatus");m_serialStatus->setWordWrap(false);
    m_serialStatus->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    m_serialStatus->setFixedHeight(m_serialStatus->fontMetrics().height()+4);root->addWidget(m_serialStatus);
    m_serialControls=new QWidget;m_serialControls->setObjectName("tactileSerialControls");
    auto *serialLayout=new QVBoxLayout(m_serialControls);serialLayout->setContentsMargins(16,8,16,8);serialLayout->setSpacing(0);
    m_minimizeConnection=tactileMinimizeButton(serialLayout,m_connectionSettings,"tactileMinimizeConnection");
    m_serialControls->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
    m_port=new TactilePortComboBox([this]{refreshSerialPorts();});m_port->setObjectName("tactilePort");m_port->setEditable(false);m_port->setMinimumContentsLength(7);
    m_baud=new TactileSettingsCombo;m_baud->setObjectName("tactileBaud");m_baud->addItems({"38400","115200","921600"});m_baud->setCurrentText("921600");
    m_slave=new TactileSettingsSpin<QSpinBox>;m_slave->setObjectName("tactileSlave");m_slave->setRange(0,127);m_slave->setValue(1);m_slave->setKeyboardTracking(false);
    m_timeout=new TactileSettingsSpin<QSpinBox>;m_timeout->setObjectName("tactileTimeout");m_timeout->setRange(50,5000);m_timeout->setValue(250);
    m_interval=new TactileSettingsSpin<QSpinBox>;m_interval->setObjectName("tactileInterval");m_interval->setRange(10,60000);m_interval->setValue(10);
    m_interval->setKeyboardTracking(false);
    connect(m_timeout,&QSpinBox::valueChanged,this,[](int value){tactilePreferences().setValue("tactile/serial/timeoutMs",value);});
    connect(m_interval,&QSpinBox::valueChanged,this,[this](int value){
        m_serial.setIntervalMs(value);m_connectedSettings.intervalMs=value;
        tactilePreferences().setValue("tactile/serial/intervalMs",value);
    });
    m_scanPorts=new QPushButton;m_scanPorts->setObjectName("tactileRefreshPorts");
    m_availableSerialPorts=[] {
        QStringList ports;for(const auto &port:QSerialPortInfo::availablePorts())ports.append(port.portName());return ports;
    };
    refreshSerialPorts();
    connect(m_scanPorts,&QPushButton::clicked,this,&TactileVisualizationWidget::refreshSerialPorts);
    m_portScanTimer.setInterval(1000);
    connect(&m_portScanTimer,&QTimer::timeout,this,[this] {
        if(!isVisible())return;
        if(m_serial.isRunning()){
            if(m_serial.stopping()||m_serial.configBusy()||m_connectedSettings.port.isEmpty()){
                m_missingPortScans=0;return;
            }
            const auto ports=m_availableSerialPorts();
            if(ports.contains(m_connectedSettings.port,Qt::CaseInsensitive)){
                m_missingPortScans=0;return;
            }
            // Require two consecutive misses to avoid reacting to a transient scan.
            if(++m_missingPortScans>=2){
                m_observedSerialPorts=ports;
                m_removedSerialPort=m_connectedSettings.port;
                cancelSerialRetry();
                m_serialSample.error=tr("The connected serial port was removed.");
                disconnectSerial();
            }
        }else if((m_connectionSettings->isChecked()||m_autoConnectEnabled)&&!m_port->view()->isVisible())scanIdleSerialPorts();
    });
    auto *connectionRow=new QHBoxLayout;connectionRow->setSpacing(24);
    auto *portField=tactileSettingRow(m_port),*baudField=tactileSettingRow(m_baud),*slaveField=tactileSettingRow(m_slave);
    m_baudLabel=baudField->findChild<QLabel *>("tactileBaudLabel");m_slaveLabel=slaveField->findChild<QLabel *>("tactileSlaveLabel");
    auto *portActions=new QHBoxLayout;portActions->addStretch();portActions->addWidget(m_scanPorts,0,Qt::AlignVCenter);
    connectionRow->addWidget(portField,1);connectionRow->addLayout(portActions,1);
    serialLayout->addLayout(connectionRow);
    serialLayout->addWidget(baudField);serialLayout->addWidget(slaveField);
    serialLayout->addWidget(tactileSettingRow(m_timeout,true));serialLayout->addWidget(tactileSettingRow(m_interval,true));
    m_connect=new QPushButton;m_connect->setObjectName("tactileConnect");m_disconnect=new QPushButton;m_disconnect->setObjectName("tactileDisconnect");
    m_writeAddress=new QPushButton;m_writeAddress->setObjectName("tactileWriteAddress");
    auto *connectionActions=new QHBoxLayout;connectionActions->setContentsMargins(0,8,0,8);connectionActions->setSpacing(0);
    connectionActions->addWidget(m_connect);connectionActions->addStretch(1);connectionActions->addWidget(m_disconnect);connectionActions->addStretch(1);
    connectionActions->addWidget(m_writeAddress);serialLayout->addLayout(connectionActions);
    m_configStatus=new QLabel;m_configStatus->setObjectName("tactileConfigStatus");m_configStatus->setWordWrap(true);
    m_configStatus->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);m_configStatus->hide();
    // Configuration completion is independent of paused/hidden sample refresh.
    connect(&m_configTimer,&QTimer::timeout,this,[this]{
        tactile::DeviceConfigResult result;
        if(m_serial.takeConfigResult(result)){m_configAwaitClose=result.writeAttempted;receiveDeviceConfig(result);}
        if(!m_serial.configBusy()&&(!m_configAwaitClose||!m_serial.isRunning())){m_configTimer.stop();m_configAwaitClose=false;}
        updateSerialStatus();
    });
    m_configStatus->setContentsMargins(0,8,0,4);serialLayout->addWidget(m_configStatus);
    connect(m_writeAddress,&QPushButton::clicked,this,&TactileVisualizationWidget::writeDeviceAddress);
    connect(m_baud,&QComboBox::activated,this,[this]{deviceSettingsEdited();});
    connect(m_slave,&QSpinBox::valueChanged,this,[this]{deviceSettingsEdited();});
    root->addWidget(m_serialControls);m_serialControls->hide();
    connect(m_connectionSettings,&QPushButton::toggled,this,[this](bool expanded) {
        m_serialControls->setVisible(expanded);
        if(expanded){refreshSerialPorts();if(isVisible())m_portScanTimer.start();}
        else if(!m_serial.isRunning()&&!m_autoConnectEnabled)m_portScanTimer.stop();
    });
    connect(m_connect,&QPushButton::clicked,this,[this]{connectSerial();});
    connect(m_disconnect,&QPushButton::clicked,this,[this]{cancelSerialRetry();disconnectSerial();});
    m_serialRetryTimer.setSingleShot(true);m_serialRetryTimer.setInterval(3000);
    m_serialRetryTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_serialRetryTimer,&QTimer::timeout,this,[this]{
        if(isVisible()&&!m_serial.isRunning()&&!m_port->view()->isVisible())scanIdleSerialPorts();
    });
    m_initialResponseTimer.setSingleShot(true);
    connect(&m_initialResponseTimer,&QTimer::timeout,this,[this]{
        if(!m_waitingForFirstHardwareFrame)return;
        // A busy GUI may deliver the timeout before its 33 ms refresh. Consume
        // the worker's latest response before declaring a connection failure.
        tactile::SerialSample last;if(m_serial.takeLatest(last))receiveSerialSample(last);
        if(!m_waitingForFirstHardwareFrame||m_autoZeroResult.started)return;
        disconnectSerial();
        m_serialSample.error=tr("No valid sensor response. Check the serial port, baud rate and device ID.");
        if(!m_serialSample.modelIdentified)
            m_serialSample.error=tr("Could not identify the sensor model. Check the baud rate and device ID, then connect again.");
        setDeviceConfigStatus(m_serialSample.error);
        updateSerialStatus();
    });
    connect(&m_serial,&tactile::TactileSerialSource::finished,this,[this]{
        tactile::SerialSample last;if(m_serial.takeLatest(last))receiveSerialSample(last);
        if(m_waitingForFirstHardwareFrame&&!m_serialSample.modelIdentified){
            m_serialSample.error=tr("Could not identify the sensor model. Check the baud rate and device ID, then connect again.");
            setDeviceConfigStatus(m_serialSample.error);
        }
        if(m_waitingForFirstHardwareFrame||(m_serialSample.portOpen&&!m_configTimer.isActive()))disconnectSerial();
        if(!m_connectionSettings->isChecked()&&!m_autoConnectEnabled)m_portScanTimer.stop();
        updateSerialStatus();refreshSerialPorts();
    });
    connect(m_crc,&QCheckBox::toggled,this,[this](bool on){m_serial.setVerifyCrc(on);m_displayFilter.reset();m_liveFrame={};m_heldTableFrame={};clearHardwareDisplay();m_serialHistory.clear();m_tareFrame={};m_autoZeroResult={};updateValues();});
}
QStringList TactileVisualizationWidget::refreshSerialPorts()
{
    // Enumeration never opens a device or changes an active connection.
    if(m_serial.isRunning()||!m_port->isEnabled())return {};
    auto ports=m_availableSerialPorts();ports.removeDuplicates();ports.sort(Qt::CaseInsensitive);
    QStringList listed;for(int i=0;i<m_port->count();++i)listed.append(m_port->itemText(i));
    if(ports==listed)return ports;
    const auto current=m_port->currentText();
    const auto remembered=m_removedSerialPort.isEmpty()?m_defaultSerialPort:m_removedSerialPort;
    const auto target=current.isEmpty()?remembered:current;
    const QSignalBlocker blocker(m_port);
    m_port->clear();m_port->addItems(ports);
    if(!target.isEmpty())m_port->setCurrentIndex(m_port->findText(target));
    return ports;
}
void TactileVisualizationWidget::scanIdleSerialPorts()
{
    if(!m_port->isEnabled()||m_configTimer.isActive()||m_canvas->status()!=QQuickWidget::Ready)return;
    auto ports=refreshSerialPorts();
    if(!m_autoConnectEnabled)return;
    m_portsReportedRemoved.removeIf([&](const QString &port){return !ports.contains(port,Qt::CaseInsensitive);});
    ports.removeIf([&](const QString &port){return m_portsReportedRemoved.contains(port.toUpper());});
    QStringList added;
    for(const auto &port:ports)
        if(!m_observedSerialPorts.contains(port,Qt::CaseInsensitive))added.append(port);
    m_observedSerialPorts=ports;
    const auto preferred=m_removedSerialPort.isEmpty()?m_defaultSerialPort:m_removedSerialPort;
    const auto target=preferred.isEmpty()&&added.size()==1?added.first():preferred;
    if(!target.isEmpty()&&added.contains(target,Qt::CaseInsensitive)){
        const int index=m_port->findText(target,Qt::MatchFixedString);
        if(index<0)return;
        m_port->setCurrentIndex(index);
        connectSerial(SerialAttempt::Hotplug);
        return;
    }
    if(m_serialRetryPort.isEmpty())return;
    if(!ports.contains(m_serialRetryPort,Qt::CaseInsensitive)
        ||m_port->currentText().compare(m_serialRetryPort,Qt::CaseInsensitive)!=0){
        cancelSerialRetry();updateSerialStatus();
    }else if(!m_serialRetryTimer.isActive())connectSerial(SerialAttempt::Retry);
}
void TactileVisualizationWidget::serialPortChanged(const QString &port,bool arrived)
{
    if(!m_autoConnectEnabled||!port.startsWith("COM",Qt::CaseInsensitive))return;
    if(arrived){
        m_portsReportedRemoved.remove(port.toUpper());
        if(isVisible()&&!m_serial.isRunning()&&!m_port->view()->isVisible())scanIdleSerialPorts();
        return;
    }
    // The same COM name can disappear and return between enumeration ticks.
    m_portsReportedRemoved.insert(port.toUpper());
    m_observedSerialPorts.removeIf([&](const QString &seen){return seen.compare(port,Qt::CaseInsensitive)==0;});
    if(m_serialRetryPort.compare(port,Qt::CaseInsensitive)==0){
        cancelSerialRetry();updateSerialStatus();
    }
    if(m_serial.isRunning()&&!m_serial.stopping()&&!m_configTimer.isActive()
        &&m_connectedSettings.port.compare(port,Qt::CaseInsensitive)==0){
        m_removedSerialPort=m_connectedSettings.port;
        m_serialSample.error=tr("The connected serial port was removed.");
        disconnectSerial();
    }
}
void TactileVisualizationWidget::cancelSerialRetry()
{
    m_serialRetryTimer.stop();
    m_serialRetryPort.clear();
}
void TactileVisualizationWidget::connectSerial(SerialAttempt attempt)
{
    if(m_serial.isRunning()||m_canvas->status()!=QQuickWidget::Ready)return;
    m_defaultConnectRequested=false;m_defaultConnectAttempted=true;
    cancelSerialRetry();
    setDeviceConfigStatus({});
    tactile::SerialSettings s;s.port=m_port->currentText().trimmed();s.baud=m_baud->currentText().toInt();s.slave=m_slave->value();s.type=m_finger;
    s.timeoutMs=m_timeout->value();s.intervalMs=m_interval->value();s.verifyCrc=m_crc->isChecked();
    if(!s.valid()){m_serialStatus->setText(tr("Invalid connection settings"));return;}
    if(attempt!=SerialAttempt::Manual)m_serialRetryPort=s.port;
    start();setSuspended(false);m_hardwareType=m_finger;m_serialSample={};m_liveFrame={};m_heldTableFrame={};m_displayFilter.reset();clearHardwareDisplay();m_tareFrame={};m_autoZeroResult={};
    m_connectedSettings=s;
    if(!m_observedSerialPorts.contains(s.port,Qt::CaseInsensitive))m_observedSerialPorts.append(s.port);
    m_missingPortScans=0;
    m_removedSerialPort.clear();
    m_verifyingAddress=m_pendingAddress==s.slave&&m_pendingBaud==s.baud&&m_pendingPort==s.port;
    m_waitingForFirstHardwareFrame=m_serial.begin(s,attempt==SerialAttempt::Retry,true);
    // USB can enumerate before the sensor is ready to answer after power-up.
    if(m_waitingForFirstHardwareFrame)
        m_initialResponseTimer.start(qMax(attempt==SerialAttempt::Hotplug?10000:1000,4*s.timeoutMs+500));
    m_serialHistory.begin(m_serial.sessionId(),m_serial.generation());updateValues();
}
void TactileVisualizationWidget::disconnectSerial()
{
    m_defaultConnectRequested=false;m_initialResponseTimer.stop();m_waitingForFirstHardwareFrame=false;
    if(auto *notice=findChild<QMessageBox *>("tactileSaveReminder"))notice->close();
    m_configTimer.stop();m_configAwaitClose=false;
    if(m_serial.configBusy())setDeviceConfigStatus(tr("Configuration interrupted; device state may have changed. Reconnect and read parameters before another write."));
    m_verifyingAddress=false;
    m_serial.stop();m_displayFilter.reset();m_liveFrame={};m_heldTableFrame={};clearHardwareDisplay();m_serialSample.portOpen=false;m_serialSample.accepted=false;m_serialHistory.clear();m_tareFrame={};m_autoZeroResult={};updateValues();refreshSerialPorts();
    if(!m_serialRetryPort.isEmpty())m_serialRetryTimer.start();
}
void TactileVisualizationWidget::restoreSerialSettings()
{
    auto settings=tactilePreferences();settings.beginGroup("tactile/serial");
    m_defaultSerialPort=settings.value("port").toString();
    if(!m_defaultSerialPort.isEmpty())m_port->setCurrentIndex(m_port->findText(m_defaultSerialPort));
    const int baud=m_baud->findText(settings.value("baud",921600).toString());
    if(baud>=0)m_baud->setCurrentIndex(baud);
    bool ok=false;const int slave=settings.value("slave",1).toInt(&ok);
    if(ok&&slave>=0&&slave<=127){const QSignalBlocker blocker(m_slave);m_slave->setValue(slave);}
    const int type=settings.value("type",0).toInt(&ok);
    if(ok&&type>=0&&type<3){m_finger=type;m_fingers->button(type)->setChecked(true);}
    const QSignalBlocker timeoutBlock(m_timeout),intervalBlock(m_interval);
    const int timeout=settings.value("timeoutMs",250).toInt(&ok);
    if(ok&&timeout>=m_timeout->minimum()&&timeout<=m_timeout->maximum())m_timeout->setValue(timeout);
    const int interval=settings.value("intervalMs",10).toInt(&ok);
    if(ok&&interval>=m_interval->minimum()&&interval<=m_interval->maximum())m_interval->setValue(interval);
}
void TactileVisualizationWidget::connectDefaultSerial()
{
    if(m_defaultConnectAttempted)return;
    m_defaultConnectRequested=true;
    if(!isVisible()||m_canvas->status()!=QQuickWidget::Ready)return;
    m_defaultConnectRequested=false;m_defaultConnectAttempted=true;
    m_observedSerialPorts=refreshSerialPorts();
    m_autoConnectEnabled=true;
    // A remembered but missing port must not silently become a different device.
    if(!m_defaultSerialPort.isEmpty())m_port->setCurrentIndex(m_port->findText(m_defaultSerialPort));
    else if(m_port->currentIndex()<0&&m_port->count()>0)m_port->setCurrentIndex(0);
    if(m_port->currentIndex()<0)return;
    connectSerial();
}
void TactileVisualizationWidget::finishInitialSerialConnection(const tactile::SerialSample &sample)
{
    if(!m_waitingForFirstHardwareFrame||!sample.modelIdentified||sample.frame.sessionId!=m_serial.sessionId()
        ||sample.frame.generation!=m_serial.generation()||sample.frame.hardwareType!=m_hardwareType
        ||sample.settings.port!=m_connectedSettings.port||sample.settings.baud!=m_connectedSettings.baud
        ||sample.settings.slave!=m_connectedSettings.slave||sample.settings.type!=m_hardwareType)return;
    m_autoZeroResult=sample.autoZero;
    if(m_autoZeroResult.started) {
        m_initialResponseTimer.stop();cancelSerialRetry();
    }
    if(m_autoZeroResult.phase==tactile::AutoZeroPhase::Collecting)return;
    const bool complete=m_autoZeroResult.phase==tactile::AutoZeroPhase::Complete;
    const auto &baseline=m_autoZeroResult.baseline;
    if(complete&&(!baseline.crcOk||!baseline.rangeOk||!tactile::validateFrame(baseline)
        ||baseline.sessionId!=sample.frame.sessionId||baseline.generation!=sample.frame.generation
        ||baseline.hardwareType!=m_hardwareType))return;
    if(!complete&&m_autoZeroResult.phase!=tactile::AutoZeroPhase::Failed)return;
    m_initialResponseTimer.stop();m_waitingForFirstHardwareFrame=false;cancelSerialRetry();
    if(complete)applyDisplayZero(baseline);
    else {m_serialHistory.begin(m_serial.sessionId(),m_serial.generation());updateValues();}
    auto settings=tactilePreferences();settings.beginGroup("tactile/serial");
    settings.setValue("port",sample.settings.port);settings.setValue("baud",sample.settings.baud);
    settings.setValue("slave",sample.settings.slave);settings.setValue("type",sample.settings.type);settings.sync();
    m_defaultSerialPort=sample.settings.port;
}
void TactileVisualizationWidget::updateZeroStatus()
{
    QString text;
    if(m_finger==m_hardwareType) {
        if(m_waitingForFirstHardwareFrame)
            text=m_autoZeroResult.started
                ?tr("Auto zeroing... Keep the sensor unloaded."):tr("Waiting for sensor data...");
        else if(hasDisplayBaseline()&&m_autoZeroResult.phase!=tactile::AutoZeroPhase::Complete)
            text=tr("Delta zero set");
    }
    m_tareButton->setEnabled(!m_waitingForFirstHardwareFrame);
    m_zeroStatus->setVisible(!text.isEmpty());m_zeroStatus->setToolTip(text);
    m_zeroStatus->setFixedHeight(m_zeroStatus->fontMetrics().height()+4);
    m_zeroStatus->setText(m_zeroStatus->fontMetrics().elidedText(text,Qt::ElideRight,m_zeroStatus->width()));
}
void TactileVisualizationWidget::setDeviceConfigStatus(const QString &text)
{
    m_configStatus->setText(text);m_configStatus->setVisible(!text.isEmpty());
}
void TactileVisualizationWidget::deviceSettingsEdited()
{
    if(m_waitingForFirstHardwareFrame
        &&(m_slave->value()!=m_connectedSettings.slave||m_baud->currentText().toInt()!=m_connectedSettings.baud)){
        disconnectSerial();return;
    }
    updateSerialStatus();
    const auto invalidTarget=tr("New device IDs must be 1-127. Address 0 is only for connecting; it cannot be written back to the sensor.");
    if(m_serial.isRunning() && m_connectedSettings.slave!=0 && m_slave->value()==0)
        setDeviceConfigStatus(invalidTarget);
    else if(m_configStatus->text()==invalidTarget)
        setDeviceConfigStatus({});
    const auto generation=m_serial.generation();
    if(!m_writeAddress->isEnabled()||m_saveReminderGeneration==generation)return;
    m_saveReminderGeneration=generation;
    auto *notice=new QMessageBox(QMessageBox::Information,tr("Save and power cycle required"),
        tr("Click 'Write and save'. Once saving succeeds, power-cycle the sensor once by unplugging and reconnecting its power, then connect using the new settings."),QMessageBox::Ok,this);
    notice->setObjectName("tactileSaveReminder");notice->setAttribute(Qt::WA_DeleteOnClose);notice->open();
}
void TactileVisualizationWidget::writeDeviceAddress()
{
    const auto candidate=tactile::SaveCandidate::Decimal18013;
    const int target=m_slave->value()!=m_connectedSettings.slave?m_slave->value():0;
    const int baud=m_baud->currentText().toInt()!=m_connectedSettings.baud?m_baud->currentText().toInt():0;
    if(!m_serial.isRunning()||m_serial.stopping()||m_serial.configBusy()||m_configAwaitClose
        ||!m_serialRetryPort.isEmpty()||!m_serialSample.portOpen||(!target&&!baud))return;
    if(m_connectedSettings.slave!=0 && m_slave->value()==0){
        setDeviceConfigStatus(tr("New device IDs must be 1-127. Address 0 is only for connecting; it cannot be written back to the sensor."));return;
    }
    const auto addressChange=target?tr("Requested device ID: %1").arg(target)
        :tr("Keep the device ID read from the sensor; only the baud rate will be changed.");
    const auto question=tr("Connect only ONE sensor on %1.\nCurrent communication ID: %2\n%3\nDevice baud: %4 -> %5\nWrite the selected changes and save together? Acquisition will stop. After saving, disconnect sensor power and unplug/replug ONCE, then connect using the new settings.")
        .arg(m_connectedSettings.port).arg(m_connectedSettings.slave).arg(addressChange)
        .arg(m_connectedSettings.baud).arg(m_baud->currentText());
    if(QMessageBox::question(this,tr("Change sensor settings"),question,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    if(!m_serial.requestDeviceConfig(target,candidate,baud))return;
    m_configAwaitClose=false;m_configTimer.start(20);
    m_pendingPort=m_connectedSettings.port;m_verifyingAddress=false;setDeviceConfigStatus(tr("Writing selected parameters and checking readback; do not unplug yet..."));
    m_liveFrame={};m_heldTableFrame={};clearHardwareDisplay();m_serialHistory.clear();m_tareFrame={};updateValues();
}
void TactileVisualizationWidget::receiveDeviceConfig(const tactile::DeviceConfigResult &result)
{
    if(result.writeAttempted){
        m_pendingAddress=result.requestedAddress?result.requestedAddress:result.configuredAddress;
        m_pendingBaud=result.requestedBaud?result.requestedBaud:result.originalBaud;
        m_pendingPort=m_connectedSettings.port;m_verifyingAddress=false;
        // Public communication ID 0 need not be the device's stored ID. Reconnect
        // with the requested/read-back ID without triggering an edit reminder.
        if(m_pendingAddress>=0 && m_pendingAddress<=127){
            const QSignalBlocker blocker(m_slave);m_slave->setValue(m_pendingAddress);
        }
        m_liveFrame={};m_heldTableFrame={};clearHardwareDisplay();m_displayFilter.reset();m_serialHistory.clear();m_tareFrame={};
        m_serialSample.portOpen=false;m_serialSample.accepted=false;
        const bool saved=result.saveAcknowledged&&result.writeAcknowledged
            &&(!result.requestedAddress||result.addressVerified)&&(!result.requestedBaud||result.baudVerified);
        setDeviceConfigStatus(saved
            ?tr("Selected settings read back and save acknowledged: ID %1, baud %2. Power-cycle once, then click 'Connect sensor' using these new values.").arg(m_pendingAddress).arg(m_pendingBaud)
            :tr("Device state uncertain; one or both parameters may have changed. Persistence NOT verified. Do not repeat blindly. Power-cycle, then check ID %1 / baud %2; if unavailable, read using the previous settings. Details: %3").arg(m_pendingAddress).arg(m_pendingBaud).arg(result.error));
        if(!saved){
            auto *notice=new QMessageBox(QMessageBox::Warning,tr("Device configuration uncertain"),
                m_configStatus->text(),QMessageBox::Ok,this);
            notice->setObjectName("tactilePowerCycleNotice");notice->setAttribute(Qt::WA_DeleteOnClose);notice->open();
        }
    }else if(result.readOk && result.error.isEmpty()){
        setDeviceConfigStatus(tr("Device parameter ID %1; baud %2; responding ID %3. Parameter readback alone does not prove persistence.")
            .arg(result.configuredAddress).arg(result.configuredBaud).arg(result.respondingAddress));
    }else setDeviceConfigStatus(tr("Device configuration failed: %1").arg(result.error));
    updateSerialStatus();
}
void TactileVisualizationWidget::selectFrame()
{
    m_frame=m_finger==m_hardwareType?m_liveFrame:tactile::Frame{};
    QString label;
    if(m_finger==m_hardwareType&&(m_frame.valid||visualFrame().valid)){label="HARDWARE";
        if(!m_crc->isChecked())label+=" / CRC OFF";
        if(m_frame.valid&&!m_frame.crcOk)label+=" / CRC ERROR";
        if(m_frame.valid&&!m_frame.rangeOk)label+=" / RANGE WARNING";
        if(visualFrame().valid&&!m_displayFilter.reason.isEmpty())label+=" / DISPLAY HELD";
    }
    if(hasDisplayBaseline())label+=" / DELTA";
    m_adapter->setSourceLabel(label);
}
void TactileVisualizationWidget::updateSerialStatus()
{
    updateZeroStatus();
    const bool active=m_serial.isRunning();
    const bool stopping=m_serial.stopping();
    const bool busy=m_serial.configBusy();
    const bool changed=m_slave->value()!=m_connectedSettings.slave||m_baud->currentText().toInt()!=m_connectedSettings.baud;
    m_writeAddress->setEnabled(active&&!m_waitingForFirstHardwareFrame&&!stopping&&!busy&&!m_configAwaitClose&&m_serialRetryPort.isEmpty()&&m_serialSample.portOpen
        &&(m_connectedSettings.slave==0||m_slave->value()>0)&&changed);
    m_slave->setEnabled(!stopping&&!busy&&!m_configAwaitClose);m_baud->setEnabled(!stopping&&!busy&&!m_configAwaitClose);
    if(m_verifyingAddress&&m_serialSample.accepted&&m_serialSample.crcOk&&m_serialSample.settings.slave==m_pendingAddress
        &&m_serialSample.settings.baud==m_pendingBaud&&m_serialSample.settings.port==m_pendingPort){
        setDeviceConfigStatus(tr("ID %1 at baud %2 responds with CRC-valid sensor data. Communication with the new settings verified.").arg(m_pendingAddress).arg(m_pendingBaud));
        m_verifyingAddress=false;m_pendingAddress=0;m_pendingBaud=0;m_pendingPort.clear();
    }
    for(QWidget *w:QVector<QWidget*>{m_port,m_timeout,m_scanPorts})w->setEnabled(!active);
    m_interval->setEnabled(!stopping&&!busy&&!m_configAwaitClose);
    m_connect->setEnabled(!active);m_disconnect->setEnabled(!stopping&&(active||!m_serialRetryPort.isEmpty()));
    m_disconnect->setText(stopping?tr("Disconnecting..."):tr("Disconnect"));
    const auto sensor=QString("FGS").mid(m_finger,1);
    QString text=sensor;
    if(!m_frame.valid&&!visualFrame().valid)
        text=tr("%1: %2").arg(sensor,tr("No current hardware data"));
    if(m_hardwareType>=0)text+=tr(" | %1 ID %2 | RX accepted %3/%4 | CRC errors %5 | Timeouts %6 | Protocol errors %7")
        .arg(m_connectedSettings.port).arg(m_connectedSettings.slave).arg(m_serialSample.acceptedCount).arg(m_serialSample.requests).arg(m_serialSample.crcErrors).arg(m_serialSample.timeouts).arg(m_serialSample.protocolErrors);
    if(!m_crc->isChecked())text+=tr(" | CRC OFF: data unverified");
    if(!m_serialSample.error.isEmpty()&&m_serialSample.error!=QStringLiteral("CRC mismatch"))
        text+=" | "+m_serialSample.error;
    if(stopping)text=tr("Waiting for the serial port to close. If this persists, unplug/replug the USB device.");
    m_serialStatus->setVisible(m_statusToggle->isChecked());
    // Keep exactly one line even when errors appear or counters gain digits.
    m_serialStatus->setFixedHeight(m_serialStatus->fontMetrics().height()+4);
    m_serialStatus->setToolTip(text);
    m_serialStatus->setText(m_serialStatus->fontMetrics().elidedText(text,Qt::ElideRight,m_serialStatus->width()));
}
