#include "RvUsbEnrollment.h"
#include "RvImportDiagnostics.h"
#include "RvInferenceSettings.h"
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include "RvDeviceManagerDialog.h"
#include "RvInferenceSettings.h"
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
#include <optional>
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
