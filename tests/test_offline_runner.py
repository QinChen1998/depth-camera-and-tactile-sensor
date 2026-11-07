"""Regression tests for false successes and accidental non-offline execution."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('offline_runner', Path(__file__).resolve().parents[1] / 'tools/run_offline_tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class OfflineRunnerTests(unittest.TestCase):
    def test_report_requires_checks_and_successful_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / 'report.txt'
            for content, code, expected in (
                ('', 0, False), ('Finished successfully', 0, False), ('RESULT PASS\n', 0, False),
                ('CASE PASS\n', 1, False), ('CASE PASS\nOTHER FAIL\n', 0, False),
                ('CASE PASS\nPASS SECOND\n', 0, True), ('CASE BYPASS\n', 0, False),
            ):
                with self.subTest(content=content, code=code):
                    report.write_text(content, encoding='utf-8')
                    self.assertEqual(runner.assess_report(code, report)['passed'], expected)

    def test_missing_report_is_failure(self):
        result = runner.assess_report(0, Path('not-a-real-report.txt'))
        self.assertFalse(result['passed'])
        self.assertFalse(result['report_present'])

    def test_inherited_environment_cannot_enable_hardware(self):
        inherited = dict(PATH='untrusted', RV_TEST_OFFLINE_ONLY='0', RV_CONTROL_CPP='remote',
                         DCTS_MODE='live', PYTHONPATH='foreign', QT_PLUGIN_PATH='old',
                         QML2_IMPORT_PATH='old', CONDA_PREFIX='foreign', SystemRoot='C:/Windows')
        with patch.dict(os.environ, inherited, clear=True):
            env = runner.test_environment(Path('qt'), Path('workspace/tmp'))
        self.assertEqual(env['RV_TEST_OFFLINE_ONLY'], '1')
        self.assertEqual(env['TEMP'], str(Path('workspace/tmp')))
        self.assertEqual(env['TMP'], env['TEMP'])
        for key in ('RV_CONTROL_CPP', 'DCTS_MODE', 'PYTHONPATH', 'CONDA_PREFIX'):
            self.assertNotIn(key, env)
        self.assertNotIn('untrusted', env['PATH'])
        self.assertEqual(env['QT_PLUGIN_PATH'], str(Path('qt/plugins')))

    def test_non_offline_commands_and_empty_selection_rejected(self):
        base = ['--configuration', 'Release', '--qt-root', 'qt', '--output', 'out']
        for flags in (['--flags', 'rv-live-test'], ['--flags', 'import-settings'], ['--flags']):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    runner.arguments().parse_args(base + flags)
                self.assertEqual(error.exception.code, 2)
        self.assertEqual(len(runner.arguments().parse_args(base).flags), 8)


if __name__ == '__main__':
    unittest.main()
