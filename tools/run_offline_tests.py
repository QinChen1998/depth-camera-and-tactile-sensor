"""Run desktop regressions with isolated settings, temp files and no hardware."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

OFFLINE_FLAGS = (
    'port-ui-smoke-test', 'rv-tuner-test', 'tactile-serial-test',
    'tactile-visualization-test', 'tactile-3d-test', 'tactile-render-test',
    'rv-assist-recovery-test', 'rv-usb-enrollment-test',
)


def test_environment(qt_root, temporary, runtime=None):
    env = {k: v for k, v in os.environ.items()
           if not k.upper().startswith(('PYTHON', 'CONDA', 'RV_', 'DCTS_', 'QT_', 'QML'))}
    system = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32'
    env.update(PATH=str(runtime or qt_root / 'bin') + os.pathsep + str(system),
               QT_PLUGIN_PATH=str(runtime or qt_root / 'plugins'),
               QML_IMPORT_PATH=str((runtime or qt_root) / 'qml'),
               QML2_IMPORT_PATH=str((runtime or qt_root) / 'qml'),
               TEMP=str(temporary), TMP=str(temporary), RV_TEST_OFFLINE_ONLY='1')
    return env


def assess_report(returncode, report):
    text = report.read_text(encoding='utf-8-sig', errors='replace') if report.is_file() else ''
    lines = text.splitlines()
    checks = sum(bool(re.search(r'(?:^PASS\b|\bPASS$)', line.strip()))
                 for line in lines if not line.strip().startswith('RESULT '))
    failures = [line for line in lines if re.search(r'\bFAIL\b', line)]
    return dict(passed=returncode == 0 and checks > 0 and not failures,
                exit_code=returncode, checks=checks, failures=failures,
                report_present=report.is_file())


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=('Release', 'Debug'), required=True)
    parser.add_argument('--qt-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, help='Optional isolated build of the same desktop application')
    parser.add_argument('--local-runtime', action='store_true')
    parser.add_argument('--flags', nargs='+', choices=OFFLINE_FLAGS, default=OFFLINE_FLAGS)
    return parser


def main():
    args = arguments().parse_args()
    root = Path(__file__).resolve().parents[1]
    executable = args.executable.resolve() if args.executable else root / 'x64' / args.configuration / '触想工作台.exe'
    if not executable.is_file():
        raise SystemExit('Build the requested configuration first: ' + str(executable))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    temporary = output / 'tmp'
    temporary.mkdir()
    env = test_environment(args.qt_root.resolve(), temporary,
                           executable.parent if args.local_runtime else None)
    results = []
    for flag in dict.fromkeys(args.flags):
        report = output / (flag + '.txt')
        started = time.monotonic()
        # Debug UI checks include real reconnect/settling waits.
        timeout = 240 if args.configuration == 'Debug' and flag == 'tactile-visualization-test' else 150
        print('RUN ' + args.configuration + ' ' + flag, flush=True)
        try:
            process = subprocess.run([str(executable), '--' + flag, str(report)], cwd=root, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
            (output / (flag + '.log')).write_bytes(process.stdout)
            row = assess_report(process.returncode, report)
        except subprocess.TimeoutExpired as error:
            (output / (flag + '.log')).write_bytes(error.stdout or b'')
            row = dict(passed=False, timeout=True, timeout_seconds=timeout)
        except OSError as error:
            row = dict(passed=False, error=str(error))
        row.update(flag=flag, seconds=round(time.monotonic() - started, 2))
        results.append(row)
        print(json.dumps(row), flush=True)
    summary = dict(passed=bool(results) and all(row['passed'] for row in results),
                   configuration=args.configuration, local_runtime=args.local_runtime,
                   hardware_started=False, results=results)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
