#include "SensorMainWindow.h"
#include "LanguageManager.h"
#include "RenderingSettings.h"
#include "TactileDockWidget.h"
#include "sensors/DepthMapWidget.h"
#include "sensors/RtspCameraPage.h"
#include "sensors/RvDeviceManagerDialog.h"
#include "sensors/RvTunerController.h"
#include "sensors/TactileVisualizationWidget.h"
#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dbt.h>
#include <algorithm>
#include <cstddef>
#endif

bool SensorMainWindow::nativeEvent(const QByteArray &eventType,void *message,qintptr *result)
{
#ifdef Q_OS_WIN
    if(eventType=="windows_generic_MSG"&&m_tactile&&!m_closing){
        const auto *event=static_cast<const MSG *>(message);
        if(event->message==WM_DEVICECHANGE&&event->lParam
            &&(event->wParam==DBT_DEVICEARRIVAL||event->wParam==DBT_DEVICEREMOVECOMPLETE)){
            const auto *header=reinterpret_cast<const DEV_BROADCAST_HDR *>(event->lParam);
            constexpr auto nameOffset=offsetof(DEV_BROADCAST_PORT_W,dbcp_name);
            if(header->dbch_devicetype==DBT_DEVTYP_PORT&&header->dbch_size>=nameOffset+sizeof(wchar_t)){
                const auto *data=reinterpret_cast<const DEV_BROADCAST_PORT_W *>(header);
                const auto *first=data->dbcp_name;
                const auto *end=first+(header->dbch_size-nameOffset)/sizeof(wchar_t);
                const auto port=QString::fromWCharArray(first,std::find(first,end,L'\0')-first);
                const bool arrived=event->wParam==DBT_DEVICEARRIVAL;
                // Keep serial enumeration and session changes out of the Windows callback.
                auto *display=m_tactile->display();
                QTimer::singleShot(0,display,[display,port,arrived]{display->serialPortChanged(port,arrived);});
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType,message,result);
}

SensorMainWindow::SensorMainWindow(LanguageManager *language, QWidget *parent)
    : QMainWindow(parent), m_language(language)
{
    setObjectName("sensorMainWindow");
    setDockNestingEnabled(true);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks);
    auto *center=new QWidget(this);center->setMaximumSize(0,0);setCentralWidget(center);
    m_session=new RvSessionController(this);
    connect(m_session,&RvSessionController::hostInferenceUnavailable,this,[this](const QString &reason,const QString &detail){
        if(m_closing||m_inferenceWarning)return;
        auto *dialog=new QMessageBox(QMessageBox::Warning,tr("Host-assisted inference unavailable"),reason,QMessageBox::Ok,this);
        m_inferenceWarning=dialog;
        dialog->setObjectName("hostInferenceUnavailable");
        dialog->setTextFormat(Qt::PlainText);
        dialog->setInformativeText(tr("For now, choose On-board inference under Imaging scheme, then click Start if needed."));
        if(!detail.isEmpty())dialog->setDetailedText(detail);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    },Qt::QueuedConnection);
    connect(m_session,&RvSessionController::boardFilesRequired,this,&SensorMainWindow::offerBoardImport,Qt::QueuedConnection);
    m_tactile=new TactileDockWidget(this);
    m_depthDock=new QDockWidget(this);m_depthDock->setObjectName("depthMapDock");
    m_depthDock->setMinimumSize(380,220);
    m_depth=new DepthMapWidget(m_depthDock);m_depth->setSessionController(m_session);
    auto *scroll=new QScrollArea(m_depthDock);scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(m_depth);m_depthDock->setWidget(scroll);
    m_cameraDock=new QDockWidget(this);m_cameraDock->setObjectName("rtspCameraDock");
    m_camera=new RtspCameraPage(m_cameraDock);m_camera->setSessionController(m_session);
    m_cameraDock->setWidget(m_camera);
    m_docks={m_tactile,m_depthDock,m_cameraDock};
    for(auto *dock:m_docks)dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    connect(m_session->tuner(),&RvTunerController::shutdownFinished,this,[this]{
        if(m_closing){m_tunerCloseReady=true;QTimer::singleShot(0,this,&QWidget::close);}
    });

    m_file=menuBar()->addMenu(QString());m_file->setObjectName("fileMenu");
    m_exit=m_file->addAction(QString());connect(m_exit,&QAction::triggered,this,&QWidget::close);
    m_view=menuBar()->addMenu(QString());m_view->setObjectName("viewMenu");
    for(auto *dock:m_docks)m_view->addAction(dock->toggleViewAction());
    m_view->addSeparator();m_reset=m_view->addAction(QString());m_reset->setObjectName("resetLayoutAction");
    connect(m_reset,&QAction::triggered,this,&SensorMainWindow::resetLayout);
    m_presets=m_view->addMenu(QString());m_presets->setObjectName("layoutPresetMenu");
    connect(m_presets,&QMenu::aboutToShow,this,&SensorMainWindow::refreshPresets);
    m_savePreset=m_view->addAction(QString());connect(m_savePreset,&QAction::triggered,this,&SensorMainWindow::savePreset);
    m_deletePreset=m_view->addAction(QString());
    connect(m_deletePreset,&QAction::triggered,this,[this]{
        QSettings settings;settings.beginGroup("workspace/presets");const auto names=settings.childGroups();
        bool ok=false;const auto name=QInputDialog::getItem(this,tr("Delete layout"),tr("Layout name:"),names,0,false,&ok);
        if(ok&&!name.isEmpty())settings.remove(name);
    });
    m_settings=menuBar()->addMenu(QString());m_settings->setObjectName("settingsMenu");
    m_deviceAction=m_settings->addAction(QString());m_deviceAction->setObjectName("rvDeviceManagerAction");
    connect(m_deviceAction,&QAction::triggered,this,[this]{showDeviceManager();});
    m_rendering=m_settings->addMenu(QString());m_rendering->setObjectName("renderModeMenu");
    auto *renderGroup=new QActionGroup(this);renderGroup->setExclusive(true);
    m_standard=m_rendering->addAction(QString());m_standard->setObjectName("actionRenderStandard");
    m_smooth=m_rendering->addAction(QString());m_smooth->setObjectName("actionRenderSmooth");
    for(auto *action:{m_standard,m_smooth}){action->setCheckable(true);renderGroup->addAction(action);}
    auto *render=RenderingSettings::instance();
    const auto syncRendering=[this,render]{m_standard->setChecked(!render->lowCostRendering());m_smooth->setChecked(render->lowCostRendering());};
    connect(render,&RenderingSettings::lowCostRenderingChanged,this,syncRendering);syncRendering();
    connect(m_standard,&QAction::triggered,render,[render]{render->setLowCostRendering(false);});
    connect(m_smooth,&QAction::triggered,render,[render]{render->setLowCostRendering(true);});
    m_languages=m_settings->addMenu(QString());m_languages->setObjectName("languageMenu");
    auto *languageGroup=new QActionGroup(this);languageGroup->setExclusive(true);
    m_english=m_languages->addAction(QString());m_chinese=m_languages->addAction(QString());
    for(auto *action:{m_english,m_chinese}){action->setCheckable(true);languageGroup->addAction(action);}
    connect(m_english,&QAction::triggered,this,[this]{m_language->setLanguage(LanguageManager::Language::English);});
    connect(m_chinese,&QAction::triggered,this,[this]{m_language->setLanguage(LanguageManager::Language::SimplifiedChinese);});
    m_about=menuBar()->addAction(QString(),this,&SensorMainWindow::showAbout);
    m_about->setObjectName("aboutAction");
    connect(m_language,&LanguageManager::languageChanged,this,[this]{retranslate();});
    resize(1680,1040);resetLayout();
    QSettings settings;
    if(settings.contains("workspace/geometry"))restoreGeometry(settings.value("workspace/geometry").toByteArray());
    if(settings.contains("workspace/state"))restoreState(settings.value("workspace/state").toByteArray(),1);
    retranslate();
}

void SensorMainWindow::showAbout()
{
    auto *dialog = new QDialog(this, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("About"));

    auto *logo = new QLabel(dialog);
    logo->setPixmap(windowIcon().pixmap(QSize(112, 112), dialog->devicePixelRatioF()));
    auto *name = new QLabel(QApplication::applicationDisplayName(), dialog);
    auto *version = new QLabel(QStringLiteral("v%1").arg(QApplication::applicationVersion()), dialog);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(32, 24, 32, 24);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    layout->addWidget(logo, 0, Qt::AlignHCenter);
    layout->addWidget(name, 0, Qt::AlignHCenter);
    layout->addWidget(version, 0, Qt::AlignHCenter);
    dialog->open();
}

RvDeviceManagerDialog *SensorMainWindow::showDeviceManager(bool modal)
{
    if (!m_devices) {
        m_devices = new RvDeviceManagerDialog(m_session, this);
        m_devices->setAttribute(Qt::WA_DeleteOnClose);
    }
    if (modal && !m_devices->isModal()) {
        m_devices->hide();
        m_devices->setWindowModality(Qt::WindowModal);
    }
    m_devices->show();
    m_devices->raise();
    m_devices->activateWindow();
    return m_devices;
}

void SensorMainWindow::offerBoardImport(const RvDeviceProfile &device)
{
    const auto stillSelected = [this, &device] {
        return !m_closing && m_session->userStopped() && !m_session->busy()
            && m_session->deviceProfile().json() == device.json();
    };
    if (!stillSelected()) return;
    if (QMessageBox::question(this, tr("Install required board files"),
            tr("%1 is missing required board files. Open the on-board inference model import to install them?")
                .arg(device.name), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes
        || !stillSelected()) return;
    showDeviceManager(true)->importBoardFilesForStart(device);
}

void SensorMainWindow::monitorCameraConnection()
{
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1") return;
    auto *timer = new QTimer(this);
    timer->setInterval(2500);
    connect(timer, &QTimer::timeout, this, &SensorMainWindow::probeCameraConnection);
    timer->start();
    QTimer::singleShot(0, this, &SensorMainWindow::probeCameraConnection);
}

void SensorMainWindow::probeCameraConnection()
{
    if (m_closing || m_cameraProbe || !m_session->userStopped() || m_session->busy()
        || !m_session->controlledSource() || QApplication::activeModalWidget()
        || (m_devices && m_devices->isVisible())) return;
    const auto device = m_session->deviceProfile();
    if (!device.ready()) return;
    auto *probe = new QTcpSocket(this);
    m_cameraProbe = probe;
    probe->setProxy(QNetworkProxy::NoProxy);
    const auto finish = [this, probe, device](bool reachable) {
        if (m_cameraProbe != probe) return;
        m_cameraProbe.clear();
        probe->abort();
        probe->deleteLater();
        if (m_closing || m_session->deviceProfile().json() != device.json()) return;
        if (!reachable) {
            m_session->deviceUnreachable(device.id);
        } else if (!QApplication::activeModalWidget() && (!m_devices || !m_devices->isVisible())) {
            m_session->startAutomatically();
        }
    };
    connect(probe, &QTcpSocket::connected, this, [finish]{finish(true);});
    connect(probe, &QTcpSocket::errorOccurred, this, [finish](QAbstractSocket::SocketError){finish(false);});
    QTimer::singleShot(1800, probe, [finish]{finish(false);});
    probe->connectToHost(device.host, device.sshPort);
}

SensorMainWindow::~SensorMainWindow()
{
    m_closing=true;
    m_tactile->display()->stop();m_session->stop();
    // Destroy subscribers while the session is still alive.
    for(auto *dock:m_docks)delete dock;
    if(m_devices)delete m_devices;
    delete m_session;
}

void SensorMainWindow::resetLayout()
{
    for(auto *dock:m_docks){removeDockWidget(dock);dock->setFloating(false);}
    addDockWidget(Qt::LeftDockWidgetArea,m_tactile);
    addDockWidget(Qt::RightDockWidgetArea,m_depthDock);
    splitDockWidget(m_depthDock,m_cameraDock,Qt::Vertical);
    m_tactile->show();m_depthDock->show();m_cameraDock->show();m_depthDock->raise();
    resizeDocks({m_tactile,m_depthDock},{780,880},Qt::Horizontal);
    resizeDocks({m_depthDock,m_cameraDock},{480,480},Qt::Vertical);
}

void SensorMainWindow::saveLayout()
{
    QSettings settings;settings.setValue("workspace/geometry",saveGeometry());settings.setValue("workspace/state",saveState(1));settings.sync();
}

void SensorMainWindow::refreshPresets()
{
    m_presets->clear();QSettings settings;settings.beginGroup("workspace/presets");
    const auto names=settings.childGroups();m_deletePreset->setEnabled(!names.isEmpty());
    for(const auto &name:names){
        const auto state=settings.value(name+"/state").toByteArray();
        auto *action=m_presets->addAction(name);connect(action,&QAction::triggered,this,[this,state]{restoreState(state,1);});
    }
    if(names.isEmpty())m_presets->addAction(tr("No saved layouts"))->setEnabled(false);
}

void SensorMainWindow::savePreset()
{
    bool ok=false;const auto name=QInputDialog::getText(this,tr("Save layout"),tr("Layout name:"),QLineEdit::Normal,{},&ok).trimmed();
    if(ok&&!name.isEmpty()&&!name.contains('/')&&!name.contains('\\')){
        QSettings settings;settings.setValue("workspace/presets/"+name+"/state",saveState(1));
    }
}

void SensorMainWindow::closeEvent(QCloseEvent *event)
{
    if(m_session->tuner()->shutdownPending()&&!m_tunerCloseReady){
        event->ignore();m_closing=true;m_session->tuner()->stop();return;
    }
    m_closing=true;saveLayout();m_tactile->display()->stop();m_session->stop();event->accept();
}

void SensorMainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);if(event->type()==QEvent::LanguageChange)retranslate();
}

void SensorMainWindow::retranslate()
{
    setWindowTitle(QApplication::applicationDisplayName());
    m_tactile->setWindowTitle(tr("Magnetic tactile sensors"));m_depthDock->setWindowTitle(tr("Depth Map"));
    m_cameraDock->setWindowTitle(tr("Camera raw video"));
    m_file->setTitle(tr("File"));m_exit->setText(tr("Exit"));m_view->setTitle(tr("View"));
    m_reset->setText(tr("Reset layout"));m_presets->setTitle(tr("Layout Presets"));
    m_savePreset->setText(tr("Save layout..."));m_deletePreset->setText(tr("Delete layout..."));
    m_settings->setTitle(tr("Settings"));m_deviceAction->setText(tr("Device Manager..."));
    m_rendering->setTitle(tr("Rendering Mode"));m_standard->setText(tr("Standard Mode"));m_smooth->setText(tr("Smooth Mode"));
    m_languages->setTitle(tr("Language"));m_english->setText(tr("English"));m_chinese->setText(tr("Simplified Chinese"));
    m_about->setText(tr("About"));
    m_english->setChecked(m_language->language()==LanguageManager::Language::English);
    m_chinese->setChecked(m_language->language()==LanguageManager::Language::SimplifiedChinese);
    for(auto *dock:m_docks)dock->toggleViewAction()->setText(dock->windowTitle());
    m_session->retranslateState();
}
