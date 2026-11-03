#include "RvUsbEnrollment.h"
#include "RvSessionController.h"
#include "RvTunerController.h"
#include "../RuntimeDiscovery.h"

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

struct Board {
    RvDeviceProfile profile;
    QString boot;
    QString address;
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
    bool servicesReady;
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
                fields[3].simplified().section(' ', 0, 0), fields[4].trimmed() == "ready", {}};
    RvDeviceStore::keyFingerprint(probe.key);
    for (qsizetype i = 5; i < fields.size(); ++i) probe.addresses.append(fields[i].trimmed());
    return probe;
}

Board enrollmentBoard(const Probe &probe)
{
    if (!probe.servicesReady)
        throw std::runtime_error("RV1103 is missing the depth camera service.");
    Board result;
    auto &profile = result.profile;
    profile.id = profile.serial = probe.serial;
    profile.name = "RV1103 - " + probe.serial;
    profile.host = host;
    profile.calibration = "rv20261008-f1";
    if (probe.geometry != profile.geometrySha()) profile.calibration = "rv20261008-f2";
    profile.inputRotation = 0;
    // Use the existing conservative coordinator contract for an enrolled board.
    profile.publicKey = probe.key;
    if (probe.geometry != profile.geometrySha())
        throw std::runtime_error("RV1103 does not have a supported calibration.");
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
test "$(sha256sum /root/r144_depth/M48_geometry.bin | cut -d' ' -f1)" = '%4'
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
)SH").arg(board.profile.serial, board.boot, board.profile.publicKey, board.profile.geometrySha());
}

class Enrollment final : public QObject {
    Q_DECLARE_TR_FUNCTIONS(RvUsbEnrollment)
public:
    Enrollment(RvSessionController *session, QWidget *window, const QString &adb)
        : QObject(window), m_session(session), m_window(window), m_adb(adb)
    {
        m_poll.setInterval(2500);
        m_timeout.setSingleShot(true);
        connect(&m_poll, &QTimer::timeout, this, [this] { scan(); });
        connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) completed(-1);
        });
        connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            completed(status == QProcess::NormalExit ? code : -1);
        });
        m_poll.start();
        QTimer::singleShot(0, this, [this] { scan(); });
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
    void run(const QString &program, const QStringList &args, int timeout, Result result,
             const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment())
    {
        m_result = std::move(result);
        m_process.setProcessEnvironment(environment);
        m_timeout.start(timeout);
        m_process.start(program, args);
    }
    void completed(int code)
    {
        m_timeout.stop();
        const auto output = m_process.readAllStandardOutput();
        m_process.readAllStandardError();
        auto callback = std::exchange(m_result, {});
        if (callback) callback(code, output);
    }
    void scan()
    {
        if (m_prompting || m_result || !canOffer() || m_session->busy() || QApplication::activeModalWidget()) return;
        run(m_adb, {"devices", "-l"}, 6000, [this](int code, const QByteArray &output) {
            if (code != 0) return;
            const auto [serial, transport] = findTransport(output);
            if (serial.isEmpty()) { m_seen.clear(); m_notice.clear(); return; }
            const auto attachment = serial + "/" + transport;
            if (!canOffer() || attachment == m_seen) return;
            m_serial = serial;
            m_transport = transport;
            shell(probeCommand, [this, attachment](int status, const QByteArray &probe) {
                if (status != 0 || !canOffer() || m_session->busy() || QApplication::activeModalWidget()) return;
                try {
                    const auto identity = parseIdentity(m_serial, probe);
                    RvDeviceStore store(m_session->deviceStorePath());
                    if (knownIdentity(store.profiles(), identity)) {
                        m_seen = attachment;
                        return;
                    }
                    m_board = enrollmentBoard(identity);
                } catch (const std::exception &error) {
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
                offer();
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
        QMessageBox confirm(QMessageBox::Question, tr("New RV1103 detected"),
            tr("Add USB device %1 and start imaging?\n\nSSH fingerprint: %2\n"
               "This uses the factory login and F=%3 calibration with 0° mounting and 0° model input. "
               "Confirm these settings match the device. Any current imaging session will stop. "
               "Windows may ask for permission to configure the USB network adapter.")
                .arg(m_serial, m_board.profile.fingerprint()).arg(m_board.profile.fov()),
            QMessageBox::Yes | QMessageBox::Cancel, m_window);
        confirm.setObjectName("rvUsbEnrollmentPrompt");
        confirm.setDefaultButton(QMessageBox::Cancel);
        const auto answer = confirm.exec();
        m_prompting = false;
        if (answer != QMessageBox::Yes || !canOffer()) return;
        const auto worker = RuntimeDiscovery::rvControlForLaunch();
        if (!worker.error.isEmpty()) { fail(worker.error); return; }
        if (!m_session->userStopped() || m_session->busy()) m_session->stop();
        if (!idle()) { fail(tr("Stop imaging before setting up the new RV1103.")); return; }
        shell(probeCommand, [this](int status, const QByteArray &probe) {
            if (!idle()) return;
            try {
                if (status != 0) throw std::runtime_error("USB identity probe failed.");
                const auto current = enrollmentBoard(parseIdentity(m_serial, probe));
                if (current.boot != m_board.boot || current.profile.json() != m_board.profile.json()
                    || current.address != m_board.address)
                    throw std::runtime_error("The RV1103 identity or address changed.");
            } catch (const std::exception &) {
                fail(tr("The RV1103 connection changed. Reconnect the board and try again."));
                return;
            }
            configureNetwork(false);
        });
    }
    void configureNetwork(bool elevated)
    {
        if (!idle()) return;
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
        const auto worker = RuntimeDiscovery::rvControlForLaunch();
        if (!worker.error.isEmpty()) { fail(worker.error); return; }
        run(worker.executable, {"--verify-usb-enrollment",
            QString::fromUtf8(QJsonDocument(m_board.profile.json()).toJson(QJsonDocument::Compact))},
            50000, [this](int code, const QByteArray &output) {
                if (!idle()) return;
                const auto receipt=QJsonDocument::fromJson(output).object();
                if (code != 0 || receipt["verified"] != true) {
                    fail(tr("RV1103 verification failed. Check its factory login, calibration and USB connection."));
                    return;
                }
                try {
                    RvDeviceStore store(m_session->deviceStorePath());
                    auto profile = m_board.profile;
                    const auto old = store.profiles();
                    const auto updated = RvDeviceStore::mergeProfiles(old, {profile});
                    if (updated.size() != old.size() + 1)
                        throw std::runtime_error("The device was already added in another window.");
                    store.save(updated, profile.id);
                    QSettings().remove(RvSessionController::legacyHostInputRotationKey(profile));
                    m_session->reloadDeviceProfiles();
                    const auto model=receipt["litesr_model"].toString();
                    const auto automaticTof=receipt["automatic_tof"].toString();
                    if (automaticTof!="ready" || model!="ready") showModelStatus(model,automaticTof);
                    if (automaticTof=="ready" && model=="ready" && idle()
                        && m_session->deviceProfile().json() == profile.json())
                        m_session->startAutomatically();
                } catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }
            }, RuntimeDiscovery::handPoseNativeEnvironment(worker.executable));
    }
    void showModelStatus(const QString &model,const QString &automaticTof)
    {
        QString message;
        if (automaticTof=="missing")
            message=tr("The device was added. Open Settings > Device Manager > Install protected on-board inference to install the required files.");
        else if (automaticTof!="ready")
            message=tr("The device was added. The board capture script is unavailable or unsupported.");
        else if (model=="missing")
            message=tr("The device was added. To use on-board inference, open Settings > Device Manager and choose Install protected on-board inference.");
        else if (model=="mismatch")
            message=tr("The device was added. The existing on-board inference model checksum differs. Review the board model before starting on-board inference.");
        else
            message=tr("The device was added. On-board inference requires its runtime to be installed on the device.");
        m_prompting=true;
        QMessageBox::information(m_window,tr("RV1103 on-board inference model"),message);
        m_prompting=false;
    }
    void fail(const QString &message)
    {
        m_prompting = true;
        QMessageBox::warning(m_window, tr("RV1103 setup failed"),
            message + "\n" + tr("Reconnect the USB device to retry, or open Device Manager."));
        m_prompting = false;
    }

    QPointer<RvSessionController> m_session;
    QWidget *m_window;
    QString m_adb, m_seen, m_notice, m_serial, m_transport;
    Board m_board;
    bool m_prompting = false;
    QTimer m_poll, m_timeout;
    QProcess m_process;
    Result m_result;
};
}

void watchRvUsbDevices(RvSessionController *session, QWidget *window)
{
    if (!session || qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1"
        || !RuntimeDiscovery::componentAvailable("rv")) return;
    const auto installed = QDir(QCoreApplication::applicationDirPath()).filePath("platform-tools/adb.exe");
    const auto adb = QFileInfo::exists(installed) ? installed : QStandardPaths::findExecutable("adb.exe");
    if (!adb.isEmpty()) new Enrollment(session, window, adb);
}

QJsonObject checkRvUsbEnrollment()
{
    const QString serial = "1234567890abcdef";
    const QByteArray attached = serial.toLatin1() + " device product:occam transport_id:2\r\n";
    const auto found = findTransport(attached);
    const auto reference = RvDeviceStore::defaults().first();
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
    return {
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
            && blank.profile.inputRotation == 0 && blank.profile.cameraMountRotation == 0},
        {"USB_MISMATCHED_SERIAL_REJECTED", rejects([&] { parseIdentity("0000000000000000", identity); })},
        {"USB_F2_GEOMETRY_SELECTS_F2_PROFILE", enrollmentBoard(f2).profile.fov() == 2},
        {"USB_KNOWN_SERIAL_NOT_REENROLLED", knownIdentity({addressed.profile}, changedKey)},
        {"USB_KNOWN_KEY_NOT_REENROLLED", knownIdentity({addressed.profile}, changedSerial)},
        {"USB_MISSING_SERVICES_REJECTED", rejects([&] { enrollmentBoard(noServices); })},
        {"USB_WRONG_GEOMETRY_REJECTED", rejects([&] {
            auto changed = identity;
            changed.replace(reference.geometrySha().toLatin1(), QByteArray(64, '0'));
            enrollmentBoard(parseIdentity(serial, changed));
        })},
    };
}
