#include "DepthMapWidget.h"
#include "RvCameraView.h"
#include "../UiStyle.h"
#include "RvDtofCandidates.h"
#include "RvDepthView.h"
#include "RvDepthDiagnostic.h"
#include "ImagePresentation.h"
#include "RvSessionController.h"
#include "RvAssistDepth.h"

#include <QtCore/QSettings>
#include <QtCore/QDir>
#include <QtCore/QList>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProgressBar>
#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
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
constexpr auto ColorModeKey = "depth/colorMode";
enum ColorMode { FixedColor, DynamicColor };

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
    if(RvAssistDepth::supportsHostModel(QLatin1String(RvCameraView::UniDepthVgaNative48))){
        m_rvModel->addItem(QString(),QLatin1String(RvSessionController::GlobalRgbImagingScheme));
        m_rvModel->addItem(QString(),QLatin1String(RvSessionController::ReuseLatestTofImagingScheme));
    }
    m_rvModel->addItem(QString(),"board_native_tof48");
    m_rvModel->addItem(QString(),"board_native_tof48_high_accuracy");
    m_rvModel->setMinimumWidth(0);
    m_rvModel->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_rvModel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    rvLayout->addWidget(m_rvModelCaption);
    rvLayout->addWidget(m_rvModel);
    bool angleValid=false;
    const auto savedAngle=QSettings().value("depth/nativeTofRotationDegrees",90).toInt(&angleValid);
    m_nativeTofRotation=angleValid&&savedAngle>=0&&savedAngle<360?savedAngle:90;
    QSettings().setValue("depth/nativeTofRotationDegrees",m_nativeTofRotation);
    m_nativeTofAngle=new QPushButton(m_rvControls);m_nativeTofAngle->setObjectName("rvNativeTofAngle");
    connect(m_nativeTofAngle,&QPushButton::clicked,this,&DepthMapWidget::editNativeTofRotation);
    rvLayout->addWidget(m_nativeTofAngle);
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
    m_rvCanvas=new RvDepthView(m_depthTabs);m_rvCanvas->setObjectName("rvNativeDepthCanvas");m_rvCanvas->setRadialMode(false);m_rvCanvas->setNativeGridSize(QSize(48,32));
    m_rvCanvas->setNativeDisplayMirrored(true);
    syncNativeTofRotation();
    m_assistCanvas=new RvDepthView(m_depthTabs);m_assistCanvas->setObjectName("rvAssistDepthCanvas");
    m_depthTabs->addTab(m_rvCanvas,QString());m_depthTabs->addTab(m_assistCanvas,QString());
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
    connect(m_imageSettings.data(), &ImageDisplaySettings::rotationChanged, this, [this](int degrees) {
        m_rvCanvas->setDisplayRotationDegrees(degrees);
        m_assistCanvas->setDisplayRotationDegrees(degrees);
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
    m_monocularButton=new QPushButton(this);
    m_monocularButton->setObjectName("depthMonocularOnly");
    m_monocularButton->setCheckable(true);
    m_monocularButton->hide();
    saveControls->addWidget(m_monocularButton);
    connect(m_monocularButton,&QPushButton::toggled,this,[this](bool enabled){
        if(m_session)m_session->setMonocularDisplay(enabled);
    });
    m_splitCorrectionButton=new QPushButton(this);
    m_splitCorrectionButton->setObjectName("depthSplitCorrection");
    m_splitCorrectionButton->setCheckable(true);
    m_splitCorrectionButton->hide();
    saveControls->addWidget(m_splitCorrectionButton);
    connect(m_splitCorrectionButton,&QPushButton::toggled,this,[this](bool enabled){
        if(m_session)m_session->setSplitCorrectionEnabled(enabled);
        syncSessionControls();
    });
    m_colorMode = new QComboBox(statusRow);
    m_colorMode->setObjectName("rvDepthColorMode");
    m_colorMode->addItems({QString(), QString()});
    m_colorMode->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    saveControls->addWidget(m_colorMode);

    m_blackOutsideRange = new QCheckBox(statusRow);
    m_blackOutsideRange->setObjectName("depthBlackOutsideRange");
    m_blackOutsideRange->setChecked(m_imageSettings->blackOutsideRange());
    saveControls->addWidget(m_blackOutsideRange);
    connect(m_blackOutsideRange, &QCheckBox::toggled, this, [this](bool enabled){m_imageSettings->setBlackOutsideRange(enabled);});
    connect(m_imageSettings.data(), &ImageDisplaySettings::blackOutsideRangeChanged, this, [this](bool enabled){const QSignalBlocker block(m_blackOutsideRange);m_blackOutsideRange->setChecked(enabled);});

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
    QList<QWidget *> fixedColorControls{m_blackOutsideRange, m_applyColorRange};
    for (const auto &[label, field] : {qMakePair(m_colorMinimumLabel, minimum), qMakePair(m_colorMaximumLabel, maximum)}) {
        auto *range = new QWidget(statusRow);
        auto *pair = new QHBoxLayout(range);
        pair->setContentsMargins(0, 0, 0, 0);
        pair->setSpacing(ImagePresentation::Spacing);
        pair->addWidget(label);
        pair->addWidget(field);
        saveControls->addWidget(range);
        fixedColorControls.append(range);
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
        if(validRange()){
            m_imageSettings->setColorRange(minimum->value(),maximum->value());
        }
    });
    updateApply();

    const auto applyColorMode = [this, fixedColorControls](int index) {
        const bool dynamic = index == DynamicColor;
        for (auto *control : fixedColorControls)
            control->setVisible(!dynamic);
        m_rvCanvas->setDynamicColoring(dynamic);
        m_assistCanvas->setDynamicColoring(dynamic);
    };
    m_colorMode->setCurrentIndex(QSettings().value(ColorModeKey).toString() == "dynamic" ? DynamicColor : FixedColor);
    applyColorMode(m_colorMode->currentIndex());
    connect(m_colorMode, &QComboBox::currentIndexChanged, this, [applyColorMode](int index) {
        QSettings().setValue(ColorModeKey, index == DynamicColor ? QStringLiteral("dynamic") : QStringLiteral("fixed"));
        applyColorMode(index);
    });

    auto *fpsRefresh=new QTimer(this);fpsRefresh->setInterval(250);
    connect(fpsRefresh,&QTimer::timeout,this,[this,fpsLabel]{
        double fps=0;
        for(auto *view:{m_rvCanvas,m_assistCanvas})
            if(view->isVisible())fps=view->displayedFps();
        fpsLabel->setText(QStringLiteral("FPS: %1").arg(fps,0,'f',1));
    });
    fpsRefresh->start();
    connect(m_rvStart, &QPushButton::clicked, this, [this]{if (m_session) m_session->start();});
    connect(m_rvStop, &QPushButton::clicked, this, [this]{if (m_session) m_session->stop();});
    connect(m_rvReconnect, &QPushButton::clicked, this, [this]{if (m_session) m_session->reconnectNow();});
    layout->addWidget(statusRow);
    m_diagnosticsRow = new QWidget(this);
    auto *diagnostics = UiStyle::wrappingRow(m_diagnosticsRow,ImagePresentation::Spacing);
    m_tofPointsButton = new QPushButton(m_diagnosticsRow);
    m_tofPointsButton->setObjectName("depthPairedTofPoints");
    m_tofPointsButton->setCheckable(true);
    diagnostics->addWidget(m_tofPointsButton);
    connect(m_tofPointsButton,&QPushButton::toggled,this,[this](bool visible) {
        m_assistCanvas->setTofPointsVisible(visible);
        m_tofPointsButton->setText(visible?tr("Hide ToF points"):tr("Show ToF points"));
    });
    m_pairedRgbButton = new QPushButton(m_diagnosticsRow);
    m_pairedRgbButton->setObjectName("depthPairedRgb");
    m_pairedRgbButton->setCheckable(true);
    diagnostics->addWidget(m_pairedRgbButton);
    connect(m_pairedRgbButton,&QPushButton::toggled,this,[this](bool visible) {
        m_assistCanvas->setPairedRgbVisible(visible);
        m_pairedRgbButton->setText(visible?tr("Return to depth"):tr("ToF / RGB alignment"));
        updateRvPresentation();
    });
    m_recordButton = new QPushButton(m_diagnosticsRow);
    m_recordButton->setObjectName("depthDiagnosticRecord");
    diagnostics->addWidget(m_recordButton);
    connect(m_recordButton,&QPushButton::clicked,this,&DepthMapWidget::chooseDiagnosticRecordingFolder);
    m_openRecording = new QPushButton(m_diagnosticsRow);
    m_openRecording->setEnabled(false);
    diagnostics->addWidget(m_openRecording);
    connect(m_openRecording,&QPushButton::clicked,this,[this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_recordingFolder));
    });
    m_diagnosticsRow->hide();
    layout->addWidget(m_diagnosticsRow);
    m_recordStatus = new QLabel(this);
    m_recordStatus->setObjectName("depthDiagnosticStatus");
    m_recordStatus->setWordWrap(true);
    m_recordStatus->hide();
    layout->addWidget(m_recordStatus);
    m_recordTimer = new QTimer(this);
    m_recordTimer->setInterval(100);
    connect(m_recordTimer,&QTimer::timeout,this,&DepthMapWidget::updateDiagnosticRecording);
    connect(m_saveButton,&QPushButton::clicked,this,&DepthMapWidget::saveCurrentFrame);

    syncDisplayColors();
    retranslateUi();
    syncSessionControls();
}

int DepthMapWidget::selectedWidth() const
{
    if(m_depthTabs->currentIndex()==1&&m_session)return RvCameraView::outputProfile(m_session->assistHostModel()).outputWidth;
    if(m_session&&RvDtofCandidates::isNativeDepth(m_session->dtofCandidateMode()))return 48;
    return m_session ? m_session->depthWidth() : m_snapshot.metadata["width"].toInt(144);
}

int DepthMapWidget::selectedHeight() const
{
    if(m_depthTabs->currentIndex()==1&&m_session)return RvCameraView::outputProfile(m_session->assistHostModel()).outputHeight;
    if(m_session&&RvDtofCandidates::isNativeDepth(m_session->dtofCandidateMode()))return 32;
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

void DepthMapWidget::syncNativeTofRotation()
{
    m_nativeTofAngle->setText(tr("Input angle (%1°)").arg(m_nativeTofRotation));
    m_nativeTofAngle->setToolTip(tr("Rotate both native ToF displays clockwise. UniDepth, raw measurements and saved images keep their orientation."));
    m_rvCanvas->setNativeDisplayRotationDegrees(m_nativeTofRotation);
}

void DepthMapWidget::editNativeTofRotation()
{
    QInputDialog dialog(this);dialog.setObjectName("rvNativeTofAngleDialog");
    dialog.setWindowTitle(tr("Native ToF angle"));dialog.setInputMode(QInputDialog::IntInput);
    dialog.setLabelText(tr("Clockwise angle (0–359°):"));dialog.setIntRange(0,359);dialog.setIntValue(m_nativeTofRotation);
    dialog.setOkButtonText(tr("Apply"));dialog.setCancelButtonText(tr("Cancel"));
    if(dialog.exec()!=QDialog::Accepted)return;
    m_nativeTofRotation=dialog.intValue();QSettings().setValue("depth/nativeTofRotationDegrees",m_nativeTofRotation);
    syncNativeTofRotation();
}

void DepthMapWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange)
        retranslateUi();
}

void DepthMapWidget::retranslateUi()
{
    syncNativeTofRotation();
    m_sessionControlsKey.clear();
    m_colorMode->setItemText(FixedColor, tr("Fixed color range"));
    m_colorMode->setItemText(DynamicColor, tr("Dynamic + histogram equalization"));
    m_colorMode->setAccessibleName(tr("Depth map color mode"));
    m_colorMode->setItemData(DynamicColor, tr("Equalize depths from 0.10 to 10.00 m in each frame. Colors can vary across frames."), Qt::ToolTipRole);
    m_blackOutsideRange->setText(tr("Black outside range"));
    m_colorMinimumLabel->setText(tr("Minimum (m)"));
    m_colorMaximumLabel->setText(tr("Maximum (m)"));
    m_applyColorRange->setText(tr("Apply"));
    m_saveButton->setText(tr("Save depth map"));
    m_monocularButton->setToolTip(tr("Show the same frame before ToF correction in the depth map and point cloud."));
    m_monocularButton->setText(m_monocularButton->isChecked()?tr("Restore corrected depth"):tr("Show monocular depth only"));
    m_splitCorrectionButton->setToolTip(tr("Use verified distant ToF for the background and paired ToF for the near hand. Applies to the depth map and point cloud."));
    m_splitCorrectionButton->setText(m_splitCorrectionButton->isChecked()?tr("Restore original G"):tr("Try split correction"));
    m_tofPointsButton->setText(m_tofPointsButton->isChecked()?tr("Hide ToF points"):tr("Show ToF points"));
    m_tofPointsButton->setToolTip(tr("Point centers use the measured depth color. White outlines mark points used to correct this frame."));
    m_pairedRgbButton->setText(m_pairedRgbButton->isChecked()?tr("Return to depth"):tr("ToF / RGB alignment"));
    m_pairedRgbButton->setToolTip(tr("Show projected ToF points on the RGB image paired with this depth frame."));
    if(!m_recorder)m_recordButton->setText(tr("Record diagnostic (10 s)"));
    m_recordButton->setToolTip(tr("Record paired RGB, native ToF, monocular and corrected depth for 10 seconds (about 1 GB)."));
    m_openRecording->setText(tr("Open recording folder"));
    m_saveCamera->setText(tr("Save camera image too"));
    m_rvModelCaption->setText(tr("Imaging scheme:"));
    m_rvStart->setText(tr("Start"));
    m_rvStop->setText(tr("Stop"));
    m_rvReconnect->setText(tr("Reconnect now"));
    for (auto *button : {m_rvStart, m_rvStop, m_rvReconnect})
        button->setToolTip(tr("Start, Stop and Reconnect apply to the shared camera + depth stream."));
    auto label=[this](const char* id,const QString& text){m_rvModel->setItemText(m_rvModel->findData(id),text);};
    label(RvSessionController::GlobalRgbImagingScheme,tr("Host-assisted inference (G)"));
    label(RvSessionController::ReuseLatestTofImagingScheme,tr("Host-assisted inference (B, reuse ToF)"));
    label("board_native_tof48",tr("Board inference"));
    label("board_native_tof48_high_accuracy",tr("Board inference high accuracy"));
    m_depthTabs->setTabText(0,tr("Board depth"));m_depthTabs->setTabText(1,tr("Host assisted depth"));
    m_rvModel->setAccessibleName(tr("Imaging scheme:"));
    m_rvModel->setToolTip(tr("Host-assisted depth inference (480×640)."));
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
    m_snapshot={};m_assistSnapshot={};
    for(auto *view:{m_rvCanvas,m_assistCanvas}){
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
    m_rvCanvas->clear();m_assistCanvas->clear();
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
        connect(m_session,&RvSessionController::dtofCandidateSelectionChanged,this,&DepthMapWidget::syncSessionControls);
        connect(m_session,&RvSessionController::assistStateChanged,this,&DepthMapWidget::syncSessionControls);
        connect(m_session,&RvSessionController::monocularDisplayChanged,this,[this](bool enabled){
            const QSignalBlocker block(m_monocularButton);
            m_monocularButton->setChecked(enabled);
            m_monocularButton->setText(enabled?tr("Restore corrected depth"):tr("Show monocular depth only"));
            if(!m_assistCanvas->setMonocularVisible(enabled))m_assistSnapshot={};
            updateRvPresentation();
        });
        connect(m_session,&RvSessionController::imagingSwitchStarted,this,&DepthMapWidget::beginImagingSwitchUi);
        connect(m_session,&RvSessionController::imagingSwitchFinished,this,[this]{finishImagingSwitchUi();});
        connect(m_session,&RvSessionController::imagingSwitchAborted,this,[this]{cancelImagingSwitchUi();});
        connect(m_session,&RvSessionController::assistDepthSnapshotReady,this,&DepthMapWidget::acceptAssistSnapshot);
        connect(m_session,&RvSessionController::assistDepthStreamCleared,this,&DepthMapWidget::clearAssistDepth);
        connect(m_session,&RvSessionController::recoveryDisplayExpired,this,[this] {
            if(m_rvCanvas->frameHeld())clearRvDepth(tr("No fresh dToF data"));
            if(m_assistCanvas->frameHeld())clearAssistDepth(tr("No fresh dToF data"));
        });
        connect(m_session, &QObject::destroyed, this, [this] {
            m_session = nullptr;
            cancelImagingSwitchUi();
            clearRvDepth(tr("RV session unavailable"));
            clearAssistDepth(tr("RV session unavailable"));
            syncSessionControls();
        });
    }
    m_assistCanvas->setMonocularVisible(m_session&&m_session->monocularDisplay());
    syncSessionControls();
}

RvSessionController *DepthMapWidget::sessionController() const
{
    return m_session.data();
}

void DepthMapWidget::syncSessionControls()
{
    m_nativeTofAngle->setVisible(m_session&&!m_session->hostHdAssist()
        &&RvDtofCandidates::isNativeDepth(m_session->dtofCandidateMode()));
    const bool available = !m_session.isNull();
    const bool busy = available && m_session->busy();
    const bool stopped = !available || m_session->userStopped();
    m_rvStart->setEnabled(available && !busy && stopped);
    m_rvStop->setEnabled(available && !stopped);
    m_rvReconnect->setEnabled(available && !busy);
    if(m_switchUiActive&&(!m_session||m_session->userStopped()))cancelImagingSwitchUi();
    const bool assisting=m_session&&(m_session->cameraWidth()==576||m_session->hostHdAssist())&&m_session->assistDepthEnabled();
    const bool monocular=m_session&&m_session->monocularDisplay();
    m_monocularButton->setVisible(assisting);
    if(monocular!=m_monocularButton->isChecked()){
        const QSignalBlocker block(m_monocularButton);
        m_monocularButton->setChecked(monocular);
        m_monocularButton->setText(monocular?tr("Restore corrected depth"):tr("Show monocular depth only"));
    }
    const bool splitAvailable=m_session&&m_session->splitCorrectionAvailable();
    const bool split=m_session&&m_session->splitCorrectionEnabled();
    m_splitCorrectionButton->setVisible(splitAvailable);
    if(split!=m_splitCorrectionButton->isChecked()){
        const QSignalBlocker block(m_splitCorrectionButton);
        m_splitCorrectionButton->setChecked(split);
    }
    const auto splitText=split?tr("Restore original G"):tr("Try split correction");
    if(m_splitCorrectionButton->text()!=splitText)m_splitCorrectionButton->setText(splitText);
    const QStringList key{QString::number(available), QString::number(assisting),
        QString::number(m_session?m_session->dtofCandidateMode():-1), QString::number(busy), QString::number(m_switchUiActive),
        QString::number(m_session&&m_session->controlledSource()),
        QString::number(m_session?m_session->assistLoadingState():0),
        m_session?m_session->assistHostModel():QString()};
    // Frame statistics change much more often than configuration. Keep status
    // current without rebuilding combo-box permissions/tooltips on every frame.
    if(key!=m_sessionControlsKey){
    m_sessionControlsKey=key;
    const QSignalBlocker modelBlocker(m_rvModel);
    const bool offerAssist=m_session&&m_session->controlledSource();
    m_depthTabs->setCurrentIndex(assisting?1:0);
    setProperty("dtofComputeLocation",assisting?"host":"native_sensor");
    const int loading=assisting&&!m_switchUiActive?m_session->assistLoadingState():0;
    m_assistCanvas->setLoadingState(loading==2&&m_assistCanvas->hasFrame()?0:loading);
    const bool configurable = m_session && !m_session->busy()&&!m_switchUiActive
        && m_session->controlledSource();
    m_rvModel->setCurrentIndex(m_session?m_rvModel->findData(m_session->currentImagingScheme()):-1);
    if(auto* items=qobject_cast<QStandardItemModel*>(m_rvModel->model())){
        for(int i=0;i<m_rvModel->count();++i){
            const auto id=m_rvModel->itemData(i).toString();
            if(id.startsWith("board_native_")){items->item(i)->setEnabled(true);items->item(i)->setToolTip(tr("Native 48×32 ToF radial depth; no neural inference."));}
            else{
                const QString model=QLatin1String(RvCameraView::UniDepthVgaNative48);
                const bool available=RvAssistDepth::hostModelAvailable(model);
                items->item(i)->setEnabled(offerAssist&&available);
                QString tooltip=RvAssistDepth::tr("Selected host model runtime is missing; prepare the optional runtime first");
                if(available){
                    if(id==QLatin1String(RvSessionController::ReuseLatestTofImagingScheme))
                        tooltip=tr("RGB frames can reuse the latest eligible 48×32 ToF measurement within the 200 ms pairing budget.");
                    else tooltip=tr("RGB frames trigger inference. Each fresh 48×32 ToF measurement can update the global scale once.");
                }
                items->item(i)->setToolTip(tooltip);
            }
        }
    }
    m_rvModel->setEnabled(configurable);
    }
    updateRvPresentation();
}
void DepthMapWidget::syncDisplayColors()
{
    for(auto *view:{m_rvCanvas,m_assistCanvas}){
        view->setDisplayRange(m_imageSettings->colorMinimum(),m_imageSettings->colorMaximum());
        view->setBlackOutsideRange(m_imageSettings->blackOutsideRange());
    }
    if(auto *field=findChild<QDoubleSpinBox*>("rvColorMinimum"))field->setValue(m_imageSettings->colorMinimum());
    if(auto *field=findChild<QDoubleSpinBox*>("rvColorMaximum"))field->setValue(m_imageSettings->colorMaximum());
    if(m_blackOutsideRange){const QSignalBlocker block(m_blackOutsideRange);m_blackOutsideRange->setChecked(m_imageSettings->blackOutsideRange());}
    if(m_colorMinimumLabel)m_colorMinimumLabel->setText(tr("Minimum (m)"));
    if(m_colorMaximumLabel)m_colorMaximumLabel->setText(tr("Maximum (m)"));
    // Expired pixels cannot be recolored after their metric samples are revoked.
}
void DepthMapWidget::acceptRvDepth(const QByteArray& depth,const QJsonObject& m)
{
    if(!m_rvCanvas->setDepth(depth,m)){clearRvDepth(tr("Invalid depth packet"));return;}
    m_snapshot={depth,{},{},m,QDateTime::currentDateTimeUtc()};
    updateRvPresentation();
}
void DepthMapWidget::clearRvDepth(const QString& reason)
{
    m_snapshot={};
    const bool holding=m_session&&m_session->retainingRecoveryDisplay();
    if(holding)m_rvCanvas->holdFrame();else m_rvCanvas->clear(UiStyle::depthErrorText(reason));
    updateRvPresentation();
}

void DepthMapWidget::acceptAssistSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,
                                         const RvTofFrame &tof,const QByteArray &diagnostic,const QImage &inferenceInput)
{
    if(!m_session||!m_session->assistDepthEnabled())return;
    if(!m_assistCanvas->setDepth(depth,metadata,diagnostic,camera,tof)){clearAssistDepth(tr("Invalid host assisted depth result"));return;}
    m_assistSnapshot={depth,{},camera,metadata,QDateTime::currentDateTimeUtc()};
    if(m_recorder)m_recorder->append(depth,metadata,inferenceInput,tof,diagnostic);
    updateRvPresentation();
}
void DepthMapWidget::clearAssistDepth(const QString &reason)
{
    if(m_recorder)m_recorder->stop();
    m_assistSnapshot={};
    if(m_session&&m_session->retainingRecoveryDisplay())m_assistCanvas->holdFrame();else m_assistCanvas->clear(UiStyle::depthErrorText(reason));
    updateRvPresentation();
}

void DepthMapWidget::updateRvPresentation()
{
    if(!m_recordButton)return;

    const bool assisting=m_session&&m_session->hostHdAssist()&&m_session->assistDepthEnabled();
    const bool diagnosticsReady=assisting&&m_depthTabs->currentIndex()==1&&!m_switchUiActive;
    const bool hasFreshRgb=diagnosticsReady&&m_assistCanvas->hasPairedRgb();
    const bool viewingRgb=m_pairedRgbButton->isChecked();
    m_diagnosticsRow->setVisible(assisting);
    m_recordStatus->setVisible(assisting&&!m_recordStatus->text().isEmpty());
    m_pairedRgbButton->setEnabled(hasFreshRgb||(assisting&&viewingRgb));
    m_tofPointsButton->setEnabled(hasFreshRgb&&!viewingRgb);
    m_recordButton->setEnabled(hasFreshRgb&&!m_recorder&&!m_session->retainingRecoveryDisplay());
    m_monocularButton->setEnabled(m_depthTabs->currentIndex()==1&&!m_switchUiActive&&!viewingRgb
        &&(m_monocularButton->isChecked()||!m_assistSnapshot.metresFloat32LE.isEmpty()));
    m_splitCorrectionButton->setEnabled(m_session&&m_session->splitCorrectionAvailable()&&!m_session->busy()
        &&!m_switchUiActive&&!m_session->monocularDisplay());
    m_saveCamera->setEnabled(!m_session||m_session->hostHdAssist()
        ||!RvDtofCandidates::isNativeDepth(m_session->dtofCandidateMode()));
    const auto &snapshot = m_depthTabs->currentIndex()==1 ? m_assistSnapshot : m_snapshot;
    m_saveButton->setEnabled(!m_switchUiActive
        && !(m_session && m_session->retainingRecoveryDisplay()) && !snapshot.metresFloat32LE.isEmpty());
}

void DepthMapWidget::chooseDiagnosticRecordingFolder()
{
    QSettings settings;
    const auto target=QFileDialog::getExistingDirectory(this,tr("Select diagnostic recording folder"),
        settings.value("depthMap/diagnosticDirectory",QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString());
    if(target.isEmpty())
        return;
    QString error;
    if(startDiagnosticRecording(target,&error).isEmpty()){
        m_recordStatus->setText(tr("Recording failed: %1").arg(error));
        updateRvPresentation();
        return;
    }
    settings.setValue("depthMap/diagnosticDirectory",target);
}

QString DepthMapWidget::startDiagnosticRecording(const QString &folder,QString *error)
{
    if(folder.isEmpty()||m_recorder||!m_recordButton->isEnabled()){
        if(error)*error=tr("Wait for a fresh host-assisted depth frame.");
        return {};
    }
    auto recorder=std::make_unique<RvDepthDiagnostic::SequenceRecorder>();
    if(!recorder->start(folder,error))
        return {};
    m_recordingFolder=recorder->status()["folder"].toString();
    m_recorder=std::move(recorder);
    m_openRecording->setEnabled(false);
    m_recordTimer->start();
    updateDiagnosticRecording();
    return m_recordingFolder;
}

void DepthMapWidget::updateDiagnosticRecording()
{
    const auto status=m_recorder->status();
    const int frames=status["frames"].toInt(),dropped=status["dropped_frames"].toInt();
    const double remaining=std::max(0.,RvDepthDiagnostic::SequenceRecorder::DurationMs-status["elapsed_ms"].toDouble());
    if(status["complete"]==true){
        m_recordTimer->stop();
        const auto error=status["error"].toString();
        m_recordStatus->setText(error.isEmpty()
            ?tr("Saved %1 frames (%2 dropped) to %3").arg(frames).arg(dropped).arg(QDir::toNativeSeparators(m_recordingFolder))
            :tr("Recording failed: %1").arg(error));
        m_openRecording->setEnabled(true);
        m_recorder.reset();
        m_recordButton->setText(tr("Record diagnostic (10 s)"));
    }else{
        m_recordButton->setText(tr("Recording %1 s").arg(remaining/1000.,0,'f',1));
        m_recordStatus->setText(tr("Recording: %1 frames, %2 dropped").arg(frames).arg(dropped));
        if(remaining<=0)m_recorder->stop();
    }
    updateRvPresentation();
}

void DepthMapWidget::acceptRvSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera)
{
    acceptRvDepth(depth,metadata);
    if(m_rvCanvas->hasFrame())m_snapshot.cameraImage=camera;
}

DepthSnapshot DepthMapWidget::snapshot() const
{
    auto result=m_depthTabs->currentIndex()==1?m_assistSnapshot:m_snapshot;
    if(result.metresFloat32LE.isEmpty())return {};
    result.depthImage=m_depthTabs->currentIndex()==1?m_assistCanvas->snapshotImage():m_rvCanvas->snapshotImage();
    result.metadata["display_minimum_m"]=m_rvCanvas->displayMinimum();
    result.metadata["display_maximum_m"]=m_rvCanvas->displayMaximum();
    result.metadata["display_out_of_range_black"]=m_rvCanvas->blackOutsideRange();
    if(m_depthTabs->currentIndex()==0){
        result.metadata["native_tof_display_rotation_degrees"]=m_nativeTofRotation;
        result.metadata["native_tof_display_mirror_horizontal"]=true;
        result.metadata["display_rotation_degrees"]=(displayRotationDegrees()+m_nativeTofRotation)%360;
    }
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

DepthMapWidget::~DepthMapWidget(){
    if(m_session){
        disconnect(m_session,nullptr,this,nullptr);
    }
}
