#include "RvUsbEnrollment.h"
#include "RvImportDiagnostics.h"
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include "RvDeviceManagerDialog.h"
#include "RvSessionController.h"
#include "RvTunerController.h"
#include "../RuntimeDiscovery.h"
#include "../SensorMainWindow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QVBoxLayout>
#include <functional>
#include <stdexcept>
#include <utility>

namespace {
constexpr auto host = "172.32.0.93";
constexpr auto probeCommand = R"SH(
set -e
tr '\000' '\n' </proc/device-tree/compatible | grep -qx 'rockchip,rv1103'
sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo
cat /proc/sys/kernel/random/boot_id
cat /etc/ssh/ssh_host_ed25519_key.pub
sha256sum /root/r144_depth/M48_geometry.bin 2>/dev/null || echo missing
if [ -x /root/r144_depth/control.sh ] && [ -x /root/r144_depth/depth_status ]; then
    echo ready
elif [ ! -e /root/r144_depth ] && [ ! -L /root/r144_depth ] &&
     [ ! -e /root/dtof_candidates_20261009 ] && [ ! -L /root/dtof_candidates_20261009 ]; then
    echo bootstrap
else
    echo missing
fi
addresses=$(ip -4 addr show dev usb0)
printf '%s\n' "$addresses" | awk '$1=="inet"{print $2}'
)SH";

// Always resolve the adapter again inside the privileged process. Alias,
// interface index and the generic RNDIS description alone are not identities.
constexpr auto networkScript = R"PS(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Get-Target {
    $result = @(Get-NetAdapter | Where-Object {
        $_.Status -eq 'Up' -and $_.PnPDeviceID -like 'USB\VID_2207&PID_0019&MI_*' -and
        (Get-PnpDeviceProperty -InstanceId $_.PnPDeviceID -KeyName DEVPKEY_Device_Parent).Data -eq $usbParent
    })
    if ($result.Count -ne 1) { throw 'The RV1103 USB network adapter is missing or ambiguous.' }
    $result[0]
}
function Test-BoardRoute($index) {
    $route = @(Find-NetRoute -RemoteIPAddress '172.32.0.93')
    return ($route.Count -gt 0 -and @($route | Where-Object InterfaceIndex -ne $index).Count -eq 0)
}
$target = Get-Target
$index = $target.ifIndex
$addresses = @(Get-NetIPAddress -InterfaceIndex $index -AddressFamily IPv4 -ErrorAction SilentlyContinue)
$existing = @($addresses | Where-Object {
    $_.IPAddress -like '172.32.*' -and $_.IPAddress -ne '172.32.0.93' -and
    $_.PrefixLength -eq 16 -and $_.AddressState -eq 'Preferred' -and -not $_.SkipAsSource
})
if ($existing.Count -gt 0 -and (Test-BoardRoute $index)) { exit 0 }
if (@($addresses | Where-Object IPAddress -notlike '169.254.*').Count -gt 0) {
    throw 'The RV1103 USB adapter already has a different IPv4 configuration.'
}
if (@(Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -like '172.32.*').Count -gt 0) {
    throw 'Another Windows interface is already using the RV1103 subnet.'
}
if (-not $elevated) { exit 10 }
$original = Get-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4
if ((Get-Target).InterfaceGuid -ne $target.InterfaceGuid) { throw 'The USB adapter changed.' }
try {
    New-NetIPAddress -InterfaceIndex $index -IPAddress '172.32.0.100' -PrefixLength 16 | Out-Null
    for ($i=0; $i -lt 20; $i++) {
        $address = Get-NetIPAddress -InterfaceIndex $index -IPAddress '172.32.0.100'
        if ($address.AddressState -eq 'Preferred' -and (Test-BoardRoute $index)) { exit 0 }
        if ($address.AddressState -eq 'Duplicate') { break }
        Start-Sleep -Milliseconds 500
    }
    throw 'The RV1103 USB network address or route did not become usable.'
} catch {
    if ((Get-Target).InterfaceGuid -eq $target.InterfaceGuid) {
        Remove-NetIPAddress -InterfaceIndex $index -IPAddress '172.32.0.100' -Confirm:$false -ErrorAction SilentlyContinue
        if ($original.Dhcp -eq 'Enabled') { Set-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4 -Dhcp Enabled }
    }
    throw
}
)PS";

QString encodedPowerShell(const QString &script)
{
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(script.utf16()),
                                         script.size() * 2).toBase64());
}

QString findAdb()
{
    const auto installed = QDir(QCoreApplication::applicationDirPath()).filePath("platform-tools/adb.exe");
    return QFileInfo::exists(installed) ? installed : QStandardPaths::findExecutable("adb.exe");
}

struct Board {
    RvDeviceProfile profile;
    QString boot;
    QString address;
    bool bootstrap = false;
    bool existing = false;
};

struct UsbTransport {
    QString serial;
    QString id;
};

UsbTransport findTransport(const QByteArray &output)
{
    UsbTransport result;
    static const QRegularExpression entry("^([0-9a-f]{16})\\s+device\\s+.*\\btransport_id:([0-9]+)\\s*$");
    for (const auto &line : QString::fromLatin1(output).split('\n')) {
        const auto match = entry.match(line.trimmed());
        if (!match.hasMatch()) continue;
        if (!result.serial.isEmpty()) return {};
        result = {match.captured(1), match.captured(2)};
    }
    return result;
}

struct Probe {
    QString serial, boot, key, geometry;
    bool servicesReady, bootstrap;
    QStringList addresses;
};

Probe parseIdentity(const QString &serial, const QByteArray &output)
{
    const auto fields = QString::fromLatin1(output).trimmed().split('\n');
    if (fields.size() < 5 || fields[0].trimmed() != serial
        || !QRegularExpression("^[0-9a-f-]{36}$").match(fields[1].trimmed()).hasMatch())
        throw std::runtime_error("Unexpected RV1103 identity response.");
    if (!fields[2].startsWith("ssh-ed25519 "))
        throw std::runtime_error("RV1103 requires an Ed25519 host key.");
    Probe probe{serial, fields[1].trimmed(), fields[2].simplified().section(' ', 1, 1),
                fields[3].simplified().section(' ', 0, 0), fields[4].trimmed() == "ready", fields[4].trimmed() == "bootstrap", {}};
    RvDeviceStore::keyFingerprint(probe.key);
    for (qsizetype i = 5; i < fields.size(); ++i) probe.addresses.append(fields[i].trimmed());
    return probe;
}

Board enrollmentBoard(const Probe &probe, const QVector<RvDeviceProfile> &profiles = {})
{
    Board result;
    result.bootstrap = !probe.servicesReady;
    auto &profile = result.profile;
    profile.id = profile.serial = probe.serial;
    profile.name = "RV1103 - " + probe.serial;
    profile.host = host;
    profile.calibration = "rv20261008-f1";
    if (probe.geometry != profile.geometrySha()) profile.calibration = "rv20261008-f2";
    profile.inputRotation = 0;
    profile.cameraMountRotation = 180;
    // Use the existing conservative coordinator contract for an enrolled board.
    profile.publicKey = probe.key;
    if (probe.geometry != profile.geometrySha()) profile.calibration = "rv20261008-f1";
    for (const auto &saved : profiles) {
        if (saved.serial != probe.serial && saved.publicKey != probe.key) continue;
        if (saved.serial != probe.serial || saved.publicKey != probe.key)
            throw std::runtime_error("The RV1103 serial and SSH key conflict with a saved device identity.");
        profile = saved;
        profile.host = host;
        profile.sshPort = 22;
        result.existing = true;
        RvDeviceProfile f1, f2; f1.calibration="rv20261008-f1";f2.calibration="rv20261008-f2";
        if ((probe.geometry==f1.geometrySha() || probe.geometry==f2.geometrySha()) && probe.geometry!=profile.geometrySha())
            throw std::runtime_error("The RV1103 calibration differs from its saved device profile.");
    }
    profile.validate();
    result.boot = probe.boot;
    if (!probe.addresses.isEmpty() && probe.addresses != QStringList{"172.32.0.93/16"})
        throw std::runtime_error("RV1103 usb0 has a different IPv4 configuration.");
    result.address = probe.addresses.value(0);
    return result;
}

bool knownIdentity(const QVector<RvDeviceProfile> &profiles, const Probe &probe)
{
    for (const auto &profile : profiles) {
        if (profile.serial == probe.serial || profile.publicKey == probe.key) return true;
    }
    return false;
}

bool stripShellReceipt(QByteArray &output)
{
    output = output.trimmed();
    if (output == "RV_USB_OK") {
        output.clear();
        return true;
    }
    if (!output.endsWith("\nRV_USB_OK")) return false;
    output.chop(sizeof("\nRV_USB_OK") - 1);
    return true;
}

// Only an unaddressed board gets the managed startup script. It leaves any
// IPv4 configured by the image or by an administrator alone.
QString configureBoardCommand(const Board &board)
{
    return QString::fromLatin1(R"SH(
set -e
test "$(sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo)" = '%1'
test "$(cat /proc/sys/kernel/random/boot_id)" = '%2'
test "$(awk 'NR==1{print $2}' /etc/ssh/ssh_host_ed25519_key.pub)" = '%3'
addresses=$(ip -4 addr show dev usb0)
if printf '%s\n' "$addresses" | grep -q 'inet '; then exit 1; fi
file=/etc/init.d/S99zzDepthCameraUsb
umask 022
tmp=$(mktemp /etc/init.d/.DepthCameraUsb.XXXXXX)
trap 'rm -f "$tmp"' EXIT
cat >"$tmp" <<'RV_USB_INIT'
#!/bin/sh
[ "$1" = start ] || exit 0
addresses=$(ip -4 addr show dev usb0) || exit 1
if printf '%s\n' "$addresses" | grep -q 'inet '; then exit 0; fi
ip addr add 172.32.0.93/16 dev usb0 || exit 1
ip link set dev usb0 up
RV_USB_INIT
chmod 755 "$tmp"
if [ -e "$file" ]; then cmp -s "$tmp" "$file"; else ln "$tmp" "$file"; fi
sh "$file" start
ip -4 addr show dev usb0 | grep -q 'inet 172\.32\.0\.93/16 '
)SH").arg(board.profile.serial, board.boot, board.profile.publicKey);
}

class Enrollment final : public QObject {
    Q_DECLARE_TR_FUNCTIONS(RvUsbEnrollment)
public:
    Enrollment(RvSessionController *session, SensorMainWindow *window)
        : QObject(window), m_session(session), m_window(window), m_adb(findAdb())
    {
        m_poll.setInterval(2500);
        m_timeout.setSingleShot(true);
        connect(&m_poll, &QTimer::timeout, this, [this] { scan(false); });
        connect(window, &SensorMainWindow::usbDeviceConnectionRequested, this,
                [this](RvDeviceManagerDialog *dialog) { connectFromManager(dialog); });
        connect(&m_timeout, &QTimer::timeout, this, [this] {
            m_processStatus = "timeout";
            m_process.kill();
        });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                m_processStatus = "failed_to_start";
                completed(-1);
            }
        });
        connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            completed(status == QProcess::NormalExit ? code : -1);
        });
        m_poll.start();
        QTimer::singleShot(0, this, [this] { scan(false); });
    }
    ~Enrollment() override
    {
        m_process.disconnect(this);
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }

private:
    using Result = std::function<void(int, const QByteArray &)>;

    bool canOffer() const
    {
        return m_session && m_window->isVisible() && !m_session->tuner()->shutdownPending();
    }
    bool idle() const
    {
        return canOffer() && m_session->userStopped() && !m_session->busy();
    }
    bool canOfferAutomatically() const
    {
        const auto *dialog = m_window->findChild<RvDeviceManagerDialog *>();
        return canOffer() && !m_session->busy() && !QApplication::activeModalWidget()
            && !(dialog && dialog->isVisible());
    }
    QWidget *promptParent() const
    {
        return m_requester ? static_cast<QWidget *>(m_requester.data()) : m_window;
    }
    void connectFromManager(RvDeviceManagerDialog *dialog)
    {
        if (!canOffer() || m_session->busy() || m_prompting || m_result) {
            QMessageBox::information(dialog, tr("Connect via USB"),
                tr("Wait for the current device operation to finish, then try again."));
            return;
        }
        m_adb = findAdb();
        if (m_adb.isEmpty()) {
            RvImportDiagnostics::showFailure(dialog,"usb_enrollment",
                tr("ADB was not found. Place adb.exe and its DLLs in the platform-tools folder next to the application."),
                {{"phase","find_adb"}});
            return;
        }
        m_requester = dialog;
        m_added = false;
        dialog->beginUsbEnrollment();
        scan(true);
    }
    void run(const QString &program, const QStringList &args, int timeout, Result result,
             const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment())
    {
        m_result = std::move(result);
        m_processStatus.clear();
        m_process.setProcessEnvironment(environment);
        m_timeout.start(timeout);
        m_process.start(program, args);
    }
    void completed(int code)
    {
        m_timeout.stop();
        const auto output = m_process.readAllStandardOutput();
        const auto errors=m_process.readAllStandardError();
        QJsonObject event{{"phase",m_phase},{"exit_code",code},
            {"process_status",m_processStatus.isEmpty() ? (code<0 ? "abnormal_exit" : "exited") : m_processStatus}};
        if (code!=0 && m_phase.startsWith("windows_rndis")) {
            for (const auto *known : {"The RV1103 USB adapter already has a different IPv4 configuration.",
                                      "Another Windows interface is already using the RV1103 subnet.",
                                      "The USB adapter changed.",
                                      "The RV1103 USB network address or route did not become usable."})
                if (errors.contains(known)) { event["network_error"]=QString::fromLatin1(known); break; }
        }
        m_history.append(event);
        auto callback = std::exchange(m_result, {});
        if (callback) callback(code, output);
        if (!m_result && m_requester) {
            const auto dialog = std::exchange(m_requester, {});
            dialog->finishUsbEnrollment(m_added);
        }
    }
    void scan(bool manual)
    {
        if (!manual && (m_requester || m_adb.isEmpty() || m_prompting || m_result || !canOfferAutomatically())) return;
        m_started = QDateTime::currentDateTimeUtc();
        m_added = false;
        m_history = QJsonArray();
        m_receipt = QJsonObject();
        m_board = Board();
        m_serial.clear();
        m_transport.clear();
        m_phase = "adb_device_discovery";
        run(m_adb, {"devices", "-l"}, 6000, [this, manual](int code, const QByteArray &output) {
            if (code != 0) {
                if (manual) fail(tr("Could not list USB devices with ADB."));
                return;
            }
            const auto [serial, transport] = findTransport(output);
            if (serial.isEmpty()) {
                m_seen.clear();
                m_notice.clear();
                if (manual) fail(tr("Connect exactly one RV1103 via USB and make sure ADB is enabled."));
                return;
            }
            const auto attachment = serial + "/" + transport;
            if (manual ? !canOffer() : (!canOfferAutomatically() || attachment == m_seen)) return;
            m_serial = serial;
            m_transport = transport;
            m_phase = "adb_identity_and_delivery_probe";
            shell(probeCommand, [this, attachment, manual](int status, const QByteArray &probe) {
                if (manual ? !canOffer() : !canOfferAutomatically()) return;
                if (status != 0) {
                    if (manual) fail(tr("Could not read the RV1103 identity over USB."));
                    return;
                }
                try {
                    const auto identity = parseIdentity(m_serial, probe);
                    RvDeviceStore store(m_session->deviceStorePath());
                    if (knownIdentity(store.profiles(), identity) && !manual) {
                        m_seen = attachment;
                        return;
                    }
                    m_board = enrollmentBoard(identity,store.profiles());
                } catch (const std::exception &error) {
                    if (manual) {
                        fail(tr("RV1103 setup unavailable: %1").arg(QString::fromUtf8(error.what())));
                        return;
                    }
                    // A newly booting board can enumerate over ADB before its
                    // USB network or depth services are ready. Retry the probe
                    // without repeating the status message for this attachment.
                    if (m_notice != attachment) {
                        m_notice = attachment;
                        if (auto *main = qobject_cast<QMainWindow *>(m_window))
                            main->statusBar()->showMessage(tr("RV1103 setup unavailable: %1").arg(QString::fromUtf8(error.what())), 10000);
                    }
                    return;
                }
                m_seen = attachment;
                if (m_board.existing && m_board.profile.ready())
                    prepare(m_board.profile.cameraMountRotation,m_board.profile.inputRotation);
                else offer();
            });
        });
    }
    void shell(const QString &command, Result result)
    {
        // The Buildroot adbd uses the legacy PTY protocol, which does not
        // reliably carry a remote exit code.
        run(m_adb, {"-t", m_transport, "shell", command + "\nprintf '\\nRV_USB_OK\\n'"},
            10000, [result = std::move(result)](int code, QByteArray output) {
                if (!stripShellReceipt(output)) { result(-1, {}); return; }
                result(code, output);
            });
    }
    void offer()
    {
        m_prompting = true;
        QDialog confirm(promptParent());
        confirm.setObjectName("rvUsbEnrollmentPrompt");
        confirm.setWindowTitle(tr("New RV1103 detected"));
        auto *layout = new QVBoxLayout(&confirm);
        auto *summary = new QLabel(
            tr("Add USB device %1 and start imaging?\n\nSSH fingerprint: %2\n"
               "This uses the factory login and F=%3 calibration. Verify the two angles below. "
               "Any current imaging session will stop. "
               "Windows may ask for permission to configure the USB network adapter.")
                .arg(m_serial, m_board.profile.fingerprint()).arg(m_board.profile.fov()), &confirm);
        summary->setText(summary->text()+"\n\n"+tr("Required board files will be checked and replaced when missing or different. "
            "Continue only if the camera and ToF assembly match the selected calibration."));
        summary->setWordWrap(true);
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(summary);
        auto *angles = new QFormLayout;
        layout->addLayout(angles);
        const auto addAngle = [&](const QString &label) {
            auto *choice = new QComboBox(&confirm);
            choice->addItem(QStringLiteral("0\u00B0"), 0);
            choice->addItem(QStringLiteral("180\u00B0"), 180);
            angles->addRow(label, choice);
            return choice;
        };
        auto *mount = addAngle(tr("Camera mounting angle"));
        mount->setCurrentIndex(mount->findData(m_board.profile.cameraMountRotation));
        auto *input = addAngle(tr("Host inference input orientation"));
        input->setCurrentIndex(input->findData(m_board.profile.inputRotation<0?0:m_board.profile.inputRotation));
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &confirm);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Add device"));
        auto *cancel = buttons->button(QDialogButtonBox::Cancel);
        cancel->setText(tr("Cancel"));
        cancel->setDefault(true);
        cancel->setFocus();
        connect(buttons, &QDialogButtonBox::accepted, &confirm, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &confirm, &QDialog::reject);
        layout->addWidget(buttons);
        confirm.resize(510, confirm.sizeHint().height());
        const auto answer = confirm.exec();
        m_prompting = false;
        if (answer != QDialog::Accepted || !canOffer()) return;
        prepare(mount->currentData().toInt(),input->currentData().toInt());
    }
    void prepare(int mountAngle,int inputAngle)
    {
        m_phase = "prepare_installation";
        const auto worker = RuntimeDiscovery::rvControlForLaunch();
        if (!worker.error.isEmpty()) { fail(worker.error); return; }
        if (!m_session->userStopped() || m_session->busy()) m_session->stop();
        if (!idle()) { fail(tr("Stop imaging before setting up the new RV1103.")); return; }
        m_phase = "adb_identity_recheck";
        shell(probeCommand, [this, mountAngle, inputAngle](int status, const QByteArray &probe) {
            if (!idle()) return;
            try {
                if (status != 0) throw std::runtime_error("USB identity probe failed.");
                RvDeviceStore store(m_session->deviceStorePath());
                const auto current = enrollmentBoard(parseIdentity(m_serial, probe),store.profiles());
                if (current.boot != m_board.boot || current.profile.json() != m_board.profile.json()
                    || current.address != m_board.address || current.bootstrap != m_board.bootstrap)
                    throw std::runtime_error("The RV1103 identity or address changed.");
            } catch (const std::exception &) {
                fail(tr("The RV1103 connection changed. Reconnect the board and try again."));
                return;
            }
            m_board.profile.cameraMountRotation = mountAngle;
            m_board.profile.inputRotation = inputAngle;
            configureNetwork(false);
        });
    }
    void configureNetwork(bool elevated)
    {
        if (!idle()) return;
        m_phase = elevated ? "windows_rndis_configuration_elevated" : "windows_rndis_configuration";
        const auto ps = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
            .filePath("System32/WindowsPowerShell/v1.0/powershell.exe");
        auto script = QStringLiteral("$usbParent = 'USB\\VID_2207&PID_0019\\%1'\n$elevated = $%2\n")
            .arg(m_serial, elevated ? "true" : "false") + QString::fromLatin1(networkScript);
        if (elevated) {
            script = QStringLiteral(
                "$ErrorActionPreference='Stop'; $p=Start-Process -FilePath "
                "(Join-Path $PSHOME 'powershell.exe') -ArgumentList "
                "'-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand %1' "
                "-WindowStyle Hidden -Verb RunAs -Wait -PassThru; exit $p.ExitCode")
                .arg(encodedPowerShell(script));
        }
        run(ps, {"-NoProfile", "-NonInteractive", "-EncodedCommand", encodedPowerShell(script)},
            elevated ? 120000 : 15000, [this, elevated](int code, const QByteArray &) {
                if (!idle()) return;
                if (!elevated && code == 10) { configureNetwork(true); return; }
                if (code != 0) { fail(tr("Could not configure the RV1103 USB network adapter.")); return; }
                if (m_board.address.isEmpty()) {
                    m_phase = "board_usb_ipv4_configuration";
                    shell(configureBoardCommand(m_board), [this](int status, const QByteArray &) {
                        if (status != 0) { fail(tr("Could not assign an IPv4 address to the RV1103.")); return; }
                        verify();
                    });
                } else verify();
            });
    }
    void verify()
    {
        if (!idle()) return;
        m_phase = "ssh_runtime_check_and_repair";
        const auto worker = RuntimeDiscovery::rvControlForLaunch();
        if (!worker.error.isEmpty()) { fail(worker.error); return; }
        QStringList args{"--verify-usb-enrollment",
            QString::fromUtf8(QJsonDocument(m_board.profile.json()).toJson(QJsonDocument::Compact)),
            m_board.boot, m_board.bootstrap ? QStringLiteral("fresh") : QStringLiteral("installed")};
        try {
            RvDeviceStore store(m_session->deviceStorePath());m_registryRevision=store.revision();
            if(m_board.existing)args<<"--device-store"<<store.path()<<"--device-revision"<<m_registryRevision<<"--device-id"<<m_board.profile.id;
        }catch(const std::exception &error){fail(QString::fromUtf8(error.what()));return;}
        run(worker.executable, args,
            180000, [this](int code, const QByteArray &output) {
                if (!idle()) return;
                QJsonParseError parse;
                const auto receipt=QJsonDocument::fromJson(output,&parse).object();
                m_receipt={{"json_status",parse.errorString()},{"response_bytes",output.size()},
                           {"verified",receipt["verified"].toBool()},
                           {"litesr_model",receipt["litesr_model"].toString()},
                           {"automatic_tof",receipt["automatic_tof"].toString()},
                           {"runtime_replaced",receipt["runtime_replaced"].toBool()}};
                if (code != 0 || receipt["verified"] != true || receipt["litesr_model"]!="ready" || receipt["automatic_tof"]!="ready") {
                    const auto error=receipt["setup_error"].toString();
                    fail(error.isEmpty() ? tr("RV1103 verification failed. Check its factory login, calibration and USB connection.")
                                         : tr("RV1103 setup unavailable: %1").arg(error));
                    return;
                }
                if(m_requester)m_requester->setProperty("rvUsbRuntimeReplaced",receipt["runtime_replaced"].toBool());
                m_phase = "device_profile_registration";
                try {
                    RvDeviceStore store(m_session->deviceStorePath());
                    if(store.revision()!=m_registryRevision)throw std::runtime_error("Device registry changed during USB setup; retry the connection.");
                    auto profile = m_board.profile;
                    const auto old = store.profiles();
                    auto updated = RvDeviceStore::mergeProfiles(old, {profile});
                    if(m_board.existing)for(auto &entry:updated)if(entry.id==profile.id)entry=profile;
                    store.save(updated, profile.id);
                    m_added = true;
                    if(!m_board.existing)QSettings().remove(RvSessionController::legacyHostInputRotationKey(profile));
                    m_session->reloadDeviceProfiles();
                    if (idle() && m_session->deviceProfile().json() == profile.json())m_session->start();
                } catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }
            }, RuntimeDiscovery::nativeWorkerEnvironment(worker.executable));
    }
    void fail(const QString &message)
    {
        m_prompting = true;
        QJsonObject details{{"phase",m_phase},{"started_utc",m_started.toString(Qt::ISODateWithMs)},
            {"serial",m_serial},{"boot_id",m_board.boot},{"usb_address_before_setup",m_board.address},
            {"fresh_bootstrap",m_board.bootstrap},{"fov",m_board.profile.fov()},
            {"camera_mount_degrees",m_board.profile.cameraMountRotation},
            {"inference_input_degrees",m_board.profile.inputRotation},{"profile_registered",m_added},
            {"process_history",m_history},{"board_receipt",m_receipt}};
        if (!m_board.profile.publicKey.isEmpty()) details["ssh_fingerprint"]=m_board.profile.fingerprint();
        RvImportDiagnostics::showFailure(promptParent(),"usb_enrollment",message,details);
        m_prompting = false;
    }

    QPointer<RvSessionController> m_session;
    QPointer<RvDeviceManagerDialog> m_requester;
    QWidget *m_window;
    QString m_adb, m_seen, m_notice, m_serial, m_transport, m_phase, m_processStatus, m_registryRevision;
    QDateTime m_started;
    QJsonArray m_history;
    QJsonObject m_receipt;
    Board m_board;
    bool m_prompting = false, m_added = false;
    QTimer m_poll, m_timeout;
    QProcess m_process;
    Result m_result;
};
}

void watchRvUsbDevices(RvSessionController *session, SensorMainWindow *window)
{
    if (!session || qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1") return;
    new Enrollment(session, window);
}

QJsonObject checkRvUsbEnrollment()
{
    const QString serial = "1234567890abcdef";
    const QByteArray attached = serial.toLatin1() + " device product:occam transport_id:2\r\n";
    const auto found = findTransport(attached);
    RvDeviceProfile reference;
    reference.calibration = "rv20261008-f1";
    reference.publicKey = "AAAAC3NzaC1lZDI1NTE5AAAAIAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB";
    const QByteArray identity = serial.toLatin1() + "\r\n"
        + "5313e1f0-cbaf-48c4-8020-adf4ead884cc\r\nssh-ed25519 "
        + reference.publicKey.toLatin1() + " root@luckfox\r\n"
        + reference.geometrySha().toLatin1() + "  /root/r144_depth/M48_geometry.bin\r\nready\r\n";
    const auto blank = enrollmentBoard(parseIdentity(serial, identity));
    const auto usbIdentity = parseIdentity(serial, identity + "172.32.0.93/16\r\n");
    const auto addressed = enrollmentBoard(usbIdentity);
    auto f2 = usbIdentity;
    auto f2Profile = addressed.profile;
    f2Profile.calibration = "rv20261008-f2";
    f2.geometry = f2Profile.geometrySha();
    auto changedKey = usbIdentity;
    changedKey.key = "different";
    auto changedSerial = usbIdentity;
    changedSerial.serial = "0000000000000000";
    auto noServices = usbIdentity;
    noServices.servicesReady = false;
    QByteArray silentReceipt("\r\nRV_USB_OK\r\n");
    QByteArray probeReceipt("identity\r\nRV_USB_OK\r\n");
    QByteArray failureReceipt("command failed\r\n");
    const auto rejects = [](auto check) {
        try { check(); return false; } catch (const std::exception &) { return true; }
    };
    QTemporaryDir temp;
    const auto report=RvImportDiagnostics::writeReport(temp.filePath("diagnostics"),"test_usb_setup","test failure",
        {{"phase","adb_identity_and_delivery_probe"},{"serial",serial}});
    QFile reportFile(report);
    const auto diagnostic=reportFile.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(reportFile.readAll()).object() : QJsonObject();
    return {
        {"USB_IMPORT_DIAGNOSTIC_REPORT_WRITTEN", temp.isValid() && diagnostic["operation"]=="test_usb_setup"
            && diagnostic["reason"]=="test failure" && diagnostic["details"].toObject()["serial"]==serial
            && diagnostic["details"].toObject()["phase"]=="adb_identity_and_delivery_probe"},
        {"USB_IMPORT_DIAGNOSTIC_WRITE_FAILURE_HANDLED", temp.isValid()
            && RvImportDiagnostics::writeReport(report,"test_usb_setup","test failure",{}).isEmpty()},
        {"USB_SILENT_SUCCESS_RECEIPT_ACCEPTED", stripShellReceipt(silentReceipt) && silentReceipt.isEmpty()},
        {"USB_PROBE_PAYLOAD_RETAINED", stripShellReceipt(probeReceipt) && probeReceipt.trimmed() == "identity"},
        {"USB_MISSING_COMPLETION_RECEIPT_REJECTED", !stripShellReceipt(failureReceipt)},
        {"USB_SINGLE_ADB_DEVICE_DETECTED", found.serial == serial && found.id == "2"},
        {"USB_AMBIGUOUS_TRANSPORT_REJECTED", findTransport(attached + attached).id.isEmpty()},
        {"USB_OFFLINE_AND_TCP_TRANSPORTS_IGNORED",
         findTransport("1234567890abcdef offline transport_id:2\n172.32.0.93:5555 device transport_id:3\n").id.isEmpty()},
        {"USB_UNADDRESSED_BOARD_ACCEPTED", blank.address.isEmpty() && blank.profile.host == host},
        {"USB_EXISTING_SUPPORTED_ADDRESS_PRESERVED", addressed.address == "172.32.0.93/16"},
        {"USB_EXISTING_DIFFERENT_ADDRESS_REJECTED", rejects([&] {
            enrollmentBoard(parseIdentity(serial, identity + "172.32.0.94/16"));
        })},
        {"USB_PROFILE_USES_F1_AND_VERIFIED_ZERO_ORIENTATION", blank.profile.ready() && blank.profile.fov() == 1
            && blank.profile.inputRotation == 0 && blank.profile.cameraMountRotation == 180},
        {"USB_MISMATCHED_SERIAL_REJECTED", rejects([&] { parseIdentity("0000000000000000", identity); })},
        {"USB_F2_GEOMETRY_SELECTS_F2_PROFILE", enrollmentBoard(f2).profile.fov() == 2},
        {"USB_PARTIAL_F2_RUNTIME_PRESERVES_CALIBRATION", [&] {
            auto partial=f2;partial.servicesReady=false;return enrollmentBoard(partial).profile.fov()==2;
        }()},
        {"USB_KNOWN_SERIAL_NOT_REENROLLED", knownIdentity({addressed.profile}, changedKey)},
        {"USB_KNOWN_KEY_NOT_REENROLLED", knownIdentity({addressed.profile}, changedSerial)},
        {"USB_PARTIAL_RUNTIME_CAN_BE_REPAIRED", enrollmentBoard(noServices).bootstrap},
        {"USB_SAVED_IDENTITY_IS_REUSED_WITH_SETTINGS", [&] {
            auto saved=addressed.profile;saved.id="local-alias";saved.name="Saved board";saved.temporaryFovCalibration=true;
            saved.cameraMountRotation=0;saved.inputRotation=180;saved.host="192.168.1.2";
            auto expected=saved;expected.host=host;
            const auto board=enrollmentBoard(usbIdentity,{saved});
            return board.existing&&board.profile.json()==expected.json();
        }()},
        {"USB_SAVED_SERIAL_KEY_CONFLICT_REJECTED", rejects([&] {enrollmentBoard(changedKey,{addressed.profile});})},
        {"USB_SAVED_KEY_SERIAL_CONFLICT_REJECTED", rejects([&] {enrollmentBoard(changedSerial,{addressed.profile});})},
        {"USB_SAVED_CALIBRATION_CONFLICT_REJECTED", rejects([&] {enrollmentBoard(f2,{addressed.profile});})},
        {"USB_FRESH_BOARD_ACCEPTED_WITHOUT_DELIVERY_DRIVER", [&] {
            auto raw=identity;
            raw.replace(reference.geometrySha().toLatin1()+"  /root/r144_depth/M48_geometry.bin\r\nready",
                         "missing\r\nbootstrap");
            const auto board=enrollmentBoard(parseIdentity(serial,raw));
            return board.bootstrap && board.profile.ready() && board.profile.fov()==1
                && board.address.isEmpty() && configureBoardCommand(board).contains(board.boot)
                && !configureBoardCommand(board).contains("tof_driver")
                && !configureBoardCommand(board).contains("M48_geometry.bin");
        }()},
        {"USB_INSTALLED_BOARD_IPV4_CONFIGURATION_RECHECKS_IDENTITY",
            configureBoardCommand(blank).contains(blank.profile.publicKey)
            && !configureBoardCommand(blank).contains("/root/rv1103_mw_spi/bin/tof_driver")},
        {"USB_PARTIAL_BOARD_WITH_GEOMETRY_CAN_BE_REPAIRED", [&] {
            auto probe=noServices;
            probe.bootstrap=true;
            return enrollmentBoard(probe).profile.fov()==1;
        }()},
        {"USB_DAMAGED_GEOMETRY_REQUIRES_F1_CONFIRMATION", [&] {
            auto changed = identity;
            changed.replace(reference.geometrySha().toLatin1(), QByteArray(64, '0'));
            return enrollmentBoard(parseIdentity(serial, changed)).profile.fov()==1;
        }()},
    };
}
