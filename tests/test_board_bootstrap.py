"""Exercise fresh board installation and rollback in a temporary filesystem."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


SERIAL = '1234567890abcdef'
BOOT = '11111111-2222-3333-4444-555555555555'
CAPABILITIES = ('{"native_capture_protocol":"native-m48-m16-m8-v2","fov":6,'
                '"model_inference":false,"photon_enabled":false,"m8":false,'
                '"m16":true,"m48":true,"native_hole_fill_protocol":"native48-hole-fill-v1"}')
SCRIPT = Path(__file__).resolve().parents[1] / 'tools/rv_control/bootstrap/install.sh'


def shell_path(path):
    value = str(path.resolve()).replace('\\', '/')
    if os.name == 'nt':
        # Git Bash accepts /c/...; a short path keeps unquoted fixture paths
        # equivalent to the board's fixed, space-free absolute paths.
        import ctypes
        buffer = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.kernel32.GetShortPathNameW(str(path.resolve()), buffer, len(buffer)):
            value = buffer.value.replace('\\', '/')
        value = '/' + value[0].lower() + value[2:]
    if re.search(r"[\s'\"]", value):
        raise RuntimeError('Bootstrap fixture requires a space-free temporary path')
    return value


class BoardBootstrapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        git_bash = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe'
        cls.bash = str(git_bash) if os.name == 'nt' and git_bash.is_file() else shutil.which('bash')
        if not cls.bash:
            raise unittest.SkipTest('bash is required for isolated board shell tests')

    def write(self, name, content, executable=False):
        path = self.root / name.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode() if isinstance(content, str) else content)
        if executable:
            path.chmod(0o755)
        return path

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dcts-bootstrap-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / 'stage'
        self.previous_driver = b'a different customer delivery driver\n'
        self.new_driver = b'the bundled capture driver\n'
        self.write('proc/cpuinfo', 'Serial\t: ' + SERIAL + '\n')
        self.write('proc/sys/kernel/random/boot_id', BOOT + '\n')
        self.write('proc/device-tree/compatible', b'rockchip,rv1103\x00')
        self.write('proc/net/tcp', '')
        self.write('root/rv1103_mw_spi/bin/tof_driver', self.previous_driver, True)
        self.write('oem/usr/bin/rkipc', '#!/bin/sh\nexit 0\n', True)
        (self.root / 'oem/usr/share/iqfiles').mkdir(parents=True)
        self.camera_ini = '# original camera settings\n[video.source]\n[video.0]\n[isp]\n'
        self.write('userdata/rkipc.ini', self.camera_ini)
        self.write('stage/depth/camera_rtsp_config.awk', (SCRIPT.parent / 'camera_rtsp_config.awk').read_bytes())
        camera_driver = self.root / 'sys/bus/i2c/devices/4-0030/driver'
        camera_driver.parent.mkdir(parents=True)
        if os.name == 'nt':
            camera_driver.write_text('sc3336 sysfs binding fixture\n')
        else:
            camera_driver.symlink_to('sc3336')
        self.original_oem = ('#!/bin/sh\nstart_camera(){\n\tif [ -d "' + shell_path(self.root)
                             + '/oem/usr/share/iqfiles" ]; then\n\t :\n\tfi\n}\n')
        self.write('oem/usr/bin/RkLunch.sh', self.original_oem, True)
        (self.root / 'run').mkdir()
        (self.root / 'etc/init.d').mkdir(parents=True)
        self.write('commands/pidof', '#!/bin/sh\n[ "${BOOTSTRAP_ACTIVE_TOF:-0}" = 1 ] || exit 1\necho 1234\n', True)
        self.write('commands/mv', '#!/bin/sh\n'
                   'if [ "${BOOTSTRAP_FAIL_CANDIDATE_MOVE:-0}" = 1 ] && '
                   '[ "$1" = "$DCTS_FIXTURE_ROOT/stage/candidate" ] && '
                   '[ "$2" = "$DCTS_FIXTURE_ROOT/root/dtof_candidates_20261009" ]; then echo "fixture: candidate move failed"; exit 1; fi\n'
                   'if [ "${BOOTSTRAP_FAIL_RESTORE:-0}" = 1 ] && [ "$1" = "$DCTS_FIXTURE_ROOT/stage/previous/depth" ]; then exit 1; fi\n'
                   'exec /usr/bin/mv "$@"\n', True)
        self.write('stage/depth/tmf8829_ext', self.new_driver)
        self.write('stage/depth/ensure-spi.sh', '#!/bin/sh\nexit 0\n', True)
        self.write('stage/candidate/control.sh', '#!/bin/sh\nexit 0\n', True)
        self.write('stage/candidate/native_hd_gui', '#!/bin/sh\nprintf \'%s\\n\' \'' + CAPABILITIES + '\'\n', True)
        self.write('stage/S99zdepth', '#!/bin/sh\nexit 0\n', True)
        fixture_root = shell_path(self.root)
        library = (SCRIPT.parent / 'camera-takeover.sh').read_text(encoding='utf-8')
        library = re.sub(r'(?<![A-Za-z0-9_./])/(?:root|oem|usr|etc|sys|run|proc|userdata)(?=/|\b)',
                         lambda match: fixture_root + match[0], library)
        self.write('stage/depth/camera-takeover.sh', library)
        script = SCRIPT.read_text(encoding='utf-8')
        script = re.sub(r'(?<![A-Za-z0-9_./])/(?:root|oem|usr|etc|sys|run|proc|userdata)(?=/|\b)',
                        lambda match: fixture_root + match[0], script)
        # The runtime value is written into an INI and is never opened.
        # Keep a board path here, including when the fixture uses DOS short paths.
        script = script.replace('runtime=' + fixture_root + '/run/dtof_candidates/hd_setup_check',
                                'runtime=/run/dtof_candidates/hd_setup_check')
        if os.name == 'nt':
            # Represent the sysfs binding without requiring Windows symlink
            # privileges. Ordinary deployment paths retain all original checks.
            script = script.replace('[ -L ' + shell_path(camera_driver) + ' ]',
                                    '[ -f ' + shell_path(camera_driver) + ' ]')
        script = script.replace('@SPI_DRIVER_SHA@',
                                hashlib.sha256(self.new_driver).hexdigest())
        self.script = self.write('install-fixture.sh',
                                 'PATH=' + shell_path(self.root / 'commands') + ':/usr/bin:/bin:$PATH\n' + script)

    def run_setup(self, action='preflight', serial=SERIAL, mode='fresh', fov='6', **variables):
        environment = dict(os.environ, DCTS_FIXTURE_ROOT=shell_path(self.root), **variables)
        result = subprocess.run([self.bash, '--noprofile', '--norc', shell_path(self.script),
                                 action, serial, BOOT, shell_path(self.stage), mode, fov], env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        return result.returncode, result.stdout.decode(errors='replace')

    def test_different_delivery_driver_is_accepted_and_preserved_by_preflight(self):
        code, output = self.run_setup()
        self.assertEqual((code, output.strip()), (0, 'ready'))
        self.assertEqual((self.root / 'root/rv1103_mw_spi/bin/tof_driver').read_bytes(), self.previous_driver)

    def test_missing_delivery_package_is_accepted(self):
        shutil.rmtree(self.root / 'root/rv1103_mw_spi')
        code, output = self.run_setup('activate')
        self.assertEqual(code, 0, output)
        self.assertEqual((self.root / 'root/r144_depth/tmf8829_ext').read_bytes(), self.new_driver)
        self.assertFalse((self.root / 'root/rv1103_mw_spi').exists())

    def test_activation_backs_up_arbitrary_driver_and_binds_device(self):
        code, output = self.run_setup('activate')
        self.assertEqual(code, 0, output)
        driver = self.root / 'root/rv1103_mw_spi/bin/tof_driver'
        backup = driver.with_name(driver.name + '.dcts-before-' + hashlib.sha256(self.previous_driver).hexdigest())
        self.assertEqual(backup.read_bytes(), self.previous_driver)
        self.assertEqual(driver.read_bytes(), self.new_driver)
        self.assertEqual((self.root / 'root/r144_depth/spi_native16.enabled').read_text().strip(), SERIAL)
        self.assertEqual((self.root / 'root/r144_depth/rkipc_before_setup.ini').read_text(), self.camera_ini)
        self.assertEqual((self.root / 'userdata/rkipc.ini').read_text(), self.camera_ini)
        self.assertEqual((self.root / 'oem/usr/bin/RkLunch.sh.dcts-before-bootstrap').read_text(), self.original_oem)
        self.assertFalse((self.root / 'run/dcts-board-install.lock').exists())

    def test_activation_failure_restores_driver_and_startup(self):
        code, output = self.run_setup('activate', BOOTSTRAP_FAIL_CANDIDATE_MOVE='1')
        self.assertNotEqual(code, 0, output)
        self.assertIn('fixture: candidate move failed', output)
        self.assertEqual((self.root / 'root/rv1103_mw_spi/bin/tof_driver').read_bytes(), self.previous_driver)
        self.assertEqual((self.root / 'oem/usr/bin/RkLunch.sh').read_text(), self.original_oem)
        self.assertFalse((self.root / 'root/r144_depth').exists())
        self.assertFalse((self.root / 'run/dcts-board-install.lock').exists())

    def test_damaged_uploaded_driver_is_rejected_before_activation(self):
        self.write('stage/depth/tmf8829_ext', 'damaged uploaded driver\n')
        code, output = self.run_setup('activate')
        self.assertNotEqual(code, 0)
        self.assertIn('SPI driver changed during setup', output)
        self.assertEqual((self.root / 'root/rv1103_mw_spi/bin/tof_driver').read_bytes(), self.previous_driver)
        self.assertFalse((self.root / 'root/r144_depth').exists())

    def test_partial_deployment_is_rejected(self):
        (self.root / 'root/r144_depth').mkdir()
        code, output = self.run_setup()
        self.assertNotEqual(code, 0)
        self.assertIn('previous or partial', output)

    def existing_runtime(self):
        self.write('root/r144_depth/tmf8829_ext', 'old capture driver\n', True)
        self.write('root/r144_depth/service.sh', 'saved sensor arguments\n')
        self.write('root/dtof_candidates_20261009/session.sh', 'saved candidate arguments\n')
        self.write('root/dtof_candidates_20261009/old-model.dctsm', 'obsolete active payload\n')
        self.write('root/dtof_candidates_20261009/model-switch.sh', 'obsolete launcher\n', True)
        self.write('root/dtof_tuner/persistence/record.json', 'saved engineering history\n')
        self.write('etc/init.d/S99zdepth', 'old startup\n')
        self.write('stage/tuner/session.sh', '#!/bin/sh\nexit 0\n', True)
        (self.root / 'run/r144_depth').mkdir()

    def test_usb_repair_replaces_different_files_and_installs_complete_runtime(self):
        self.existing_runtime()
        staging_residue = self.write('oem/usr/bin/.RkLunch.dcts-bootstrap', 'interrupted older update\n')
        code, output = self.run_setup('activate', mode='repair')
        self.assertEqual(code, 0, output)
        self.assertTrue(staging_residue.is_file())
        self.assertIn('init_form_ini = 0', (self.stage / 'camera-check.ini').read_text())
        self.assertEqual((self.root/'root/r144_depth/tmf8829_ext').read_bytes(), self.new_driver)
        self.assertEqual((self.root/'root/rv1103_mw_spi/bin/tof_driver').read_bytes(), self.new_driver)
        self.assertFalse((self.root/'root/dtof_candidates_20261009/old-model.dctsm').exists())
        self.assertFalse((self.root/'root/dtof_candidates_20261009/model-switch.sh').exists())
        self.assertEqual((self.root/'root/dtof_tuner/persistence/record.json').read_text(), 'saved engineering history\n')
        self.assertEqual((self.root/'stage.before/depth/service.sh').read_text(), 'saved sensor arguments\n')
        self.assertFalse((self.root/'run/dtof_switch.lock').exists())
        self.assertFalse((self.root/'run/r144_depth/control.lock').exists())

    def test_failed_usb_repair_restores_existing_runtime_and_driver(self):
        self.existing_runtime()
        code, output = self.run_setup('activate', mode='repair', BOOTSTRAP_FAIL_CANDIDATE_MOVE='1')
        self.assertNotEqual(code, 0, output)
        self.assertIn('candidate move failed', output)
        self.assertEqual((self.root/'root/r144_depth/service.sh').read_text(), 'saved sensor arguments\n')
        self.assertEqual((self.root/'root/dtof_candidates_20261009/session.sh').read_text(), 'saved candidate arguments\n')
        self.assertEqual((self.root/'etc/init.d/S99zdepth').read_text(), 'old startup\n')
        self.assertEqual((self.root/'root/rv1103_mw_spi/bin/tof_driver').read_bytes(), self.previous_driver)
        self.assertEqual((self.root/'oem/usr/bin/RkLunch.sh').read_text(), self.original_oem)
        self.assertFalse((self.root/'run/dtof_switch.lock').exists())

    def test_usb_repair_refuses_latched_fault_without_replacing_files(self):
        self.existing_runtime()
        self.write('run/r144_depth/fault', 'retained fault\n')
        code, output = self.run_setup('activate', mode='repair')
        self.assertNotEqual(code, 0, output)
        self.assertIn('state or fault', output)
        self.assertEqual((self.root/'root/r144_depth/tmf8829_ext').read_text(), 'old capture driver\n')
        self.assertTrue((self.root/'run/r144_depth/fault').is_file())

    def test_incomplete_rollback_retains_original_files_and_fault(self):
        self.existing_runtime()
        code, output = self.run_setup('activate', mode='repair', BOOTSTRAP_FAIL_CANDIDATE_MOVE='1', BOOTSTRAP_FAIL_RESTORE='1')
        self.assertNotEqual(code, 0, output)
        self.assertIn('rollback incomplete', output)
        self.assertEqual((self.root/'stage/previous/depth/tmf8829_ext').read_text(), 'old capture driver\n')
        self.assertTrue((self.root/'run/r144_depth/fault').is_file())

    def test_usb_repair_rejects_obsolete_fov_before_replacement(self):
        self.existing_runtime()
        code, output = self.run_setup('activate', mode='repair', fov='2')
        self.assertNotEqual(code, 0, output)
        self.assertEqual((self.root/'root/r144_depth/tmf8829_ext').read_text(), 'old capture driver\n')

    def test_usb_repair_rejects_failed_native_capabilities_before_replacement(self):
        self.existing_runtime()
        self.write('stage/candidate/native_hd_gui', '#!/bin/sh\nexit 2\n', True)
        code, output = self.run_setup('activate', mode='repair')
        self.assertNotEqual(code, 0, output)
        self.assertIn('Native16 capture program could not start', output)
        self.assertEqual((self.root/'root/r144_depth/tmf8829_ext').read_text(), 'old capture driver\n')

    def test_usb_repair_rejects_an_old_capture_program_before_replacement(self):
        self.existing_runtime()
        old_capabilities = CAPABILITIES.split(',"native_hole_fill_protocol"')[0] + '}'
        self.write('stage/candidate/native_hd_gui', '#!/bin/sh\nprintf \'%s\\n\' \'' + old_capabilities + '\'\n', True)
        code, output = self.run_setup('activate', mode='repair')
        self.assertNotEqual(code, 0, output)
        self.assertIn('expected calibration and hole-fill capabilities', output)
        self.assertEqual((self.root/'root/r144_depth/tmf8829_ext').read_text(), 'old capture driver\n')

    def test_usb_repair_checks_entire_upload_manifest_before_replacement(self):
        self.existing_runtime()
        self.write('stage/files.sha256', '0'*64 + '  candidate/control.sh\n')
        code, output = self.run_setup('activate', mode='repair')
        self.assertNotEqual(code, 0, output)
        self.assertIn('runtime checksum mismatch', output)
        self.assertEqual((self.root/'root/r144_depth/tmf8829_ext').read_text(), 'old capture driver\n')

    def test_usb_repair_preserves_existing_factory_backup_and_guard(self):
        self.existing_runtime()
        backup = self.write('oem/usr/bin/RkLunch.sh.dcts-before-bootstrap', 'earlier factory startup\n')
        guard = '\t[ ! -f ' + shell_path(self.root) + '/root/r144_depth/spi_native16.enabled ] || return 0\n'
        self.write('oem/usr/bin/RkLunch.sh', self.original_oem.replace('start_camera(){\n', 'start_camera(){\n' + guard), True)
        code, output = self.run_setup('activate', mode='repair')
        self.assertEqual(code, 0, output)
        self.assertEqual(backup.read_text(), 'earlier factory startup\n')
        self.assertEqual((self.root/'oem/usr/bin/RkLunch.sh').read_text().count(guard), 1)

    def test_candidate_recovery_requires_the_released_generation_and_idle_resources(self):
        previous, new = 'a' * 32, 'b' * 32
        candidate = 'root/dtof_candidates_20261009'
        state = 'run/dtof_candidates'
        root = shell_path(self.root)
        self.write(candidate + '/native_gui.sh', '#!/bin/sh\n[ "$1" = validate ]\n', True)
        self.write(candidate + '/session.sh',
                   '#!/bin/sh\nprintf \'{"generation":"%s"}\\n\' "$2" > ' + root + '/' + state + '/ready.json\n', True)
        start = self.write('root/r144_depth/ensure-spi.sh',
                           '#!/bin/sh\ntouch ' + root + '/start-checked\n', True)
        receipt = self.write(state + '/released', BOOT + ' ' + previous + '\n')
        script = (SCRIPT.parent / 'candidate-control.sh').read_text(encoding='utf-8')
        script = script.replace('/proc/[0-9]*/fd/*', root + '/proc/[0-9]*/fd/*')
        script = re.sub(r'(?<![A-Za-z0-9_./])(/root/|/run/|/proc/sys/|/proc/net/)',
                        lambda match: root + match[0], script)
        path = self.write('recovery-fixture.sh',
                          'PATH=' + root + '/commands:/usr/bin:/bin:$PATH\n' + script)

        def invoke(*args, **env):
            return subprocess.run([self.bash, '--noprofile', '--norc', shell_path(path), *args],
                                  env=dict(os.environ, DCTS_FIXTURE_ROOT=root, **env), text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)

        capability = invoke('capabilities')
        self.assertEqual(capability.returncode, 0, capability.stdout)
        self.assertIn('clean-candidate-restart-v1', capability.stdout)
        args = ('auto-candidate', '24', new, '1,1', previous)
        wrong_boot = invoke(*args, '22222222-2222-3333-4444-555555555555')
        self.assertNotEqual(wrong_boot.returncode, 0, wrong_boot.stdout)
        self.assertIn('DTOF_ERROR: Board boot changed', wrong_boot.stdout)
        switch_lock = self.root / 'run/dtof_switch.lock'
        switch_lock.mkdir()
        occupied = invoke(*args, BOOT)
        self.assertNotEqual(occupied.returncode, 0, occupied.stdout)
        self.assertIn('DTOF_ERROR: Another board capture transition', occupied.stdout)
        switch_lock.rmdir()
        for file, body, message in (
            (state + '/fault', 'readback failure', 'fault is latched'),
            (state + '/fault', 'sensor-readback\n', 'Native ToF sensor readback failed'),
            ('run/r144_fusion_20260923/session.generation', new, 'different capture generation'),
            ('proc/net/tcp', '0: 00000000:2280 00000000:0000 0A 0 0 0 0 0 1234\n', 'owns the RTSP or ToF stream listener'),
            ('proc/net/tcp6', '0: 00000000000000000000000000000000:022A 0:0000 0A\n', 'owns the RTSP or ToF stream listener'),
        ):
            with self.subTest(file=file):
                fixture = self.write(file, body)
                rejected = invoke(*args, BOOT)
                self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
                self.assertIn('DTOF_ERROR: ', rejected.stdout)
                self.assertIn(message, rejected.stdout)
                if file.startswith('run/r144_fusion_20260923/'):
                    fixture.unlink()
                    fixture.parent.rmdir()
                elif file.startswith('proc/'):
                    fixture.write_text('')
                else:
                    fixture.unlink()
        self.write('commands/readlink', '#!/bin/sh\n'
                   'case "$1" in "$DCTS_FIXTURE_ROOT"/proc/4711/fd/8) echo /dev/i2c-3;;\n'
                   '*) exec /usr/bin/readlink "$@";; esac\n', True)
        descriptor = self.write('proc/4711/fd/8', '')
        occupied = invoke(*args, BOOT)
        self.assertNotEqual(occupied.returncode, 0, occupied.stdout)
        self.assertIn('DTOF_ERROR: An unrecognized process retains camera or ToF descriptors', occupied.stdout)
        descriptor.unlink()
        rejected = invoke('auto-candidate', '24', new, '1,1', 'c' * 32, BOOT)
        self.assertNotEqual(rejected.returncode, 0, rejected.stdout)
        self.assertIn('DTOF_ERROR: Previous capture has no clean-release receipt', rejected.stdout)
        self.assertFalse((self.root / 'start-checked').exists())
        self.assertTrue(start.is_file())

        accepted = invoke(*args, BOOT)
        self.assertEqual(accepted.returncode, 0, accepted.stdout)
        self.assertIn(new, accepted.stdout)
        self.assertTrue((self.root / 'start-checked').is_file())
        self.assertFalse(receipt.exists())

    def test_wrong_device_is_rejected(self):
        code, output = self.run_setup(serial='0000000000000000')
        self.assertNotEqual(code, 0)
        self.assertIn('identity or boot changed', output)

    def test_missing_firmware_dependency_is_rejected(self):
        (self.root / 'userdata/rkipc.ini').unlink()
        code, output = self.run_setup()
        self.assertNotEqual(code, 0)
        self.assertIn('required camera firmware', output)

    def test_active_capture_is_rejected(self):
        code, output = self.run_setup(BOOTSTRAP_ACTIVE_TOF='1')
        self.assertNotEqual(code, 0)
        self.assertIn('Stop the active ToF', output)


class CameraTakeoverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        git_bash = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe'
        cls.bash = str(git_bash) if os.name == 'nt' and git_bash.is_file() else shutil.which('bash')
        if not cls.bash:
            raise unittest.SkipTest('bash is required for isolated camera takeover tests')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dcts-camera-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.put('proc/sys/kernel/random/boot_id', BOOT + '\n')
        self.put('proc/net/tcp', '0: 00000000:022A 00000000:0000 0A 0 0 0 0 0 6095\n')
        self.put('proc/123/stat', '123 (udhcpc) S 1 ' + '0 ' * 17 + '100\n')
        self.put('proc/123/status', 'PPid:\t1\n')
        self.put('proc/123/exe', '/bin/busybox\n')
        self.put('proc/123/cmdline', b'udhcpc\x00-i\x00usb0\x00-T\x001\x00-A\x000\x00-b\x00-q\x00')
        self.put('proc/123/fd/48', 'socket:[6095]\n')
        self.put('proc/123/fd/66', '/dmabuf:vmpi\n')

    def put(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        return path

    def run_takeover(self, camera_active=False):
        fixture = shell_path(self.root)
        library = (SCRIPT.parent / 'camera-takeover.sh').read_text(encoding='utf-8')
        library = re.sub(r'(?<![A-Za-z0-9_./])/(?:proc|run)(?=/|\b)',
                         lambda match: fixture + match[0], library)
        # All /proc and /run access targets the fixture. Signal delivery and
        # readlink are simulated; no host process or interface is touched.
        prefix = ('set -eu\nPATH=/usr/bin:/bin:$PATH\n'
                  'readlink(){ cat "$1" 2>/dev/null; }\n'
                  'pidof(){ ' + ('echo 777; return 0;' if camera_active else 'return 1;') + ' }\n'
                  'ip(){ printf "    inet 172.32.0.93/16 scope global usb0\\n"; }\n'
                  'kill(){ printf "%s\\n" "$*" >> ' + fixture + '/signals; '
                  'rm -f ' + fixture + '/proc/123/stat; : > ' + fixture + '/proc/net/tcp; }\n')
        path = self.put('takeover-fixture.sh', prefix + library
                        + '\ndcts_release_camera_helpers\necho released=$DCTS_CAMERA_HELPERS_RELEASED\n')
        result = subprocess.run([self.bash, '--noprofile', '--norc', shell_path(path)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        return result.returncode, result.stdout.decode(errors='replace')

    def test_verified_orphan_with_rtsp_and_camera_dma_is_released(self):
        code, output = self.run_takeover()
        self.assertEqual(code, 0, output)
        self.assertIn('released=1', output)
        self.assertEqual((self.root / 'signals').read_text().strip(), '-TERM 123')

    def test_ordinary_dhcp_without_camera_resources_is_left_running(self):
        self.put('proc/net/tcp', '')
        code, output = self.run_takeover()
        self.assertEqual(code, 0, output)
        self.assertIn('released=0', output)
        self.assertTrue((self.root / 'proc/123/stat').exists())
        self.assertFalse((self.root / 'signals').exists())

    def test_unrecognized_camera_descriptor_owner_is_rejected(self):
        self.put('proc/123/cmdline', b'another-program\x00')
        code, output = self.run_takeover()
        self.assertNotEqual(code, 0)
        self.assertIn('unrecognized process', output)
        self.assertFalse((self.root / 'signals').exists())

    def test_active_camera_is_rejected_before_any_signal(self):
        code, output = self.run_takeover(camera_active=True)
        self.assertNotEqual(code, 0)
        self.assertIn('Stop the camera', output)
        self.assertFalse((self.root / 'signals').exists())


if __name__ == '__main__':
    unittest.main()
