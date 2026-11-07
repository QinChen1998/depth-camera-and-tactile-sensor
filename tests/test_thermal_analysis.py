"""Counter and missing-data regressions; no real board or hardware is accessed."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

THERMAL = Path(__file__).resolve().parents[1]/'tools/thermal'
sys.path.insert(0, str(THERMAL))
from analyze import board_rows, cpu_delta, merge_windows, residency_mhz, summarize, process_percent, merge_host, active
from run import Decoder, GPU_FIELDS, gpu_rows


class ThermalAnalysisTests(unittest.TestCase):
    def test_cpu_guest_is_not_double_counted_and_iowait_is_separate(self):
        self.assertEqual(cpu_delta('cpu 0 0 0 0 0 0 0 0 0 0',
                                  'cpu 10 0 20 60 10 0 0 0 10 0'), (30., 10.))

    def test_cpu_reset_or_missing_counters_are_not_zero_load(self):
        self.assertEqual(cpu_delta('cpu 20 0 0 100 0 0 0 0', 'cpu 1 0 0 10 0 0 0 0'), (None, None))
        self.assertEqual(cpu_delta(None, None), (None, None))

    def test_process_ticks_ignore_name_spacing_and_refuse_pid_reuse(self):
        def record(user, system, started):
            fields = ['S']+['0']*20;fields[11]=str(user);fields[12]=str(system);fields[19]=str(started)
            return '10 (worker (test name)) '+' '.join(fields)
        self.assertEqual(process_percent(record(10, 20, 100), record(12, 21, 100), 100), 3)
        self.assertIsNone(process_percent(record(10, 20, 100), record(12, 21, 101), 100))
        self.assertIsNone(process_percent(None, record(12, 21, 100), 100))

    def test_frequency_residency_uses_deltas_and_rejects_resets(self):
        self.assertEqual(residency_mhz(['400000 100', '800000 100'], ['400000 110', '800000 130']), 700.)
        self.assertIsNone(residency_mhz(['400000 100'], ['400000 10']))
        self.assertIsNone(residency_mhz([], []))

    def test_clock_summary_rate_column_comes_from_running_kernel_header(self):
        sample = {'host_unix_ms': 0, 'board_uptime_s': 1, 'board_end_uptime_s': 1.01,
                  'values': {'/sys/kernel/debug/clk/clk_summary': ['armclk 1 1 800000000 0']}}
        metadata = {'/sys/kernel/debug/clk/clk_summary': ['clock enable_cnt prepare_cnt rate accuracy']}
        self.assertEqual(board_rows([sample], metadata)[0]['clock_mhz:armclk'], 800.)
        self.assertNotIn('clock_mhz:armclk', board_rows([sample], {})[0])

    def test_parser_requires_matching_complete_snapshots(self):
        decoder = Decoder()
        self.assertIsNone(decoder.feed('M\t/proc/version\tfixture\n'))
        self.assertIsNone(decoder.feed('BEGIN\t0\t1.00\n'))
        decoder.feed('V\t/proc/stat\tcpu 0 0 0 0 0 0 0 0\n')
        with self.assertRaises(ValueError):decoder.feed('END\t1\t1.01\n')
        row = decoder.feed('END\t0\t1.01\n')
        self.assertEqual(row['values']['/proc/stat'][0], 'cpu 0 0 0 0 0 0 0 0')
        with self.assertRaises(ValueError):decoder.feed('BEGIN\t0\t1.02\n')

    def test_old_telemetry_is_not_reused_as_current_temperature(self):
        rows = [{'host_unix_ms': 0, 'temperature_c:soc': 50}]
        merged = merge_windows(rows, [{'unix_ms': 10000, 'elapsed_seconds': 10, 'depth_fps': 15}])
        self.assertNotIn('temperature_c:soc', merged[0])
        self.assertEqual(merged[0]['depth_fps'], 15)

    @staticmethod
    def warmup(cooling=False):
        return [{'elapsed_seconds': i, 'telemetry_delta_ms': 0, 'boot_id': 'same-boot',
                 'temperature_c:soc': 35+i/20, 'board_publish_fps': 15 if i<300 else 10,
                 'depth_fps': 15 if i<300 else 10, 'cpu_mhz:policy0:residency': 800 if i<300 else 400,
                 'cooling_state:cooling_device0:cpufreq': int(cooling and i>=300)} for i in range(1, 601)]

    def test_frequency_and_speed_drop_alone_do_not_prove_thermal_throttling(self):
        result = summarize(self.warmup(False), True)
        self.assertEqual(result['conclusion'], 'throughput_loss_cause_unconfirmed')
        self.assertAlmostEqual(result['throughput_drop_percent'], 100/3)

    def test_cpu_cooling_and_clock_drop_are_reported_with_throughput_loss(self):
        result = summarize(self.warmup(True), True)
        self.assertEqual(result['conclusion'], 'thermal_cooling_and_clock_drop_coincide_with_throughput_loss')
        self.assertTrue(result['complete_ten_minutes'])

    def test_short_run_or_missing_thermal_nodes_cannot_pass_thermal_assessment(self):
        self.assertEqual(summarize(self.warmup()[:60], True)['conclusion'], 'incomplete_test')
        rows = [{'elapsed_seconds': i, 'depth_fps': 15} for i in range(1, 601)]
        self.assertEqual(summarize(rows, True)['conclusion'], 'throughput_observed_but_thermal_telemetry_incomplete')

    def test_pc_receive_loss_is_distinguished_from_board_publish_rate(self):
        rows = self.warmup()
        for row in rows:row['board_publish_fps'] = 15
        result = summarize(rows, True)
        self.assertAlmostEqual(result['throughput_drop_percent'], 0)
        self.assertEqual(result['conclusion'], 'no_sustained_throughput_loss_observed')

    def test_gpu_missing_readings_are_not_inactive_or_zero(self):
        self.assertIsNone(active('[N/A]'));self.assertEqual(active('Not Active'),0);self.assertEqual(active('Active'),1)
        self.assertIsNone(summarize(self.warmup(),True)['host_gpu_thermal_slowdown_observed'])
        row=['0','"GPU, test"']+['0']*(len(GPU_FIELDS)-2)
        self.assertEqual(gpu_rows(', '.join(row))[0]['name'],'GPU, test')
        with self.assertRaises(ValueError):gpu_rows('0, incomplete')

    def test_stale_host_gpu_temperature_is_not_reused(self):
        rows=[{'host_unix_ms':10000}]
        merge_host(rows,[{'host_unix_ms':0,'gpus':[{'index':'0','temperature.gpu':'90'}]}])
        self.assertNotIn('temperature_c:host_gpu0',rows[0])

    def test_gpu_cumulative_thermal_counter_uses_run_delta(self):
        rows=self.warmup()
        for row in rows:row['host_gpu_hw_thermal_us']=5000;row['host_gpu_hw_thermal_active']=0
        self.assertFalse(summarize(rows,True)['host_gpu_thermal_slowdown_observed'])
        rows[-1]['host_gpu_hw_thermal_us']=5100
        result=summarize(rows,True)
        self.assertTrue(result['host_gpu_thermal_slowdown_observed'])
        self.assertEqual(result['host_gpu_thermal_counter_increase_us']['host_gpu_hw_thermal_us'],100)

    def test_monitor_executes_only_against_fixture_files(self):
        bash = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path('/bin/bash')
        if not bash.exists():self.skipTest('bash is unavailable')
        with tempfile.TemporaryDirectory(prefix='dcts-thermal-') as temporary:
            root = Path(temporary)
            files = {'proc/uptime': '1.00 0.00\n', 'proc/version': 'fixture-only\n',
                     'proc/stat': 'cpu 1 2 3 4 5 6 7 8\n', 'proc/sys/kernel/random/boot_id': 'fixture-boot\n',
                     'sys/class/thermal/thermal_zone0/type': 'soc\n',
                     'sys/class/thermal/thermal_zone0/temp': '42000\n',
                     'sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq': '800000\n'}
            for name, value in files.items():
                path = root/name;path.parent.mkdir(parents=True, exist_ok=True);path.write_text(value, encoding='utf-8')
            env = dict(os.environ, DCTS_THERMAL_ROOT=root.as_posix(), DCTS_THERMAL_SAMPLES='1')
            process = subprocess.run([str(bash), str(THERMAL/'monitor.sh')], capture_output=True, env=env, timeout=15)
            self.assertEqual(process.returncode, 0, process.stderr)
            decoder = Decoder();samples = []
            for line in process.stdout.decode().splitlines():
                row = decoder.feed(line)
                if row is not None:samples.append(row)
            self.assertEqual(len(samples), 1)
            self.assertEqual(decoder.metadata['/proc/version'], ['fixture-only'])
            self.assertEqual(samples[0]['values']['/sys/class/thermal/thermal_zone0/temp'], ['42000'])


if __name__ == '__main__':unittest.main()
