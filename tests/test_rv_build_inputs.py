"""Exercise native-worker dependency checks without compiling or using hardware."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
POWERSHELL = shutil.which('pwsh.exe') or shutil.which('powershell.exe')
BOARD = 'tools/rv_capture/native/payload/native-fov6.bin'
SDK = ('include/libssh2.h', 'include/openssl/crypto.h', 'lib/libssh2.lib', 'lib/libcrypto.lib',
       'bin/libssh2.dll', 'bin/libcrypto-3-x64.dll', 'LICENSE-libssh2', 'LICENSE-OpenSSL')


@unittest.skipUnless(POWERSHELL, 'PowerShell is required')
class BuildInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest = self.root / 'tools/rv_control/build_inputs.json'
        self.manifest.parent.mkdir(parents=True)
        self.data = {'schema': 1, 'version': 'test', 'files': {BOARD: hashlib.sha256(b'current capture').hexdigest()}}
        self.manifest.write_text(json.dumps(self.data), encoding='utf-8')
        for name in SDK:
            path = self.root / 'x64/rv_control_sdk/install' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'test SDK presence fixture')
        self.board = self.root / BOARD
        self.board.parent.mkdir(parents=True)
        self.board.write_bytes(b'current capture')

    def check(self):
        script = ". $env:DCTS_INPUT_CHECK; try { Assert-RvBuildInputs -ProjectRoot $env:DCTS_INPUT_ROOT; exit 0 } catch { Write-Output $_.Exception.Message; exit 1 }"
        environment = dict(os.environ, DCTS_INPUT_CHECK=str(ROOT / 'tools/rv_control/check_build_inputs.ps1'), DCTS_INPUT_ROOT=str(self.root))
        result = subprocess.run([POWERSHELL, '-NoProfile', '-NonInteractive', '-Command', script], env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        return result.returncode, result.stdout.decode('utf-8', errors='replace')

    def test_matching_inputs_accepted(self):
        self.assertEqual(self.check()[0], 0)

    def test_missing_f6_payload_names_the_required_package(self):
        self.board.unlink()
        code, message = self.check()
        self.assertNotEqual(code, 0)
        self.assertIn('Missing board file: ' + BOARD, message)
        self.assertIn('DCTS-test-RV-build-inputs.zip', message)

    def test_old_payload_rejected_without_overwrite(self):
        self.board.write_bytes(b'old capture')
        code, message = self.check()
        self.assertNotEqual(code, 0)
        self.assertIn('Board file version mismatch', message)
        self.assertEqual(self.board.read_bytes(), b'old capture')

    def test_missing_sdk_reported_together_with_missing_payload(self):
        (self.root / 'x64/rv_control_sdk/install/lib/libssh2.lib').unlink()
        self.board.unlink()
        code, message = self.check()
        self.assertNotEqual(code, 0)
        self.assertIn('Missing SDK file', message)
        self.assertIn('Missing board file', message)

    def test_manifest_cannot_reference_outside_payload_paths(self):
        self.data['files'] = {'../outside': '0' * 64}
        self.manifest.write_text(json.dumps(self.data), encoding='utf-8')
        self.assertNotEqual(self.check()[0], 0)


if __name__ == '__main__':
    unittest.main()
