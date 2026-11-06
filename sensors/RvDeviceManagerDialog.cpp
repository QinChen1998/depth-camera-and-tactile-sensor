#include "RvDeviceManagerDialog.h"
#include "RvImportDiagnostics.h"
#include "RvSessionController.h"
#include "RvTunerController.h"
#include "../RuntimeDiscovery.h"
#include "../UiStyle.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QCloseEvent>
#include <QtGui/QScreen>
#include <QtGui/QShowEvent>
#include <stdexcept>

namespace {
QString publicKeyText(const QLineEdit *field) {
    auto key=field->text().trimmed();
    if(key.startsWith("ssh-ed25519 "))key=key.mid(12).trimmed();
    return key;
}
}

RvDeviceManagerDialog::RvDeviceManagerDialog(RvSessionController *session,QWidget *parent)
    :QDialog(parent,Qt::Window),m_session(session) {
    setObjectName("rvDeviceManagerDialog");setWindowTitle(tr("Device Manager"));setModal(false);resize(960,720);
    auto *layout=new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("rvDeviceScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);
    m_active=new QLabel(body);m_active->setObjectName("rvDeviceActive");m_active->setWordWrap(true);bodyLayout->addWidget(m_active);
    m_table=new QTableWidget(0,5,body);m_table->setObjectName("rvDeviceTable");
    m_table->setMinimumHeight(100);
    m_table->setHorizontalHeaderLabels({tr("Name"),tr("Serial"),tr("Address"),tr("Calibration"),tr("Last connected")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);m_table->setSelectionMode(QAbstractItemView::SingleSelection);m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);m_table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    bodyLayout->addWidget(m_table,1);
    auto *tools = new QWidget(body);
    auto *toolbar = UiStyle::wrappingRow(tools, bodyLayout->spacing());
    bodyLayout->addWidget(tools);
    const auto button=[&](const QString &text,const char *name,auto callback){auto *b=new QPushButton(text,tools);b->setObjectName(name);toolbar->addWidget(b);m_actions.append(b);connect(b,&QPushButton::clicked,this,callback);return b;};
    button(tr("Connect via USB"),"rvDeviceConnectUsb",[this]{
        if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1") {
            message(tr("USB connection is unavailable in offline mode."), true);
            return;
        }
        emit usbConnectionRequested(this);
    })->setAutoDefault(false);
    button(tr("Add device"),"rvDeviceAdd",[this]{RvDeviceProfile p;p.id=QUuid::createUuid().toString(QUuid::Id128);p.host="172.32.0.93";p.calibration="unconfigured";m_table->clearSelection();edit(p,true);});
    m_remove=button(tr("Remove"),"rvDeviceRemove",[this]{remove();});
    button(tr("Import..."),"rvDeviceImport",[this]{importDevices();});
    button(tr("Export..."),"rvDeviceExport",[this]{exportDevices();});
    button(tr("Reload"),"rvDeviceReload",[this]{reload();});
    m_use=button(tr("Use selected device"),"rvDeviceUse",[this]{activate();});
    auto *group=new QGroupBox(tr("Device details"),body);m_details=group;auto *form=new QFormLayout(group);bodyLayout->addWidget(group);
    const auto field=[&](const char *name){auto *f=new QLineEdit(group);f->setObjectName(name);return f;};
    m_name=field("rvDeviceName");m_name->setMaxLength(80);form->addRow(tr("Name"),m_name);
    auto *address=new QHBoxLayout;m_host=field("rvDeviceHost");m_port=new QSpinBox(group);m_port->setObjectName("rvDevicePort");m_port->setRange(1,65535);m_port->setValue(22);
    address->addWidget(m_host,1);address->addWidget(new QLabel(tr("SSH port"),group));address->addWidget(m_port);form->addRow(tr("Address"),address);
    auto *serialRow=new QHBoxLayout;
    m_serial=field("rvDeviceSerial");m_serial->setMaxLength(16);m_serial->setReadOnly(true);m_serial->setPlaceholderText(tr("Read automatically when connecting"));serialRow->addWidget(m_serial,1);
    m_readSerial=new QPushButton(tr("Connect and read"),group);m_readSerial->setObjectName("rvDeviceReadSerial");m_actions.append(m_readSerial);serialRow->addWidget(m_readSerial);form->addRow(tr("Serial"),serialRow);
    m_username=field("rvDeviceUsername");m_username->setMaxLength(64);form->addRow(tr("Login username"),m_username);
    auto *keyRow=new QHBoxLayout;m_key=field("rvDeviceKey");m_key->setMaxLength(200);m_key->setPlaceholderText("ssh-ed25519 AAAA...");keyRow->addWidget(m_key,1);
    m_probe=new QPushButton(tr("Read device key"),group);m_probe->setObjectName("rvDeviceProbe");m_actions.append(m_probe);keyRow->addWidget(m_probe);form->addRow(tr("Public key"),keyRow);
    m_fingerprint=new QLabel(group);m_fingerprint->setObjectName("rvDeviceFingerprint");m_fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);m_fingerprint->setWordWrap(true);form->addRow(tr("Fingerprint"),m_fingerprint);
    m_calibration=new QComboBox(group);m_calibration->setObjectName("rvDeviceCalibration");
    m_calibration->addItem(tr("Not configured"),"unconfigured");m_calibration->addItem(tr("2026-10-08 calibration / F=1"),"rv20261008-f1");m_calibration->addItem(tr("2026-10-08 calibration / F=2"),"rv20261008-f2");form->addRow(tr("Calibration"),m_calibration);
    m_rotation=new QComboBox(group);m_rotation->setObjectName("rvDeviceRotation");m_rotation->addItem(tr("Not verified"),-1);m_rotation->addItem(tr("0 degrees"),0);m_rotation->addItem(tr("180 degrees"),180);form->addRow(tr("Host inference input orientation"),m_rotation);
    m_cameraMount=new QComboBox(group);m_cameraMount->setObjectName("rvDeviceCameraMount");
    m_cameraMount->addItem(tr("0 degrees"),0);m_cameraMount->addItem(tr("180 degrees"),180);
    m_cameraMount->setToolTip(tr("Correct the camera mounting angle before display and model preprocessing. Model input orientation is a separate setting."));
    form->addRow(tr("Camera mounting angle"),m_cameraMount);
    m_legacy=new QCheckBox(tr("Legacy coordinator (no automatic fault recovery)"),group);m_legacy->setObjectName("rvDeviceLegacy");form->addRow(m_legacy);
    m_confirm=new QCheckBox(tr("I verified the public key, serial, calibration and actual inference orientation."),group);m_confirm->setObjectName("rvDeviceConfirm");form->addRow(m_confirm);
    auto *note=new QLabel(tr("Reading a key does not establish trust. Check it against a trusted device record. Saving does not start capture; passwords stay in Windows Credential Manager."),body);note->setWordWrap(true);bodyLayout->addWidget(note);
    m_serialHelp=new QPlainTextEdit(body);m_serialHelp->setObjectName("rvDeviceSerialHelp");m_serialHelp->setReadOnly(true);m_serialHelp->setMaximumHeight(150);m_serialHelp->hide();bodyLayout->addWidget(m_serialHelp);
    m_status=new QLabel(body);m_status->setObjectName("rvDeviceStatus");m_status->setWordWrap(true);m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);bodyLayout->addWidget(m_status);
    auto *actions = new QWidget(this);
    auto *bottom = UiStyle::wrappingRow(actions, layout->spacing());
    layout->addWidget(actions);
    m_save=new QPushButton(tr("Save device"),actions);m_save->setObjectName("rvDeviceSave");m_verify=new QPushButton(tr("Verify connection"),actions);m_verify->setObjectName("rvDeviceVerify");m_forget=new QPushButton(tr("Clear saved login"),actions);m_forget->setObjectName("rvDeviceForget");
    m_importLiteSr=new QPushButton(tr("Install protected on-board inference..."),actions);m_importLiteSr->setObjectName("rvDeviceImportLiteSr");
    for(auto *b:{m_save,m_verify,m_importLiteSr,m_forget}){bottom->addWidget(b);m_actions.append(b);}
    auto *close=new QDialogButtonBox(QDialogButtonBox::Close,actions);
    auto *closeButton=close->button(QDialogButtonBox::Close);
    closeButton->setText(tr("Close"));closeButton->setObjectName("rvDeviceClose");
    bottom->addWidget(close);connect(close,&QDialogButtonBox::rejected,this,&QDialog::close);
    connect(m_table,&QTableWidget::itemSelectionChanged,this,[this]{if(m_store&&m_table->currentRow()>=0&&!m_table->selectedItems().isEmpty())edit(m_store->byId(m_table->item(m_table->currentRow(),0)->data(Qt::UserRole).toString()));});
    connect(m_save,&QPushButton::clicked,this,[this]{save();});connect(m_probe,&QPushButton::clicked,this,[this]{runWorker("--inspect-host");});
    connect(m_readSerial,&QPushButton::clicked,this,[this]{readDeviceInformation();});
    connect(m_verify,&QPushButton::clicked,this,[this]{runWorker("--verify-device");});
    connect(m_importLiteSr,&QPushButton::clicked,this,[this]{importLiteSrModel();});
    connect(m_forget,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,tr("Clear saved login"),tr("Remove the saved Windows login for this device? The next connection will ask for its password."))==QMessageBox::Yes)runWorker("--forget-device-credentials");});
    const auto changed=[this]{m_confirm->setChecked(false);updateFingerprint();};
    for(auto *f:{m_name,m_host,m_serial,m_key,m_username})connect(f,&QLineEdit::textEdited,this,changed);
    connect(m_key,&QLineEdit::textChanged,this,changed);
    for(auto *f:{m_host,m_key,m_username})connect(f,&QLineEdit::textEdited,this,[this]{invalidateSerial();});
    connect(m_port,&QSpinBox::valueChanged,this,[this]{invalidateSerial();});
    connect(m_port,&QSpinBox::valueChanged,this,changed);connect(m_calibration,&QComboBox::currentIndexChanged,this,changed);connect(m_rotation,&QComboBox::currentIndexChanged,this,changed);connect(m_cameraMount,&QComboBox::currentIndexChanged,this,changed);connect(m_legacy,&QCheckBox::toggled,this,changed);
    m_worker=new QProcess(this);m_timeout=new QTimer(this);m_timeout->setSingleShot(true);
    connect(m_timeout,&QTimer::timeout,this,[this]{m_workerFailure=tr("Device operation timed out.");m_worker->kill();});
    connect(m_worker,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart){m_timeout->stop();busy(false);operationFailed(m_worker->errorString());}});
    connect(m_worker,&QProcess::readyReadStandardOutput,this,[this]{if(m_worker->bytesAvailable()>65536){m_workerFailure=tr("Device response is too large.");m_worker->kill();}});
    connect(m_worker,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status){
        m_timeout->stop();busy(false);const auto output=m_worker->readAllStandardOutput();const auto error=m_worker->readAllStandardError().left(4096);
        if(m_operation.isEmpty())return;
        if(code||status!=QProcess::NormalExit){operationFailed(!m_workerFailure.isEmpty()?m_workerFailure:QString::fromUtf8(error).isEmpty()?tr("Device operation failed."):QString::fromUtf8(error),code);return;}
        try {
            QJsonParseError parse;const auto doc=QJsonDocument::fromJson(output,&parse);if(parse.error!=QJsonParseError::NoError||!doc.isObject())throw std::runtime_error("Invalid device worker response");const auto result=doc.object();
            acceptWorkerResult(result);
        }catch(const std::exception &e){operationFailed(QString::fromUtf8(e.what()));}
    });
    if(m_session)connect(m_session,&RvSessionController::deviceChanged,this,[this]{m_active->setText(tr("Active device: %1").arg(m_session->deviceProfile().name));});
    reload();
}
void RvDeviceManagerDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    const auto *display = parentWidget() ? parentWidget()->screen() : screen();
    if (!display) return;
    const auto workArea = display->availableGeometry();
    const auto frameMargins = frameGeometry().size() - size();
    resize(size().boundedTo(workArea.size() - frameMargins));
    if (!workArea.contains(frameGeometry()))
        move(QStyle::alignedRect(layoutDirection(), Qt::AlignCenter, frameGeometry().size(), workArea).topLeft());
}

RvDeviceManagerDialog::~RvDeviceManagerDialog(){m_worker->disconnect(this);m_timeout->stop();if(m_worker->state()!=QProcess::NotRunning){m_worker->kill();m_worker->waitForFinished(1000);}}
void RvDeviceManagerDialog::cancelOperation(){
    m_timeout->stop();m_operation.clear();m_readingSerial=false;m_resumeImaging=false;
    if(m_worker->state()!=QProcess::NotRunning){m_worker->kill();m_worker->waitForFinished(1000);}
    busy(false);
}
void RvDeviceManagerDialog::reject(){if(!m_usbEnrollmentPending){cancelOperation();QDialog::reject();}}
void RvDeviceManagerDialog::closeEvent(QCloseEvent *event){
    if (m_usbEnrollmentPending) { event->ignore(); return; }
    cancelOperation();QDialog::closeEvent(event);
}
void RvDeviceManagerDialog::beginUsbEnrollment()
{
    m_usbEnrollmentPending = true;
    busy(true);
    message(tr("Connecting to RV1103 via USB..."));
}
void RvDeviceManagerDialog::finishUsbEnrollment(bool added)
{
    m_usbEnrollmentPending = false;
    if (added) {
        reload();
        if (m_store) message(tr("The RV1103 was verified and selected for imaging."));
    } else {
        busy(false);
        message(tr("USB connection setup ended."));
    }
}
void RvDeviceManagerDialog::invalidateSerial(){
    if(!m_fresh)return;
    m_serial->clear();m_serial->setReadOnly(true);m_serialHelp->hide();
}
void RvDeviceManagerDialog::readDeviceInformation(){
    if(!m_store||m_worker->state()!=QProcess::NotRunning)return;
    if(!RvDeviceStore::validHost(m_host->text().trimmed())){message(tr("Invalid device address"),true);return;}
    if(!QRegularExpression("^[A-Za-z0-9_.-]{1,64}$").match(m_username->text().trimmed()).hasMatch()){message(tr("Invalid login username"),true);return;}
    m_probeId=m_id;m_probeHost=m_host->text().trimmed();m_probePort=m_port->value();m_probeUsername=m_username->text().trimmed();
    m_readingSerial=true;m_serial->setReadOnly(true);m_serialHelp->hide();m_confirm->setChecked(false);
    runWorker("--inspect-host");
}
void RvDeviceManagerDialog::confirmSerialRead(){
    const auto fingerprint=RvDeviceStore::keyFingerprint(m_probeKey);
    if(m_fresh||fingerprint!=m_savedFingerprint){
        // A single-use confirmation authorizes only the read below. Saving
        // trust still requires the existing full identity/calibration check.
        busy(true);
        QMessageBox confirm(QMessageBox::Question,tr("Confirm device identity"),
            tr("Connect to %1:%2 as %3 to read its serial?\nCheck this public-key fingerprint against a trusted device record:\n%4\nEnter the usual device password when asked. No capture will start.")
                .arg(m_probeHost).arg(m_probePort).arg(m_probeUsername,m_fingerprint->text()),QMessageBox::Yes|QMessageBox::Cancel,this);
        confirm.setObjectName("rvDeviceReadTrust");confirm.setDefaultButton(QMessageBox::Cancel);
        const auto answer=confirm.exec();busy(false);
        if(answer!=QMessageBox::Yes){m_readingSerial=false;m_operation.clear();message(tr("Reading cancelled. Click Connect and read to try again."));return;}
    }
    if(!m_readingSerial)return; // The window may have closed during confirmation.
    runWorker("--read-device-serial");
}
void RvDeviceManagerDialog::acceptWorkerResult(const QJsonObject &result){
    if(m_operation=="--inspect-host"||m_operation=="--read-device-serial"){
        if(m_id!=m_probeId||m_host->text().trimmed()!=m_probeHost||m_port->value()!=m_probePort
            ||(m_readingSerial&&m_username->text().trimmed()!=m_probeUsername))
            throw std::runtime_error("Device details changed during reading; connect and read again");
    }
    if(m_operation=="--inspect-host"){
        const auto key=result["public_key"].toString();RvDeviceStore::keyFingerprint(key);
        m_key->setText("ssh-ed25519 "+key);updateFingerprint();m_confirm->setChecked(false);m_probeKey=key;
        if(m_readingSerial)confirmSerialRead();
        else{m_operation.clear();message(tr("Key read. Verify the fingerprint using a trusted record before saving."));}
    }else if(m_operation=="--read-device-serial"){
        if(result["authenticated"]!=true||result["host"]!=m_probeHost||result["ssh_port"]!=m_probePort
            ||result["username"]!=m_probeUsername||result["public_key"]!=m_probeKey||publicKeyText(m_key)!=m_probeKey)
            throw std::runtime_error("Serial response does not match the confirmed device");
        const auto serial=result["serial"].toString();
        if(!QRegularExpression("^[0-9a-f]{16}$").match(serial).hasMatch())throw std::runtime_error("Cannot read a 16-character CPU Serial from /proc/cpuinfo");
        if(!m_fresh&&serial!=m_store->byId(m_id).serial)throw std::runtime_error("Read serial differs from the saved device; add the new device separately");
        m_serial->setText(serial);m_serial->setReadOnly(true);m_serialHelp->hide();m_confirm->setChecked(false);
        m_readingSerial=false;m_operation.clear();
        message(tr("Serial read automatically. Confirm the calibration and actual inference orientation, then save the device. No capture was started."));
    }else if(m_operation=="--verify-device"){
        const auto p=RvDeviceProfile::fromJson(result["trusted_device"].toObject());m_store->reload();
        if(m_store->byId(p.id).json()!=p.json())throw std::runtime_error("Device profile changed during verification");
        auto list=m_store->profiles();for(auto &entry:list)if(entry.id==p.id)entry.lastConnected=QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        m_store->save(list,m_store->activeId());refresh(p.id);m_operation.clear();
        const auto model=result["litesr_model"].toString();
        const auto automaticTof=result["automatic_tof"].toString();
        if(automaticTof=="missing")message(tr("Verified %1. Automatic dToF support is missing. Use Install protected on-board inference to install the required files.").arg(p.name));
        else if(automaticTof!="ready")message(tr("Verified %1. The board capture script is unavailable or unsupported.").arg(p.name),true);
        else if(model=="missing")message(tr("Verified %1. Protected LiteSR is missing. Use Install protected on-board inference.").arg(p.name));
        else if(model=="mismatch")message(tr("Verified %1. The existing on-board inference model checksum differs.").arg(p.name),true);
        else if(model=="runtime_missing")message(tr("Verified %1. The on-board inference runtime is incomplete.").arg(p.name),true);
        else if(model=="ready")message(tr("Verified %1. Protected LiteSR and automatic dToF are ready.").arg(p.name));
        else message(tr("Verified %1. On-board inference model status is unavailable.").arg(p.name),true);
    }else if(m_operation=="--import-litesr-model"){
        const auto p=RvDeviceProfile::fromJson(result["trusted_device"].toObject());
        m_store->reload();
        if(result["automatic_tof"]!="ready"||p.id!=m_id||m_store->byId(m_id).json()!=p.json())
            throw std::runtime_error("LiteSR import response does not match the selected device");
        m_operation.clear();
        const auto model=result["litesr_model"].toString();
        if(model!="ready")throw std::runtime_error("Protected LiteSR installation was not verified");
        if (m_resumeImaging && m_session && m_session->userStopped() && !m_session->busy()
            && m_session->deviceProfile().json()==p.json()) {
            m_resumeImaging=false;
            m_session->start();
            accept();
            return;
        }
        message(tr("Protected LiteSR and automatic dToF are ready on %1. Click Start in either imaging panel.").arg(p.name));
        if (m_operation.isEmpty()) m_resumeImaging=false;
    }else{m_operation.clear();message(tr("Saved login cleared."));}
}
void RvDeviceManagerDialog::showBoardImportFailure(const QString &reason,const QString &phase,int code)
{
    const auto translated=QCoreApplication::translate("RvDeviceManagerDialog",reason.toUtf8().constData());
    message(translated,true);
    QJsonObject details{{"phase",phase},{"worker_exit_code",code},{"resume_imaging",m_resumeImaging}};
    if (m_store) {
        try {
            const auto p=m_store->byId(m_id);
            details["serial"]=p.serial;
            details["host"]=p.host;
            details["ssh_port"]=p.sshPort;
            details["ssh_fingerprint"]=p.fingerprint();
            details["fov"]=p.fov();
            details["camera_mount_degrees"]=p.cameraMountRotation;
            details["inference_input_degrees"]=RvSessionController::effectiveHostInputRotation(p);
        } catch (const std::exception &) {}
    }
    RvImportDiagnostics::showFailure(this,"board_runtime_import",translated,details);
}

void RvDeviceManagerDialog::operationFailed(const QString &reason,int code){
    const bool serialRead=m_readingSerial||m_operation=="--read-device-serial";
    const bool boardImport=m_operation=="--import-litesr-model";
    m_readingSerial=false;m_operation.clear();
    if (boardImport) {
        showBoardImportFailure(reason,m_workerFailure.isEmpty() ? "board_import" : "worker_failure",code);
        m_resumeImaging=false;
        return;
    }
    m_resumeImaging=false;
    if(!serialRead){message(reason,true);return;}
    if(m_id!=m_probeId||m_host->text().trimmed()!=m_probeHost||m_port->value()!=m_probePort||m_username->text().trimmed()!=m_probeUsername){
        message(tr("Device details changed during reading; connect and read again"),true);return;
    }
    const auto host=m_probeHost.contains(':')?"["+m_probeHost+"]":m_probeHost;
    const auto login=QString("ssh -p %1 %2@%3").arg(m_probePort).arg(m_probeUsername,host);
    const auto translatedReason=QCoreApplication::translate("RvDeviceManagerDialog",reason.toUtf8().constData());
    m_serialHelp->setPlainText(tr("Automatic reading failed: %1\n\nCheck the address, SSH port, username and password, then click Connect and read to retry. If it still fails:\n1. Open Windows Terminal and run:\n%2\n2. After login, run:\ncat /proc/cpuinfo\n3. Copy the 16 hexadecimal characters after Serial into the Serial field. Save, then verify the connection.\nIf Serial is missing or not 16 hexadecimal characters, contact the device provider; do not invent a serial.").arg(translatedReason,login));
    m_serial->setReadOnly(false);m_serial->setPlaceholderText(tr("16-character device serial"));m_serialHelp->show();
    message(tr("Automatic serial reading failed. Retry or follow the manual steps below."),true);
}
void RvDeviceManagerDialog::updateFingerprint(){
    try{const auto current=RvDeviceStore::keyFingerprint(m_key->text().trimmed().section(' ',-1));m_fingerprint->setText(m_savedFingerprint.isEmpty()||current==m_savedFingerprint?current:tr("Saved: %1\nNew: %2").arg(m_savedFingerprint,current));}
    catch(...){m_fingerprint->setText(tr("Enter a complete Ed25519 public key"));}
}
void RvDeviceManagerDialog::message(const QString &text,bool error){m_status->setText(UiStyle::depthErrorText(QCoreApplication::translate("RvDeviceManagerDialog",text.toUtf8().constData())));m_status->setStyleSheet(error?"color: #c64a42;":"");}
void RvDeviceManagerDialog::busy(bool value){for(auto *b:m_actions)b->setEnabled(!value&&(bool(m_store)||b->objectName()=="rvDeviceReload"));m_table->setEnabled(!value&&bool(m_store));m_details->setEnabled(!value&&bool(m_store));for(auto *b:{m_verify,m_forget,m_use,m_remove,m_importLiteSr})b->setEnabled(!value&&bool(m_store)&&!m_fresh);}
void RvDeviceManagerDialog::reload(){try{m_store=std::make_unique<RvDeviceStore>(m_session?m_session->deviceStorePath():RvDeviceStore::defaultPath());busy(false);refresh(m_store->activeId());message(tr("Select a device or add a new one."));}catch(const std::exception&e){m_store.reset();busy(false);message(QString::fromUtf8(e.what()),true);}}
void RvDeviceManagerDialog::refresh(const QString &id){
    m_table->blockSignals(true);m_table->setRowCount(0);int selected=-1;
    for(const auto &p:m_store->profiles()){int row=m_table->rowCount();m_table->insertRow(row);QStringList values{p.name,p.serial,p.host+":"+QString::number(p.sshPort),p.fov()?QString("F=%1").arg(p.fov()):tr("Not configured"),QSettings().value("rvDevices/lastConnected/"+p.fingerprint(),p.lastConnected.isEmpty()?tr("Never"):p.lastConnected).toString()};
        for(int col=0;col<values.size();++col){auto *item=new QTableWidgetItem(values[col]);item->setData(Qt::UserRole,p.id);item->setToolTip(p.fingerprint());m_table->setItem(row,col,item);}if(p.id==id)selected=row;}
    m_table->blockSignals(false);m_active->setText(m_store->activeId().isEmpty()?tr("No active device"):tr("Active device: %1").arg(m_session&& !m_session->userStopped()?m_session->deviceProfile().name:m_store->active().name));
    if(selected>=0)m_table->selectRow(selected);else if(m_table->rowCount())m_table->selectRow(0);else{RvDeviceProfile p;p.id=QUuid::createUuid().toString(QUuid::Id128);p.host="172.32.0.93";p.calibration="unconfigured";edit(p,true);}
}
void RvDeviceManagerDialog::edit(const RvDeviceProfile &p,bool fresh){m_fresh=false;m_readingSerial=false;m_operation.clear();m_serialHelp->hide();m_serial->setReadOnly(true);m_serial->setPlaceholderText(tr("Read automatically when connecting"));m_savedFingerprint=fresh?QString():p.fingerprint();m_id=p.id;m_name->setText(p.name);m_host->setText(p.host);m_port->setValue(p.sshPort);m_serial->setText(p.serial);m_username->setText(p.username);m_key->setText(p.publicKey.isEmpty()?QString():"ssh-ed25519 "+p.publicKey);m_calibration->setCurrentIndex(m_calibration->findData(p.calibration));m_rotation->setCurrentIndex(m_rotation->findData(RvSessionController::effectiveHostInputRotation(p)));m_cameraMount->setCurrentIndex(m_cameraMount->findData(p.cameraMountRotation));m_legacy->setChecked(p.legacyCoordinator);m_confirm->setChecked(false);m_fingerprint->setText(fresh?tr("Enter a complete Ed25519 public key"):p.fingerprint());m_verify->setEnabled(!fresh);m_forget->setEnabled(!fresh);m_use->setEnabled(!fresh);m_remove->setEnabled(!fresh);m_importLiteSr->setEnabled(!fresh);m_fresh=fresh;}
RvDeviceProfile RvDeviceManagerDialog::draft() const {RvDeviceProfile p;p.id=m_id;p.name=m_name->text().trimmed();p.host=m_host->text().trimmed();p.sshPort=m_port->value();p.serial=m_serial->text().trimmed().toLower();p.username=m_username->text().trimmed();p.publicKey=m_key->text().trimmed();if(p.publicKey.startsWith("ssh-ed25519 "))p.publicKey=p.publicKey.mid(12).trimmed();p.calibration=m_calibration->currentData().toString();p.inputRotation=m_rotation->currentData().toInt();p.cameraMountRotation=m_cameraMount->currentData().toInt();p.legacyCoordinator=m_legacy->isChecked();p.validate();return p;}
void RvDeviceManagerDialog::save(){if(m_session&&m_session->tuner()->shutdownPending()){message(tr("Stop dToF tuning before changing trusted devices."),true);return;}if(m_serial->text().isEmpty()&&m_serial->isReadOnly()){readDeviceInformation();return;}try{auto p=draft();auto list=m_store->profiles();int index=-1;for(int i=0;i<list.size();++i)if(list[i].id==p.id)index=i;
    bool trustChanged=index<0;if(index>=0){const auto &old=list[index];trustChanged=old.publicKey!=p.publicKey||old.serial!=p.serial||old.calibration!=p.calibration||old.inputRotation!=p.inputRotation||old.cameraMountRotation!=p.cameraMountRotation||old.legacyCoordinator!=p.legacyCoordinator;p.lastConnected=trustChanged?QString():old.lastConnected;}
    if(trustChanged&&!m_confirm->isChecked())throw std::runtime_error("Verify the fingerprint, serial, calibration and orientation, then check the confirmation box");
    if(index<0)list.append(p);else list[index]=p;
    if(m_session)m_session->stop();
    m_store->save(list,m_store->activeId());
    QSettings().remove(RvSessionController::legacyHostInputRotationKey(p));
    if(m_session)m_session->reloadDeviceProfiles();refresh(p.id);message(tr("Device saved. Click Use selected device to select it for imaging."));
}catch(const std::exception&e){message(QString::fromUtf8(e.what()),true);}}
void RvDeviceManagerDialog::activate(){if(m_session&&m_session->tuner()->shutdownPending()){message(tr("Stop dToF tuning before changing trusted devices."),true);return;}try{const auto p=m_store->byId(m_id);if(!p.ready())throw std::runtime_error("Confirm calibration and inference orientation before selecting this device");if(m_session)m_session->stop();m_store->save(m_store->profiles(),p.id);if(m_session)m_session->reloadDeviceProfiles();refresh(p.id);message(tr("Device selected. Click Start in either imaging panel when ready."));}catch(const std::exception&e){message(QString::fromUtf8(e.what()),true);}}
void RvDeviceManagerDialog::remove(){if(m_session&&m_session->tuner()->shutdownPending()){message(tr("Stop dToF tuning before changing trusted devices."),true);return;}try{auto list=m_store->profiles();const auto p=m_store->byId(m_id);if(QMessageBox::question(this,tr("Remove device"),tr("Remove trust for %1? This does not change the device.").arg(p.name))!=QMessageBox::Yes)return;
    if(m_session)m_session->stop();for(int i=0;i<list.size();++i)if(list[i].id==p.id){list.removeAt(i);break;}m_store->save(list,m_store->activeId()==p.id?QString():m_store->activeId());if(m_session)m_session->reloadDeviceProfiles();refresh();message(tr("Device trust removed."));}catch(const std::exception&e){message(QString::fromUtf8(e.what()),true);}}
void RvDeviceManagerDialog::importDevices(){if(m_session&&m_session->tuner()->shutdownPending()){message(tr("Stop dToF tuning before changing trusted devices."),true);return;}const auto path=QFileDialog::getOpenFileName(this,tr("Import devices"),{},tr("Device files (*.json)"));if(path.isEmpty())return;try{QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024)throw std::runtime_error("Cannot read device import");auto imported=RvDeviceStore::parseImport(f.readAll());if(imported.isEmpty())return;
    QStringList summary;for(const auto &p:imported)summary<<p.name+" / "+p.serial+"\n"+p.fingerprint();QMessageBox confirm(QMessageBox::Question,tr("Trust imported devices"),tr("Import %1 device(s) from a trusted source? Existing devices will not be overwritten.").arg(imported.size()),QMessageBox::Yes|QMessageBox::Cancel,this);confirm.setDetailedText(summary.join("\n\n"));confirm.setDefaultButton(QMessageBox::Cancel);if(confirm.exec()!=QMessageBox::Yes)return;
    const auto list=RvDeviceStore::mergeProfiles(m_store->profiles(),imported);m_store->save(list,m_store->activeId());refresh(imported.first().id);message(tr("Devices imported. Passwords were not imported."));}catch(const std::exception&e){message(QString::fromUtf8(e.what()),true);}}
void RvDeviceManagerDialog::exportDevices(){const auto path=QFileDialog::getSaveFileName(this,tr("Export devices"),"rv1103-devices.json",tr("Device files (*.json)"));if(path.isEmpty())return;try{const auto bytes=RvDeviceStore::exportProfiles(m_store->profiles());QSaveFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit())throw std::runtime_error("Cannot save device export");message(tr("Device profiles exported without passwords."));}catch(const std::exception&e){message(QString::fromUtf8(e.what()),true);}}
void RvDeviceManagerDialog::importBoardFilesForStart(const RvDeviceProfile &device)
{
    if (!m_store || m_usbEnrollmentPending || m_worker->state()!=QProcess::NotRunning) return;
    try {
        m_store->reload();
        if (m_store->byId(device.id).json()!=device.json())
            throw std::runtime_error("Device profile changed before import");
        refresh(device.id);
        importLiteSrModel(true);
    } catch (const std::exception &error) {
        showBoardImportFailure(QString::fromUtf8(error.what()),"profile_check",-1);
    }
}

void RvDeviceManagerDialog::importLiteSrModel(bool confirmed)
{
    if (!m_store || m_fresh || m_worker->state()!=QProcess::NotRunning) return;
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1") {
        message(tr("On-board model import is unavailable in offline mode."),true);
        return;
    }
    if (m_session && (!m_session->userStopped() || m_session->busy() || m_session->tuner()->shutdownPending())) {
        message(tr("Stop imaging before importing board files."),true);
        return;
    }
    m_resumeImaging=confirmed;
    runWorker("--import-litesr-model");
}
void RvDeviceManagerDialog::runWorker(const QString &operation){if(operation=="--forget-device-credentials"&&m_session&&m_session->tuner()->shutdownPending()){message(tr("Stop dToF tuning before changing trusted devices."),true);return;}
    if (operation=="--import-litesr-model") {
        m_operation=operation;
        m_workerFailure.clear();
    }
    try{const auto worker=RuntimeDiscovery::rvControlForLaunch();if(!worker.error.isEmpty())throw std::runtime_error(worker.error.toStdString());QStringList args{operation};
    if(operation=="--inspect-host"){
        m_probeId=m_id;m_probeHost=m_host->text().trimmed();m_probePort=m_port->value();m_probeUsername=m_username->text().trimmed();
        if(!RvDeviceStore::validHost(m_probeHost))throw std::runtime_error("Invalid device address");args<<m_probeHost<<QString::number(m_probePort);
    }else if(operation=="--read-device-serial"){
        if(m_id!=m_probeId||m_host->text().trimmed()!=m_probeHost||m_port->value()!=m_probePort||m_username->text().trimmed()!=m_probeUsername||publicKeyText(m_key)!=m_probeKey)
            throw std::runtime_error("Device details changed during reading; connect and read again");
        RvDeviceStore::keyFingerprint(m_probeKey);args<<m_probeHost<<QString::number(m_probePort)<<m_probeUsername<<m_probeKey;
    }
    else{
        const auto p=m_store->byId(m_id);
        if(operation=="--import-litesr-model"&&m_resumeImaging){
            if(!m_session||m_session->deviceProfile().json()!=p.json()||!m_session->userStopped()||m_session->busy())
                throw std::runtime_error("Device or imaging state changed before import");
        }else{
            auto d=draft();d.lastConnected=p.lastConnected;
            if(d.json()!=p.json())throw std::runtime_error("Save the edited device before verifying or clearing its login");
        }
        args<<"--device-store"<<m_store->path()<<"--device-revision"<<m_store->revision()<<"--device-id"<<p.id;
    }
    m_operation=operation;m_workerFailure.clear();m_worker->setProcessEnvironment(RuntimeDiscovery::nativeWorkerEnvironment(worker.executable));m_worker->setProgram(worker.executable);m_worker->setArguments(args);busy(true);message(operation=="--read-device-serial"?tr("Log in with the usual device password to read its serial..."):tr("Working..."));m_worker->start();m_timeout->start(operation=="--read-device-serial"?180000:60000);
}catch(const std::exception&e){busy(false);operationFailed(QString::fromUtf8(e.what()));}}
