#include "DepthMapWidget.h"
#include "RvCameraView.h"
#include "../UiStyle.h"
#include "RvDtofCandidates.h"
#include "RvDepthView.h"
#include "ImagePresentation.h"
#include "RvSessionController.h"
#include "RvAssistDepth.h"

#include <QtCore/QSettings>
#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProgressBar>
#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <exception>

namespace
{
class ColorRangeInput final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

    QValidator::State validate(QString &text, int &position) const override
    {
        static const QRegularExpression decimal(QStringLiteral("\\A[0-9]*\\.?[0-9]*\\z"));
        return decimal.match(text).hasMatch() ? QDoubleSpinBox::validate(text, position) : QValidator::Invalid;
    }
};

class DepthViewTabs final : public QTabWidget {
public:
    using QTabWidget::QTabWidget;
    QSize sizeHint() const override { return minimumSizeHint(); }
    QSize minimumSizeHint() const override
    {
        return {ImagePresentation::MinimumViewportWidth, ImagePresentation::MinimumViewportHeight};
    }
};

}

DepthMapWidget::DepthMapWidget(QWidget *parent)
    : QWidget(parent)

{
    setObjectName(QStringLiteral("depthMapWidget"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ImagePresentation::Margin, ImagePresentation::Margin,
                               ImagePresentation::Margin, ImagePresentation::Margin);
    layout->setSpacing(ImagePresentation::Spacing);
    auto *settingsRow = new QWidget(this);
    settingsRow->setObjectName("imageSettingsRow");
    auto *settingsLayout = new QVBoxLayout(settingsRow);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(ImagePresentation::Spacing);
    m_rvControls = new QWidget(settingsRow);
    auto *rvLayout = UiStyle::wrappingRow(m_rvControls, ImagePresentation::Spacing);
    m_rvModelCaption = new QLabel(m_rvControls);
    m_rvModelCaption->setObjectName("rvDepthModelLabel");
    m_rvModel = new QComboBox(m_rvControls);
    m_rvModel->setObjectName("rvDepthModel");
    for (const char *model : {"board_litesr_near_384x256_host_contour_hd"})
        m_rvModel->addItem(QString::fromLatin1(model), QString::fromLatin1(model));
    if(RvAssistDepth::supportsHostModel(QLatin1String(RvCameraView::UniDepthVgaNative16))){
        const auto scheme=QStringLiteral("host_")+QLatin1String(RvCameraView::UniDepthVgaNative16);
        m_rvModel->addItem(scheme,scheme);
    }
    m_rvModel->addItem(QStringLiteral("Tiny NV12 128x80"),QString::fromLatin1(TinyNv12Contract::Scheme));
    m_rvModel->setMinimumWidth(0);
    m_rvModel->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_rvModel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    rvLayout->addWidget(m_rvModelCaption);
    rvLayout->addWidget(m_rvModel);
    m_rvStart = new QPushButton(m_rvControls); m_rvStart->setObjectName("rvDepthStart");
    m_rvStop = new QPushButton(m_rvControls); m_rvStop->setObjectName("rvDepthStop");
    m_rvReconnect = new QPushButton(m_rvControls); m_rvReconnect->setObjectName("rvDepthReconnect");
    for (auto *button : {m_rvStart, m_rvStop, m_rvReconnect}) rvLayout->addWidget(button);
    settingsLayout->addWidget(m_rvControls, 1);
    connect(m_rvModel, &QComboBox::activated, this, [this](int) {
        m_sessionControlsKey.clear();
        if(m_session)m_session->selectImagingScheme(m_rvModel->currentData().toString(),m_session->dtofCandidateSettings());
        syncSessionControls();
    });
    layout->addWidget(settingsRow);
    // The hidden tab bar does not need space, but the scroll area must reserve
    // the viewport height alongside the wrapped controls.
    m_depthTabs=new DepthViewTabs(this);m_depthTabs->setObjectName("rvDepthTabs");
    m_depthTabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border: 0; margin: 0; padding: 0; }"));
    m_rvCanvas=new RvDepthView(m_depthTabs);
    m_assistCanvas=new RvDepthView(m_depthTabs);m_assistCanvas->setObjectName("rvAssistDepthCanvas");
    m_depthTabs->addTab(m_rvCanvas,QString());m_depthTabs->addTab(m_assistCanvas,QString());
    m_candidateCanvas=new RvDepthView(m_depthTabs);
    m_candidateCanvas->setObjectName("rvDtofFastDepthCanvas");m_candidateCanvas->setRadialMode();m_candidateCanvas->setNativeGridSize(QSize(48,32));
    m_depthTabs->addTab(m_candidateCanvas,QString());
    setProperty("dtofComputeLocation","rv1103");
    m_depthTabs->tabBar()->hide();layout->addWidget(m_depthTabs,1);
    m_switchOverlay=new QWidget(m_depthTabs);m_switchOverlay->setObjectName("rvImagingSwitchOverlay");
    m_switchOverlay->setAutoFillBackground(true);
    auto switchPalette=m_switchOverlay->palette();switchPalette.setColor(QPalette::Window,QColor("#11161d"));
    switchPalette.setColor(QPalette::WindowText,QColor("#c9d1d9"));m_switchOverlay->setPalette(switchPalette);
    auto *switchLayout=new QVBoxLayout(m_switchOverlay);switchLayout->setContentsMargins(24,24,24,24);
    switchLayout->addStretch();
    m_switchTitle=new QLabel(m_switchOverlay);m_switchTitle->setAlignment(Qt::AlignCenter);m_switchTitle->setWordWrap(true);
    m_switchTitle->setStyleSheet("color: #dce6f0; background: transparent; font-weight: 600;");
    switchLayout->addWidget(m_switchTitle);
    auto *progressRow=new QHBoxLayout;progressRow->addStretch(1);
    m_switchProgress=new QProgressBar(m_switchOverlay);m_switchProgress->setObjectName("rvImagingSwitchProgress");
    m_switchProgress->setRange(0,1000);m_switchProgress->setValue(0);m_switchProgress->setTextVisible(true);
    m_switchProgress->setFormat("%p%");m_switchProgress->setMaximumWidth(400);
    m_switchProgress->setFixedHeight(22);
    m_switchProgress->setStyleSheet("QProgressBar { border: 1px solid #344355; border-radius: 5px; background: #202936; color: #ffffff; text-align: center; } QProgressBar::chunk { background: #176bb8; border-radius: 4px; }");
    progressRow->addWidget(m_switchProgress,6);progressRow->addStretch(1);switchLayout->addLayout(progressRow);
    m_switchDetail=new QLabel(m_switchOverlay);m_switchDetail->setAlignment(Qt::AlignCenter);m_switchDetail->setWordWrap(true);
    m_switchDetail->setStyleSheet("color: #a9b8c8; background: transparent;");
    switchLayout->addWidget(m_switchDetail);switchLayout->addStretch();
    m_switchOverlay->hide();m_depthTabs->installEventFilter(this);
    m_switchUiTimer=new QTimer(this);m_switchUiTimer->setInterval(33);
    connect(m_switchUiTimer,&QTimer::timeout,this,&DepthMapWidget::advanceImagingSwitchUi);
    connect(m_depthTabs,&QTabWidget::currentChanged,this,[this]{updateRvPresentation();if(m_switchUiActive)m_switchOverlay->raise();});
    m_imageSettings=ImageDisplaySettings::shared();
    connect(m_imageSettings.data(),&ImageDisplaySettings::colorRangeChanged,this,&DepthMapWidget::syncDisplayColors);
    connect(m_imageSettings.data(),&ImageDisplaySettings::blackOutsideRangeChanged,this,&DepthMapWidget::syncDisplayColors);
    auto *statusRow = new QWidget(this);
    statusRow->setObjectName("imageStatusRow");
    auto *saveControls = UiStyle::wrappingRow(statusRow, ImagePresentation::Spacing);
    m_rvCanvas->setDisplayRotationDegrees(displayRotationDegrees());
    m_assistCanvas->setDisplayRotationDegrees(displayRotationDegrees());
    m_candidateCanvas->setDisplayRotationDegrees(displayRotationDegrees());
    connect(m_imageSettings.data(), &ImageDisplaySettings::rotationChanged, this, [this](int degrees) {
        m_rvCanvas->setDisplayRotationDegrees(degrees);
        m_assistCanvas->setDisplayRotationDegrees(degrees);
        m_candidateCanvas->setDisplayRotationDegrees(degrees);
    });
    m_saveButton = new QPushButton(this);m_saveButton->setObjectName("depthMapSave");
    m_saveCamera = new QCheckBox(this);m_saveCamera->setObjectName("depthMapSaveCamera");
    m_saveCamera->setChecked(QSettings().value("depth/saveCameraImage",false).toBool());
    connect(m_saveCamera,&QCheckBox::toggled,this,[](bool checked){QSettings().setValue("depth/saveCameraImage",checked);});
    m_saveButton->setEnabled(false);
    auto *fpsLabel=new QLabel(QStringLiteral("FPS: 0.0"),statusRow);
    fpsLabel->setObjectName("depthFpsLabel");fpsLabel->setMinimumWidth(75);
    saveControls->addWidget(fpsLabel);
    saveControls->addWidget(m_saveCamera);
    saveControls->addWidget(m_saveButton);
    m_blackOutsideRange = new QCheckBox(statusRow);
    m_blackOutsideRange->setObjectName("depthBlackOutsideRange");
    m_blackOutsideRange->setChecked(m_imageSettings->blackOutsideRange());
    saveControls->addWidget(m_blackOutsideRange);
    connect(m_blackOutsideRange, &QCheckBox::toggled, m_imageSettings.data(), &ImageDisplaySettings::setBlackOutsideRange);
    connect(m_imageSettings.data(), &ImageDisplaySettings::blackOutsideRangeChanged, m_blackOutsideRange, &QCheckBox::setChecked);

    auto *minimum = new ColorRangeInput(statusRow);
    auto *maximum = new ColorRangeInput(statusRow);
    minimum->setObjectName("rvColorMinimum");
    maximum->setObjectName("rvColorMaximum");
    auto locale = QLocale::c();
    locale.setNumberOptions(QLocale::RejectGroupSeparator | QLocale::OmitGroupSeparator);
    for (auto *field : {minimum, maximum}) {
        field->setLocale(locale);
        field->setDecimals(3);
        field->setRange(ImageDisplaySettings::MinimumColorRangeM, ImageDisplaySettings::MaximumColorRangeM);
        field->setSingleStep(.1);
        field->setButtonSymbols(QAbstractSpinBox::NoButtons);
        field->setMinimumWidth(67);
    }
    minimum->setValue(m_imageSettings->colorMinimum());
    maximum->setValue(m_imageSettings->colorMaximum());
    m_colorMinimumLabel = new QLabel(statusRow);
    m_colorMaximumLabel = new QLabel(statusRow);
    m_colorMinimumLabel->setBuddy(minimum);
    m_colorMaximumLabel->setBuddy(maximum);
    m_applyColorRange = new QPushButton(statusRow);
    m_applyColorRange->setObjectName("rvColorRangeApply");
    for (const auto &[label, field] : {qMakePair(m_colorMinimumLabel, minimum), qMakePair(m_colorMaximumLabel, maximum)}) {
        auto *range = new QWidget(statusRow);
        auto *pair = new QHBoxLayout(range);
        pair->setContentsMargins(0, 0, 0, 0);
        pair->setSpacing(ImagePresentation::Spacing);
        pair->addWidget(label);
        pair->addWidget(field);
        saveControls->addWidget(range);
    }
    saveControls->addWidget(m_applyColorRange);
    const auto validRange = [minimum, maximum] {
        return minimum->hasAcceptableInput() && maximum->hasAcceptableInput()
            && minimum->value() < maximum->value();
    };
    const auto updateApply = [this, validRange] { m_applyColorRange->setEnabled(validRange()); };
    for (auto *field : {minimum, maximum}) {
        connect(field, &QDoubleSpinBox::valueChanged, this, updateApply);
        connect(field->findChild<QLineEdit *>(), &QLineEdit::textChanged, this, updateApply);
    }
    connect(m_applyColorRange, &QPushButton::clicked, this, [this, minimum, maximum, validRange] {
        if (validRange()) m_imageSettings->setColorRange(minimum->value(), maximum->value());
    });
    updateApply();
    auto *fpsRefresh=new QTimer(this);fpsRefresh->setInterval(250);
    connect(fpsRefresh,&QTimer::timeout,this,[this,fpsLabel]{
        double fps=0;
        for(auto *view:{m_rvCanvas,m_assistCanvas,m_candidateCanvas})
            if(view->isVisible())fps=view->displayedFps();
        fpsLabel->setText(QStringLiteral("FPS: %1").arg(fps,0,'f',1));
    });
    fpsRefresh->start();
    m_liteSrRepairButton=new QPushButton(m_rvControls);m_liteSrRepairButton->setObjectName("rvLiteSrRepairSettings");rvLayout->addWidget(m_liteSrRepairButton);
    m_boardLiteSrHostContour=new QCheckBox(m_rvControls);m_boardLiteSrHostContour->setObjectName("rvBoardLiteSrHostContour");rvLayout->addWidget(m_boardLiteSrHostContour);
    connect(m_boardLiteSrHostContour,&QCheckBox::toggled,this,[this](bool enabled){
        if(m_session)m_session->setBoardLiteSrHostContour(enabled,m_session->boardLiteSrHostRepairSettings());
        syncSessionControls();updateRvPresentation();
    });
    connect(m_liteSrRepairButton,&QPushButton::clicked,this,&DepthMapWidget::editLiteSrRepairSettings);
    connect(m_rvStart, &QPushButton::clicked, this, [this]{if (m_session) m_session->start();});
    connect(m_rvStop, &QPushButton::clicked, this, [this]{if (m_session) m_session->stop();});
    connect(m_rvReconnect, &QPushButton::clicked, this, [this]{if (m_session) m_session->reconnectNow();});
    layout->addWidget(statusRow);
    connect(m_saveButton,&QPushButton::clicked,this,&DepthMapWidget::saveCurrentFrame);

    syncDisplayColors();
    retranslateUi();
    syncSessionControls();
}

int DepthMapWidget::selectedWidth() const
{
    if(m_depthTabs->currentIndex()==2)return RvDtofCandidates::outputWidth(m_session?m_session->dtofCandidateMode():RvDtofCandidates::LiteSrRectFullHd);
    if(m_depthTabs->currentIndex()==1&&m_session)return RvCameraView::profile(m_session->assistHostModel()).outputWidth;
    return m_session ? m_session->depthWidth() : m_snapshot.metadata["width"].toInt(144);
}

int DepthMapWidget::selectedHeight() const
{
    if(m_depthTabs->currentIndex()==2)return RvDtofCandidates::outputHeight(m_session?m_session->dtofCandidateMode():RvDtofCandidates::LiteSrRectFullHd);
    if(m_depthTabs->currentIndex()==1&&m_session)return RvCameraView::profile(m_session->assistHostModel()).outputHeight;
    if (!m_session) return m_snapshot.metadata["height"].toInt(81);
    switch (m_session->depthWidth()) {
    case 288: return 162;
    case 384: return 256;
    case 576: return 324;
    default: return 81;
    }
}

int DepthMapWidget::displayRotationDegrees() const
{
    return m_imageSettings->rotationDegrees();
}

void DepthMapWidget::setDisplayRotationDegrees(int degrees)
{
    m_imageSettings->setRotationDegrees(degrees);
}

void DepthMapWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange)
        retranslateUi();
}

void DepthMapWidget::retranslateUi()
{
    m_sessionControlsKey.clear();
    m_blackOutsideRange->setText(tr("Black outside range"));
    m_colorMinimumLabel->setText(tr("Minimum (m)"));
    m_colorMaximumLabel->setText(tr("Maximum (m)"));
    m_applyColorRange->setText(tr("Apply"));
    m_saveButton->setText(tr("Save depth map"));
    m_saveCamera->setText(tr("Save camera image too"));
    m_rvModelCaption->setText(tr("Imaging scheme:"));
    m_rvStart->setText(tr("Start"));
    m_rvStop->setText(tr("Stop"));
    m_rvReconnect->setText(tr("Reconnect now"));
    for (auto *button : {m_rvStart, m_rvStop, m_rvReconnect})
        button->setToolTip(tr("Start, Stop and Reconnect apply to the shared camera + depth stream."));
    auto label=[this](const char* id,const QString& text){m_rvModel->setItemText(m_rvModel->findData(id),text);};
    label("host_unidepth_rgb672_vga_fp16_tof16",tr("Host-assisted inference (640×480)"));
    m_depthTabs->setTabText(0,tr("Board depth"));m_depthTabs->setTabText(1,tr("Host assisted depth"));
    m_depthTabs->setTabText(2,tr("Board depth"));
    label("board_litesr_near_384x256_host_contour_hd",tr("On-board inference (384x256)"));
    label(TinyNv12Contract::Scheme,tr("Tiny NV12 128x80 (on-board)"));
    m_liteSrRepairButton->setText(tr("Repair settings..."));
    m_boardLiteSrHostContour->setText(tr("Host contour optimization"));
    m_boardLiteSrHostContour->setToolTip(tr("RV1103 runs the full on-board INT8 model. The PC refines its depth with matched native input; no host neural network is started. The separate board contour option keeps all processing on RV1103."));
    m_rvModel->setItemData(m_rvModel->findData("board_litesr_near_384x256_host_contour_hd"),tr("48x32 depth and photon input; 384x256 output. Same sensor field of view. Experimental weight reuse; physical accuracy is unverified."),Qt::ToolTipRole);
    m_rvModel->setAccessibleName(tr("Imaging scheme:"));
    m_rvModel->setToolTip(tr("Select host-assisted or on-board depth inference."));
    if (m_session) m_session->retranslateState();
    m_sessionControlsKey.clear();syncSessionControls();
}

bool DepthMapWidget::eventFilter(QObject *watched,QEvent *event)
{
    if(watched==m_depthTabs&&m_switchOverlay&&(event->type()==QEvent::Resize||event->type()==QEvent::Show)){
        m_switchOverlay->setGeometry(m_depthTabs->rect());if(m_switchUiActive)m_switchOverlay->raise();
    }
    return QWidget::eventFilter(watched,event);
}

void DepthMapWidget::beginImagingSwitchUi(const QString &scheme,int estimatedMs)
{
    m_switchUiActive=true;m_switchUiFinishing=false;m_switchUiScheme=scheme;
    m_switchEstimatedMs=std::max(1,estimatedMs);m_switchUiClock.restart();m_switchFinishClock.invalidate();
    m_switchProgress->setValue(0);
    m_snapshot={};m_assistSnapshot={};m_candidateSnapshot={};
    for(auto *view:{m_rvCanvas,m_assistCanvas,m_candidateCanvas}){
        view->setLoadingState(0);view->clear(tr("Switching imaging scheme..."));
    }
    m_switchOverlay->setGeometry(m_depthTabs->rect());m_switchOverlay->show();m_switchOverlay->raise();
    m_switchUiTimer->start();advanceImagingSwitchUi();m_sessionControlsKey.clear();syncSessionControls();
}

void DepthMapWidget::finishImagingSwitchUi()
{
    if(!m_switchUiActive)return;
    // Completion is signalled only by an admitted depth result. Coloring stays
    // asynchronous under the overlay; do not expose an empty or old viewport.
    m_switchUiFinishing=true;m_switchFinishFrom=m_switchProgress->value();m_switchFinishClock.invalidate();
    advanceImagingSwitchUi();
}

void DepthMapWidget::cancelImagingSwitchUi()
{
    if(!m_switchUiActive)return;
    m_switchUiActive=false;m_switchUiFinishing=false;m_switchUiTimer->stop();
    m_switchProgress->setValue(0);m_switchOverlay->hide();
    setProperty("imagingSwitchVisible",false);setProperty("imagingSwitchProgress",0);
    m_sessionControlsKey.clear();syncSessionControls();updateRvPresentation();
}

void DepthMapWidget::advanceImagingSwitchUi()
{
    if(!m_switchUiActive)return;
    if(!m_session||m_session->userStopped()){cancelImagingSwitchUi();return;}
    const auto *view=static_cast<RvDepthView*>(m_depthTabs->currentWidget());
    const bool rendered=view&&view->hasRenderableFrame();
    const int waiting=std::min(950,int(960.*(1.-std::exp(-2.*m_switchUiClock.elapsed()/m_switchEstimatedMs))));
    if(m_switchUiFinishing&&rendered&&!m_switchFinishClock.isValid()){
        m_switchFinishFrom=m_switchProgress->value();m_switchFinishClock.start();
    }
    int value=std::max(m_switchProgress->value(),waiting);
    if(m_switchUiFinishing&&m_switchFinishClock.isValid())
        value=m_switchFinishFrom+int((1000-m_switchFinishFrom)*std::min(1.,m_switchFinishClock.elapsed()/180.));
    m_switchProgress->setValue(value);m_switchProgress->setAccessibleName(tr("Imaging scheme switch progress"));
    m_switchProgress->setToolTip(tr("Estimated progress. Completion waits for a valid new depth image."));
    m_switchTitle->setText(m_switchUiFinishing?(m_switchFinishClock.isValid()?tr("Switch complete"):tr("Waiting for the new depth image...")):tr("Switching imaging scheme..."));
    m_switchDetail->setText(m_rvModel->itemText(m_rvModel->findData(m_switchUiScheme)));
    setProperty("imagingSwitchVisible",true);setProperty("imagingSwitchProgress",value);
    m_switchOverlay->raise();
    if(m_switchUiFinishing&&m_switchFinishClock.isValid()&&m_switchFinishClock.elapsed()>=260&&rendered){
        m_switchProgress->setValue(1000);setProperty("imagingSwitchProgress",1000);
        m_switchUiActive=false;m_switchUiFinishing=false;m_switchUiTimer->stop();m_switchOverlay->hide();
        setProperty("imagingSwitchVisible",false);m_sessionControlsKey.clear();syncSessionControls();updateRvPresentation();
    }
}

void DepthMapWidget::setSessionController(RvSessionController *controller)
{
    m_sessionControlsKey.clear();
    if (m_session == controller) return;
    cancelImagingSwitchUi();
    if(m_session){
        disconnect(m_session,nullptr,this,nullptr);
    }
    // A recovery window belongs to one source. Rebinding never carries the
    // previous source's displayed pixels into the new session.
    m_rvCanvas->clear();m_assistCanvas->clear();m_candidateCanvas->clear();
    m_session = controller;
    clearRvDepth(tr("RV session changed"));
    clearAssistDepth(tr("RV session changed"));
    if (m_session) {
        connect(m_session, &RvSessionController::configurationChanged,
                this, [this] { m_sessionControlsKey.clear(); syncSessionControls(); });
        connect(m_session, &RvSessionController::stateChanged,
                this, &DepthMapWidget::syncSessionControls);
        connect(m_session, &RvSessionController::depthSnapshotReady,
                this, &DepthMapWidget::acceptRvSnapshot);
        connect(m_session, &RvSessionController::depthStreamCleared,
                this, &DepthMapWidget::clearRvDepth);
        connect(m_session,&RvSessionController::dtofCandidateReady,this,&DepthMapWidget::acceptCandidate);
        connect(m_session,&RvSessionController::dtofCandidateSelectionChanged,this,&DepthMapWidget::syncSessionControls);
        connect(m_session,&RvSessionController::dtofCandidateCleared,this,&DepthMapWidget::clearCandidate);
        connect(m_session,&RvSessionController::dtofCandidateExpired,this,&DepthMapWidget::holdCandidate);
        connect(m_session,&RvSessionController::assistStateChanged,this,&DepthMapWidget::syncSessionControls);
        connect(m_session,&RvSessionController::imagingSwitchStarted,this,&DepthMapWidget::beginImagingSwitchUi);
        connect(m_session,&RvSessionController::imagingSwitchFinished,this,[this]{finishImagingSwitchUi();});
        connect(m_session,&RvSessionController::imagingSwitchAborted,this,[this]{cancelImagingSwitchUi();});
        connect(m_session,&RvSessionController::assistDepthSnapshotReady,this,&DepthMapWidget::acceptAssistSnapshot);
        connect(m_session,&RvSessionController::assistDepthStreamCleared,this,&DepthMapWidget::clearAssistDepth);
        connect(m_session,&RvSessionController::recoveryDisplayExpired,this,[this] {
            if(m_rvCanvas->frameHeld())clearRvDepth(tr("No fresh dToF data"));
            if(m_assistCanvas->frameHeld())clearAssistDepth(tr("No fresh dToF data"));
            if(m_candidateCanvas->frameHeld())clearCandidate(tr("No fresh dToF data"));
        });
        connect(m_session, &QObject::destroyed, this, [this] {
            m_session = nullptr;
            cancelImagingSwitchUi();
            clearRvDepth(tr("RV session unavailable"));
            clearAssistDepth(tr("RV session unavailable"));
            syncSessionControls();
        });
    }
    syncSessionControls();
}

RvSessionController *DepthMapWidget::sessionController() const
{
    return m_session.data();
}

void DepthMapWidget::syncSessionControls()
{
    const bool available = !m_session.isNull();
    const bool busy = available && m_session->busy();
    const bool stopped = !available || m_session->userStopped();
    m_rvStart->setEnabled(available && !busy && stopped);
    m_rvStop->setEnabled(available && !stopped);
    m_rvReconnect->setEnabled(available && !busy);
    if(m_switchUiActive&&(!m_session||m_session->userStopped()))cancelImagingSwitchUi();
    if(!m_session||m_session->userStopped()||m_session->busy())clearCandidate(tr("No fresh dToF data"));
    const bool assisting=m_session&&(m_session->cameraWidth()==576||m_session->hostHdAssist())&&m_session->assistDepthEnabled();
    const bool candidate=m_session&&m_session->dtofCandidateMode()>=0&&!RvDtofCandidates::isCaptureOnly(m_session->dtofCandidateMode());
    const int boardMode=m_session?m_session->dtofCandidateMode():-1;
    const bool boardFull=RvDtofCandidates::isLiteSrFull(boardMode)&&!RvDtofCandidates::isCaptureOnly(boardMode);
    setProperty("dtofComputeLocation",boardFull&&m_session->boardLiteSrHostContourEnabled()?"rv1103+host_cpu":"rv1103");
    m_boardLiteSrHostContour->setVisible(boardFull);
    {const QSignalBlocker blocker(m_boardLiteSrHostContour);m_boardLiteSrHostContour->setChecked(m_session&&m_session->boardLiteSrHostContourEnabled());}
    m_boardLiteSrHostContour->setEnabled(!LiteSrBoardContract::isDepthOnly(boardMode)&&m_session&&!m_session->busy()&&!RvDtofCandidates::isLiteSrHd(boardMode)&&!LiteSrBoardContract::isHighResolution(boardMode));
    m_liteSrRepairButton->setVisible(boardMode==RvDtofCandidates::LiteSrContour||(boardFull&&m_session->boardLiteSrHostContourEnabled()));
    m_liteSrRepairButton->setEnabled(m_session&&!m_session->busy());
    m_liteSrRepairButton->setText(boardFull&&m_session->boardLiteSrHostContourEnabled()?tr("Contour settings..."):tr("Repair settings..."));
    const QStringList key{QString::number(available), QString::number(assisting),
        QString::number(boardMode), QString::number(busy), QString::number(m_switchUiActive),
        QString::number(m_session&&m_session->controlledSource()),
        QString::number(m_session?m_session->assistLoadingState():0),
        m_session?m_session->assistHostModel():QString()};
    // Frame statistics change much more often than configuration. Keep status
    // current without rebuilding combo-box permissions/tooltips on every frame.
    if(key!=m_sessionControlsKey){
    m_sessionControlsKey=key;
    const QSignalBlocker modelBlocker(m_rvModel);
    const bool offerAssist=m_session&&(m_session->cameraWidth()==576||m_session->hostHdAssist()||RvDtofCandidates::isLiteSrHd(m_session->dtofCandidateMode()));
    m_depthTabs->setCurrentIndex(candidate?2:assisting?1:0);
    const int loading=assisting&&!m_switchUiActive?m_session->assistLoadingState():0;
    m_assistCanvas->setLoadingState(loading==2&&m_assistCanvas->hasFrame()?0:loading);
    const bool configurable = m_session && !m_session->busy()&&!m_switchUiActive
        && m_session->controlledSource();
    m_rvModel->setCurrentIndex(m_session?m_rvModel->findData(m_session->currentImagingScheme()):-1);
    if(auto* items=qobject_cast<QStandardItemModel*>(m_rvModel->model())){
        for(int i=0;i<m_rvModel->count();++i){
            const auto id=m_rvModel->itemData(i).toString();
            if(id==TinyNv12Contract::Scheme){items->item(i)->setEnabled(configurable&&m_session->tinyNv12Available());items->item(i)->setToolTip(m_session&&m_session->tinyNv12Available()?tr("NV12-guided on-board depth, 80x128 pixels."):tr("Tiny NV12 requires the calibrated F1 device and its saved camera orientation."));}
            else if(id.startsWith("board_litesr_"))items->item(i)->setEnabled(configurable);
            else {
                const QString model=id=="host_assisted"?QStringLiteral("unidepth"):id.mid(5);
                const bool available=RvAssistDepth::hostModelAvailable(model);
                items->item(i)->setEnabled(offerAssist&&available);
                items->item(i)->setToolTip(available?QString():RvAssistDepth::tr("Selected host model runtime is missing; prepare the optional runtime first"));
            }
        }
    }
    m_rvModel->setEnabled(configurable);
    }
    updateRvPresentation();
}
void DepthMapWidget::syncDisplayColors()
{
    for(auto *view:{m_rvCanvas,m_assistCanvas,m_candidateCanvas}){
        view->setDisplayRange(m_imageSettings->colorMinimum(),m_imageSettings->colorMaximum());
        view->setBlackOutsideRange(m_imageSettings->blackOutsideRange());
    }
    // Expired pixels cannot be recolored after their metric samples are revoked.
    if(m_candidateCanvas->frameHeld())clearCandidate(tr("No fresh dToF data"));
}
void DepthMapWidget::acceptRvDepth(const QByteArray& depth,const QJsonObject& m)
{
    if(!m_rvCanvas->setDepth(depth,m)){clearRvDepth(tr("Invalid depth packet"));return;}
    m_snapshot={depth,{},{},m,QDateTime::currentDateTimeUtc()};
    updateRvPresentation();
}
void DepthMapWidget::clearRvDepth(const QString& reason)
{
    clearCandidate(reason);
    m_snapshot={};
    const bool holding=m_session&&m_session->retainingRecoveryDisplay();
    if(holding)m_rvCanvas->holdFrame();else m_rvCanvas->clear(UiStyle::depthErrorText(reason));
    updateRvPresentation();
}

void DepthMapWidget::acceptAssistSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,const RvTofFrame &,const QByteArray &diagnostic)
{
    if(!m_session||!m_session->assistDepthEnabled())return;
    if(!m_assistCanvas->setDepth(depth,metadata,diagnostic)){clearAssistDepth(tr("Invalid host assisted depth result"));return;}
    m_assistSnapshot={depth,{},camera,metadata,QDateTime::currentDateTimeUtc()};
    updateRvPresentation();
}
void DepthMapWidget::clearAssistDepth(const QString &reason)
{
    m_assistSnapshot={};
    if(m_session&&m_session->retainingRecoveryDisplay())m_assistCanvas->holdFrame();else m_assistCanvas->clear(UiStyle::depthErrorText(reason));
    updateRvPresentation();
}

void DepthMapWidget::updateRvPresentation()
{
    const bool candidate=m_depthTabs->currentIndex()==2;
    if(!m_saveButton)return;

    m_saveCamera->setEnabled(!candidate);
    const auto &snapshot = candidate ? m_candidateSnapshot
        : m_depthTabs->currentIndex()==1 ? m_assistSnapshot : m_snapshot;
    m_saveButton->setEnabled(!m_switchUiActive
        && !(m_session && m_session->retainingRecoveryDisplay()) && !snapshot.metresFloat32LE.isEmpty());
}

void DepthMapWidget::acceptRvSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera)
{
    acceptRvDepth(depth,metadata);
    if(m_rvCanvas->hasFrame())m_snapshot.cameraImage=camera;
}

DepthSnapshot DepthMapWidget::snapshot() const
{
    if(m_depthTabs->currentIndex()==2){
        auto result=m_candidateSnapshot;
        if(!result.metresFloat32LE.isEmpty()){
            result.depthImage=m_candidateCanvas->snapshotImage();
            result.metadata["display_minimum_m"]=m_imageSettings->colorMinimum();
            result.metadata["display_maximum_m"]=m_imageSettings->colorMaximum();
            result.metadata["display_out_of_range_black"]=m_imageSettings->blackOutsideRange();
        }
        return result;
    }
    auto result=m_depthTabs->currentIndex()==1?m_assistSnapshot:m_snapshot;
    if(result.metresFloat32LE.isEmpty())return {};
    result.depthImage=m_depthTabs->currentIndex()==1?m_assistCanvas->snapshotImage():m_rvCanvas->snapshotImage();
    result.metadata["display_minimum_m"]=m_rvCanvas->displayMinimum();
    result.metadata["display_maximum_m"]=m_rvCanvas->displayMaximum();
    result.metadata["display_out_of_range_black"]=m_rvCanvas->blackOutsideRange();
    return result;
}

void DepthMapWidget::saveCurrentFrame()
{
    const auto frozen=snapshot(); // Freeze before the dialog can process another packet.
    const bool withCamera=m_saveCamera->isEnabled()&&m_saveCamera->isChecked();
    if(frozen.depthImage.isNull() || (withCamera && frozen.cameraImage.isNull())){
        QMessageBox::warning(this,tr("Save depth map"),tr("No current depth or paired camera image. Start a camera + depth mode and wait for a frame."));return;
    }
    QSettings settings;
    const auto directory=QFileDialog::getExistingDirectory(this,tr("Select capture folder"),settings.value("depthMap/saveDirectory",QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString());
    if(directory.isEmpty())return;
    QString result;
    if(!frozen.save(directory,withCamera,&result)){QMessageBox::warning(this,tr("Save depth map"),tr("Save failed: %1").arg(result));return;}
    settings.setValue("depthMap/saveDirectory",directory);
    QMessageBox::information(this,tr("Save depth map"),tr("Saved to %1").arg(QDir::toNativeSeparators(result)));
}

void DepthMapWidget::clearCandidate(const QString& reason){
    m_candidateSnapshot={};if(m_candidateCanvas){
        if(m_session&&m_session->retainingRecoveryDisplay())m_candidateCanvas->holdFrame();else m_candidateCanvas->clear(UiStyle::depthErrorText(reason));
    }
    if(m_saveButton)updateRvPresentation();
}

void DepthMapWidget::holdCandidate(){
    m_candidateSnapshot={};m_candidateCanvas->holdFrame();updateRvPresentation();
}

DepthMapWidget::~DepthMapWidget(){
    if(m_session){
        disconnect(m_session,nullptr,this,nullptr);
    }
}
void DepthMapWidget::acceptCandidate(const RvDtofCandidates::Result& result){
    if(!m_session||m_session->userStopped()||m_session->busy()
        ||m_session->dtofCandidateMode()<0||m_depthTabs->currentIndex()!=2
        ||result.metadata["candidate_mode"].toInt(-1)!=m_session->dtofCandidateMode())return;
    if(!result.fresh){if(result.error.isEmpty())holdCandidate();else clearCandidate(result.error);return;}
    if(RvDtofCandidates::identity(result.metadata)!=RvDtofCandidates::identity(m_session->metadata()))return;
    if(RvDtofCandidates::isLiteSrFull(m_session->dtofCandidateMode())&&result.metadata["host_contour_enabled"].toBool()!=m_session->boardLiteSrHostContourEnabled())return;
    m_candidateCanvas->setBoardCameraZ(RvDtofCandidates::isTinyNv12(m_session->dtofCandidateMode()));
    m_candidateCanvas->setNativeGridSize(LiteSrBoardContract::isNative16Depth(m_session->dtofCandidateMode())?QSize(16,16):QSize(48,32));
    if(m_candidateCanvas->setDepth(result.depth,result.metadata)&&m_candidateCanvas->setCoverage(result.coverage))
        m_candidateSnapshot={result.depth,{},{},result.metadata,QDateTime::currentDateTimeUtc()};
    else clearCandidate(tr("No fresh dToF data"));
    updateRvPresentation();
}

void DepthMapWidget::editLiteSrRepairSettings(){
    QPointer<RvSessionController> board=m_session;
    const bool onBoard=board&&board->dtofCandidateMode()==RvDtofCandidates::LiteSrContour;
    const bool hybrid=board&&RvDtofCandidates::isLiteSrFull(board->dtofCandidateMode())&&board->boardLiteSrHostContourEnabled();
    if(!onBoard&&!hybrid)return;
    const auto settings=board?board->dtofCandidateSettings():RvDtofCandidates::Settings{};
    const auto current=hybrid?board->boardLiteSrHostRepairSettings():RvLiteSrContour::RepairSettings{settings.liteSrHoles,settings.liteSrOutliers};
    QDialog dialog(this);dialog.setObjectName("rvLiteSrRepairDialog");dialog.setWindowTitle(tr("Board depth repair settings"));
    if(hybrid)dialog.setWindowTitle(tr("Host contour refinement settings"));
    auto *form=new QFormLayout(&dialog);
    auto *holes=new QComboBox(&dialog);holes->setObjectName("rvLiteSrHoleMode");
    holes->addItem(tr("Off"),0);holes->addItem(tr("Small regions (up to 4 native cells)"),1);
    holes->addItem(tr("Larger regions (up to 64 native cells)"),2);holes->setCurrentIndex(current.holeMode);
    form->addRow(tr("Fill invalid regions:"),holes);
    auto *outliers=new QCheckBox(tr("Repair isolated outliers (1-2 native cells)"),&dialog);
    outliers->setObjectName("rvLiteSrRemoveOutliers");outliers->setChecked(current.removeOutliers);form->addRow(outliers);
    auto *description=new QLabel(tr("Region sizes use the original 48 x 32 ToF grid. Fill from this frame's surrounding board depth with linear interpolation. Mixed foreground/background and unsupported regions stay invalid. Isolated small real objects may also be smoothed by outlier repair."),&dialog);
    description->setWordWrap(true);description->setMaximumWidth(460);form->addRow(description);
    if(hybrid){auto* stages=new QLabel(tr("Runs on the PC: 7 x 7 edge-preserving smoothing, four tangent passes and contour antialiasing. Smoothing adjusts depth by at most 12 mm after repairs. Positive valid depths below 10 cm remain supported."),&dialog);stages->setWordWrap(true);stages->setMaximumWidth(460);form->addRow(stages);}
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    form->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()==QDialog::Accepted){
        if(hybrid&&board&&board==m_session&&RvDtofCandidates::isLiteSrFull(board->dtofCandidateMode())&&board->boardLiteSrHostContourEnabled()){
            board->setBoardLiteSrHostContour(true,{holes->currentData().toInt(),outliers->isChecked()});
        }else if(onBoard&&board&&board==m_session&&board->dtofCandidateMode()==RvDtofCandidates::LiteSrContour&&!board->busy()){
            auto updated=board->dtofCandidateSettings();updated.liteSrHoles=holes->currentData().toInt();updated.liteSrOutliers=outliers->isChecked();
            board->setDtofCandidate(RvDtofCandidates::LiteSrContour,updated);
        }
    }
}

