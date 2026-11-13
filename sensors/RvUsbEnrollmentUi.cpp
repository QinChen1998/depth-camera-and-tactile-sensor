#include "RvUsbEnrollment.h"
#include "RvDeviceManagerDialog.h"
#include "RvImportDiagnostics.h"
#include "../SensorMainWindow.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <utility>

namespace {
class UsbEnrollmentUi final : public QObject {
    Q_DECLARE_TR_FUNCTIONS(RvUsbEnrollment)
public:
    UsbEnrollmentUi(RvSessionController *session, SensorMainWindow *window) : QObject(window)
    {
        RvUsbEnrollmentHooks hooks;
        hooks.canOffer = [window] { return window->isVisible(); };
        hooks.canOfferAutomatically = [window] {
            const auto *dialog = window->findChild<RvDeviceManagerDialog *>();
            return !QApplication::activeModalWidget() && !(dialog && dialog->isVisible());
        };
        hooks.offer = [window](bool existing, bool network, const QJsonObject &identity) {
            window->offerUsbSetup(existing, network, identity);
        };
        hooks.status = [window](const QString &message) { window->statusBar()->showMessage(message, 10000); };
        hooks.started = [this] { if (m_dialog) m_dialog->beginUsbEnrollment(); };
        hooks.finished = [this](bool added, bool replaced) {
            const auto dialog = std::exchange(m_dialog, {});
            if (!dialog) return;
            dialog->setProperty("rvUsbRuntimeReplaced", replaced);
            dialog->finishUsbEnrollment(added);
        };
        hooks.failure = [this, window](const QString &message, const QJsonObject &details, bool automatic) {
            if (automatic) {
                const QDir data(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
                RvImportDiagnostics::writeReport(data.filePath("DctsDiagnostics/rv1103"), "usb_enrollment", message, details);
                window->statusBar()->showMessage(message, 10000);
                return;
            }
            QWidget *parent = m_dialog ? static_cast<QWidget *>(m_dialog.data()) : window;
            RvImportDiagnostics::showFailure(parent, "usb_enrollment", message, details);
        };
        auto *operation = createRvUsbEnrollment(session, this, std::move(hooks), true);
        connect(window, &SensorMainWindow::usbDeviceConnectionRequested, this, [this, operation](RvDeviceManagerDialog *dialog) {
            if (!m_dialog) {
                m_dialog = dialog;
                if (beginRvUsbEnrollment(operation, dialog->property("rvUsbExpectedIdentity").toJsonObject())) return;
                m_dialog.clear();
            }
            QMessageBox::information(dialog, tr("Connect via USB"),
                tr("Wait for the current device operation to finish, then try again."));
        });
    }
private:
    QPointer<RvDeviceManagerDialog> m_dialog;
};
}

void watchRvUsbDevices(RvSessionController *session, SensorMainWindow *window)
{
    if (session && qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") != "1") new UsbEnrollmentUi(session, window);
}

QJsonObject checkRvUsbEnrollment()
{
    auto checks = checkRvUsbEnrollmentCore();
    const QString serial = "1234567890abcdef";
    QTemporaryDir temporary;
    const auto report = RvImportDiagnostics::writeReport(temporary.filePath("diagnostics"), "test_usb_setup", "test failure",
        {{"phase", "adb_identity_and_delivery_probe"}, {"serial", serial}});
    QFile reportFile(report);
    const auto diagnostic = reportFile.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(reportFile.readAll()).object() : QJsonObject();
    checks["USB_IMPORT_DIAGNOSTIC_REPORT_WRITTEN"] = temporary.isValid() && diagnostic["operation"] == "test_usb_setup"
        && diagnostic["reason"] == "test failure" && diagnostic["details"].toObject()["serial"] == serial
        && diagnostic["details"].toObject()["phase"] == "adb_identity_and_delivery_probe";
    checks["USB_IMPORT_DIAGNOSTIC_WRITE_FAILURE_HANDLED"] = temporary.isValid()
        && RvImportDiagnostics::writeReport(report, "test_usb_setup", "test failure", {}).isEmpty();
    return checks;
}
