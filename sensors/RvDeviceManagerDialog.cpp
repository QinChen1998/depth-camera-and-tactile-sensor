#include "RvDeviceManagerDialog.h"
#include "RvImportDiagnostics.h"
#include "DepthSdkClient.h"
#include "RvSessionController.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

RvDeviceManagerDialog::RvDeviceManagerDialog(RvSessionController *session, QWidget *parent)
    : QDialog(parent), m_imaging(session), m_session(session->depthSdk()), m_devices(new QListWidget(this)),
      m_usb(new QPushButton(this)), m_select(new QPushButton(this)), m_remove(new QPushButton(this)),
      m_maintain(new QPushButton(this)), m_grid(new QPushButton(this)), m_recover(new QPushButton(this)),
      m_rotation(new QComboBox(this)), m_gridProgress(new QProgressBar(this)), m_status(new QLabel(this))
{
    setObjectName("rvDeviceManagerDialog");
    resize(660, 320);
    m_devices->setObjectName("rvDeviceList");
    m_usb->setObjectName("rvDeviceConnectUsb");
    m_select->setObjectName("rvDeviceUse");
    m_remove->setObjectName("rvDeviceRemove");
    m_maintain->setObjectName("rvDeviceMaintainCapture");
    m_grid->setObjectName("rvDeviceCheckRgbLsc");
    m_recover->setObjectName("rvDeviceRecover");
    m_rotation->setObjectName("rvDeviceRotation");
    m_rotation->addItem(QString(), 0);
    m_rotation->addItem(QString(), 180);
    m_gridProgress->setObjectName("rvRgbLscProgress");
    m_gridProgress->setRange(0, 0);
    m_gridProgress->setTextVisible(false);
    m_gridProgress->setAccessibleName(tr("Correction grid progress"));
    m_gridProgress->hide();
    m_status->setObjectName("rvDeviceStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_usb, 0, Qt::AlignLeft);
    layout->addWidget(m_devices, 1);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_select);
    actions->addWidget(m_maintain);
    actions->addWidget(m_grid);
    actions->addWidget(m_recover);
    actions->addStretch();
    actions->addWidget(m_remove);
    layout->addLayout(actions);
    layout->addWidget(m_rotation, 0, Qt::AlignLeft);
    layout->addWidget(m_status);
    layout->addWidget(m_gridProgress);

    connect(m_usb, &QPushButton::clicked, this, [this] { connectViaUsb(); });
    connect(m_select, &QPushButton::clicked, this, &RvDeviceManagerDialog::selectDevice);
    connect(m_remove, &QPushButton::clicked, this, &RvDeviceManagerDialog::removeDevice);
    connect(m_maintain, &QPushButton::clicked, this, &RvDeviceManagerDialog::checkBoardFiles);
    connect(m_grid, &QPushButton::clicked, this, &RvDeviceManagerDialog::checkRgbLsc);
    connect(m_recover, &QPushButton::clicked, this, [this] {
        if (canManageSavedDevice()) runBoardOperation(BoardOperation::Recovery, selectedId());
    });
    connect(m_devices, &QListWidget::currentItemChanged, this, &RvDeviceManagerDialog::syncControls);
    connect(m_rotation, &QComboBox::activated, this, [this](int index) {
        m_imaging->setSdkInputRotation(m_rotation->itemData(index).toInt());
        syncControls();
    });
    connect(m_imaging, &RvSessionController::deviceChanged, this, &RvDeviceManagerDialog::syncControls);
    connect(m_session, &DepthSdkClient::stateChanged, this, [this] {
        if (m_usbPending && m_session->userStopped()) finishUsbEnrollment(false);
        syncControls();
    });
    connect(m_session, &DepthSdkClient::deviceChanged, this, [this] { refresh(m_session->activeDeviceId()); });
    connect(m_session, &DepthSdkClient::usbStatusChanged, this, &RvDeviceManagerDialog::updateUsbOperation);
    connect(m_session, &DepthSdkClient::rgbLscChanged, this, &RvDeviceManagerDialog::updateRgbLscOperation);
    connect(m_session, &DepthSdkClient::maintenanceChanged, this, [this](const QJsonObject &status) {
        if (m_operation != BoardOperation::Recovery) updateBoardOperation(status);
    });
    connect(m_session, &DepthSdkClient::deviceRecoveryChanged, this, [this](const QJsonObject &status) {
        if (m_operation == BoardOperation::Recovery) updateBoardOperation(status);
    });

    initializeLabels();
    refresh(m_session->activeDeviceId());
}

QString RvDeviceManagerDialog::selectedId() const
{
    const auto *item = m_devices->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

bool RvDeviceManagerDialog::operationPending() const
{
    return m_usbPending || m_operation != BoardOperation::None || m_gridOperation != GridOperation::None;
}

bool RvDeviceManagerDialog::canManageSavedDevice() const
{
    return m_registryReady && !selectedId().isEmpty() && !operationPending() && m_session->canManageDevices();
}

void RvDeviceManagerDialog::refresh(const QString &id)
{
    const QSignalBlocker block(m_devices);
    m_devices->clear();
    const auto devices = m_session->savedDevices(&m_registryReady);
    int number = 0;
    for (const auto &device : devices) {
        const auto fields = device.toObject();
        const auto deviceId = fields["id"].toString();
        const auto savedName = fields["name"].toString();
        ++number;
        const auto name = savedName.isEmpty() ? tr("CXWL Depth Camera %1").arg(number) : savedName;
        auto *item = new QListWidgetItem(fields["selected"].toBool() ? tr("%1 (current)").arg(name) : name, m_devices);
        item->setData(Qt::UserRole, deviceId);
        item->setData(Qt::UserRole + 1, fields["selected"].toBool());
        if (deviceId == id) m_devices->setCurrentItem(item);
    }
    if (!m_devices->currentItem() && m_devices->count()) m_devices->setCurrentRow(0);
    if (!m_registryReady) message(tr("Could not read the saved device list."), true);
    syncControls();
}

void RvDeviceManagerDialog::syncControls()
{
    m_usb->setEnabled(m_registryReady && !operationPending() && !m_session->busy() && m_session->userStopped());
    m_devices->setEnabled(!operationPending());
    const bool allowed = canManageSavedDevice() && !m_imaging->busy();
    m_select->setEnabled(allowed && !m_devices->currentItem()->data(Qt::UserRole + 1).toBool());
    m_remove->setEnabled(allowed);
    m_maintain->setEnabled(allowed);
    m_grid->setEnabled(allowed);
    m_recover->setEnabled(allowed);
    const bool selected = m_devices->currentItem() && m_devices->currentItem()->data(Qt::UserRole + 1).toBool();
    m_rotation->setEnabled(allowed && selected && m_imaging->userStopped());
    m_rotation->setCurrentIndex(selected ? m_rotation->findData(m_imaging->assistInputRotationDegrees()) : -1);
    m_gridProgress->setVisible(m_gridOperation == GridOperation::Checking || m_gridOperation == GridOperation::Calibrating);
}

void RvDeviceManagerDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        initializeLabels();
        refresh(m_session->activeDeviceId());
    }
}

void RvDeviceManagerDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    refresh(m_session->activeDeviceId());
    if (m_registryReady && !operationPending() && m_status->text().isEmpty()) {
        message(m_devices->count() ? tr("Stop capture to switch or maintain saved devices.")
                                  : tr("Connect a CXWL Depth Camera with a USB data cable to get started."));
    }
}

void RvDeviceManagerDialog::reject()
{
    if (!operationPending()) QDialog::reject();
}

void RvDeviceManagerDialog::closeEvent(QCloseEvent *event)
{
    if (operationPending()) event->ignore();
    else QDialog::closeEvent(event);
}

void RvDeviceManagerDialog::connectViaUsb(quint64 checkId)
{
    if (operationPending()) return;
    const auto result = m_session->connectViaUsb(checkId);
    if (result != CXWL_DEPTH_OK) {
        message(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1"
            ? tr("USB connection is unavailable in offline mode.")
            : tr("Wait for the current device operation to finish, then try again."), true);
        return;
    }
    m_usbPending = true;
    setProperty("rvUsbRuntimeReplaced", QVariant());
    message(tr("Connecting to CXWL Depth Camera via USB..."));
    syncControls();
}

void RvDeviceManagerDialog::updateUsbOperation(const QJsonObject &status)
{
    if (!m_usbPending) return;
    const auto state = status["state"];
    if (state == "enrolled" || state == "error" || state == "cancelled") {
        setProperty("rvUsbRuntimeReplaced", status["runtime_replaced"].toBool());
        if (state == "enrolled" && status["rgb_lsc_offer"].toBool()) {
            offerUsbRgbLsc();
            return;
        }
        finishUsbEnrollment(state == "enrolled");
    }
}

void RvDeviceManagerDialog::finishUsbEnrollment(bool added)
{
    if (!m_usbPending) return;
    m_usbPending = false;
    if (added) {
        refresh(m_session->activeDeviceId());
        if (m_registryReady) message(tr("The CXWL Depth Camera was verified and selected for imaging."));
    } else {
        message(tr("USB connection setup ended."));
        syncControls();
    }
}

void RvDeviceManagerDialog::selectDevice()
{
    if (!canManageSavedDevice()) return;
    if (m_session->selectDevice(selectedId()) == CXWL_DEPTH_OK)
        message(tr("Device selected. Click Start in either imaging panel when ready."));
    else
        message(tr("Could not select the device. Reopen Device Manager and try again."), true);
}

void RvDeviceManagerDialog::removeDevice()
{
    if (!canManageSavedDevice()) return;
    const auto id = selectedId();
    if (QMessageBox::question(this, tr("Remove device"), tr("Remove the selected device from this computer?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes
        || !canManageSavedDevice() || selectedId() != id) return;
    if (m_session->removeDevice(id) == CXWL_DEPTH_OK) message(tr("Device removed."));
    else message(tr("Could not remove the device. Reopen Device Manager and try again."), true);
}

void RvDeviceManagerDialog::checkBoardFiles()
{
    if (!canManageSavedDevice()) return;
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1") {
        message(tr("Board file maintenance is unavailable in offline mode."), true);
        return;
    }
    const auto id = selectedId();
    if (QMessageBox::question(this, tr("Check board files"),
            tr("Check the selected device, repair required files, and archive older runtime files on this computer?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes
        && canManageSavedDevice() && id == selectedId()) runBoardOperation(BoardOperation::Maintenance, id);
}

void RvDeviceManagerDialog::checkRgbLsc()
{
    if (!canManageSavedDevice()) return;
    m_gridDevice = selectedId();
    if (m_session->checkRgbLsc(m_gridDevice) != CXWL_DEPTH_OK) {
        message(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1"
            ? tr("Correction grid calibration is unavailable in offline mode.")
            : tr("Could not check the correction grid. Check the device connection."), true);
        return;
    }
    m_gridOperation = GridOperation::Checking;
    message(tr("Checking the correction grid..."));
    syncControls();
}

bool RvDeviceManagerDialog::confirmRgbLsc(bool fromUsb)
{
    auto description = tr("Keep the depth camera still and the lighting steady. The device will collect 21 raw frames at six exposure levels to calculate the correction grid in memory. The frames will not be saved. The grid will be deployed after the ISP trial.");
    if (fromUsb) description += "\n\n" + tr("Choose Skip to continue connecting with the current camera settings.");
    QMessageBox prompt(QMessageBox::Question, tr("Check correction grid"), description,
                       QMessageBox::Ok | QMessageBox::Cancel, this);
    prompt.setObjectName("rvRgbLscPrompt");
    prompt.setDefaultButton(QMessageBox::Cancel);
    prompt.button(QMessageBox::Ok)->setText(tr("Start calibration"));
    if (fromUsb) prompt.button(QMessageBox::Cancel)->setText(tr("Skip"));
    return prompt.exec() == QMessageBox::Ok;
}

void RvDeviceManagerDialog::offerUsbRgbLsc()
{
    m_gridDevice = m_session->activeDeviceId();
    m_gridOperation = GridOperation::Confirming;
    finishUsbEnrollment(true);
    const auto revision = m_session->stopRevision();
    QTimer::singleShot(0, this, [this, revision] {
        const bool calibrate = m_session->stopRevision() == revision && confirmRgbLsc(true);
        const bool continuing = m_session->stopRevision() == revision
            && m_session->finishUsbGridOffer(m_gridDevice, calibrate);
        m_gridOperation = continuing && calibrate ? GridOperation::Calibrating : GridOperation::None;
        if (m_gridOperation == GridOperation::Calibrating) message(tr("Capturing, calculating and applying the correction grid..."));
        else if (!continuing) message(tr("USB connection setup ended."));
        syncControls();
    });
}

void RvDeviceManagerDialog::updateRgbLscOperation(const QJsonObject &status)
{
    if (status["device_id"] != m_gridDevice || (status["state"] != "ready" && status["state"] != "error")) return;
    const auto operation = m_gridOperation;
    const auto correction = status["correction"].toObject();
    if (operation == GridOperation::Checking && status["operation"] == "check") {
        m_gridOperation = GridOperation::None;
        if (status["state"] != "ready" || correction["state"] == "unsupported") {
            message(status["state"] == "ready" ? tr("This device does not have a compatible SC3336 correction grid.")
                : tr("Could not check the correction grid. Check the device connection."), true);
            syncControls();
            return;
        }
        m_gridOperation = GridOperation::Confirming;
        syncControls();
        const auto revision = m_session->stopRevision();
        QTimer::singleShot(0, this, [this, revision] {
            const bool confirmed = m_session->stopRevision() == revision && confirmRgbLsc(false);
            m_gridOperation = GridOperation::None;
            if (confirmed && m_session->stopRevision() == revision && selectedId() == m_gridDevice
                && canManageSavedDevice()) {
                if (m_session->startRgbLscCalibration(m_gridDevice) == CXWL_DEPTH_OK) {
                    m_gridOperation = GridOperation::Calibrating;
                    message(tr("Capturing, calculating and applying the correction grid..."));
                } else message(tr("Could not start correction grid calibration. Stop capture and try again."), true);
            } else message(tr("Correction grid calibration cancelled."));
            syncControls();
        });
    } else if (operation == GridOperation::Calibrating && status["operation"] == "calibrate") {
        m_gridOperation = GridOperation::None;
        const auto result = status["result"].toString();
        if (status["state"] == "ready" && result == "installed" && status["iq_replaced"] == true) {
            message(tr("The board IQ was replaced and verified."));
        } else if (status["state"] == "ready" && result == "already_deployed" && status["iq_replaced"] == false) {
            message(tr("The board already uses this correction grid. The IQ was verified."));
        } else if (result == "insufficient_raw_samples") {
            message(tr("Correction grid replacement failed. The RAW exposure differences were insufficient. Improve the lighting and try again."), true);
        } else if (result == "replacement_unconfirmed") {
            message(tr("Could not confirm the IQ replacement. Reconnect the device and check the grid status."), true);
        } else {
            const auto detail = status["error"].toString();
            message(detail.isEmpty() ? tr("Correction grid replacement failed. Check the device connection and read the grid status.")
                : tr("Correction grid replacement failed. %1").arg(detail), true);
        }
        syncControls();
    }
}

void RvDeviceManagerDialog::importBoardFilesForStart(const QString &id)
{
    if (!operationPending() && m_session->activeDeviceId() == id) runBoardOperation(BoardOperation::RepairForStart, id);
}

void RvDeviceManagerDialog::runBoardOperation(BoardOperation operation, const QString &id)
{
    const auto result = operation == BoardOperation::Recovery ? m_session->recoverDevice(id)
        : m_session->maintainBoardFiles(id,
            operation == BoardOperation::Maintenance ? CXWL_DEPTH_BOARD_MAINTENANCE : CXWL_DEPTH_BOARD_REPAIR);
    if (result != CXWL_DEPTH_OK) {
        const bool offline = qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1";
        message(operation == BoardOperation::Recovery
            ? (offline ? tr("Device recovery is unavailable in offline mode.")
                       : tr("Could not start device recovery. Stop capture and check the device connection."))
            : (offline ? tr("Board file maintenance is unavailable in offline mode.")
                       : tr("Board file check failed. Check the device connection and try again.")), true);
        return;
    }
    m_operation = operation;
    m_operationDevice = id;
    m_operationStopRevision = m_session->stopRevision();
    message(operation == BoardOperation::Recovery ? tr("Recovering device...") : tr("Checking board files..."));
    syncControls();
}

void RvDeviceManagerDialog::updateBoardOperation(const QJsonObject &status)
{
    if (m_operation == BoardOperation::None || status["state"] == "running") return;
    const auto operation = m_operation;
    m_operation = BoardOperation::None;
    syncControls();
    if (status["state"] != "ready") {
        const auto summary = operation == BoardOperation::Recovery
            ? tr("Device recovery failed. Check the diagnostic report.")
            : tr("Board file check failed. Check the device connection and try again.");
        message(summary, true);
        RvImportDiagnostics::showFailure(this, operation == BoardOperation::Recovery ? "device_recovery" : "board_runtime_maintenance", summary, status);
        return;
    }
    if (operation == BoardOperation::Recovery) {
        message(tr("Device recovered. Click Start to begin capture."));
        return;
    }
    message(status["archived_rollbacks"].toInt() > 0
        ? tr("Board files checked. Archived rollback copies on this computer.") : tr("Board files are ready."));
    if (operation == BoardOperation::RepairForStart && m_session->canManageDevices()
        && m_session->stopRevision() == m_operationStopRevision && m_session->activeDeviceId() == m_operationDevice) {
        m_session->start();
    }
}

void RvDeviceManagerDialog::message(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? "color: #c64a42;" : "");
}

void RvDeviceManagerDialog::initializeLabels()
{
    m_rotation->setAccessibleName(tr("Host inference input rotation"));
    m_rotation->setItemText(0, tr("Inference input: 0°"));
    m_rotation->setItemText(1, tr("Inference input: 180°"));
    setWindowTitle(tr("Device Manager"));
    m_devices->setAccessibleName(tr("Saved devices"));
    m_usb->setText(tr("Connect via USB"));
    m_select->setText(tr("Use selected device"));
    m_remove->setText(tr("Remove device"));
    m_maintain->setText(tr("Check board files"));
    m_grid->setText(tr("Check correction grid"));
    m_recover->setText(tr("Recover device"));
    m_maintain->setToolTip(tr("Repair the board runtime and archive older rollback copies on this computer."));
}
