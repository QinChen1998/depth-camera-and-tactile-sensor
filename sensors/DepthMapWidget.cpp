#include "DepthMapWidget.h"
#include "RvCameraView.h"
#include "../UiStyle.h"
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSlider>
#include "RvDtofCandidates.h"
#include "RvDepthView.h"
#include "ImagePresentation.h"
#include "RvSessionController.h"
#include "RvAssistDepth.h"
#include "RvLiteSrDepth.h"

#include <QtCore/QSettings>
#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QFile>
#include <QtCore/QThread>
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
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QLinearGradient>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include "ImageHoverTip.h"
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <exception>
#include <limits>

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

QColor heatColor(double normalized)
{
    normalized = qBound(0.0, normalized, 1.0);
    const struct Stop { double position; QColor color; } stops[] = {
        {0.00, QColor("#30123b")}, {0.25, QColor("#2c7bb6")},
        {0.50, QColor("#00a6ca")}, {0.70, QColor("#a8ddb5")},
        {0.85, QColor("#fdae61")}, {1.00, QColor("#d7191c")}
    };
    for (int index = 1; index < 6; ++index) {
        if (normalized <= stops[index].position) {
            const Stop &a = stops[index - 1];
            const Stop &b = stops[index];
            const double t = (normalized - a.position) / (b.position - a.position);
            return QColor::fromRgbF(
                a.color.redF() + (b.color.redF() - a.color.redF()) * t,
                a.color.greenF() + (b.color.greenF() - a.color.greenF()) * t,
                a.color.blueF() + (b.color.blueF() - a.color.blueF()) * t);
        }
    }
    return stops[5].color;
}

bool pixelValue(const Tmf8829Pixel &pixel,
                DepthMapWidget::DisplayMode mode, double *value)
{
    if (!value)
        return false;
    const Tmf8829Peak *peak = pixel.valid ? pixel.nearestValidPeak() : nullptr;
    switch (mode) {
    case DepthMapWidget::DisplayMode::Depth:
        if (!peak) return false;
        *value = peak->distanceMm;
        return true;
    case DepthMapWidget::DisplayMode::Snr:
        if (!peak) return false;
        *value = peak->snr;
        return true;
    case DepthMapWidget::DisplayMode::Signal:
        if (!peak || !peak->hasSignal) return false;
        *value = peak->signal;
        return true;
    case DepthMapWidget::DisplayMode::Noise:
        if (!pixel.hasNoise) return false;
        *value = pixel.noise;
        return true;
    case DepthMapWidget::DisplayMode::Xtalk:
        if (!pixel.hasXtalk) return false;
        *value = pixel.xtalk;
        return true;
    }
    return false;
}
}

class DepthMapCanvas : public QWidget
{
public:
    explicit DepthMapCanvas(QWidget *parent = nullptr) : QWidget(parent),m_hoverTip(this)
    {
        setObjectName(QStringLiteral("depthMapCanvas"));
        setMouseTracking(true);
        setMinimumSize(ImagePresentation::MinimumViewportWidth, ImagePresentation::MinimumViewportHeight);
    }

    void setDisplayRotationDegrees(int degrees)
    {
        m_rotation = ImagePresentation::normalizedRotation(degrees);
        m_gridRect = {};
        refreshHover();
        update();
    }

    void setFrame(const Tmf8829Frame &frame, const QImage &image,
                  double minimum, double maximum,
                  DepthMapWidget::DisplayMode mode)
    {
        m_frame = frame;
        m_image = image;
        m_minimum = minimum;
        m_maximum = maximum;
        m_mode = mode;
        if(m_image.isNull())m_hoverTip.clear();else refreshHover();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#11161d"));
        if (m_image.isNull()) {
            painter.setPen(QColor("#8b949e"));
            painter.drawText(rect(), Qt::AlignCenter,
                             DepthMapWidget::tr("Disconnected / No data"));
            return;
        }
        const QRect available = rect().adjusted(8, 8, -66, -8);
        m_gridRect = ImagePresentation::fittedRect(available, m_image.size(), m_rotation);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        ImagePresentation::drawImage(painter, m_gridRect, m_image, m_rotation);
        painter.save();
        painter.translate(m_gridRect.center());
        painter.rotate(m_rotation);
        const QRectF grid = ImagePresentation::unrotatedRect(m_gridRect, m_rotation);
        painter.setPen(QPen(QColor(255, 255, 255, 55), 1));
        const double cellWidth = grid.width() / m_frame.width;
        const double cellHeight = grid.height() / m_frame.height;
        if (qMin(cellWidth, cellHeight) >= 5.0) {
            for (int column = 0; column <= m_frame.width; ++column) {
                const double x = grid.left() + column * cellWidth;
                painter.drawLine(QPointF(x, grid.top()), QPointF(x, grid.bottom()));
            }
            for (int row = 0; row <= m_frame.height; ++row) {
                const double y = grid.top() + row * cellHeight;
                painter.drawLine(QPointF(grid.left(), y), QPointF(grid.right(), y));
            }
        }
        painter.restore();

        const QRectF legend(m_gridRect.right() + 18, m_gridRect.top(),
                            16, m_gridRect.height());
        QLinearGradient gradient(legend.bottomLeft(), legend.topLeft());
        for (int index = 0; index <= 20; ++index) {
            const double position = index / 20.0;
            gradient.setColorAt(position, heatColor(position));
        }
        painter.fillRect(legend, gradient);
        painter.setPen(QColor("#c9d1d9"));
        painter.drawRect(legend);
        painter.drawText(QRectF(legend.right() + 4, legend.top() - 4, 28, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(m_maximum, 'f', m_mode == DepthMapWidget::DisplayMode::Depth ? 0 : 1));
        painter.drawText(QRectF(legend.right() + 4, legend.bottom() - 14, 28, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(m_minimum, 'f', m_mode == DepthMapWidget::DisplayMode::Depth ? 0 : 1));
    }

    void mouseMoveEvent(QMouseEvent *) override {refreshHover();}
    void leaveEvent(QEvent *) override {m_hoverTip.clear();}

private:
    void refreshHover()
    {
        if(!m_hoverTip.tracking())return;
        m_gridRect=ImagePresentation::fittedRect(rect().adjusted(8,8,-66,-8),m_image.size(),m_rotation);
        const auto position=m_hoverTip.position();
        if (m_image.isNull() || m_gridRect.isEmpty() || !m_gridRect.contains(position)) {
            m_hoverTip.hideText();
            return;
        }
        const QPointF source = ImagePresentation::sourcePosition(QPointF(
            (position.x() - m_gridRect.left()) / m_gridRect.width(),
            (position.y() - m_gridRect.top()) / m_gridRect.height()), m_rotation);
        const int column = qBound(0, int(source.x() * m_frame.width), m_frame.width - 1);
        const int row = qBound(0, int(source.y() * m_frame.height), m_frame.height - 1);
        const Tmf8829Pixel *pixel = m_frame.pixelAt(row, column);
        QStringList lines{DepthMapWidget::tr("Row %1, Column %2")
                              .arg(row).arg(column)};
        if (!pixel || !pixel->valid) {
            lines.append(DepthMapWidget::tr("Invalid / no return"));
        } else {
            const Tmf8829Peak *peak = pixel->nearestValidPeak();
            if (peak) {
                lines.append(DepthMapWidget::tr("Distance: %1 mm")
                                 .arg(peak->distanceMm, 0, 'f', 2));
                lines.append(DepthMapWidget::tr("SNR / confidence: %1")
                                 .arg(peak->snr));
                if (peak->hasSignal)
                    lines.append(DepthMapWidget::tr("Signal: %1")
                                     .arg(peak->signal));
            }
            if (pixel->hasNoise)
                lines.append(DepthMapWidget::tr("Noise: %1")
                                 .arg(pixel->noise));
            if (pixel->hasXtalk)
                lines.append(DepthMapWidget::tr("Xtalk: %1")
                                 .arg(pixel->xtalk));
        }
        const QString hoverText = lines.join('\n');
        m_hoverTip.showText(hoverText);
    }
    ImageHoverTip m_hoverTip;
    Tmf8829Frame m_frame;
    QImage m_image;
    QRectF m_gridRect;
    int m_rotation = 0;
    double m_minimum = 0.0;
    double m_maximum = 1.0;
    DepthMapWidget::DisplayMode m_mode
        = DepthMapWidget::DisplayMode::Depth;
};

DepthMapWidget::DepthMapWidget(QWidget *parent)
    : QWidget(parent)
    , m_canvas(new DepthMapCanvas(this))
    , m_statusLabel(new QLabel(this))
    , m_infoLabel(new QLabel(this))
    , m_sourceCaption(new QLabel(this))
    , m_resolutionCaption(new QLabel(this))
    , m_displayCaption(new QLabel(this))
    , m_sourceCombo(new QComboBox(this))
    , m_resolutionCombo(new QComboBox(this))
    , m_displayCombo(new QComboBox(this))
    , m_pauseCheck(new QCheckBox(this))
    , m_refreshTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("depthMapWidget"));
    m_statusLabel->setObjectName(QStringLiteral("depthMapStatusLabel"));
    m_statusLabel->setStyleSheet(QStringLiteral(
        "color:#d29922;font-weight:bold;padding:2px;"));
    m_infoLabel->setStyleSheet(QStringLiteral("color:#8b949e;"));
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_infoLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setMinimumWidth(0);
    m_infoLabel->setMinimumWidth(0);
    m_sourceCombo->addItem(QString(), static_cast<int>(SensorSourceMode::Mock));
    m_sourceCombo->addItem(QString(), static_cast<int>(SensorSourceMode::OfflinePlayback));
    m_sourceCombo->addItem(QString(), static_cast<int>(SensorSourceMode::Hardware));
    if (auto *model = qobject_cast<QStandardItemModel *>(m_sourceCombo->model())) {
        if (QStandardItem *hardware = model->item(2))
            hardware->setEnabled(false);
    }
    const struct Resolution { const char *name; int width; int height; } resolutions[] = {
        {"8 x 8", 8, 8}, {"16 x 16", 16, 16}, {"32 x 32", 32, 32},
        {"48 x 32", 48, 32}
    };
    for (const Resolution &resolution : resolutions) {
        m_resolutionCombo->addItem(QString::fromLatin1(resolution.name),
                                   QSize(resolution.width, resolution.height));
    }
    m_resolutionCombo->setCurrentIndex(1);
    for (int index = 0; index < 5; ++index)
        m_displayCombo->addItem(QString(), index);

    auto *sourceRow = new QWidget(this);
    m_sourceControls = sourceRow;
    sourceRow->setObjectName("depthSourceControls");
    auto *controls = new QHBoxLayout(sourceRow);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(ImagePresentation::Spacing);
    controls->addWidget(m_sourceCaption);
    controls->addWidget(m_sourceCombo);
    controls->addWidget(m_resolutionCaption);
    controls->addWidget(m_resolutionCombo);
    controls->addWidget(m_displayCaption);
    controls->addWidget(m_displayCombo);
    controls->addWidget(m_pauseCheck);
    controls->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ImagePresentation::Margin, ImagePresentation::Margin,
                               ImagePresentation::Margin, ImagePresentation::Margin);
    layout->setSpacing(ImagePresentation::Spacing);
    auto *settingsRow = new QWidget(this);
    settingsRow->setObjectName("imageSettingsRow");
    auto *settingsLayout = new QVBoxLayout(settingsRow);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(ImagePresentation::Spacing);
    settingsLayout->addWidget(sourceRow, 1);
    m_rvControls = new QWidget(settingsRow);
    auto *rvLayout = UiStyle::wrappingRow(m_rvControls, ImagePresentation::Spacing);
    m_rvModelCaption = new QLabel(m_rvControls);
    m_rvModelCaption->setObjectName("rvDepthModelLabel");
    m_rvModel = new QComboBox(m_rvControls);
    m_rvModel->setObjectName("rvDepthModel");
    for (const char *model : {"host_unidepth_wide_576_fp16_tof16", "board_litesr_near_full_host_contour_hd"})
        m_rvModel->addItem(QString::fromLatin1(model), QString::fromLatin1(model));
    m_rvResolutionCaption = new QLabel(m_rvControls);
    m_rvResolutionCaption->setObjectName("rvDepthResolutionLabel");
    m_rvResolution = new QComboBox(m_rvControls);
    m_rvResolution->setObjectName("rvDepthResolution");
    for (int width : {144, 288, 384, 576})
        m_rvResolution->addItem(QString(), width);
    m_rvOnBoard = new QCheckBox(m_rvControls);
    m_rvOnBoard->setObjectName("rvDepthOnBoard");
    m_rvMethod = new QComboBox(m_rvControls);
    m_rvMethod->setObjectName("rvDepthMethod");
    m_rvMethod->addItem(QString(), QStringLiteral("bilinear_valid"));
    m_rvMethod->addItem(QString(), QStringLiteral("fast_guided_guarded"));
    for (QComboBox *combo : {m_rvModel, m_rvResolution, m_rvMethod}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(combo == m_rvModel ? 9 : 10);
        combo->setMinimumWidth(0);
        combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    m_rvModel->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_rvModel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    rvLayout->addWidget(m_rvModelCaption);
    rvLayout->addWidget(m_rvModel);
    m_rvStart = new QPushButton(m_rvControls); m_rvStart->setObjectName("rvDepthStart");
    m_rvStop = new QPushButton(m_rvControls); m_rvStop->setObjectName("rvDepthStop");
    m_rvReconnect = new QPushButton(m_rvControls); m_rvReconnect->setObjectName("rvDepthReconnect");
    for (auto *button : {m_rvStart, m_rvStop, m_rvReconnect}) rvLayout->addWidget(button);
    rvLayout->addWidget(m_rvResolutionCaption);
    rvLayout->addWidget(m_rvResolution);
    rvLayout->addWidget(m_rvOnBoard);
    rvLayout->addWidget(m_rvMethod);
    settingsLayout->addWidget(m_rvControls, 1);
    m_rvControls->hide();
    connect(m_rvModel, &QComboBox::activated, this, [this](int) {
        m_sessionControlsKey.clear();
        if(m_session)m_session->selectImagingScheme(m_rvModel->currentData().toString(),m_session->dtofCandidateSettings());
        syncSessionControls();
    });
    connect(m_rvResolution, &QComboBox::activated, this, [this](int) {
        if (m_session) m_session->setDepthWidth(m_rvResolution->currentData().toInt());
    });
    connect(m_rvOnBoard, &QCheckBox::clicked, this, [this](bool checked) {
        if (m_session) m_session->setDepthLocation(checked ? QStringLiteral("rv1103") : QStringLiteral("host"));
    });
    connect(m_rvMethod, &QComboBox::activated, this, [this](int) {
        if (m_session) m_session->setDepthMethod(m_rvMethod->currentData().toString());
    });
    layout->addWidget(settingsRow);
    layout->addWidget(m_canvas, 1);
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
    m_liteSrCanvas=new RvDepthView(m_depthTabs);m_liteSrCanvas->setObjectName("rvLiteSrDepthCanvas");
    m_liteSrCanvas->setRadialMode();m_liteSrCanvas->setNativeGridSize(QSize(48,32));m_depthTabs->addTab(m_liteSrCanvas,QString());
    setProperty("dtofComputeLocation","rv1103");
    m_depthTabs->tabBar()->hide();m_depthTabs->hide();layout->addWidget(m_depthTabs,1);
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
    m_resolutionCombo->setObjectName("depthMapResolutionCombo");
    auto *statusRow = new QWidget(this);
    statusRow->setObjectName("imageStatusRow");
    auto *saveControls = UiStyle::wrappingRow(statusRow, ImagePresentation::Spacing);
    m_canvas->setDisplayRotationDegrees(displayRotationDegrees());
    m_rvCanvas->setDisplayRotationDegrees(displayRotationDegrees());
    m_assistCanvas->setDisplayRotationDegrees(displayRotationDegrees());
    m_candidateCanvas->setDisplayRotationDegrees(displayRotationDegrees());m_liteSrCanvas->setDisplayRotationDegrees(displayRotationDegrees());
    connect(m_imageSettings.data(), &ImageDisplaySettings::rotationChanged, this, [this](int degrees) {
        m_canvas->setDisplayRotationDegrees(degrees);
        m_rvCanvas->setDisplayRotationDegrees(degrees);
        m_assistCanvas->setDisplayRotationDegrees(degrees);
        m_candidateCanvas->setDisplayRotationDegrees(degrees);m_liteSrCanvas->setDisplayRotationDegrees(degrees);
    });
    m_saveButton = new QPushButton(this);m_saveButton->setObjectName("depthMapSave");
    m_saveCamera = new QCheckBox(this);m_saveCamera->setObjectName("depthMapSaveCamera");
    m_saveCamera->setChecked(QSettings().value("depth/saveCameraImage",false).toBool());
    connect(m_saveCamera,&QCheckBox::toggled,this,[](bool checked){QSettings().setValue("depth/saveCameraImage",checked);});
    m_saveButton->setEnabled(false);
    m_colorOutput=new QComboBox(m_rvControls);m_colorOutput->setObjectName("rvColorOutput");
    m_colorOutput->addItem(QString(),false);m_colorOutput->addItem(QString(),true);
    m_colorOutput->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    rvLayout->addWidget(m_colorOutput);
    connect(m_colorOutput,&QComboBox::activated,this,[this]{
        if(m_session)m_session->setColorRawPreview(m_colorOutput->currentData().toBool());
        m_sessionControlsKey.clear();syncSessionControls();
    });
    saveControls->addWidget(m_statusLabel);saveControls->addWidget(m_infoLabel);
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
        if(m_rvMode){
            for(auto *view:{m_rvCanvas,m_assistCanvas,m_candidateCanvas,m_liteSrCanvas})
                if(view&&view->isVisible())fps=view->displayedFps();
        }else if(!m_pauseCheck->isChecked()&&m_fpsTimer.elapsed()<1000)fps=m_fps;
        fpsLabel->setText(QStringLiteral("FPS: %1").arg(fps,0,'f',1));
    });
    fpsRefresh->start();
    m_liteSrWhiteCaption=new QLabel(m_rvControls);rvLayout->addWidget(m_liteSrWhiteCaption);
    m_liteSrWhite=new QSpinBox(m_rvControls);m_liteSrWhite->setObjectName("rvLiteSrWhiteCounts");
    m_liteSrWhite->setRange(1,8388352);m_liteSrWhite->setKeyboardTracking(false);m_liteSrWhite->setValue(512);rvLayout->addWidget(m_liteSrWhite);
    m_liteSrOrientation=new QComboBox(m_rvControls);m_liteSrOrientation->setObjectName("rvLiteSrInputRotation");
    m_liteSrOrientation->addItem(QString(),0);m_liteSrOrientation->addItem(QString(),180);rvLayout->addWidget(m_liteSrOrientation);
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
    connect(m_liteSrWhite,&QSpinBox::valueChanged,this,[this](int value){if(m_session&&m_session->liteSrDepth())m_session->liteSrDepth()->setWhiteCounts(value);});
    connect(m_liteSrOrientation,&QComboBox::currentIndexChanged,this,[this]{if(m_session&&m_session->liteSrDepth())m_session->liteSrDepth()->setInputRotation(m_liteSrOrientation->currentData().toInt());});
    layout->addWidget(statusRow);
    connect(m_saveButton,&QPushButton::clicked,this,&DepthMapWidget::saveCurrentFrame);

    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this, [this]() {
        emit sourceModeRequested(static_cast<SensorSourceMode>(
            m_sourceCombo->currentData().toInt()));
    });
    connect(m_resolutionCombo, &QComboBox::currentIndexChanged, this, [this]() {
        const QSize size = m_resolutionCombo->currentData().toSize();
        emit mockResolutionRequested(size.width(), size.height());
    });
    connect(m_pauseCheck, &QCheckBox::toggled,
            this, &DepthMapWidget::pausedChanged);
    m_displayCombo->setObjectName(QStringLiteral("depthMapDisplayCombo"));
    connect(m_displayCombo, &QComboBox::currentIndexChanged,
            this, &DepthMapWidget::renderCurrentFrame);
    connect(m_refreshTimer, &QTimer::timeout,
            this, &DepthMapWidget::pollLatestFrame);
    m_refreshTimer->start(33);
    m_fpsTimer.start();
    syncDisplayColors();
    retranslateUi();
    setSourceState(SensorSourceMode::None, SensorConnectionState::NoData,
                   tr("Disconnected / No data"));
    syncSessionControls();
}

void DepthMapWidget::setFrameBuffer(
    const QSharedPointer<LatestSensorFrameBuffer<Tmf8829Frame>> &buffer)
{
    m_snapshot = {};m_saveButton->setEnabled(false);
    m_buffer = buffer;
    m_currentFrame = Tmf8829Frame();
    m_framesSinceFpsUpdate = 0;
    m_droppedFrames = 0;
    m_fps = 0.0;
    m_fpsTimer.restart();
    m_canvas->setFrame({}, {}, 0.0, 1.0, DisplayMode::Depth);
    m_canvas->setProperty("lastHoverText", QString());
    m_infoLabel->clear();
}

void DepthMapWidget::setSourceState(
    SensorSourceMode mode, SensorConnectionState state, const QString &detail)
{
    m_sourceMode = mode;
    m_connectionState = state;
    m_statusDetail = detail;
    QString prefix;
    if (mode == SensorSourceMode::Mock)
        prefix = tr("Depth Map — MOCK / SIMULATED");
    else if (mode == SensorSourceMode::OfflinePlayback)
        prefix = tr("Depth Map — OFFLINE PLAYBACK");
    else if (mode == SensorSourceMode::Hardware && state == SensorConnectionState::Connected)
        prefix = tr("Depth Map — CONNECTED");
    else
        prefix = tr("Depth Map — DISCONNECTED / NO DATA");
    m_statusLabel->setText(prefix + QStringLiteral(" | ") + detail);
    m_statusLabel->setToolTip(m_statusLabel->text());
}

void DepthMapWidget::setSelectedSourceMode(SensorSourceMode mode)
{
    const int index = m_sourceCombo->findData(static_cast<int>(mode));
    if (index >= 0) {
        const QSignalBlocker blocker(m_sourceCombo);
        m_sourceCombo->setCurrentIndex(index);
    }
}

QString DepthMapWidget::statusText() const
{
    return m_statusLabel->text();
}

int DepthMapWidget::selectedWidth() const
{
    if(m_rvMode&&m_depthTabs->currentIndex()==3)return 256;
    if(m_rvMode&&m_depthTabs->currentIndex()==2)return RvDtofCandidates::outputWidth(m_session?m_session->dtofCandidateMode():RvDtofCandidates::FastDepth);
    if(m_rvMode&&m_depthTabs->currentIndex()==1)return 576;
    if (m_rvMode)
        return m_session ? m_session->depthWidth() : m_snapshot.metadata["width"].toInt(144);
    return m_resolutionCombo->currentData().toSize().width();
}

int DepthMapWidget::selectedHeight() const
{
    if(m_rvMode&&m_depthTabs->currentIndex()==3)return 256;
    if(m_rvMode&&m_depthTabs->currentIndex()==2)return RvDtofCandidates::outputHeight(m_session?m_session->dtofCandidateMode():RvDtofCandidates::FastDepth);
    if(m_rvMode&&m_depthTabs->currentIndex()==1)return 324;
    if (m_rvMode) {
        if (!m_session) return m_snapshot.metadata["height"].toInt(81);
        switch (m_session->depthWidth()) {
        case 288: return 162;
        case 384: return 256;
        case 576: return 324;
        default: return 81;
        }
    }
    return m_resolutionCombo->currentData().toSize().height();
}

int DepthMapWidget::displayRotationDegrees() const
{
    return m_imageSettings->rotationDegrees();
}

void DepthMapWidget::setDisplayRotationDegrees(int degrees)
{
    m_imageSettings->setRotationDegrees(degrees);
}

QImage DepthMapWidget::renderHeatmap(
    const Tmf8829Frame &frame, DisplayMode mode, double *minimum, double *maximum)
{
    QString reason;
    if (!frame.isValid(&reason))
        return {};
    double low = std::numeric_limits<double>::max();
    double high = std::numeric_limits<double>::lowest();
    for (const Tmf8829Pixel &pixel : frame.pixels) {
        double value = 0.0;
        if (pixelValue(pixel, mode, &value)) {
            low = qMin(low, value);
            high = qMax(high, value);
        }
    }
    if (low == std::numeric_limits<double>::max()) {
        // A valid frame with no returns is still a frame, not a disconnected
        // source. Render the invalid-cell checkerboard with a neutral legend.
        low = 0.0;
        high = 1.0;
    }
    if (qFuzzyCompare(low, high))
        high = low + 1.0;
    QImage image(frame.width, frame.height, QImage::Format_RGB32);
    for (int row = 0; row < frame.height; ++row) {
        QRgb *scan = reinterpret_cast<QRgb *>(image.scanLine(row));
        for (int column = 0; column < frame.width; ++column) {
            const Tmf8829Pixel &pixel = frame.pixels.at(row * frame.width + column);
            double value = 0.0;
            if (!pixelValue(pixel, mode, &value)) {
                scan[column] = ((row + column) % 2 == 0
                    ? QColor("#343a40") : QColor("#161b22")).rgb();
            } else {
                scan[column] = heatColor((value - low) / (high - low)).rgb();
            }
        }
    }
    if (minimum) *minimum = low;
    if (maximum) *maximum = high;
    return image;
}

void DepthMapWidget::pollLatestFrame()
{
    if (m_rvMode || !m_buffer || m_pauseCheck->isChecked())
        return;
    Tmf8829Frame frame;
    quint64 dropped = 0;
    if (!m_buffer->takeLatest(&frame, nullptr, &dropped))
        return;
    m_droppedFrames += dropped;
    m_currentFrame = frame;
    ++m_framesSinceFpsUpdate;
    if (m_fpsTimer.elapsed() >= 1000) {
        m_fps = m_framesSinceFpsUpdate * 1000.0 / m_fpsTimer.elapsed();
        m_framesSinceFpsUpdate = 0;
        m_fpsTimer.restart();
    }
    renderCurrentFrame();
}

void DepthMapWidget::renderCurrentFrame()
{
    if(m_rvMode)return;
    if (m_currentFrame.pixels.isEmpty())
        return;
    const Tmf8829Frame &frame = m_currentFrame;
    const DisplayMode mode = static_cast<DisplayMode>(
        m_displayCombo->currentData().toInt());
    double low = 0.0;
    double high = 0.0;
    const QImage image = renderHeatmap(frame, mode, &low, &high);
    m_canvas->setFrame(frame, image, low, high, mode);
    m_saveButton->setEnabled(true);

    double minimumDistance = std::numeric_limits<double>::max();
    double maximumDistance = std::numeric_limits<double>::lowest();
    for (const Tmf8829Pixel &pixel : frame.pixels) {
        if (!pixel.valid)
            continue;
        if (const Tmf8829Peak *peak = pixel.nearestValidPeak()) {
            minimumDistance = qMin(minimumDistance, peak->distanceMm);
            maximumDistance = qMax(maximumDistance, peak->distanceMm);
        }
    }
    const QString time = QDateTime::fromMSecsSinceEpoch(
        frame.timestampNsSinceEpoch / 1000000LL).toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString validRange = minimumDistance == std::numeric_limits<double>::max()
        ? tr("none")
        : tr("%1–%2 mm").arg(minimumDistance, 0, 'f', 1)
                              .arg(maximumDistance, 0, 'f', 1);
    m_infoLabel->setText(
        tr("%1 x %2 | %3 fps | Frame %4 | %5 | Valid depth %6 | Dropped %7")
            .arg(frame.width).arg(frame.height).arg(m_fps, 0, 'f', 1)
            .arg(frame.frameNumber).arg(time).arg(validRange)
            .arg(m_droppedFrames));
    m_infoLabel->setToolTip(m_infoLabel->text());
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
    m_saveButton->setText(m_rvMode ? tr("Save depth map") : tr("Save"));
    m_saveCamera->setText(tr("Save camera image too"));
    m_rvModelCaption->setText(tr("Imaging scheme:"));
    m_rvStart->setText(tr("Start"));
    m_rvStop->setText(tr("Stop"));
    m_rvReconnect->setText(tr("Reconnect now"));
    for (auto *button : {m_rvStart, m_rvStop, m_rvReconnect})
        button->setToolTip(tr("Start, Stop and Reconnect apply to the shared camera + depth stream."));
    auto label=[this](const char* id,const QString& text){m_rvModel->setItemText(m_rvModel->findData(id),text);};
    m_colorOutput->setItemText(0,tr("Display: filtered depth"));
    m_colorOutput->setItemText(1,tr("Display: raw network output"));
    m_colorOutput->setAccessibleName(tr("Depth output display"));
    m_colorOutput->setToolTip(tr("Raw prediction without output masks or range rejection. Color limits saturate; nonfinite values are magenta. Input preprocessing is unchanged."));
    label("host_unidepth_wide_576_fp16_tof16",tr("Host-assisted inference"));
    m_rvModel->setItemData(m_rvModel->findData("host_unidepth_wide_576_fp16_tof16"),tr("Requires saved 16 x 16 ordinary or high accuracy ToF settings with one target and Noise off. Does not change saved sensor settings."),Qt::ToolTipRole);
    m_depthTabs->setTabText(0,tr("Board depth"));m_depthTabs->setTabText(1,tr("Host assisted depth"));
    m_depthTabs->setTabText(2,tr("Board depth"));m_depthTabs->setTabText(3,tr("Host depth preview"));
    label("board_litesr_near_full_host_contour_hd",tr("On-board inference"));
    for(int i=0;i<m_rvModel->count();++i){
        const auto id=m_rvModel->itemData(i).toString();
        if(id.startsWith("board_litesr_"))m_rvModel->setItemData(i,tr("On-board INT8 inference and all depth processing run on RV1103. Same-frame native D/I, 256 x 256 output displayed at the native ToF aspect ratio. Positive near returns are interpolated separately below the trained 10 cm minimum."),Qt::ToolTipRole);
    }
    m_liteSrRepairButton->setText(tr("Repair settings..."));
    m_boardLiteSrHostContour->setText(tr("Host contour optimization"));
    m_boardLiteSrHostContour->setToolTip(tr("RV1103 runs the full on-board INT8 model. The PC refines its depth with matched native input; no host neural network is started. The separate board contour option keeps all processing on RV1103."));
    m_rvModel->setItemData(m_rvModel->findData("board_litesr_near_full_host_contour_hd"),tr("Official near-depth post-training, 1-140 cm input and output. INT8 inference runs on RV1103; matched contour refinement and display run on the PC. Real near-depth accuracy is unverified. RGB is not a model input."),Qt::ToolTipRole);
    m_liteSrWhiteCaption->setText(tr("Counts white:"));
    m_liteSrWhite->setToolTip(tr("Fixed experimental mapping: intensity = min(counts / white, 1). Shared across candidates; not per-frame auto range or physical calibration."));
    m_liteSrOrientation->setItemText(0,tr("Native D/I: original"));m_liteSrOrientation->setItemText(1,tr("Native D/I: rotate 180 deg"));
    m_liteSrOrientation->setToolTip(tr("Rotate both native D and I together for inference, then restore output. Independent of RGB and display rotation."));
    m_rvResolution->setItemText(0, tr("Raw 144×81"));
    m_rvResolution->setItemText(1, QStringLiteral("288×162"));
    m_rvResolution->setItemText(2, QStringLiteral("384×256"));
    m_rvResolution->setItemText(3, tr("576×324 (Host)"));
    m_rvOnBoard->setText(tr("On RV1103"));
    m_rvOnBoard->setAccessibleName(tr("Process on RV1103"));
    m_rvMethod->setItemText(0, tr("Bilinear"));
    m_rvMethod->setItemText(1, tr("Guided"));
    m_rvModel->setAccessibleName(tr("Imaging scheme:"));
    m_rvResolution->setAccessibleName(tr("Depth output:"));
    m_rvMethod->setAccessibleName(tr("Depth enhancement method"));
    m_rvModel->setToolTip(tr("Select host-assisted or on-board depth inference."));
    m_rvResolution->setToolTip(tr("Depth output size is independent of camera size. The 2304×1296 camera-only mode has no depth; F models support original depth only."));
    m_rvOnBoard->setToolTip(tr("Process enhanced depth on RV1103 when checked, or on this computer when unchecked. 576×324 depth requires processing on this computer."));
    m_rvMethod->setToolTip(tr("Bilinear uses valid depth values. Guided is a candidate requiring a 576×324 camera and processing on this computer."));
    m_sourceCaption->setText(tr("Source"));
    m_resolutionCaption->setText(tr("Resolution"));
    m_displayCaption->setText(tr("Display"));
    m_sourceCombo->setItemText(0, tr("Mock"));
    m_sourceCombo->setItemText(1, tr("Offline JSON..."));
    m_sourceCombo->setItemText(2, tr("Hardware (Unavailable)"));
    m_displayCombo->setItemText(0, tr("Depth"));
    m_displayCombo->setItemText(1, tr("SNR / Confidence"));
    m_displayCombo->setItemText(2, tr("Signal"));
    m_displayCombo->setItemText(3, tr("Noise"));
    m_displayCombo->setItemText(4, tr("Xtalk"));
    m_pauseCheck->setText(tr("Pause"));
    if(m_rvMode)m_sourceCombo->setItemText(2,QStringLiteral("RV1103"));
    if (m_session) m_session->retranslateState();
    m_sessionControlsKey.clear();syncSessionControls();
}

void DepthMapWidget::setRvDepthMode(bool enabled)
{
    m_saveButton->setText(enabled ? tr("Save depth map") : tr("Save"));
    if(!enabled)cancelImagingSwitchUi();
    if(!enabled&&m_session)m_session->setAssistDepthEnabled(false);
    m_sourceControls->setVisible(!enabled);
    m_rvControls->setVisible(enabled);
    m_statusLabel->setVisible(!enabled);
    m_infoLabel->setVisible(!enabled);
    QWidget *inactiveControls[] = {m_sourceCaption, m_sourceCombo, m_displayCaption,
                                   m_displayCombo, m_pauseCheck, m_resolutionCaption,
                                   m_resolutionCombo};
    for (QWidget *control : inactiveControls)
        control->setVisible(!enabled);
    if(!enabled)clearCandidate(tr("RV session unavailable"));
    m_rvMode=enabled;m_canvas->setVisible(!enabled);m_depthTabs->setVisible(enabled);
    const QSignalBlocker a(m_resolutionCombo),b(m_displayCombo),c(m_sourceCombo);
    m_displayCombo->setCurrentIndex(0);
    m_resolutionCombo->setEnabled(!enabled);m_displayCombo->setEnabled(!enabled);m_sourceCombo->setEnabled(!enabled);
    m_pauseCheck->setEnabled(!enabled);m_pauseCheck->setChecked(false);
    if(enabled){m_sourceCombo->setCurrentIndex(2);m_sourceCombo->setItemText(2,"RV1103");clearRvDepth(tr("Start the camera stream to receive depth"));}
    else m_sourceCombo->setItemText(2, tr("Hardware (Unavailable)"));
    syncSessionControls();
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
    if(!m_rvMode)return;
    m_switchUiActive=true;m_switchUiFinishing=false;m_switchUiScheme=scheme;
    m_switchEstimatedMs=std::max(1,estimatedMs);m_switchUiClock.restart();m_switchFinishClock.invalidate();
    m_switchProgress->setValue(0);
    m_snapshot={};m_assistSnapshot={};m_candidateSnapshot={};m_liteSrSnapshot={};
    for(auto *view:{m_rvCanvas,m_assistCanvas,m_candidateCanvas,m_liteSrCanvas}){
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
        if(m_session->liteSrDepth())disconnect(m_session->liteSrDepth(),nullptr,this,nullptr);
    }
    // A recovery window belongs to one source. Rebinding never carries the
    // previous source's displayed pixels into the new session.
    m_rvCanvas->clear();m_assistCanvas->clear();m_candidateCanvas->clear();
    m_session = controller;
    clearRvDepth(tr("RV session changed"));
    clearAssistDepth(tr("RV session changed"));clearLiteSrDepth(tr("RV session changed"));
    if (m_session) {
        setRvDepthMode(true);
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
        connect(m_session,&RvSessionController::depthStreamCleared,this,&DepthMapWidget::clearLiteSrDepth);
        connect(m_session->liteSrDepth(),&RvLiteSrDepth::changed,this,&DepthMapWidget::syncSessionControls);
        connect(m_session->liteSrDepth(),&RvLiteSrDepth::resultReady,this,&DepthMapWidget::acceptLiteSrDepth);
        connect(m_session->liteSrDepth(),&RvLiteSrDepth::cleared,this,&DepthMapWidget::clearLiteSrDepth);
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
            clearAssistDepth(tr("RV session unavailable"));clearLiteSrDepth(tr("RV session unavailable"));
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
    const bool hasDepth = m_session && m_session->cameraWidth() != 2304
        && m_session->dtofCandidateMode()<0;
    const int width = m_session ? m_session->depthWidth() : 144;
    const QString location = m_session ? m_session->depthLocation() : QStringLiteral("none");
    const QString method = m_session ? m_session->depthMethod() : QStringLiteral("none");
    const bool assisting=m_rvMode&&m_session&&(m_session->cameraWidth()==576||m_session->hostHdAssist())&&m_session->assistDepthEnabled();
    const bool candidate=m_session&&m_session->dtofCandidateMode()>=0&&!RvDtofCandidates::isCaptureOnly(m_session->dtofCandidateMode());
    const int boardMode=m_session?m_session->dtofCandidateMode():-1;
    auto *liteDepth=m_session?m_session->liteSrDepth():nullptr;
    const bool lite=m_rvMode&&liteDepth&&liteDepth->selected();
    const bool boardFull=RvDtofCandidates::isLiteSrFull(boardMode)&&!RvDtofCandidates::isCaptureOnly(boardMode);
    setProperty("dtofComputeLocation",lite?"host_native":boardFull&&m_session->boardLiteSrHostContourEnabled()?"rv1103+host_cpu":"rv1103");
    m_boardLiteSrHostContour->setVisible(boardFull);
    {const QSignalBlocker blocker(m_boardLiteSrHostContour);m_boardLiteSrHostContour->setChecked(m_session&&m_session->boardLiteSrHostContourEnabled());}
    m_boardLiteSrHostContour->setEnabled(m_session&&!m_session->busy()&&!RvDtofCandidates::isLiteSrHd(boardMode)&&!LiteSrBoardContract::isHighResolution(boardMode));
    m_liteSrRepairButton->setVisible((lite&&liteDepth->model()=="area_full_contour")||boardMode==RvDtofCandidates::LiteSrContour||(boardFull&&m_session->boardLiteSrHostContourEnabled()));
    m_liteSrRepairButton->setEnabled(m_session&&!m_session->busy());
    m_liteSrRepairButton->setText(boardFull&&m_session->boardLiteSrHostContourEnabled()?tr("Contour settings..."):tr("Repair settings..."));
    const QStringList key{QString::number(m_session&&m_session->colorRawPreview()),QString::number(m_rvMode),QString::number(bool(m_session)),QString::number(width),location,method,
        m_session?m_session->depthModel():QString(),QString::number(m_session?m_session->cameraWidth():0),
        QString::number(assisting),QString::number(boardMode),QString::number(m_session&&m_session->busy()),QString::number(m_switchUiActive),
        QString::number(m_session&&m_session->controlledSource()),QString::number(m_session?m_session->assistLoadingState():0),
        m_session?m_session->assistHostModel():QString(),QString::number(lite),liteDepth?liteDepth->model():QString(),QString::number(liteDepth?liteDepth->whiteCounts():0),QString::number(liteDepth?liteDepth->inputRotation():0)};
    // Frame statistics change much more often than configuration. Keep status
    // current without rebuilding combo-box permissions/tooltips on every frame.
    if(key!=m_sessionControlsKey){
    m_sessionControlsKey=key;
    const QSignalBlocker modelBlocker(m_rvModel),resolutionBlocker(m_rvResolution),
        boardBlocker(m_rvOnBoard),methodBlocker(m_rvMethod);
    const bool offerAssist=m_rvMode&&m_session&&(m_session->cameraWidth()==576||m_session->hostHdAssist()||RvDtofCandidates::isLiteSrHd(m_session->dtofCandidateMode()));
    m_depthTabs->setCurrentIndex(lite?3:candidate?2:assisting?1:0);
    for(auto* control:std::initializer_list<QWidget*>{m_liteSrWhiteCaption,m_liteSrWhite,m_liteSrOrientation})control->setVisible(lite);
    if(liteDepth){const QSignalBlocker a(m_liteSrWhite),b(m_liteSrOrientation);m_liteSrWhite->setValue(liteDepth->whiteCounts());m_liteSrOrientation->setCurrentIndex(m_liteSrOrientation->findData(liteDepth->inputRotation()));}
    const int loading=assisting&&!m_switchUiActive?m_session->assistLoadingState():0;
    m_assistCanvas->setLoadingState(loading==2&&m_assistCanvas->hasFrame()?0:loading);
    m_colorOutput->setVisible(m_rvMode&&assisting&&m_session&&RvCameraView::isColorLiteSR(m_session->assistHostModel()));
    m_colorOutput->setEnabled(m_session&&!m_session->busy()&&!m_switchUiActive);
    {const QSignalBlocker blocker(m_colorOutput);m_colorOutput->setCurrentIndex(m_session&&m_session->colorRawPreview()?1:0);}
    m_rvModelCaption->setText(tr("Imaging scheme:"));
    m_rvResolutionCaption->setText(tr("Board depth output:"));
    for(QWidget* w:std::initializer_list<QWidget*>{m_rvResolutionCaption,m_rvResolution,m_rvOnBoard,m_rvMethod})w->hide();
    const bool configurable = m_rvMode && m_session && !m_session->busy()&&!m_switchUiActive
        && m_session->controlledSource();
    const QString hostScheme=m_session&&m_session->assistHostModel()!="unidepth"?"host_"+m_session->assistHostModel():QStringLiteral("host_assisted");
    m_rvModel->setCurrentIndex(lite?m_rvModel->findData("host_litesr_"+m_session->liteSrDepth()->model()):candidate?m_rvModel->findData(RvDtofCandidates::name(boardMode)):assisting?m_rvModel->findData(hostScheme):m_rvModel->findData(QStringLiteral("host_assisted")));
    if(auto* items=qobject_cast<QStandardItemModel*>(m_rvModel->model())){
        for(int i=0;i<m_rvModel->count();++i){
            const auto id=m_rvModel->itemData(i).toString();
            if(id.startsWith("board_litesr_"))items->item(i)->setEnabled(configurable);
            else if(id.startsWith("host_litesr_")){
                const bool available=RvLiteSrDepth::runtimeAvailable(id.mid(12));
                items->item(i)->setEnabled(configurable&&offerAssist&&available);
                items->item(i)->setToolTip(available?tr("Same-frame native ToF and photon counts, host FP32 inference, 1-140 cm. No near-range repair. Experimental signal mapping; physical accuracy is unverified."):RvAssistDepth::tr("Selected host model runtime is missing; prepare the optional runtime first"));
            }
            else {
                const QString model=id=="host_assisted"?QStringLiteral("unidepth"):id.mid(5);
                const bool available=RvAssistDepth::hostModelAvailable(model);
                items->item(i)->setEnabled(offerAssist&&available);
                items->item(i)->setToolTip(available?QString():RvAssistDepth::tr("Selected host model runtime is missing; prepare the optional runtime first"));
            }
        }
    }
    m_rvResolution->setCurrentIndex(m_rvResolution->findData(width));
    m_rvOnBoard->setChecked(candidate||location == QStringLiteral("rv1103"));
    m_rvMethod->setCurrentIndex(m_rvMethod->findData(
        method == QStringLiteral("fast_guided_guarded") ? method : QStringLiteral("bilinear_valid")));
    m_rvModel->setEnabled(configurable);
    m_rvResolution->setEnabled(configurable && hasDepth);
    if (auto *items = qobject_cast<QStandardItemModel *>(m_rvResolution->model())) {
        for (int i = 0; i < m_rvResolution->count(); ++i) {
            if (QStandardItem *item = items->item(i)) {
                const bool allowed = configurable && m_session->canSelectDepthWidth(m_rvResolution->itemData(i).toInt());
                item->setEnabled(allowed);
                item->setToolTip(allowed ? m_rvResolution->itemText(i)
                    : tr("Unavailable for the current model and processing settings"));
            }
        }
    }
    const bool enhanced = configurable && hasDepth && width != 144;
    m_rvOnBoard->setEnabled(enhanced && m_session->canSelectDepthLocation(
        m_rvOnBoard->isChecked() ? QStringLiteral("host") : QStringLiteral("rv1103")));
    m_rvMethod->setEnabled(enhanced);
    if (auto *items = qobject_cast<QStandardItemModel *>(m_rvMethod->model())) {
        for (int i = 0; i < m_rvMethod->count(); ++i) {
            if (QStandardItem *item = items->item(i)) {
                const bool allowed = enhanced && m_session->canSelectDepthMethod(m_rvMethod->itemData(i).toString());
                item->setEnabled(allowed);
                item->setToolTip(allowed ? m_rvMethod->itemText(i)
                    : tr("Unavailable for the current camera and processing location"));
            }
        }
    }
    }
    m_liteSrCanvas->setLoadingState(lite?m_session->liteSrDepth()->loadingState():0);
    updateRvPresentation();
}
void DepthMapWidget::syncDisplayColors()
{
    for(auto *view:{m_rvCanvas,m_assistCanvas,m_candidateCanvas,m_liteSrCanvas}){
        view->setDisplayRange(m_imageSettings->colorMinimum(),m_imageSettings->colorMaximum());
        view->setBlackOutsideRange(m_imageSettings->blackOutsideRange());
    }
    // Expired pixels cannot be recolored after their metric samples are revoked.
    if(m_candidateCanvas->frameHeld())clearCandidate(tr("No fresh dToF data"));
}
void DepthMapWidget::acceptRvDepth(const QByteArray& depth,const QJsonObject& m)
{
    if(!m_rvMode)return;
    if(!m_rvCanvas->setDepth(depth,m)){clearRvDepth(tr("Invalid depth packet"));return;}
    m_snapshot={depth,{},{},m,QDateTime::currentDateTimeUtc()};
    updateRvPresentation();
}
void DepthMapWidget::clearRvDepth(const QString& reason)
{
    clearCandidate(reason);
    m_snapshot={};
    if(!m_rvMode)return;
    const bool holding=m_session&&m_session->retainingRecoveryDisplay();
    if(holding)m_rvCanvas->holdFrame();else m_rvCanvas->clear(UiStyle::depthErrorText(reason));
    updateRvPresentation();m_statusLabel->setText(holding?tr("Restoring camera and depth..."):tr("RV1103 depth: no current data")+" | "+UiStyle::depthErrorText(reason));m_statusLabel->setToolTip(m_statusLabel->text());
}

void DepthMapWidget::acceptAssistSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,const RvTofFrame &,const QByteArray &diagnostic)
{
    if(!m_rvMode||!m_session||!m_session->assistDepthEnabled())return;
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
    const bool lite=m_depthTabs->currentIndex()==3;
    if(!m_rvMode||!m_saveButton)return;

    m_saveCamera->setEnabled(!candidate && !lite);
    const auto &snapshot = candidate ? m_candidateSnapshot : lite ? m_liteSrSnapshot
        : m_depthTabs->currentIndex()==1 ? m_assistSnapshot : m_snapshot;
    m_saveButton->setEnabled(!m_switchUiActive
        && !(m_session && m_session->retainingRecoveryDisplay()) && !snapshot.metresFloat32LE.isEmpty());
}

void DepthMapWidget::acceptRvSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera)
{
    acceptRvDepth(depth,metadata);
    if(m_rvMode && m_rvCanvas->hasFrame())m_snapshot.cameraImage=camera;
}

DepthSnapshot DepthMapWidget::snapshot() const
{
    if(m_rvMode&&m_depthTabs->currentIndex()==3){
        auto result=m_liteSrSnapshot;if(result.metresFloat32LE.isEmpty())return {};
        result.depthImage=m_liteSrCanvas->snapshotImage();result.metadata["display_rotation_degrees"]=displayRotationDegrees();
        result.metadata["display_minimum_m"]=m_imageSettings->colorMinimum();result.metadata["display_maximum_m"]=m_imageSettings->colorMaximum();
        result.metadata["display_out_of_range_black"]=m_imageSettings->blackOutsideRange();return result;
    }
    if(m_rvMode&&m_depthTabs->currentIndex()==2){
        auto result=m_candidateSnapshot;
        if(!result.metresFloat32LE.isEmpty()){
            result.depthImage=m_candidateCanvas->snapshotImage();
            result.metadata["display_minimum_m"]=m_imageSettings->colorMinimum();
            result.metadata["display_maximum_m"]=m_imageSettings->colorMaximum();
            result.metadata["display_out_of_range_black"]=m_imageSettings->blackOutsideRange();
        }
        return result;
    }
    if(m_rvMode){auto result=m_depthTabs->currentIndex()==1?m_assistSnapshot:m_snapshot;
        if(result.metresFloat32LE.isEmpty())return {};
        result.depthImage=m_depthTabs->currentIndex()==1?m_assistCanvas->snapshotImage():m_rvCanvas->snapshotImage();
        result.metadata["display_minimum_m"]=m_rvCanvas->displayMinimum();
        result.metadata["display_maximum_m"]=m_rvCanvas->displayMaximum();
        result.metadata["display_out_of_range_black"]=result.metadata.value("color_raw_preview")==true?false:m_rvCanvas->blackOutsideRange();return result;}
    DepthSnapshot result;
    if(m_currentFrame.pixels.isEmpty())return result;
    result.depthImage=renderHeatmap(m_currentFrame,DisplayMode::Depth);
    result.capturedUtc=QDateTime::currentDateTimeUtc();
    result.metadata=QJsonObject{{"source",m_sourceMode==SensorSourceMode::Mock?"MOCK / SIMULATED":"OFFLINE PLAYBACK"},
        {"source_timestamp_ns",QString::number(m_currentFrame.timestampNsSinceEpoch)},
        {"frame_id",double(m_currentFrame.frameNumber)}};
    result.metresFloat32LE.resize(m_currentFrame.width*m_currentFrame.height*4);
    for(int i=0;i<m_currentFrame.pixels.size();++i){
        const auto &pixel=m_currentFrame.pixels[i];const auto *peak=pixel.valid?pixel.nearestValidPeak():nullptr;
        float value=peak?float(peak->distanceMm/1000.0):0;quint32 bits;std::memcpy(&bits,&value,4);
        qToLittleEndian(bits,result.metresFloat32LE.data()+4*i);
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
        if(m_session->liteSrDepth())disconnect(m_session->liteSrDepth(),nullptr,this,nullptr);
    }
}
void DepthMapWidget::acceptCandidate(const RvDtofCandidates::Result& result){
    if(!m_rvMode||!m_session||m_session->userStopped()||m_session->busy()
        ||m_session->dtofCandidateMode()<0||m_depthTabs->currentIndex()!=2
        ||result.metadata["candidate_mode"].toInt(-1)!=m_session->dtofCandidateMode())return;
    if(!result.fresh){if(result.error.isEmpty())holdCandidate();else clearCandidate(result.error);return;}
    if(RvDtofCandidates::identity(result.metadata)!=RvDtofCandidates::identity(m_session->metadata()))return;
    if(RvDtofCandidates::isLiteSrFull(m_session->dtofCandidateMode())&&result.metadata["host_contour_enabled"].toBool()!=m_session->boardLiteSrHostContourEnabled())return;
    if(m_candidateCanvas->setDepth(result.depth,result.metadata)&&m_candidateCanvas->setCoverage(result.coverage))
        m_candidateSnapshot={result.depth,{},{},result.metadata,QDateTime::currentDateTimeUtc()};
    else clearCandidate(tr("No fresh dToF data"));
    updateRvPresentation();
}

void DepthMapWidget::editLiteSrRepairSettings(){
    QPointer<RvLiteSrDepth> target=m_session?m_session->liteSrDepth():nullptr;
    QPointer<RvSessionController> board=m_session;
    const bool onBoard=board&&board->dtofCandidateMode()==RvDtofCandidates::LiteSrContour;
    const bool hybrid=board&&RvDtofCandidates::isLiteSrFull(board->dtofCandidateMode())&&board->boardLiteSrHostContourEnabled();
    if(!onBoard&&!hybrid&&(!target||target->model()!="area_full_contour"))return;
    const auto settings=board?board->dtofCandidateSettings():RvDtofCandidates::Settings{};
    const auto current=hybrid?board->boardLiteSrHostRepairSettings():onBoard?RvLiteSrContour::RepairSettings{settings.liteSrHoles,settings.liteSrOutliers}:target->repairSettings();
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
        }else if(!onBoard&&!hybrid&&target&&target->model()=="area_full_contour")target->setRepairSettings({holes->currentData().toInt(),outliers->isChecked()});
    }
}
void DepthMapWidget::acceptLiteSrDepth(const QByteArray &depth,const QJsonObject &metadata,const QByteArray &coverage){
    if(!m_rvMode||!m_session||m_session->userStopped()||m_session->busy()||!m_session->liteSrDepth()||!m_session->liteSrDepth()->selected()
        ||metadata["generation"]!=m_session->metadata()["generation"]||metadata["sr_presentation_epoch"].toInteger()!=qint64(m_session->presentationEpoch()))return;
    if(m_liteSrCanvas->setDepth(depth,metadata)&&(coverage.isEmpty()||m_liteSrCanvas->setCoverage(coverage)))m_liteSrSnapshot={depth,{},{},metadata,QDateTime::currentDateTimeUtc()};
    else m_liteSrSnapshot={};updateRvPresentation();
}
void DepthMapWidget::clearLiteSrDepth(const QString &reason){
    m_liteSrSnapshot={};if(m_liteSrCanvas)m_liteSrCanvas->clear(reason);
    if(m_saveButton)updateRvPresentation();
}
