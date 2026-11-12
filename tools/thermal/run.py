"""Authorized ten-minute board/host run with read-only thermal telemetry."""
import argparse
import csv
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import threading
import time

from analyze import analyze, number, first


class Decoder:
    """Accept only complete, ordered snapshots from the board's TSV stream."""
    def __init__(self):
        self.metadata = {}
        self.current = None
        self.last_index = -1

    def feed(self, line):
        fields = line.rstrip('\r\n').split('\t', 2)
        if len(fields) != 3:
            raise ValueError('Malformed board telemetry')
        kind, key, value = fields
        if kind == 'M':
            self.metadata.setdefault(key, []).append(value)
        elif kind == 'BEGIN':
            index, uptime = int(key), float(value)
            if index <= self.last_index or self.current is not None:
                raise ValueError('Repeated or unfinished board snapshot')
            self.current = {'index': index, 'board_uptime_s': uptime,
                            'host_unix_ms': time.time_ns()/1e6,
                            'host_monotonic_ns': time.monotonic_ns(), 'values': {}}
        elif kind == 'V':
            if self.current is None:
                raise ValueError('Board value outside snapshot')
            self.current['values'].setdefault(key, []).append(value)
        elif kind == 'END':
            if self.current is None or int(key) != self.current['index'] or float(value) < self.current['board_uptime_s']:
                raise ValueError('Board snapshot boundary mismatch')
            row = self.current;self.current = None;self.last_index = row['index']
            row['board_end_uptime_s'] = float(value)
            return row
        else:
            raise ValueError('Unknown board telemetry record')
        return None


class Monitor:
    def __init__(self, adb, serial, directory):
        self.decoder = Decoder();self.latest = None;self.error = None;self.samples = 0
        self.directory = directory
        self.stderr = (directory/'monitor.stderr.log').open('wb')
        script = Path(__file__).with_name('monitor.sh').read_text(encoding='utf-8')
        # Factory adbd supports only the legacy PTY shell service, not exec-out
        # or `shell -T`. Pass a safely quoted command, never script text on its
        # echoed stdin. The parser accepts the legacy transport's CR/LF output.
        command = '/bin/sh -c '+shlex.quote(script)
        self.process = subprocess.Popen([str(adb), '-s', serial, 'shell', command],
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=self.stderr)
        self.thread = threading.Thread(target=self.read, daemon=True);self.thread.start()

    def read(self):
        try:
            with (self.directory/'board.tsv').open('w', encoding='utf-8', newline='') as raw, \
                 (self.directory/'board.jsonl').open('w', encoding='utf-8') as output:
                for binary in self.process.stdout:
                    line = binary.decode('utf-8', errors='strict');raw.write(line)
                    row = self.decoder.feed(line)
                    if row is not None:
                        self.latest = row;self.samples += 1
                        output.write(json.dumps(row, separators=(',', ':'))+'\n');output.flush();raw.flush()
                        if self.samples == 1:
                            (self.directory/'board-metadata.json').write_text(json.dumps(self.decoder.metadata, indent=2), encoding='utf-8')
        except Exception as error:
            self.error = str(error)

    def check(self):
        if self.error or self.process.poll() is not None:
            raise RuntimeError('Board telemetry stopped: '+str(self.error or self.process.returncode))

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=5)
        self.thread.join(timeout=5);self.stderr.close()
        (self.directory/'board-metadata.json').write_text(json.dumps(self.decoder.metadata, indent=2), encoding='utf-8')


GPU_FIELDS = ['index','name','temperature.gpu','utilization.gpu','utilization.memory',
              'clocks.current.sm','clocks.current.memory','power.draw','power.limit',
              'memory.used','memory.total','pstate','clocks_throttle_reasons.hw_thermal_slowdown',
              'clocks_throttle_reasons.sw_thermal_slowdown','clocks_throttle_reasons.sw_power_cap',
              'clocks_event_reasons_counters.hw_thermal_slowdown','clocks_event_reasons_counters.sw_thermal_slowdown']


def gpu_rows(text):
    result = []
    for row in csv.reader(text.splitlines(), skipinitialspace=True):
        if not row:continue
        if len(row)!=len(GPU_FIELDS):raise ValueError('GPU telemetry field count mismatch')
        result.append(dict(zip(GPU_FIELDS, (item.strip() for item in row))))
    return result


class HostMonitor:
    def __init__(self, directory):
        self.directory=directory;self.stop=threading.Event();self.latest=None
        self.smi=shutil.which('nvidia-smi')
        self.thread=threading.Thread(target=self.read, daemon=True);self.thread.start()

    def read(self):
        try:import psutil
        except ImportError:psutil=None
        if psutil:psutil.cpu_percent()
        with (self.directory/'host.jsonl').open('w',encoding='utf-8') as output:
            first_sample=True
            while not self.stop.is_set():
                began=time.monotonic()
                row={'host_unix_ms':time.time_ns()/1e6,'host_monotonic_ns':time.monotonic_ns()}
                if psutil:
                    row['cpu_percent']=None if first_sample else psutil.cpu_percent()
                    row['memory_available_mb']=psutil.virtual_memory().available/1024**2
                    battery=psutil.sensors_battery();row['ac_power_connected']=battery.power_plugged if battery else None
                if self.smi:
                    try:
                        query=subprocess.run([self.smi,'--query-gpu='+','.join(GPU_FIELDS),'--format=csv,noheader,nounits'],capture_output=True,timeout=5)
                        if query.returncode:raise RuntimeError(query.stderr.decode(errors='replace'))
                        row['gpus']=gpu_rows(query.stdout.decode('utf-8',errors='replace'))
                    except Exception as error:row['gpu_query_error']=str(error)
                else:row['gpu_query_error']='nvidia-smi unavailable'
                self.latest=row;output.write(json.dumps(row,separators=(',',':'))+'\n');output.flush();first_sample=False
                self.stop.wait(max(.01,5-(time.monotonic()-began)))

    def close(self):
        self.stop.set();self.thread.join(timeout=8)


def clean_environment(folder, serial):
    result = {k: v for k, v in os.environ.items()
              if not k.upper().startswith(('PYTHON', 'CONDA', 'RV_', 'DCTS_', 'QT_', 'QML'))}
    result.update(PATH=str(folder)+';'+os.environ.get('SystemRoot', 'C:/Windows')+'/System32',
                  QT_PLUGIN_PATH=str(folder), QML_IMPORT_PATH=str(folder/'qml'),
                  QML2_IMPORT_PATH=str(folder/'qml'), DCTS_LIVE_ENABLE='1', DCTS_LIVE_SERIAL=serial)
    return result


def heartbeat(monitor, gui_report, phase):
    row = monitor.latest or {};values = row.get('values', {})
    progress = []
    if gui_report.exists():
        try:
            progress = [json.loads(line) for line in gui_report.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
        except (OSError, json.JSONDecodeError):pass
    window = next((r for r in reversed(progress) if r.get('phase') == 'window'), {})
    print(json.dumps({'phase': phase, 'board_uptime_s': row.get('board_uptime_s'),
        'samples': monitor.samples, 'inference_seconds': window.get('elapsed_seconds'),
        'depth_fps': window.get('depth_fps'), 'board_publish_fps': window.get('board_publish_fps'),
        'temperatures_millicelsius': {k: number(first(values, k)) for k in values if '/thermal_zone' in k and k.endswith('/temp')},
        'cpu_khz': {k: number(first(values, k)) for k in values if k.endswith('/scaling_cur_freq')}}, ensure_ascii=True), flush=True)


def sample_wait(seconds, monitor, journal, phase):
    end = time.monotonic()+seconds
    while time.monotonic() < end:
        monitor.check();time.sleep(min(1, max(.01, end-time.monotonic())))
    heartbeat(monitor, journal, phase)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=('host',), default='host')
    parser.add_argument('--environment-note', default='Current assembly and passive/active cooling unchanged; ambient temperature not independently measured.')
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{16}', args.serial):raise SystemExit('Explicit lowercase RV1103 serial required')
    if os.environ.get('RV_TEST_OFFLINE_ONLY') == '1':raise SystemExit('Live test forbidden in offline mode')
    root = Path(__file__).resolve().parents[2]
    folder = root/'x64/Release';adb = folder/'platform-tools/adb.exe'
    gui = next(p for p in folder.glob('*.exe') if not p.name.isascii())
    target = args.output.resolve();target.mkdir(parents=True, exist_ok=False)
    manifest = {'serial': args.serial, 'started_utc': datetime.now(timezone.utc).isoformat(),
                'mode':args.mode,'model': 'host_unidepth_rgb672_vga_fp16_tof48_g', 'camera_enabled': args.mode=='host',
                'idle_seconds': 30, 'inference_seconds': 600, 'cooldown_seconds': 60,
                'sensor_saved_parameters_written': False, 'governor_or_thermal_controls_written': False,
                'environment_note': args.environment_note}
    (target/'run.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    state = subprocess.run([str(adb), '-s', args.serial, 'get-state'], capture_output=True, timeout=15)
    if state.returncode or state.stdout.strip() != b'device':raise SystemExit('Selected ADB device is not online')
    kernel = subprocess.run([str(adb), '-s', args.serial, 'shell', 'dmesg'], capture_output=True, timeout=10)
    (target/'kernel-before.log').write_bytes(kernel.stdout+kernel.stderr)
    # The ordinary GUI still authenticates SSH identity, registry and calibration,
    # and uses the existing owner/fault coordinator for start and normal stop.
    monitor = Monitor(adb, args.serial, target)
    host_monitor = HostMonitor(target) if args.mode=='host' else None
    gui_report = target/'inference.json';journal = target/'inference.json.progress.jsonl'
    child = None
    try:
        for _ in range(30):
            monitor.check()
            if monitor.latest:break
            time.sleep(.1)
        if not monitor.latest:raise RuntimeError('No complete telemetry snapshot')
        heartbeat(monitor, journal, 'idle_baseline_started')
        sample_wait(30, monitor, journal, 'idle_baseline_finished')
        with (target/'inference.process.log').open('wb') as output:
            flag='--host-thermal-live-validation'
            child = subprocess.Popen([str(gui), flag, str(gui_report)],
                                     cwd=root, env=clean_environment(folder, args.serial), stdout=output, stderr=subprocess.STDOUT)
            print(json.dumps({'phase': 'inference_gui_started', 'pid': child.pid}), flush=True)
            started = time.monotonic();last = started
            while child.poll() is None:
                monitor.check();time.sleep(.5)
                if time.monotonic()-last >= 30:
                    heartbeat(monitor, journal, 'inference_running');last = time.monotonic()
                if time.monotonic()-started > 850:
                    raise RuntimeError('Inference test exceeded its bounded runtime; inspect the visible GUI and stop normally')
        heartbeat(monitor, journal, 'inference_gui_finished')
        manifest['gui_exit_code'] = child.returncode
        sample_wait(60, monitor, journal, 'post_stop_observation_finished')
        kernel = subprocess.run([str(adb), '-s', args.serial, 'shell', 'dmesg'], capture_output=True, timeout=10)
        (target/'kernel-after.log').write_bytes(kernel.stdout+kernel.stderr)
    except Exception as error:
        manifest['error'] = str(error)
        # Keep a still-running GUI visible for its ordinary Stop/Close cleanup;
        # never use a broad device kill or reset to hide a failed test.
        manifest['gui_still_running'] = child is not None and child.poll() is None
        raise
    finally:
        monitor.close()
        if host_monitor:host_monitor.close()
        manifest['finished_utc'] = datetime.now(timezone.utc).isoformat()
        manifest['telemetry_samples'] = monitor.samples;manifest['monitor_error'] = monitor.error
        (target/'run.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    result = analyze(target)
    print(json.dumps({k: result.get(k) for k in ('conclusion', 'complete_ten_minutes', 'throughput_drop_percent', 'live_test_passed')}), flush=True)
    return 0 if result['complete_ten_minutes'] and result['live_test_passed'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
