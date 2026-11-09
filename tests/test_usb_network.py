"""Run the actual USB probe/network scripts against isolated OS fixtures."""
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_board_bootstrap import shell_path


SOURCE = (Path(__file__).resolve().parents[1] / 'sensors/RvUsbEnrollment.cpp').read_text(encoding='utf-8')
NETWORK = re.search(r'networkScript = R"PS\((.*?)\)PS";', SOURCE, re.S).group(1)
PROBE = re.search(r'probeCommand = R"SH\((.*?)\)SH";', SOURCE, re.S).group(1)
CONFIGURE = re.search(r'QString configureBoardCommand.*?R"SH\((.*?)\)SH"\)', SOURCE, re.S).group(1)
SERIAL = '1234567890abcdef'
BOOT = '11111111-2222-3333-4444-555555555555'
PARENT = 'USB\\VID_2207&PID_0019\\' + SERIAL


@unittest.skipUnless(os.name == 'nt', 'Windows network script')
class WindowsUsbTests(unittest.TestCase):
    def run_network(self, adapters, presence=True, wait=False, late=False):
        fixture = json.dumps(adapters).replace("'", "''")
        script = r'''
$fixture = ConvertFrom-Json '__FIXTURE__'
$script:reads=0
function Get-NetAdapter {
    $script:reads++
    if ($late -and $script:reads -lt 3) { return }
    $fixture
}
function Get-PnpDeviceProperty($InstanceId,$KeyName) {
    [pscustomobject]@{Data=($fixture | Where-Object PnPDeviceID -eq $InstanceId | Select-Object -First 1).Parent}
}
function Get-NetIPAddress($InterfaceIndex,$AddressFamily) {
    if ($presenceOnly) { throw 'Presence check must not access network settings' }
    [pscustomobject]@{IPAddress='172.32.0.101';PrefixLength=16;AddressState='Preferred';SkipAsSource=$false}
}
function Find-NetRoute($RemoteIPAddress) { [pscustomobject]@{InterfaceIndex=23} }
function New-NetIPAddress { throw 'Unexpected network write' }
function Remove-NetIPAddress { throw 'Unexpected network write' }
function Set-NetIPInterface { throw 'Unexpected network write' }
function Start-Sleep { }
'''.replace('__FIXTURE__', fixture)
        script = ("$usbParent='%s';$presenceOnly=$%s;$waitForAdapter=$%s;$late=$%s;$elevated=$false\n" %
                  (PARENT, str(presence).lower(), str(wait).lower(), str(late).lower())) + script + NETWORK
        command = base64.b64encode(script.encode('utf-16le')).decode()
        executable = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        return subprocess.run([str(executable), '-NoProfile', '-NonInteractive', '-EncodedCommand', command],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)

    @staticmethod
    def adapter(status='Up', parent=PARENT):
        return dict(Status=status, PnPDeviceID='USB\\VID_2207&PID_0019&MI_00\\fixture',
                    Parent=parent, ifIndex=23, InterfaceGuid='fixture-guid')

    def test_no_adapter_is_disconnected_not_import_failure(self):
        self.assertEqual(self.run_network([]).returncode, 11)

    def test_disconnected_saved_adapter_is_not_present(self):
        self.assertEqual(self.run_network([self.adapter('Disconnected')]).returncode, 11)

    def test_other_board_cannot_satisfy_stale_adb_identity(self):
        self.assertEqual(self.run_network([self.adapter(parent=PARENT[:-1] + '0')]).returncode, 11)

    def test_correct_parent_is_present_without_reading_or_writing_ip(self):
        self.assertEqual(self.run_network([self.adapter()]).returncode, 0)

    def test_manual_wait_accepts_late_rndis_enumeration(self):
        self.assertEqual(self.run_network([self.adapter()], wait=True, late=True).returncode, 0)

    def test_automatic_discovery_does_not_wait_or_modify_adapter(self):
        self.assertEqual(self.run_network([self.adapter()], late=True).returncode, 11)

    def test_ambiguous_adapters_rejected(self):
        self.assertEqual(self.run_network([self.adapter(), self.adapter()]).returncode, 1)

    def test_existing_compatible_host_address_preserved(self):
        self.assertEqual(self.run_network([self.adapter()], presence=False).returncode, 0)

    def test_unplug_before_configuration_is_disconnected(self):
        self.assertEqual(self.run_network([], presence=False).returncode, 11)


class BoardUsbTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dcts-usb-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bash = str(Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe') if os.name == 'nt' else 'bash'
        self.write('proc/cpuinfo', 'Serial : ' + SERIAL + '\n')
        self.write('proc/sys/kernel/random/boot_id', BOOT + '\n')
        self.write('proc/device-tree/compatible', b'rockchip,rv1103\0')
        self.write('etc/ssh/ssh_host_ed25519_key.pub', 'ssh-ed25519 fixturekey root@fixture\n')
        (self.root / 'etc/init.d').mkdir()
        self.write('addresses', '172.32.0.70/16\n')
        self.write('bin/sleep', '#!/bin/sh\nexit 0\n')
        self.write('bin/ip', r'''#!/bin/sh
set -e
root="$DCTS_USB_TEST_ROOT"
if [ "$1 $2 $3" = '-4 addr show' ]; then
    if [ "${DCTS_USB_TEST_TRANSITION:-0}" = 1 ]; then
        n=$(cat "$root/reads" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" >"$root/reads"
        [ "$n" -lt 3 ] || echo '172.32.0.93/16' >"$root/addresses"
    fi
    while read -r address; do [ -z "$address" ] || echo "    inet $address scope global usb0"; done <"$root/addresses"
elif [ "$1 $2" = 'addr add' ]; then
    echo "$*" >>"$root/writes"
    if [ "${DCTS_USB_TEST_FAIL_ADD:-0}" = 1 ] && [ "$3" = '172.32.0.93/16' ]; then exit 1; fi
    echo "$3" >>"$root/addresses"
elif [ "$1 $2" = 'addr del' ]; then
    echo "$*" >>"$root/writes"
    grep -vxF "$3" "$root/addresses" >"$root/remaining" || true
    mv "$root/remaining" "$root/addresses"
elif [ "$1 $2" = 'link set' ]; then
    echo "$*" >>"$root/writes"
else
    echo "unexpected network command: $*" >&2; exit 1
fi
''')

    def write(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents if isinstance(contents, bytes) else contents.encode())
        path.chmod(0o755)
        return path

    def run_script(self, body, transition=False, fail_add=False):
        root = shell_path(self.root)
        for directory in ('/proc/', '/etc/', '/root/'):
            body = body.replace(directory, root + directory)
        body = body.replace('%1', SERIAL).replace('%2', BOOT).replace('%3', 'fixturekey')
        script = self.write('run.sh', 'export PATH="' + root + '/bin:$PATH"\n' + body)
        env = dict(os.environ, DCTS_USB_TEST_ROOT=root, DCTS_USB_TEST_TRANSITION=str(int(transition)),
                   DCTS_USB_TEST_FAIL_ADD=str(int(fail_add)))
        return subprocess.run([self.bash, '--noprofile', '--norc', shell_path(script)], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)

    def test_probe_waits_for_firmware_temporary_address(self):
        result = self.run_script(PROBE, transition=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.decode().splitlines()[-1], '172.32.0.93/16')
        self.assertFalse((self.root / 'writes').exists())

    def test_persistently_different_address_remains_available_for_setup(self):
        result = self.run_script(PROBE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.decode().splitlines()[-1], '172.32.0.70/16')
        self.assertFalse((self.root / 'writes').exists())

    def test_configuration_adds_managed_address_preserving_firmware_address(self):
        result = self.run_script(CONFIGURE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'addresses').read_text().splitlines(), ['172.32.0.70/16', '172.32.0.93/16'])
        before = (self.root / 'writes').read_bytes()
        self.assertEqual(self.run_script(CONFIGURE).returncode, 0)
        self.assertEqual((self.root / 'writes').read_bytes(), before)

    def test_changed_identity_cannot_configure_network(self):
        self.write('proc/sys/kernel/random/boot_id', 'changed-boot\n')
        self.assertNotEqual(self.run_script(CONFIGURE).returncode, 0)
        self.assertFalse((self.root / 'writes').exists())

    def test_wrong_prefix_corrected_without_removing_other_addresses(self):
        self.write('addresses', '172.32.0.70/16\n172.32.0.93/24\n')
        result = self.run_script(CONFIGURE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'addresses').read_text().splitlines(), ['172.32.0.70/16', '172.32.0.93/16'])

    def test_failed_prefix_change_restores_original_address(self):
        self.write('addresses', '172.32.0.70/16\n172.32.0.93/24\n')
        result = self.run_script(CONFIGURE, fail_add=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual((self.root / 'addresses').read_text().splitlines(), ['172.32.0.70/16', '172.32.0.93/24'])

    def test_managed_startup_file_replaced_with_original_retained(self):
        original = '#!/bin/sh\n# old managed startup\n'
        self.write('etc/init.d/S99zzDepthCameraUsb', original)
        self.assertEqual(self.run_script(CONFIGURE).returncode, 0)
        self.assertEqual((self.root / 'etc/init.d/S99zzDepthCameraUsb.previous').read_text(), original)


if __name__ == '__main__':
    unittest.main()
