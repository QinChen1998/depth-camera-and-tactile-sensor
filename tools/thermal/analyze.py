"""Reduce read-only board telemetry and admitted GUI frame counters together."""
import argparse
import bisect
import csv
import json
from pathlib import Path
import statistics


def number(value):
    try:
        result = float(value)
        return result if abs(result) < float('inf') else None
    except (TypeError, ValueError):
        return None


def first(values, path):
    lines = values.get(path, [])
    return lines[0] if lines else None


def cpu_delta(before, after):
    """Exclude guest double counting; report iowait separately from busy."""
    try:
        a = list(map(int, before.split()[1:9]))
        b = list(map(int, after.split()[1:9]))
    except (ValueError, AttributeError):
        return None, None
    if len(a) != 8 or len(b) != 8:
        return None, None
    delta = [y-x for x, y in zip(a, b)]
    total = sum(delta)
    if total <= 0 or min(delta) < 0:
        return None, None
    return 100*(total-delta[3]-delta[4])/total, 100*delta[4]/total


def residency_mhz(before, after):
    """Use counter differences, never cumulative boot-time averages."""
    try:
        a = {int(f): int(t) for f, t in (line.split() for line in before)}
        b = {int(f): int(t) for f, t in (line.split() for line in after)}
    except (ValueError, TypeError):
        return None
    if a.keys() != b.keys() or any(b[f] < a[f] for f in a):
        return None
    ticks = sum(b[f]-a[f] for f in a)
    return sum(f*(b[f]-a[f]) for f in a)/ticks/1000 if ticks else None


def process_percent(before, after, total_ticks):
    """Same PID/start time only; process names may contain spaces/parentheses."""
    try:
        a = before.rsplit(') ', 1)[1].split();b = after.rsplit(') ', 1)[1].split()
        if a[19] != b[19] or total_ticks <= 0:return None
        ticks = int(b[11])+int(b[12])-int(a[11])-int(a[12])
        return 100*ticks/total_ticks if ticks >= 0 else None
    except (AttributeError, IndexError, ValueError):
        return None


def read_jsonl(path):
    rows = []
    if path.exists():
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # An interrupted final line is not a complete sample.
    return rows


def board_rows(samples, metadata):
    result = []
    previous = None
    observer_pid = first(metadata, 'observer_pid')
    rate_index = None
    for line in metadata.get('/sys/kernel/debug/clk/clk_summary', []):
        fields = line.split()
        if 'clock' in fields and 'rate' in fields:
            rate_index = fields.index('rate');break
    for sample in samples:
        values = sample['values']
        row = {'host_unix_ms': sample['host_unix_ms'],
               'board_uptime_s': sample['board_uptime_s'],
               'sample_read_seconds': sample['board_end_uptime_s']-sample['board_uptime_s']}
        boot = first(values, '/proc/sys/kernel/random/boot_id')
        row['boot_id'] = boot
        same_boot = previous is not None and boot and boot == first(previous['values'], '/proc/sys/kernel/random/boot_id')
        if same_boot:
            row['cpu_busy_percent'], row['cpu_iowait_percent'] = cpu_delta(
                first(previous['values'], '/proc/stat'), first(values, '/proc/stat'))
            try:
                total = sum(map(int, first(values, '/proc/stat').split()[1:9]))-sum(map(int, first(previous['values'], '/proc/stat').split()[1:9]))
            except (AttributeError, ValueError):total = 0
            for path in values:
                if path.startswith('/proc/') and path.endswith('/stat') and path!='/proc/stat':
                    percent = process_percent(first(previous['values'], path), first(values, path), total)
                    pid = path.split('/')[2]
                    name = first(values, path[:-4]+'comm')
                    if pid == observer_pid:row['observer_cpu_percent'] = percent
                    elif name:row['process_cpu_percent:'+name] = percent
        for line in values.get('/proc/meminfo', []):
            fields = line.split()
            if len(fields)>=2 and fields[0]=='MemAvailable:':row['memory_available_kb'] = number(fields[1])
        for path, lines in values.items():
            value = number(lines[0]) if lines else None
            if '/thermal_zone' in path and path.endswith('/temp'):
                zone = path.rsplit('/', 1)[0]
                name = first(metadata, zone+'/type') or zone.split('/')[-1]
                row['temperature_c:'+name] = None if value is None else value/1000
            elif '/cooling_device' in path and path.endswith('/cur_state'):
                cooler = path.rsplit('/', 1)[0]
                name = first(metadata, cooler+'/type') or cooler.split('/')[-1]
                row['cooling_state:'+cooler.split('/')[-1]+':'+name] = value
            elif '/cpufreq/' in path and path.endswith(('/scaling_cur_freq', '/cpuinfo_cur_freq', '/scaling_max_freq')):
                policy, field = path.rsplit('/', 1)
                row['cpu_mhz:'+policy.split('/')[-1]+':'+field] = None if value is None else value/1000
            elif '/cpufreq/' in path and path.endswith('/stats/time_in_state') and same_boot:
                policy = path.split('/stats/')[0].split('/')[-1]
                row['cpu_mhz:'+policy+':residency'] = residency_mhz(previous['values'].get(path, []), lines)
            elif '/devfreq/' in path and path.endswith(('/cur_freq', '/max_freq')):
                device, field = path.rsplit('/', 1)
                name = first(metadata, device+'/name') or device.split('/')[-1]
                row['devfreq_mhz:'+name+':'+field] = None if value is None else value/1e6
            elif path.endswith('/clk/clk_summary'):
                for line in lines:
                    fields = line.split()
                    # Older kernels omit protect_count; use the actual header.
                    if rate_index is not None and len(fields) > rate_index:
                        rate = number(fields[rate_index])
                        if rate is not None:
                            row['clock_mhz:'+fields[0]] = rate/1e6
        result.append(row)
        previous = sample
    return result


def merge_windows(rows, windows):
    """Nearest same-host time within 2.5s; missing samples stay missing."""
    times = [row['host_unix_ms'] for row in rows]
    merged = []
    for window in windows:
        value = {'elapsed_seconds': window['elapsed_seconds'], 'host_unix_ms': window['unix_ms']}
        pos = bisect.bisect_left(times, window['unix_ms'])
        candidates = [i for i in (pos-1, pos) if 0 <= i < len(rows)]
        nearest = min(candidates, key=lambda i: abs(times[i]-window['unix_ms'])) if candidates else None
        if nearest is not None and abs(times[nearest]-window['unix_ms']) <= 2500:
            value.update(rows[nearest]);value['telemetry_delta_ms'] = times[nearest]-window['unix_ms']
        for key in ('depth_fps', 'painted_fps', 'board_publish_fps', 'native_source_fps', 'video_fps',
                    'received_native_fps', 'received_photon_fps', 'candidate_age_ms', 'board_send_dropped'):
            value[key] = number(window.get(key))
        for key in ('npu', 'board_compute', 'host_contour', 'host_inference', 'host_total'):
            value[key+'_mean_ms'] = number(window.get(key, {}).get('mean_ms'))
        merged.append(value)
    return merged


def mean(rows, key):
    values = [row[key] for row in rows if number(row.get(key)) is not None]
    return statistics.mean(values) if values else None


def active(value):
    value = str(value).strip().lower()
    return 1 if value=='active' else 0 if value in ('not active','inactive') else None


def merge_host(rows, samples):
    times = [v['host_unix_ms'] for v in samples]
    for row in rows:
        pos = bisect.bisect_left(times, row['host_unix_ms'])
        candidates = [i for i in (pos-1,pos) if 0<=i<len(samples)]
        nearest = min(candidates,key=lambda i:abs(times[i]-row['host_unix_ms'])) if candidates else None
        if nearest is None or abs(times[nearest]-row['host_unix_ms'])>3000:continue
        sample=samples[nearest];row['host_telemetry_delta_ms']=sample['host_unix_ms']-row['host_unix_ms']
        row['host_cpu_percent']=number(sample.get('cpu_percent'));row['host_memory_available_mb']=number(sample.get('memory_available_mb'))
        gpus=sample.get('gpus',[])
        if not gpus:continue
        gpu=next((g for g in gpus if str(g.get('index'))=='0'),gpus[0])
        for source,destination in [('temperature.gpu','temperature_c:host_gpu0'),('clocks.current.sm','host_gpu_sm_mhz'),
            ('clocks.current.memory','host_gpu_memory_mhz'),('utilization.gpu','host_gpu_utilization_percent'),
            ('power.draw','host_gpu_power_watts'),('memory.used','host_gpu_memory_used_mb'),
            ('clocks_event_reasons_counters.hw_thermal_slowdown','host_gpu_hw_thermal_us'),
            ('clocks_event_reasons_counters.sw_thermal_slowdown','host_gpu_sw_thermal_us')]:
            row[destination]=number(gpu.get(source))
        for source,destination in [('clocks_throttle_reasons.hw_thermal_slowdown','host_gpu_hw_thermal_active'),
            ('clocks_throttle_reasons.sw_thermal_slowdown','host_gpu_sw_thermal_active'),
            ('clocks_throttle_reasons.sw_power_cap','host_gpu_power_cap_active')]:row[destination]=active(gpu.get(source))
    return rows


def summarize(rows, completed):
    if not rows:
        return {'conclusion': 'insufficient_data', 'complete_ten_minutes': False}
    end = max(r['elapsed_seconds'] for r in rows)
    early = [r for r in rows if r['elapsed_seconds'] <= 60]
    late = [r for r in rows if r['elapsed_seconds'] >= max(60, end-60)]
    keys = sorted(set().union(*(r.keys() for r in rows)) - {'host_unix_ms', 'boot_id'})
    early_means = {k: mean(early, k) for k in keys if k != 'elapsed_seconds'}
    late_means = {k: mean(late, k) for k in keys if k != 'elapsed_seconds'}
    # Board counters, not PC receive/draw rate, identify board throughput loss.
    rate = 'board_publish_fps' if mean(rows, 'board_publish_fps') is not None else 'depth_fps'
    initial, final = early_means.get(rate), late_means.get(rate)
    drop = 100*(1-final/initial) if initial and final is not None else None
    loss = drop is not None and drop >= 10
    cooling_keys = [k for k in keys if k.startswith('cooling_state:') and any(s in k.lower() for s in ('cpu', 'npu', 'devfreq'))]
    cooling = any(number(r.get(k)) is not None and r[k] > 0 for r in rows for k in cooling_keys)
    frequencies = [k for k in keys if k.startswith(('cpu_mhz:', 'devfreq_mhz:', 'clock_mhz:'))]
    lower_clock = [k for k in frequencies if early_means.get(k) and late_means.get(k) is not None
                   and late_means[k] < .95*early_means[k]]
    boots = {r['boot_id'] for r in rows if r.get('boot_id')}
    full = completed and end >= 599 and len(rows) >= 550 and len(boots) <= 1
    matched = sum('telemetry_delta_ms' in r for r in rows)
    has_temperature = any(k.startswith('temperature_c:') and not k.startswith('temperature_c:host_gpu') for k in keys)
    telemetry_sufficient = matched >= .9*len(rows) and has_temperature and bool(frequencies)
    gpu_thermal_deltas={}
    for key in ('host_gpu_hw_thermal_us','host_gpu_sw_thermal_us'):
        readings=[r[key] for r in rows if number(r.get(key)) is not None]
        gpu_thermal_deltas[key]=readings[-1]-readings[0] if len(readings)>1 and readings[-1]>=readings[0] else None
    gpu_thermal=any(r.get(key)==1 for r in rows for key in ('host_gpu_hw_thermal_active','host_gpu_sw_thermal_active')) or any(v and v>0 for v in gpu_thermal_deltas.values())
    gpu_telemetry=any(number(r.get(k)) is not None for r in rows for k in ('host_gpu_hw_thermal_us','host_gpu_sw_thermal_us','host_gpu_hw_thermal_active','host_gpu_sw_thermal_active'))
    gpu_power_telemetry=any(number(r.get('host_gpu_power_cap_active')) is not None for r in rows)
    if not full:
        conclusion = 'incomplete_test'
    elif not telemetry_sufficient:
        conclusion = 'throughput_observed_but_thermal_telemetry_incomplete'
    elif gpu_thermal:
        conclusion = 'host_gpu_thermal_slowdown_observed'+('_with_output_rate_loss' if loss else '_without_sustained_output_rate_loss')
    elif cooling and lower_clock and loss:
        conclusion = 'thermal_cooling_and_clock_drop_coincide_with_throughput_loss'
    elif cooling:
        conclusion = 'thermal_cooling_observed'+('_with_speed_loss_cause_not_isolated' if loss else '_without_sustained_throughput_loss')
    elif loss:
        conclusion = 'throughput_loss_cause_unconfirmed'
    else:
        conclusion = 'no_sustained_throughput_loss_observed'
    return {'conclusion': conclusion, 'complete_ten_minutes': full, 'duration_seconds': end,
            'samples': len(rows), 'board_throughput_source': rate, 'first_minute': early_means,
            'throughput_measurement': 'independent board publication counter' if rate=='board_publish_fps' else 'PC-observed valid output arrivals; not an independent board publication counter',
            'last_minute': late_means, 'throughput_drop_percent': drop,
            'frequency_series_lower_at_end': lower_clock, 'cpu_npu_cooling_active_observed': cooling,
            'host_gpu_thermal_telemetry_available':gpu_telemetry,
            'host_gpu_thermal_slowdown_observed': gpu_thermal if gpu_telemetry else None,'host_gpu_thermal_counter_increase_us':gpu_thermal_deltas,
            'host_gpu_power_capping_observed':any(r.get('host_gpu_power_cap_active')==1 for r in rows) if gpu_power_telemetry else None,
            'telemetry_samples_matched': matched, 'thermal_telemetry_sufficient': telemetry_sufficient,
            'temperature_available': has_temperature,
            'frequency_available': bool(frequencies),
            'interpretation': 'Correlation in one natural warm-up run; absent throttling here does not prove it cannot occur in other ambient/cooling conditions. Missing nodes are not zero readings.'}


def write_csv(path, rows):
    fields = sorted(set().union(*(row.keys() for row in rows))) if rows else []
    with path.open('w', encoding='utf-8-sig', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=fields);writer.writeheader();writer.writerows(rows)


def plot(path, rows, metadata=None):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    chinese = Path('C:/Windows/Fonts/msyh.ttc').is_file()
    if chinese:
        from matplotlib import font_manager
        font_manager.fontManager.addfont('C:/Windows/Fonts/msyh.ttc')
        plt.rcParams['font.family'] = font_manager.FontProperties(fname='C:/Windows/Fonts/msyh.ttc').get_name()
        plt.rcParams['axes.unicode_minus'] = False
    if not rows:
        return
    keys = sorted(set().union(*(r.keys() for r in rows)))
    panels = [('Temperature (C)', [k for k in keys if k.startswith('temperature_c:')]),
              ('Clock (MHz)', [k for k in keys if k.startswith(('cpu_mhz:', 'devfreq_mhz:', 'clock_mhz:'))]+['host_gpu_sm_mhz']),
              ('CPU (%)', ['cpu_busy_percent', 'cpu_iowait_percent', 'observer_cpu_percent','host_cpu_percent','host_gpu_utilization_percent']),
              ('Time per frame (ms)', ['npu_mean_ms', 'board_compute_mean_ms', 'host_contour_mean_ms','host_inference_mean_ms','host_total_mean_ms']),
              ('Frames / second', ['native_source_fps', 'board_publish_fps', 'depth_fps', 'painted_fps','video_fps'])]
    chinese_labels = {'Temperature (C)':'温度（°C）','Clock (MHz)':'报告频率（MHz）','CPU (%)':'负载（%）',
        'Time per frame (ms)':'单帧耗时（ms）','Frames / second':'帧率（fps）',
        'temperature_c:soc-thermal':'RV1103 SoC 温度','cpu_busy_percent':'RV1103 CPU 忙碌','cpu_iowait_percent':'RV1103 I/O 等待',
        'observer_cpu_percent':'采样脚本','npu_mean_ms':'NPU 推理','board_compute_mean_ms':'板端整帧',
        'host_contour_mean_ms':'主机轮廓处理','native_source_fps':'原生 dToF 测量',
        'board_publish_fps':'板端发布计数速率','depth_fps':'有效深度输出','painted_fps':'深度绘制','video_fps':'原视频',
        'temperature_c:host_gpu0':'电脑 GPU 温度','host_gpu_sm_mhz':'电脑 GPU SM 频率',
        'host_cpu_percent':'电脑 CPU 忙碌','host_gpu_utilization_percent':'电脑 GPU 利用率',
        'host_inference_mean_ms':'主机模型推理','host_total_mean_ms':'主机整帧处理'}
    def display_name(key):
        if not chinese:return key
        if key.startswith('cpu_mhz:'):
            return {'scaling_cur_freq':'CPU 驱动报告频率','cpuinfo_cur_freq':'CPU 读回频率',
                    'scaling_max_freq':'CPU 允许上限','residency':'CPU 驻留加权频率'}.get(key.split(':')[-1],key)
        return chinese_labels.get(key,key)
    figure, axes = plt.subplots(len(panels), 1, figsize=(12, 15), sharex=True)
    for axis, (label, series) in zip(axes, panels):
        for key in series:
            data = [(r['elapsed_seconds']/60, r.get(key)) for r in rows if number(r.get(key)) is not None]
            if data:
                axis.plot([x for x, y in data], [y for x, y in data], label=display_name(key), linewidth=1, alpha=.8)
        axis.set_ylabel(chinese_labels.get(label,label) if chinese else label);axis.grid(alpha=.2)
        if axis.lines:axis.legend(fontsize=7, loc='best')
    for key in metadata or {}:
        if '/trip_point_' in key and key.endswith('_temp'):
            kind = first(metadata, key[:-5]+'_type');temperature = number(first(metadata, key))
            if kind=='passive' and temperature is not None:
                axes[0].axhline(temperature/1000, linestyle='--', color='#a05a16', alpha=.55,
                               label=('配置的被动温控点：%g°C' if chinese else 'Configured passive trip: %g C')%(temperature/1000))
    if axes[0].lines:axes[0].legend(fontsize=7, loc='best')
    axes[-1].set_xlabel('时间（开始推理测量后的分钟）' if chinese else 'Minutes since admitted inference measurement began')
    host_mode=any(number(r.get('host_inference_mean_ms')) is not None for r in rows)
    figure.suptitle(('RV1103＋上位机辅助推理（640×480）温升观察' if host_mode else 'RV1103 板端 LiteSR 384×256 自然温升观察') if chinese else ('RV1103 host-assisted depth | 10 minutes' if host_mode else 'RV1103 LiteSR 384x256 | 10 minutes'))
    figure.tight_layout();figure.savefig(path, dpi=160);plt.close(figure)


def analyze(directory):
    samples = read_jsonl(directory/'board.jsonl')
    metadata = json.loads((directory/'board-metadata.json').read_text(encoding='utf-8')) if (directory/'board-metadata.json').exists() else {}
    progress = read_jsonl(directory/'inference.json.progress.jsonl')
    windows = [v for v in progress if v.get('phase') == 'window']
    live = json.loads((directory/'inference.json').read_text(encoding='utf-8-sig')) if (directory/'inference.json').exists() else {}
    board = board_rows(samples, metadata);merged = merge_host(merge_windows(board, windows),read_jsonl(directory/'host.jsonl'))
    result = summarize(merged, live.get('passed', False))
    result['live_test_passed'] = live.get('passed', False)
    result['firmware_thermal_configuration'] = {k:v for k,v in metadata.items() if 'trip_point_' in k or k.endswith(('/scaling_governor','/cpuinfo_max_freq','/scaling_available_frequencies'))}
    result['unavailable_telemetry'] = {
        'independent_board_publication_counter': not any(number(r.get('board_publish_fps')) is not None for r in merged),
        'npu_frequency': not any('npu' in k.lower() and k.startswith(('devfreq_mhz:', 'clock_mhz:')) for r in board for k in r),
        'cpu_frequency_residency': not any(k.endswith(':residency') for r in board for k in r),
        'cooling_device_state': not any(k.startswith('cooling_state:') for r in board for k in r)}
    result['sources'] = ['https://docs.kernel.org/admin-guide/pm/cpufreq.html',
                         'https://docs.kernel.org/driver-api/thermal/sysfs-api.html',
                         'https://docs.kernel.org/cpu-freq/cpufreq-stats.html']
    write_csv(directory/'board.csv', board);write_csv(directory/'timeline.csv', merged)
    (directory/'summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    plot(directory/'timeline.png', merged, metadata)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument('directory', type=Path)
    print(json.dumps(analyze(parser.parse_args().directory), ensure_ascii=False, indent=2))
