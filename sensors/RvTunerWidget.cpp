#include "RvTunerWidget.h"
#include "../UiStyle.h"
#include "../tools/rv_photon/Wire.h"
#include <QtCore/QEvent>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSplitterHandle>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>
#include <cmath>

class RvTunerCanvas final:public QWidget {
public:
    explicit RvTunerCanvas(QWidget *parent):QWidget(parent){setMouseTracking(true);setMinimumSize(150,190);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);}
    QImage image;QString title,empty,scale;QRectF target;QSize native;bool aligned=false;int rotation=0,highlight=-1;
    std::function<void(int)> hover;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.fillRect(rect(),QColor("#11161d"));p.setPen(QColor("#dce3ed"));p.drawText(QRect(8,4,width()-16,24),Qt::AlignCenter,title);
        if(image.isNull()){p.drawText(rect().adjusted(12,32,-12,-35),Qt::AlignCenter|Qt::TextWordWrap,empty);return;}
        const QRectF available=QRectF(rect()).adjusted(10,34,-10,-46);QSizeF size=image.size();size.scale(available.size(),Qt::KeepAspectRatio);target=QRectF(available.center()-QPointF(size.width()/2,size.height()/2),size);
        p.drawImage(target,image);
        if(highlight>=0){for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(RvTuner::displayIndex(x,y,native,aligned,rotation)==highlight){p.setPen(QPen(Qt::magenta,2));p.drawRect(QRectF(target.x()+x*target.width()/image.width(),target.y()+y*target.height()/image.height(),target.width()/image.width(),target.height()/image.height()));}}
        QLinearGradient gradient(10,0,width()-10,0);gradient.setColorAt(0,QColor::fromHsvF(2./3,1,1));gradient.setColorAt(.5,Qt::green);gradient.setColorAt(1,Qt::red);p.fillRect(QRect(10,height()-37,width()-20,8),gradient);
        p.setPen(QColor("#dce3ed"));p.drawText(QRect(6,height()-27,width()-12,24),Qt::AlignCenter,scale);
    }
    void mouseMoveEvent(QMouseEvent *e) override {int index=-1;if(!image.isNull()&&target.contains(e->position()))index=RvTuner::displayIndex(int((e->position().x()-target.x())*image.width()/target.width()),int((e->position().y()-target.y())*image.height()/target.height()),native,aligned,rotation);if(hover)hover(index);}
    void leaveEvent(QEvent *) override {if(hover)hover(-1);}
};
RvTunerWidget::RvTunerWidget(RvTunerController *controller,QWidget *parent):QWidget(parent),m_controller(controller){
    setObjectName("rvTunerWidget");auto *root=new QVBoxLayout(this);root->setContentsMargins(6,6,6,6);
    m_splitter=new QSplitter(Qt::Vertical,this);m_splitter->setObjectName("tunerVerticalSplitter");m_splitter->setChildrenCollapsible(false);m_splitter->setHandleWidth(8);root->addWidget(m_splitter);
    m_splitter->setStyleSheet("QSplitter#tunerVerticalSplitter::handle:vertical { background: palette(midlight); margin: 1px 0; } QSplitter#tunerVerticalSplitter::handle:vertical:hover { background: palette(highlight); }");
    auto *scroll=new QScrollArea(m_splitter);scroll->setObjectName("tunerParametersScroll");scroll->setWidgetResizable(true);scroll->setMinimumHeight(100);
    auto *parameters=new QWidget(scroll);UiStyle::card(parameters);auto *grid=new QGridLayout(parameters);grid->setContentsMargins(12,12,12,12);grid->setHorizontalSpacing(12);grid->setVerticalSpacing(8);scroll->setWidget(parameters);m_splitter->addWidget(scroll);
    // Both panes can shrink independently. Scroll the measurements at compact
    // heights instead of letting their content minimum pin the divider in place.
    auto *measurements=new QScrollArea(m_splitter);measurements->setObjectName("tunerMeasurementsScroll");measurements->setWidgetResizable(true);measurements->setFrameShape(QFrame::NoFrame);measurements->setMinimumHeight(180);
    auto *body=new QWidget;body->setObjectName("tunerMeasurements");auto *outer=new QVBoxLayout(body);outer->setContentsMargins(0,0,0,0);outer->setSpacing(5);measurements->setWidget(body);m_splitter->addWidget(measurements);
    m_splitter->setStretchFactor(0,1);m_splitter->setStretchFactor(1,2);m_splitter->setSizes({280,560});m_splitter->handle(1)->setCursor(Qt::SplitVCursor);
    const auto combo=[&](const char *name){auto *w=new QComboBox(this);w->setObjectName(name);return w;};
    const auto spin=[&](const char *name,int lo,int hi,int value){auto *w=new QSpinBox(this);w->setObjectName(name);w->setRange(lo,hi);w->setValue(value);return w;};
    m_resolution=combo("tunerResolution");m_resolution->addItems({"8 × 8","16 × 16","32 × 32","48 × 32"});m_resolution->setCurrentIndex(3);m_mode=combo("tunerMode");
    m_iterations=spin("tunerIterations",100,10000,1200);m_short=spin("tunerShortIterations",1,10000,100);m_snr=spin("tunerSnr",4,255,4);m_period=spin("tunerPeriod",20,2000,66);m_objects=spin("tunerObjects",1,4,1);
    m_signal=new QCheckBox(this);m_noise=new QCheckBox(this);m_signal->setObjectName("tunerSignal");m_noise->setObjectName("tunerNoise");
    const QList<QPair<QString,QWidget*>> fields={{"Resolution",m_resolution},{"Mode",m_mode},{"Iterations (k)",m_iterations},{"SNR threshold",m_snr},{"Period (ms)",m_period},{"Dual near iterations (k)",m_short},{"Targets per zone",m_objects}};
    for(int i=0;i<fields.size();++i){auto *label=new QLabel(this);m_labels[fields[i].first]=label;grid->addWidget(label,i/2,(i%2)*2);grid->addWidget(fields[i].second,i/2,(i%2)*2+1);}
    grid->addWidget(m_signal,3,2);grid->addWidget(m_noise,3,3);
    auto *operations=new QHBoxLayout;grid->addLayout(operations,4,0,1,4);
    const auto button=[&](const QString &key,QHBoxLayout *row){auto *b=new QPushButton(this);b->setObjectName("tuner"+key);m_buttons[key]=b;row->addWidget(b);return b;};
    connect(button("Read",operations),&QPushButton::clicked,controller,&RvTunerController::read);
    connect(button("Start",operations),&QPushButton::clicked,this,[this]{try{m_controller->start(settings());}catch(const std::exception &e){m_status->setText(QString::fromUtf8(e.what()));}});
    connect(button("Stop",operations),&QPushButton::clicked,controller,&RvTunerController::stop);
    connect(button("Apply",operations),&QPushButton::clicked,this,[this]{try{m_controller->apply(settings());}catch(const std::exception &e){m_status->setText(QString::fromUtf8(e.what()));}});
    connect(button("Persist",operations),&QPushButton::clicked,controller,&RvTunerController::persist);
    auto *presets=new QHBoxLayout;grid->addLayout(presets,5,0,1,4);
    connect(button("SavePreset",presets),&QPushButton::clicked,this,&RvTunerWidget::savePreset);connect(button("LoadPreset",presets),&QPushButton::clicked,this,&RvTunerWidget::loadPreset);
    connect(button("SignalPreview",presets),&QPushButton::clicked,this,[this]{try{auto s=m_controller->appliedSettings().isEmpty()?settings():RvTuner::Settings::fromJson(m_controller->appliedSettings());s.signal=true;s.validate();if(m_controller->exclusive())m_controller->apply(s);else m_controller->start(s);}catch(const std::exception &e){m_status->setText(QString::fromUtf8(e.what()));}});
    m_pending=new QLabel(this);m_applied=new QLabel(this);m_saved=new QLabel(this);for(auto *label:{m_pending,m_applied,m_saved}){label->setWordWrap(true);label->setTextInteractionFlags(Qt::TextSelectableByMouse);}
    grid->addWidget(m_pending,6,0,1,4);grid->addWidget(m_applied,7,0,1,4);grid->addWidget(m_saved,8,0,1,4);
    m_status=new QLabel(this);m_status->setObjectName("tunerStatus");m_status->setWordWrap(true);m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);outer->addWidget(m_status);
    auto *display=new QHBoxLayout;outer->addLayout(display);
    m_target=combo("tunerTarget");m_target->addItem("1",1);m_direction=combo("tunerDirection");m_direction->addItems({QString(),QString()});m_rotation=combo("tunerRotation");m_rotation->addItems({"0°","180°"});
    m_distanceMax=new QDoubleSpinBox(this);m_distanceMax->setObjectName("tunerDistanceMaximum");m_distanceMax->setRange(1,16383.75);m_distanceMax->setValue(1400);UiStyle::setUnit(m_distanceMax,"mm");
    m_signalMax=spin("tunerSignalMaximum",1,8388352,32767);UiStyle::setUnit(m_signalMax,"counts");m_log=new QCheckBox(this);m_log->setChecked(true);
    for(auto *w:QList<QWidget*>{m_target,m_direction,m_rotation,UiStyle::numberField(m_distanceMax),UiStyle::numberField(m_signalMax),m_log})display->addWidget(w);
    auto *plots=new QHBoxLayout;m_distance=new RvTunerCanvas(this);m_distance->setObjectName("tunerDistanceCanvas");m_photon=new RvTunerCanvas(this);m_photon->setObjectName("tunerSignalCanvas");plots->addWidget(m_distance);plots->addWidget(m_photon);outer->addLayout(plots,1);
    m_stats=new QLabel(this);m_stats->setWordWrap(true);m_stats->setObjectName("tunerStatistics");outer->addWidget(m_stats);
    m_hover=new QLabel(this);m_hover->setWordWrap(true);m_hover->setMinimumHeight(36);m_hover->setObjectName("tunerHover");outer->addWidget(m_hover);
    auto hover=[this](int index){m_distance->highlight=m_photon->highlight=index;m_distance->update();m_photon->update();m_hover->clear();if(index<0||m_frame.samples.isEmpty())return;const auto &s=m_frame.sample(m_target->currentData().toInt(),index);const auto dim=m_frame.settings.size();m_hover->setText(tr("ID %1 | row %2, column %3 | target %4 | measurement %5\nRange %6 mm (radial) | SNR %7 | slot %8 | Signal %9 | Noise encoded %10")
        .arg(index).arg(index/dim.width()).arg(index%dim.width()).arg(m_target->currentData().toInt()).arg(m_frame.sequence).arg(s.range*.25,0,'f',2).arg(s.snr).arg(s.valid)
        .arg(m_frame.settings.signal?tr("%1 counts, encoded 0x%2").arg(rv_photon_count(s.signal)).arg(s.signal,4,16,QChar('0')):tr("not enabled"))
        .arg(m_frame.settings.noise?QString::number(s.noise):tr("not enabled")));};m_distance->hover=m_photon->hover=hover;
    auto *recordRow=new QHBoxLayout;outer->addLayout(recordRow);connect(button("Record",recordRow),&QPushButton::clicked,this,&RvTunerWidget::record);connect(button("Export",recordRow),&QPushButton::clicked,this,&RvTunerWidget::exportComparisons);recordRow->addStretch();
    m_results=new QTableWidget(0,6,this);m_results->setObjectName("tunerComparisons");m_results->setMaximumHeight(150);m_results->setMinimumHeight(60);m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);outer->addWidget(m_results);
    connect(m_resolution,&QComboBox::currentIndexChanged,this,[this]{modes();refresh();});
    for(auto *w:{m_mode,m_target,m_direction,m_rotation})connect(w,&QComboBox::currentIndexChanged,this,[this]{refresh();render();});
    for(auto *w:{m_iterations,m_short,m_snr,m_period,m_objects,m_signalMax})connect(w,&QSpinBox::valueChanged,this,[this]{refresh();render();});
    for(auto *w:{m_signal,m_noise,m_log})connect(w,&QCheckBox::toggled,this,[this]{refresh();render();});
    connect(m_distanceMax,&QDoubleSpinBox::valueChanged,this,[this]{render();});
    connect(controller,&RvTunerController::configurationRead,this,[this](const QJsonObject &s){if(!m_loaded){setControls(RvTuner::Settings::fromJson(s));m_loaded=true;}refresh();});
    connect(controller,&RvTunerController::changed,this,&RvTunerWidget::refresh);connect(controller,&RvTunerController::frameReady,this,&RvTunerWidget::acceptFrame);
    connect(controller,&RvTunerController::cleared,this,[this](const QString &why){m_frame={};m_distance->empty=m_photon->empty=why;m_hover->clear();render();refresh();});
    modes();retranslateUi();refresh();render();
}
void RvTunerWidget::modes(){
    const QSignalBlocker block(m_mode);const int old=m_mode->currentIndex();m_mode->clear();const int group=m_resolution->currentIndex();
    if(group==0){m_mode->addItem(tr("Ordinary"),0);m_mode->addItem(tr("Long range"),1);m_mode->addItem(tr("High accuracy"),2);m_mode->addItem(tr("Dual"),3);m_mode->addItem(tr("Dual long range"),4);}
    else{const int base=group==1?5:group==2?8:11;m_mode->addItem(tr("Ordinary"),base);m_mode->addItem(tr("High accuracy"),base+1);m_mode->addItem(tr("Dual"),base+2);}
    m_mode->setCurrentIndex(std::clamp(old,0,int(m_mode->count()-1)));
}
RvTuner::Settings RvTunerWidget::settings() const {RvTuner::Settings s;s.mode=m_mode->currentData().toInt();s.iterations=m_iterations->value();s.shortIterations=m_short->value();s.snr=m_snr->value();s.periodMs=m_period->value();s.objects=m_objects->value();s.signal=m_signal->isChecked();s.noise=m_noise->isChecked();s.validate();return s;}
void RvTunerWidget::setControls(const RvTuner::Settings &s){m_updating=true;m_resolution->setCurrentIndex(s.mode<=4?0:s.mode<=7?1:s.mode<=10?2:3);modes();m_mode->setCurrentIndex(m_mode->findData(s.mode));m_iterations->setValue(s.iterations);m_short->setValue(s.shortIterations);m_snr->setValue(s.snr);m_period->setValue(s.periodMs);m_objects->setValue(s.objects);m_signal->setChecked(s.signal);m_noise->setChecked(s.noise);m_updating=false;refresh();}
QString RvTunerWidget::describe(const QJsonObject &j) const {if(j.isEmpty())return tr("not read / not applied");const auto s=RvTuner::Settings::fromJson(j);const auto size=s.size();return tr("%1×%2 / mode %3 / SNR %4 / %5k / %6ms / near %7k / %8 target(s) / Signal %9 / Noise %10").arg(size.width()).arg(size.height()).arg(s.mode).arg(s.snr).arg(s.iterations).arg(s.periodMs).arg(s.shortIterations).arg(s.objects).arg(s.signal?tr("on"):tr("off")).arg(s.noise?tr("on"):tr("off"));}
void RvTunerWidget::refresh(){
    if(m_updating)return;const bool pending=m_controller->pending(),running=m_controller->state()==RvTunerController::Running,exclusive=m_controller->exclusive();
    bool valid=true;QJsonObject draft;QString error;try{const auto s=settings();draft=s.json();m_short->setEnabled(s.dual());}catch(const std::exception &e){valid=false;error=QString::fromUtf8(e.what());}
    m_pending->setText(tr("Pending: %1").arg(m_loaded?(valid?describe(draft):error):tr("Read the current device configuration first.")));
    m_applied->setText(tr("Experiment: %1").arg(describe(m_controller->appliedSettings())));m_saved->setText(tr("Long-term saved: %1").arg(describe(m_controller->savedSettings())));
    m_status->setText(m_controller->detail().isEmpty()?tr("Read the current device configuration first. Opening or restoring this dock does not start acquisition."):m_controller->detail());
    m_buttons["Read"]->setEnabled(!pending&&!exclusive);m_buttons["Start"]->setEnabled(m_loaded&&valid&&!pending&&!exclusive);
    m_buttons["Stop"]->setEnabled(exclusive||pending);m_buttons["Apply"]->setEnabled(valid&&running&&!pending);
    const auto applied=m_controller->appliedSettings();m_buttons["Persist"]->setEnabled(valid&&running&&!pending&&!applied.isEmpty()&&draft==applied);
    m_buttons["Persist"]->setToolTip(tr("All supported resolutions can be saved permanently. Apply pending edits first. Both native service and host capture command lines are backed up and verified. Host-assisted imaging uses a separate 48×32 session preset."));
    m_buttons["SignalPreview"]->setEnabled(m_loaded&&!pending&&(!exclusive||running));m_buttons["Record"]->setEnabled(!m_frame.samples.isEmpty());m_buttons["Export"]->setEnabled(!m_records.isEmpty());
}
void RvTunerWidget::acceptFrame(const RvTuner::Frame &frame){
    m_frame=frame;const int selected=m_target->currentData().toInt();if(m_target->count()!=frame.settings.objects){const QSignalBlocker block(m_target);m_target->clear();for(int i=1;i<=frame.settings.objects;++i)m_target->addItem(tr("Target %1").arg(i),i);m_target->setCurrentIndex(std::clamp(selected-1,0,frame.settings.objects-1));}
    render();refresh();
}
void RvTunerWidget::render(){
    if(!m_distance)return;for(auto *canvas:{m_distance,m_photon}){canvas->image={};canvas->target={};canvas->setProperty("measurement",QVariant());canvas->setProperty("target",QVariant());}
    m_distance->setProperty("hasFrame",!m_frame.samples.isEmpty());m_photon->setProperty("hasFrame",!m_frame.samples.isEmpty()&&m_frame.settings.signal);
    if(m_frame.samples.isEmpty()){m_stats->clear();m_distance->update();m_photon->update();return;}
    const int peak=m_target->currentData().toInt();if(peak<1||peak>m_frame.settings.objects)return;
    const auto dim=m_frame.settings.size();const bool aligned=m_direction->currentIndex()==1;const int rotation=m_rotation->currentIndex()*180;
    const QSize display=aligned?QSize(dim.height(),dim.width()):dim;
    for(auto *canvas:{m_distance,m_photon}){canvas->native=dim;canvas->aligned=aligned;canvas->rotation=rotation;canvas->setProperty("measurement",m_frame.sequence);canvas->setProperty("target",peak);}
    m_distance->image=QImage(display,QImage::Format_RGB32);if(m_frame.settings.signal)m_photon->image=QImage(display,QImage::Format_RGB32);
    const auto color=[](double n){return QColor::fromHsvF((1-std::clamp(n,0.,1.))*2/3,1,1).rgb();};
    for(int y=0;y<display.height();++y)for(int x=0;x<display.width();++x){const auto &s=m_frame.sample(peak,RvTuner::displayIndex(x,y,dim,aligned,rotation));m_distance->image.setPixel(x,y,s.valid&&s.range&&s.snr>=m_frame.settings.snr?color(s.range*.25/m_distanceMax->value()):qRgb(0,0,0));
        if(m_frame.settings.signal){const double count=std::min(double(rv_photon_count(s.signal)),double(m_signalMax->value()));m_photon->image.setPixel(x,y,s.valid?color(m_log->isChecked()?std::log1p(count)/std::log1p(m_signalMax->value()):count/m_signalMax->value()):qRgb(0,0,0));}}
    m_distance->scale=tr("0 – %1 mm (radial)").arg(m_distanceMax->value());m_photon->scale=tr("0 – %1 counts (%2)").arg(m_signalMax->value()).arg(m_log->isChecked()?tr("log"):tr("linear"));
    if(!m_frame.settings.signal)m_photon->empty=tr("Signal not enabled / no data. Enable Signal preview to compare both plots. Preview does not save permanent settings.");
    const auto stats=m_frame.statistics(peak);m_stats->setText(tr("Valid %1/%2 (%3%) | SNR min/mean/max %4/%5/%6 | received %7 FPS | %8 °C | warnings 0x%9 | dropped %10")
        .arg(stats["valid"].toInt()).arg(stats["total"].toInt()).arg(stats["coverage"].toDouble()*100,0,'f',1).arg(stats["snr_min"].toInt()).arg(stats["snr_mean"].toDouble(),0,'f',1).arg(stats["snr_max"].toInt()).arg(m_controller->fps(),0,'f',2).arg(m_frame.temperature).arg(m_frame.warnings,2,16,QChar('0')).arg(m_frame.dropped));
    m_distance->update();m_photon->update();
}
void RvTunerWidget::record(){if(m_frame.samples.isEmpty())return;if(m_records.size()>=200){m_records.removeFirst();m_results->removeRow(0);}auto record=m_frame.statistics(m_target->currentData().toInt());record["settings"]=m_frame.settings.json();record["received_fps"]=m_controller->fps();record["time_utc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODate);record["token"]=m_frame.token;record["source"]="DTN1";m_records.append(record);const int row=m_results->rowCount();m_results->insertRow(row);const QStringList values{describe(m_frame.settings.json()),QString::number(record["target"].toInt()),QString::number(record["coverage"].toDouble()*100,'f',1),QString::number(record["snr_mean"].toDouble(),'f',1),QString::number(m_controller->fps(),'f',2),QString::number(m_frame.sequence)};for(int i=0;i<values.size();++i)m_results->setItem(row,i,new QTableWidgetItem(values[i]));refresh();}
namespace {void writeJson(const QString &path,const QJsonDocument &doc){QSaveFile file(path);const auto bytes=doc.toJson();if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())throw std::runtime_error("Atomic file save failed");}}
void RvTunerWidget::savePreset(){try{const auto s=settings();const auto path=QFileDialog::getSaveFileName(this,tr("Save experiment preset"),{},tr("JSON files (*.json)"));if(!path.isEmpty())writeJson(path,QJsonDocument(QJsonObject{{"format","dtof-experiment-v1"},{"settings",s.json()}}));}catch(const std::exception &e){m_status->setText(QString::fromUtf8(e.what()));}}
void RvTunerWidget::loadPreset(){const auto path=QFileDialog::getOpenFileName(this,tr("Load experiment preset"),{},tr("JSON files (*.json)"));if(path.isEmpty())return;try{QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>16384)throw std::runtime_error("Cannot read experiment preset");const auto j=QJsonDocument::fromJson(f.readAll()).object();if(j["format"]!="dtof-experiment-v1")throw std::runtime_error("Unknown preset format");setControls(RvTuner::Settings::fromJson(j["settings"].toObject()));}catch(const std::exception &e){m_status->setText(QString::fromUtf8(e.what()));}}
void RvTunerWidget::exportComparisons(){const auto path=QFileDialog::getSaveFileName(this,tr("Export comparison results"),{},tr("JSON files (*.json)"));if(path.isEmpty())return;try{writeJson(path,QJsonDocument(QJsonObject{{"format","dtof-comparison-v1"},{"records",m_records}}));}catch(const std::exception &e){m_status->setText(QString::fromUtf8(e.what()));}}
void RvTunerWidget::changeEvent(QEvent *e){QWidget::changeEvent(e);if(e->type()==QEvent::LanguageChange)retranslateUi();}
void RvTunerWidget::retranslateUi(){
    const QMap<QString,QString> buttons={{"Read",tr("Read current")},{"Start",tr("Start")},{"Stop",tr("Stop")},{"Apply",tr("Apply experiment")},{"Persist",tr("Long-term save")},{"SavePreset",tr("Save preset")},{"LoadPreset",tr("Load preset")},{"SignalPreview",tr("Enable Signal preview")},{"Record",tr("Record comparison")},{"Export",tr("Export results")}};
    for(auto it=buttons.begin();it!=buttons.end();++it)m_buttons[it.key()]->setText(it.value());
    const QStringList texts={tr("Resolution"),tr("Mode"),tr("Iterations (k)"),tr("SNR threshold"),tr("Period (ms)"),tr("Dual near iterations (k)"),tr("Targets per zone")};
    const QStringList keys={"Resolution","Mode","Iterations (k)","SNR threshold","Period (ms)","Dual near iterations (k)","Targets per zone"};for(int i=0;i<keys.size();++i)m_labels[keys[i]]->setText(texts[i]);
    m_signal->setText(tr("Signal output"));m_noise->setText(tr("Noise output"));m_log->setText(tr("Log"));m_direction->setItemText(0,tr("Native grid"));m_direction->setItemText(1,tr("Aligned display"));
    m_target->setToolTip(tr("Both plots use the same complete measurement and target."));m_distanceMax->setToolTip(tr("Distance display maximum; does not change measurements."));m_signalMax->setToolTip(tr("Signal display maximum in decoded counts. Encoded values are shown on hover."));
    m_distance->title=tr("Distance (radial)");m_photon->title=tr("Photon intensity (Signal counts)");
    m_distance->empty=m_photon->empty=tr("No measurement. Read configuration, then click Start.");
    m_results->setHorizontalHeaderLabels({tr("Parameters"),tr("Target"),tr("Valid %"),tr("Mean SNR"),tr("Received FPS"),tr("Measurement")});modes();refresh();render();
    for(int i=0;i<m_target->count();++i)m_target->setItemText(i,tr("Target %1").arg(i+1));
    for(int i=0;i<m_records.size();++i)if(m_results->item(i,0))m_results->item(i,0)->setText(describe(m_records[i].toObject()["settings"].toObject()));
}
