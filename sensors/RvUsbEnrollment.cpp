#include "RvUsbEnrollment.h"
#include "RvImportDiagnostics.h"
#include "RvInferenceSettings.h"
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
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStatusBar>
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
# ADB appears before S99usb0config has changed the firmware's temporary
# 172.32.0.70 address to its final address. Give that service time to settle.
attempt=0
while :; do
    addresses=$(ip -4 addr show dev usb0)
    if printf '%s\n' "$addresses" | grep -q 'inet 172\.32\.0\.93/16 '; then break; fi
    attempt=$((attempt + 1))
    [ "$attempt" -lt 10 ] || break
    sleep .5
done
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
    if ($result.Count -eq 0) { return $null }
    if ($result.Count -ne 1) { throw 'The RV1103 USB network adapter is ambiguous.' }
    $result[0]
}
function Test-BoardRoute($index) {
    $route = @(Find-NetRoute -RemoteIPAddress '172.32.0.93')
    return ($route.Count -gt 0 -and @($route | Where-Object InterfaceIndex -ne $index).Count -eq 0)
}
$target = Get-Target
for ($attempt = 0; $null -eq $target -and $waitForAdapter -and $attempt -lt 8; $attempt++) {
    Start-Sleep -Milliseconds 500
    $target = Get-Target
}
if ($null -eq $target) { exit 11 }
if ($presenceOnly) { exit 0 }
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

enum class AdbIssue { None, Empty, Multiple, Offline, Unauthorized, MissingTransport, Unsupported };
struct AdbDiscovery {
    UsbTransport transport;
    AdbIssue issue = AdbIssue::Empty;
    QJsonArray devices;
};

AdbDiscovery discoverAdb(const QByteArray &output)
{
    AdbDiscovery result;
    static const QRegularExpression entry("^(\\S+)\\s+(device|offline|unauthorized|no permissions)(?:\\s|$)");
    static const QRegularExpression serialPattern("^[0-9a-f]{16}$");
    static const QRegularExpression transportPattern("(?:^|\\s)transport_id:([1-9][0-9]*)(?:\\s|$)");
    int candidates=0;
    for (const auto &line : QString::fromLatin1(output).split('\n')) {
        const auto match = entry.match(line.trimmed());
        if (!match.hasMatch()) continue;
        const auto serial=match.captured(1),state=match.captured(2);
        const auto transport=transportPattern.match(line.trimmed()).captured(1);
        if(result.devices.size()<128)result.devices.append(QJsonObject{{"serial",serial},{"state",state},{"transport_id",transport}});
        // Keep network/emulator transports and unknown serial formats out of USB
        // enrollment. The subsequent board probe still verifies RV1103 identity.
        if(!serialPattern.match(serial).hasMatch())continue;
        ++candidates;
        result.transport={serial,transport};
        result.issue=state=="offline"?AdbIssue::Offline
            :state=="unauthorized"?AdbIssue::Unauthorized
            :state!="device"?AdbIssue::Unsupported
            :transport.isEmpty()?AdbIssue::MissingTransport:AdbIssue::None;
    }
    if(candidates>1)result.issue=AdbIssue::Multiple;
    else if(!candidates)result.issue=result.devices.isEmpty()?AdbIssue::Empty:AdbIssue::Unsupported;
    if(result.issue!=AdbIssue::None)result.transport={};
    return result;
}

UsbTransport findTransport(const QByteArray &output) { return discoverAdb(output).transport; }

QString adbIssueMessage(AdbIssue issue)
{
    const char *text=nullptr;
    switch(issue){
    case AdbIssue::Empty:text="ADB did not detect any device. Check the USB data cable and the ADB Interface driver in Windows Device Manager. The USB network adapter alone is not sufficient.";break;
    case AdbIssue::Multiple:text="ADB detected multiple possible RV1103 connections. Disconnect other ADB devices and reconnect this board, then retry.";break;
    case AdbIssue::Offline:text="The RV1103 ADB connection is offline. Wait for the board to finish booting, then reconnect its USB cable and retry.";break;
    case AdbIssue::Unauthorized:text="The RV1103 ADB connection is unauthorized. Check the board's ADB authorization settings; entering an SSH public key here will not resolve it.";break;
    case AdbIssue::MissingTransport:text="ADB found the device but did not provide its USB transport ID. Restart the application and retry using the bundled platform-tools.";break;
    default:text="ADB found devices, but none is a supported RV1103 USB connection. Check the ADB device list in the diagnostic report.";break;
    }
    return QCoreApplication::translate("RvUsbEnrollment",text);
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
    for (qsizetype i = 5; i < fields.size(); ++i) {
        const auto address=fields[i].trimmed();
        if (!address.isEmpty()) probe.addresses.append(address);
    }
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
    profile.calibration = QLatin1String(RvCalibration::Id);
    profile.inputRotation = 0;
    profile.cameraMountRotation = 0;
    // Use the existing conservative coordinator contract for an enrolled board.
    profile.publicKey = probe.key;
    for (const auto &saved : profiles) {
        if (saved.serial != probe.serial && saved.publicKey != probe.key) continue;
        if (saved.serial != probe.serial || saved.publicKey != probe.key)
            throw std::runtime_error("The RV1103 serial and SSH key conflict with a saved device identity.");
        profile = saved;
        profile.calibration=QLatin1String(RvCalibration::Id);profile.temporaryFovCalibration=false;
        profile.host = host;
        profile.sshPort = 22;
        result.existing = true;

    }
    if(profile.inputRotation<0) { profile.inputRotation=0;profile.cameraMountRotation=0; }
    profile.validate();
    result.boot = probe.boot;
    result.address = probe.addresses.join(',');
    return result;
}

bool boardAddressReady(const Board &board)
{
    return board.address.split(',').contains("172.32.0.93/16");
}

enum class RuntimeInspection { Ready, Update, Invalid };

RuntimeInspection inspectReceipt(const QJsonObject &receipt,const Board &board)
{
    if(receipt["verified"]!=true || receipt["serial"]!=board.profile.serial
        || receipt["boot_id"]!=board.boot || !receipt["runtime_matches"].isBool())return RuntimeInspection::Invalid;
    return receipt["runtime_matches"].toBool()?RuntimeInspection::Ready:RuntimeInspection::Update;
}

bool confirmedIdentityMatches(const QJsonObject &expected,const Probe &identity,const QString &transport)
{
    return expected.isEmpty() || (expected["serial"]==identity.serial && expected["boot_id"]==identity.boot
        && expected["public_key"]==identity.key && expected["transport"]==transport);
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

// Manual USB setup can add the managed address to an older image. Preserve
// other addresses; never flush usb0 or change another board/network interface.
QString configureBoardCommand(const Board &board)
{
    return QString::fromLatin1(R"SH(
set -e
test "$(sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo)" = '%1'
test "$(cat /proc/sys/kernel/random/boot_id)" = '%2'
test "$(awk 'NR==1{print $2}' /etc/ssh/ssh_host_ed25519_key.pub)" = '%3'
file=/etc/init.d/S99zzDepthCameraUsb
umask 022
tmp=$(mktemp /etc/init.d/.DepthCameraUsb.XXXXXX)
trap 'rm -f "$tmp"' EXIT
cat >"$tmp" <<'RV_USB_INIT'
#!/bin/sh
[ "$1" = start ] || exit 0
addresses=$(ip -4 addr show dev usb0) || exit 1
if printf '%s\n' "$addresses" | grep -q 'inet 172\.32\.0\.93/16 '; then exit 0; fi
previous=$(printf '%s\n' "$addresses" | awk '$1=="inet" && $2 ~ /^172\.32\.0\.93\//{print $2}')
for address in $previous; do ip addr del "$address" dev usb0 || exit 1; done
if ! ip addr add 172.32.0.93/16 dev usb0; then
    for address in $previous; do ip addr add "$address" dev usb0 || true; done
    exit 1
fi
ip link set dev usb0 up
RV_USB_INIT
chmod 755 "$tmp"
if [ -e "$file" ] && ! cmp -s "$tmp" "$file"; then
    [ -e "$file.previous" ] || cp -p "$file" "$file.previous"
fi
mv -f "$tmp" "$file"
sh "$file" start
ip -4 addr show dev usb0 | grep -q 'inet 172\.32\.0\.93/16 '
)SH").arg(board.profile.serial, board.boot, board.profile.publicKey);
}
}

class RvUsbEnrollment final : public QObject {
    Q_DECLARE_TR_FUNCTIONS(RvUsbEnrollment)
public:
    RvUsbEnrollment(RvSessionController *session, SensorMainWindow *window)
        : QObject(window), m_session(session), m_window(window), m_adb(findAdb())
    {
        m_poll.setInterval(2500);
        m_timeout.setSingleShot(true);
        connect(&m_poll, &QTimer::timeout, this, [this] { scan(false); });
        connect(RvInferenceSettings::instance(), &RvInferenceSettings::detectionFinished, this, [this] { scan(false); });
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
    ~RvUsbEnrollment() override
    {
        m_process.disconnect(this);
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }

private:
    using Result = std::function<void(int, const QByteArray &)>;
    using Stage = RvSessionController::AutomaticConnectionStage;

    bool canOffer() const
    {
        return m_session && m_window->isVisible() && !m_session->tuner()->shutdownPending();
    }
    bool idle() const
    {
        return canOffer() && m_session->userStopped() && !m_session->busy();
    }
    bool operationAllowed() const
    {
        return idle() && m_session->stopRevision()==m_stopRevision
            && (!m_automatic || m_session->automaticStartAllowed(m_board.profile.id));
    }
    bool canOfferAutomatically() const
    {
        if (!canOffer() || m_session->busy()) return false;
        if (waitingForInference()) return true;
        const auto *dialog = m_window->findChild<RvDeviceManagerDialog *>();
        return !QApplication::activeModalWidget() && !(dialog && dialog->isVisible());
    }
    bool waitingForInference() const
    {
        return m_session && m_session->automaticConnectionStage() == Stage::PreparingInference;
    }
    void discardInferenceWait()
    {
        if (!waitingForInference()) return;
        m_seen.clear();
        m_session->setAutomaticConnectionStage(Stage::Idle);
    }
    QWidget *promptParent() const
    {
        return m_requester ? static_cast<QWidget *>(m_requester.data()) : m_window;
    }
    void connectFromManager(RvDeviceManagerDialog *dialog)
    {
        if (!canOffer() || m_session->busy() || m_session->automaticConnecting() || m_prompting || m_result) {
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
        m_expectedIdentity=dialog->property("rvUsbExpectedIdentity").toJsonObject();
        m_automatic = false;
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
        if(m_phase=="adb_device_discovery") {
            // This command only lists transports; never record identity probes,
            // credentials or SSH key material from later process output.
            event["adb_stdout"]=QString::fromUtf8(output.left(16384));
            event["adb_stderr"]=QString::fromUtf8(errors.left(16384));
            event["adb_devices"]=discoverAdb(output).devices;
            event["output_truncated"]=output.size()>16384 || errors.size()>16384;
        }
        if (code!=0 && m_phase.startsWith("windows_rndis")) {
            if(code==11)event["network_error"]="The matching RV1103 USB network adapter is not connected.";
            for (const auto *known : {"The RV1103 USB adapter already has a different IPv4 configuration.",
                                      "The RV1103 USB network adapter is ambiguous.",
                                      "Another Windows interface is already using the RV1103 subnet.",
                                      "The USB adapter changed.",
                                      "The RV1103 USB network address or route did not become usable."})
                if (errors.contains(known)) { event["network_error"]=QString::fromLatin1(known); break; }
        }
        m_history.append(event);
        auto callback = std::exchange(m_result, {});
        if (callback) callback(code, output);
        if (!m_result && m_automatic && m_session && m_session->automaticConnectionStage() == Stage::CheckingDevice)
            m_session->setAutomaticConnectionStage(Stage::Idle);
        if (!m_result && m_requester) {
            const auto dialog = std::exchange(m_requester, {});
            dialog->finishUsbEnrollment(m_added);
        }
    }
    void scan(bool manual)
    {
        if (!manual && (m_requester || m_adb.isEmpty() || m_prompting || m_result || !canOfferAutomatically())) return;
        m_started = QDateTime::currentDateTimeUtc();
        m_automatic = !manual;
        m_added = false;
        m_history = QJsonArray();
        m_receipt = QJsonObject();
        m_board = Board();
        m_serial.clear();
        m_transport.clear();
        m_discoveryStopRevision=m_session->stopRevision();
        m_phase = "adb_device_discovery";
        run(m_adb, {"devices", "-l"}, 6000, [this, manual](int code, const QByteArray &output) {
            if (code != 0) {
                if (manual) fail(tr("Could not list USB devices with ADB."));
                else discardInferenceWait();
                return;
            }
            const auto discovery=discoverAdb(output);
            const auto [serial, transport] = discovery.transport;
            if (serial.isEmpty()) {
                m_seen.clear();
                m_notice.clear();
                if (manual) fail(adbIssueMessage(discovery.issue));
                else discardInferenceWait();
                return;
            }
            const auto attachment = serial + "/" + transport;
            if (!manual) {
                if (attachment != m_seen) discardInferenceWait();
                // Recheck identity and RNDIS after candidate detection.
                const bool resume = waitingForInference() && !RvInferenceSettings::instance()->detecting();
                if (!canOfferAutomatically() || (attachment == m_seen && !resume)) return;
            } else if (!canOffer()) return;
            m_serial = serial;
            m_transport = transport;
            m_phase = "adb_identity_and_delivery_probe";
            shell(probeCommand, [this, attachment, manual](int status, const QByteArray &probe) {
                if (manual ? !canOffer() : !canOfferAutomatically()) return;
                if (status != 0) {
                    if (manual) fail(tr("Could not read the RV1103 identity over USB."));
                    else discardInferenceWait();
                    return;
                }
                try {
                    const auto identity = parseIdentity(m_serial, probe);
                    m_history.append(QJsonObject{{"phase",m_phase},{"usb_ipv4",QJsonArray::fromStringList(identity.addresses)}});
                    if(manual && !confirmedIdentityMatches(m_expectedIdentity,identity,m_transport))
                        throw std::runtime_error("The confirmed USB device changed; reconnect it and try again.");
                    RvDeviceStore store(m_session->deviceStorePath());
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
                    discardInferenceWait();
                    return;
                }
                // ADB may briefly retain a detached transport, and RNDIS can
                // enumerate later. A saved profile/ADB response is not proof
                // that this physical USB network adapter is connected now.
                m_phase = "windows_rndis_presence";
                runNetwork(true,false,[this,attachment,manual](int code,const QByteArray &) {
                    if (code != 0) {
                        m_seen.clear();
                        if (manual) fail(tr("Could not configure the RV1103 USB network adapter."));
                        else discardInferenceWait();
                        return; // No prompt or failed-import dialog while absent.
                    }
                    if(manual ? !canOffer() : !canOfferAutomatically())return;
                    m_seen = attachment;
                    if(m_session->stopRevision()!=m_discoveryStopRevision)return;
                    if(manual)prepare(false);
                    else if(!m_board.existing)offerSetup(false);
                    else {
                        // A Stop during this attachment is never undone.
                        m_session->deviceUnreachable(m_board.profile.id);
                        if (!m_session->automaticStartAllowed(m_board.profile.id)) return;
                        m_session->setAutomaticConnectionStage(Stage::CheckingDevice);
                        if (m_session->stopRevision() == m_discoveryStopRevision) prepare(true);
                    }
                });
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
    void offerSetup(bool existingDevice,bool networkSetup=false)
    {
        m_prompting = true;
        m_session->setAutomaticConnectionStage(Stage::Idle);
        m_window->offerUsbSetup(existingDevice,networkSetup,{{"serial",m_serial},{"boot_id",m_board.boot},
            {"public_key",m_board.profile.publicKey},{"transport",m_transport}});
        m_prompting = false;
    }
    void prepare(bool automatic)
    {
        m_automatic=automatic;
        m_phase = "prepare_installation";
        const auto worker = RuntimeDiscovery::rvControlForLaunch();
        if (!worker.error.isEmpty()) { fail(worker.error); return; }
        if (!automatic && (!m_session->userStopped() || m_session->busy())) m_session->stop();
        m_stopRevision=m_session->stopRevision();
        if (!operationAllowed()) { if(!automatic)fail(tr("Stop imaging before setting up the new RV1103.")); return; }
        m_phase = "adb_identity_recheck";
        shell(probeCommand, [this](int status, const QByteArray &probe) {
            if (!operationAllowed()) return;
            try {
                if (status != 0) throw std::runtime_error("USB identity probe failed.");
                RvDeviceStore store(m_session->deviceStorePath());
                const auto current = enrollmentBoard(parseIdentity(m_serial, probe),store.profiles());
                if (current.boot != m_board.boot || current.profile.json() != m_board.profile.json())
                    throw std::runtime_error("The RV1103 identity changed.");
                // The image may have finished setting usb0 up since discovery.
                m_board=current;
            } catch (const std::exception &) {
                if(m_automatic)m_seen.clear();
                else fail(tr("The RV1103 connection changed. Reconnect the board and try again."));
                return;
            }
            if(m_automatic && !boardAddressReady(m_board)) { offerSetup(true,true); return; }
            configureNetwork(false);
        });
    }
    void runNetwork(bool presenceOnly,bool elevated,Result result)
    {
        const auto ps = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
            .filePath("System32/WindowsPowerShell/v1.0/powershell.exe");
        auto script = QStringLiteral("$usbParent = 'USB\\VID_2207&PID_0019\\%1'\n$elevated = $%2\n$presenceOnly = $%3\n$waitForAdapter = $%4\n")
            .arg(m_serial, elevated ? "true" : "false",presenceOnly ? "true" : "false",
                 m_automatic ? "false" : "true") + QString::fromLatin1(networkScript);
        if (elevated) {
            script = QStringLiteral(
                "$ErrorActionPreference='Stop'; $p=Start-Process -FilePath "
                "(Join-Path $PSHOME 'powershell.exe') -ArgumentList "
                "'-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand %1' "
                "-WindowStyle Hidden -Verb RunAs -Wait -PassThru; exit $p.ExitCode")
                .arg(encodedPowerShell(script));
        }
        run(ps, {"-NoProfile", "-NonInteractive", "-EncodedCommand", encodedPowerShell(script)},
            elevated ? 120000 : 15000,std::move(result));
    }
    void configureNetwork(bool elevated)
    {
        if (!operationAllowed()) return;
        m_phase = elevated ? "windows_rndis_configuration_elevated" : "windows_rndis_configuration";
        runNetwork(false,elevated,[this, elevated](int code, const QByteArray &) {
                if (!operationAllowed()) return;
                if (code == 11 && m_automatic) { m_seen.clear(); return; }
                if (!elevated && code == 10) {
                    if(m_automatic)offerSetup(true,true);
                    else configureNetwork(true);
                    return;
                }
                if (code != 0) { fail(tr("Could not configure the RV1103 USB network adapter.")); return; }
                if (!boardAddressReady(m_board)) {
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
        if (!operationAllowed()) return;
        m_phase = m_automatic ? "ssh_complete_runtime_inspection" : "ssh_runtime_check_and_repair";
        const auto worker = RuntimeDiscovery::rvControlForLaunch();
        if (!worker.error.isEmpty()) { fail(worker.error); return; }
        QStringList args{m_automatic ? "--check-usb-enrollment" : "--verify-usb-enrollment",
            QString::fromUtf8(QJsonDocument(m_board.profile.json()).toJson(QJsonDocument::Compact)),
            m_board.boot, m_board.bootstrap ? QStringLiteral("fresh") : QStringLiteral("installed")};
        try {
            RvDeviceStore store(m_session->deviceStorePath());m_registryRevision=store.revision();
            if(m_board.existing)args<<"--device-store"<<store.path()<<"--device-revision"<<m_registryRevision<<"--device-id"<<m_board.profile.id;
        }catch(const std::exception &error){fail(QString::fromUtf8(error.what()));return;}
        run(worker.executable, args,
            180000, [this](int code, const QByteArray &output) {
                if (!operationAllowed()) return;
                QJsonParseError parse;
                const auto receipt=QJsonDocument::fromJson(output,&parse).object();
                m_receipt={{"json_status",parse.errorString()},{"response_bytes",output.size()},
                           {"verified",receipt["verified"].toBool()},
                           {"capture_runtime",receipt["capture_runtime"].toString()},
                           {"automatic_tof",receipt["automatic_tof"].toString()},
                           {"runtime_replaced",receipt["runtime_replaced"].toBool()}};
                if(m_automatic) {
                    if(code==0 && parse.error==QJsonParseError::NoError) {
                        const auto inspection=inspectReceipt(receipt,m_board);
                        if(inspection==RuntimeInspection::Update) { offerSetup(true); return; }
                        if(inspection==RuntimeInspection::Ready) { registerAndStart(); return; }
                    }
                    const auto error=receipt["setup_error"].toString();
                    fail(error.isEmpty() ? tr("RV1103 verification failed. Check its factory login, calibration and USB connection.")
                                        : tr("RV1103 setup unavailable: %1").arg(error));
                    return;
                }
                if (code != 0 || receipt["verified"] != true || receipt["capture_runtime"]!="ready" || receipt["automatic_tof"]!="ready") {
                    const auto error=receipt["setup_error"].toString();
                    fail(error.isEmpty() ? tr("RV1103 verification failed. Check its factory login, calibration and USB connection.")
                                         : tr("RV1103 setup unavailable: %1").arg(error));
                    return;
                }
                if(m_requester)m_requester->setProperty("rvUsbRuntimeReplaced",receipt["runtime_replaced"].toBool());
                m_phase = "device_profile_registration";
                registerAndStart();
            }, RuntimeDiscovery::nativeWorkerEnvironment(worker.executable));
    }
    void registerAndStart()
    {
        if(!operationAllowed())return;
        if (m_automatic && m_session->assistDepthEnabled() && RvInferenceSettings::instance()->detecting()) {
            m_session->setAutomaticConnectionStage(Stage::PreparingInference);
            return;
        }
        try {
            RvDeviceStore store(m_session->deviceStorePath());
            if(store.revision()!=m_registryRevision)throw std::runtime_error("Device registry changed during USB setup; retry the connection.");
            const auto profile=m_board.profile;
            auto updated=RvDeviceStore::mergeProfiles(store.profiles(),{profile});
            if(m_board.existing)for(auto &entry:updated)if(entry.id==profile.id)entry=profile;
            store.save(updated,profile.id);
            m_added=true;
            if(!m_board.existing)QSettings().remove(RvSessionController::legacyHostInputRotationKey(profile));
            m_session->reloadDeviceProfiles();
            // Reload internally calls Stop once. A further Stop revokes the
            // pending start, including one from a synchronous UI observer.
            if(idle() && m_session->stopRevision()==m_stopRevision+1 && m_session->deviceProfile().json()==profile.json()) {
                if(m_session->assistDepthEnabled() && RvInferenceSettings::instance()->detecting())return;
                if(m_automatic)m_session->startAutomatically(true);
                else m_session->start();
            }
        } catch(const std::exception &error) { fail(QString::fromUtf8(error.what())); }
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
        if (m_automatic) {
            // Background detection must not interrupt a disconnected startup
            // or a hot unplug. Explicit USB setup still reports actionable errors.
            const auto directory=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
            RvImportDiagnostics::writeReport(QDir(directory).filePath("DctsDiagnostics/rv1103"),
                                            "usb_enrollment",message,details);
            m_window->statusBar()->showMessage(message,10000);
            m_prompting=false;
            return;
        }
        RvImportDiagnostics::showFailure(promptParent(),"usb_enrollment",message,details);
        m_prompting = false;
    }

    QPointer<RvSessionController> m_session;
    QPointer<RvDeviceManagerDialog> m_requester;
    SensorMainWindow *m_window;
    QString m_adb, m_seen, m_notice, m_serial, m_transport, m_phase, m_processStatus, m_registryRevision;
    QDateTime m_started;
    QJsonArray m_history;
    QJsonObject m_receipt;
    QJsonObject m_expectedIdentity;
    Board m_board;
    bool m_prompting = false, m_added = false;
    bool m_automatic=false;
    quint64 m_stopRevision=0;
    quint64 m_discoveryStopRevision=0;
    QTimer m_poll, m_timeout;
    QProcess m_process;
    Result m_result;
};

void watchRvUsbDevices(RvSessionController *session, SensorMainWindow *window)
{
    if (!session || qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1") return;
    new RvUsbEnrollment(session, window);
}

QJsonObject checkRvUsbEnrollment()
{
    const QString serial = "1234567890abcdef";
    const QByteArray attached = serial.toLatin1() + " device product:occam transport_id:2\r\n";
    const auto found = findTransport(attached);
    RvDeviceProfile reference;
    reference.calibration = QLatin1String(RvCalibration::Id);
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
    const QJsonObject confirmedIdentity{{"serial",serial},{"boot_id",usbIdentity.boot},
        {"public_key",usbIdentity.key},{"transport","2"}};
    return {
        {"USB_IMPORT_DIAGNOSTIC_REPORT_WRITTEN", temp.isValid() && diagnostic["operation"]=="test_usb_setup"
            && diagnostic["reason"]=="test failure" && diagnostic["details"].toObject()["serial"]==serial
            && diagnostic["details"].toObject()["phase"]=="adb_identity_and_delivery_probe"},
        {"USB_IMPORT_DIAGNOSTIC_WRITE_FAILURE_HANDLED", temp.isValid()
            && RvImportDiagnostics::writeReport(report,"test_usb_setup","test failure",{}).isEmpty()},
        {"USB_SILENT_SUCCESS_RECEIPT_ACCEPTED", stripShellReceipt(silentReceipt) && silentReceipt.isEmpty()},
        {"USB_PROBE_PAYLOAD_RETAINED", stripShellReceipt(probeReceipt) && probeReceipt.trimmed() == "identity"},
        {"USB_MISSING_COMPLETION_RECEIPT_REJECTED", !stripShellReceipt(failureReceipt)},
        {"USB_CONFIRMED_HANDOFF_ACCEPTS_SAME_ATTACHMENT",confirmedIdentityMatches(confirmedIdentity,usbIdentity,"2")},
        {"USB_CONFIRMED_HANDOFF_REJECTS_OTHER_SERIAL",!confirmedIdentityMatches(confirmedIdentity,changedSerial,"2")},
        {"USB_CONFIRMED_HANDOFF_REJECTS_OTHER_KEY",!confirmedIdentityMatches(confirmedIdentity,changedKey,"2")},
        {"USB_CONFIRMED_HANDOFF_REJECTS_REPLUG",!confirmedIdentityMatches(confirmedIdentity,usbIdentity,"3")},
        {"USB_CONFIRMED_HANDOFF_REJECTS_REBOOT",[&]{auto reboot=usbIdentity;reboot.boot="00000000-0000-0000-0000-000000000000";
            return !confirmedIdentityMatches(confirmedIdentity,reboot,"2");}()},
        {"USB_SINGLE_ADB_DEVICE_DETECTED", found.serial == serial && found.id == "2"},
        {"USB_NO_ATTACHED_DEVICE_IGNORED",findTransport("List of devices attached\r\n\r\n").serial.isEmpty()},
        {"USB_EMPTY_LIST_HAS_DISTINCT_DIAGNOSTIC",discoverAdb("List of devices attached\r\n\r\n").issue==AdbIssue::Empty},
        {"USB_OFFLINE_HAS_DISTINCT_DIAGNOSTIC",discoverAdb("1234567890abcdef offline transport_id:2\n").issue==AdbIssue::Offline},
        {"USB_UNAUTHORIZED_HAS_DISTINCT_DIAGNOSTIC",discoverAdb("1234567890abcdef unauthorized transport_id:2\n").issue==AdbIssue::Unauthorized},
        {"USB_MISSING_TRANSPORT_HAS_DISTINCT_DIAGNOSTIC",discoverAdb("1234567890abcdef device product:occam\n").issue==AdbIssue::MissingTransport},
        {"USB_MULTIPLE_HAS_DISTINCT_DIAGNOSTIC",discoverAdb(attached+attached).issue==AdbIssue::Multiple},
        {"USB_UNSUPPORTED_SERIAL_HAS_DISTINCT_DIAGNOSTIC",discoverAdb("emulator-5554 device transport_id:3\n").issue==AdbIssue::Unsupported},
        {"USB_TRANSPORT_TOKEN_ORDER_INDEPENDENT",findTransport("1234567890abcdef device transport_id:2 product:occam\n").id=="2"},
        {"USB_DISCOVERY_RETAINS_DEVICE_STATE",discoverAdb("1234567890abcdef offline transport_id:2\n").devices.first().toObject()["state"]=="offline"},
        {"USB_ONLINE_AND_OFFLINE_CANDIDATES_AMBIGUOUS",discoverAdb(attached+"0000000000000000 offline transport_id:3\n").issue==AdbIssue::Multiple},
        {"USB_AMBIGUOUS_TRANSPORT_REJECTED", findTransport(attached + attached).id.isEmpty()},
        {"USB_OFFLINE_AND_TCP_TRANSPORTS_IGNORED",
         findTransport("1234567890abcdef offline transport_id:2\n172.32.0.93:5555 device transport_id:3\n").id.isEmpty()},
        {"USB_UNADDRESSED_BOARD_ACCEPTED", blank.address.isEmpty() && blank.profile.host == host},
        {"USB_EXISTING_SUPPORTED_ADDRESS_PRESERVED", addressed.address == "172.32.0.93/16"},
        {"USB_FIRMWARE_TEMPORARY_ADDRESS_CAN_BE_CONFIGURED",[&] {
            const auto temporary=enrollmentBoard(parseIdentity(serial,identity+"172.32.0.70/16\r\n"));
            return !boardAddressReady(temporary) && temporary.address=="172.32.0.70/16" && temporary.profile.ready();
        }()},
        {"USB_ADDITIONAL_BOARD_ADDRESS_ACCEPTED",boardAddressReady(enrollmentBoard(parseIdentity(serial,
            identity+"192.168.1.1/24\r\n172.32.0.93/16\r\n")))},
        {"USB_BLANK_ADDRESS_LINES_IGNORED",boardAddressReady(enrollmentBoard(parseIdentity(serial,
            identity+"\r\n172.32.0.93/16\r\n\r\n")))},
        {"USB_WRONG_PREFIX_REQUIRES_CONFIGURATION",!boardAddressReady(enrollmentBoard(parseIdentity(serial,
            identity+"172.32.0.93/24\r\n")))},
        {"USB_PROFILE_USES_F6_AND_VERIFIED_ZERO_ORIENTATION", blank.profile.ready() && blank.profile.fov() == 6
            && blank.profile.inputRotation == 0 && blank.profile.cameraMountRotation == 0},
        {"USB_MISMATCHED_SERIAL_REJECTED", rejects([&] { parseIdentity("0000000000000000", identity); })},
        {"USB_OLD_F2_GEOMETRY_MIGRATES_TO_F6", enrollmentBoard(f2).profile.fov() == 6},
        {"USB_PARTIAL_RUNTIME_USES_CURRENT_CALIBRATION", [&] {
            auto partial=f2;partial.servicesReady=false;return enrollmentBoard(partial).profile.fov()==6;
        }()},
        {"USB_AUTOMATIC_MATCH_REQUIRES_COMPLETE_INSPECTION", inspectReceipt({{"verified",true},
            {"serial",serial},{"boot_id",addressed.boot},{"runtime_matches",true}},addressed)==RuntimeInspection::Ready},
        {"USB_AUTOMATIC_MISMATCH_REQUIRES_UPDATE", inspectReceipt({{"verified",true},
            {"serial",serial},{"boot_id",addressed.boot},{"runtime_matches",false}},addressed)==RuntimeInspection::Update},
        {"USB_PARTIAL_RUNTIME_RECEIPT_CANNOT_AUTOSTART", inspectReceipt({{"verified",true},
            {"serial",serial},{"boot_id",addressed.boot},{"capture_runtime","ready"},{"automatic_tof","ready"}},addressed)==RuntimeInspection::Invalid},
        {"USB_WRONG_SERIAL_INSPECTION_REJECTED", inspectReceipt({{"verified",true},
            {"serial","0000000000000000"},{"boot_id",addressed.boot},{"runtime_matches",true}},addressed)==RuntimeInspection::Invalid},
        {"USB_PREVIOUS_BOOT_INSPECTION_REJECTED", inspectReceipt({{"verified",true},
            {"serial",serial},{"boot_id","00000000-0000-0000-0000-000000000000"},{"runtime_matches",true}},addressed)==RuntimeInspection::Invalid},
        {"USB_STRING_MATCH_RECEIPT_REJECTED", inspectReceipt({{"verified",true},
            {"serial",serial},{"boot_id",addressed.boot},{"runtime_matches","true"}},addressed)==RuntimeInspection::Invalid},
        {"USB_UNVERIFIED_INSPECTION_REJECTED", inspectReceipt({{"verified",false},
            {"serial",serial},{"boot_id",addressed.boot},{"runtime_matches",true}},addressed)==RuntimeInspection::Invalid},
        {"USB_PARTIAL_RUNTIME_CAN_BE_REPAIRED", enrollmentBoard(noServices).bootstrap},
        {"USB_SAVED_IDENTITY_IS_REUSED_WITH_SETTINGS", [&] {
            auto saved=addressed.profile;saved.id="local-alias";saved.name="Saved board";saved.temporaryFovCalibration=true;
            saved.cameraMountRotation=0;saved.inputRotation=180;saved.host="192.168.1.2";
            auto expected=saved;expected.host=host;expected.temporaryFovCalibration=false;
            const auto board=enrollmentBoard(usbIdentity,{saved});
            return board.existing&&board.profile.json()==expected.json();
        }()},
        {"USB_UNCONFIGURED_SAVED_CALIBRATION_DETECTED_AUTOMATICALLY",[&]{
            auto saved=addressed.profile;saved.calibration="unconfigured";saved.inputRotation=-1;
            const auto board=enrollmentBoard(usbIdentity,{saved});
            return board.existing && board.profile.fov()==6 && board.profile.inputRotation==0
                && board.profile.cameraMountRotation==0 && board.profile.ready();
        }()},
        {"USB_SAVED_SERIAL_KEY_CONFLICT_REJECTED", rejects([&] {enrollmentBoard(changedKey,{addressed.profile});})},
        {"USB_SAVED_KEY_SERIAL_CONFLICT_REJECTED", rejects([&] {enrollmentBoard(changedSerial,{addressed.profile});})},
        {"USB_OLD_CALIBRATION_MIGRATES_TO_RELEASE", enrollmentBoard(f2,{addressed.profile}).profile.fov()==6},
        {"USB_FRESH_BOARD_ACCEPTED_WITHOUT_DELIVERY_DRIVER", [&] {
            auto raw=identity;
            raw.replace(reference.geometrySha().toLatin1()+"  /root/r144_depth/M48_geometry.bin\r\nready",
                         "missing\r\nbootstrap");
            const auto board=enrollmentBoard(parseIdentity(serial,raw));
            return board.bootstrap && board.profile.ready() && board.profile.fov()==6
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
            return enrollmentBoard(probe).profile.fov()==6;
        }()},
        {"USB_DAMAGED_GEOMETRY_USES_DEFAULT_F6", [&] {
            auto changed = identity;
            changed.replace(reference.geometrySha().toLatin1(), QByteArray(64, '0'));
            return enrollmentBoard(parseIdentity(serial, changed)).profile.fov()==6;
        }()},
    };
}
