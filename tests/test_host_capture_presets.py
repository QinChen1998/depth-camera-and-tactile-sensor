"""Execute host capture policy with a fake producer; never open hardware."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / 'tools/rv_capture/payload/dtof-host.sh'


class HostCapturePresets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        git_bash = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe'
        cls.shell = str(git_bash) if os.name == 'nt' and git_bash.is_file() else shutil.which('sh')
        if not cls.shell:
            raise RuntimeError('A POSIX shell is required')

    def run_preset(self, mode, saved, grid=16):
        code = HOOK.read_text(encoding='utf-8')
        code += '\nlaunch_tof(){ printf "%s\\n" "$@"; }\n'
        code += 'dcts_tof ' + shlex.join([str(grid), str(mode), 'launch_tof', str(mode), './tmf8829_ext', *saved]) + '\n'
        return subprocess.run([self.shell, '-s'], input=code, text=True, capture_output=True)

    def test_host_overrides_only_session_grid_targets_signal_and_noise(self):
        for snr, period, iterations, short in [(6, 66, 600, 100), (9, 91, 1234, 71), (255, 2000, 10000, 1)]:
            with self.subTest(snr=snr):
                saved = ['-m', '-s', str(short), '-b', '1', '-d', '12', '-p', str(period), '-t', '2', '-o', '3', '-r', str(snr), '-i', str(iterations), '-g', '-n', '--socket', '/run/extrinsic_capture/tof.sock', '--quiet', '--poll-us', '1000']
                result = self.run_preset(14, saved)
                self.assertEqual(result.returncode, 0, result.stderr)
                args = result.stdout.splitlines()[2:]
                for flag, value in [('-d', 5), ('-o', 1), ('-t', 0), ('-r', snr), ('-p', period), ('-i', iterations), ('-s', short), ('-b', 1)]:
                    self.assertEqual(args.count(flag), 1)
                    self.assertEqual(args[args.index(flag)+1], str(value))
                self.assertNotIn('-g', args)
                self.assertNotIn('-n', args)
                self.assertEqual(args[args.index('--socket')+1], '/run/extrinsic_capture/tof.sock')

    def test_native48_ordinary_and_high_accuracy_keep_saved_timing(self):
        for mode, sensor_mode in [(24, 11), (25, 12)]:
            result = self.run_preset(mode, ['-m', '-d', '5', '-p', '91', '-i', '600', '-r', '6', '-g', '-n'], 48)
            self.assertEqual(result.returncode, 0, result.stderr)
            args = result.stdout.splitlines()[2:]
            for flag, value in [('-d', sensor_mode), ('-p', 91), ('-i', 600), ('-r', 6), ('-o', 1), ('-t', 0)]:
                self.assertEqual(args.count(flag), 1)
                self.assertEqual(args[args.index(flag)+1], str(value))
            self.assertNotIn('-g', args)
            self.assertNotIn('-n', args)
            self.assertEqual(self.run_preset(mode, ['-d', '5'], 16).returncode, 2)

    def test_all_retired_modes_and_wrong_grids_are_refused(self):
        for mode, grid in [(m, g) for m in (10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22) for g in (16, 48)] + [(14, 48), (14, '')]:
            with self.subTest(mode=mode, grid=grid):
                result = self.run_preset(mode, ['-m', '-d', '11'], grid)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')

    def test_incomplete_numeric_override_is_refused(self):
        for flag in ('-d', '-o', '-t'):
            result = self.run_preset(14, ['-m', flag])
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, '')

    def test_board_launchers_reject_retired_models_before_accessing_hardware(self):
        token = 'a' * 32
        for script, prefix in [('tools/rv_capture/payload/native_gui.sh', ['validate']),
                               ('tools/rv_control/bootstrap/candidate-control.sh', ['candidate']),
                               ('tools/rv_control/bootstrap/session.sh', [])]:
            for mode in (16, 22):
                # session.sh checks serial before the mode guard, so test that
                # guard in isolation; the full session is syntax-checked below.
                if not prefix:
                    continue
                result = subprocess.run([self.shell, str(ROOT / script), *prefix, str(mode), token, '1,1'], capture_output=True)
                self.assertEqual(result.returncode, 2)
        for script in ('tools/rv_capture/payload/native_gui.sh', 'tools/rv_capture/payload/dtof-host.sh',
                       'tools/rv_control/bootstrap/session.sh', 'tools/rv_control/bootstrap/candidate-control.sh'):
            result = subprocess.run([self.shell, '-n', str(ROOT / script)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
