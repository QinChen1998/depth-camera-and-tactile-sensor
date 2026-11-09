"""Offline host-backend experiment. Synthetic tensors only; no application/device startup.

Reports model-forward timing, not live-camera or end-to-end GUI FPS. The original
ONNX is read only. This probe only covers the current RGB672/VGA UniDepth contract.
"""
import argparse
from collections import Counter
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import time
import traceback

import numpy as np
import psutil


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def hardware():
    result = dict(platform=platform.platform(), logical_cpus=psutil.cpu_count(),
                  physical_cores=psutil.cpu_count(logical=False),
                  memory_bytes=psutil.virtual_memory().total)
    if os.name == 'nt':
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                           r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            result['cpu'] = winreg.QueryValueEx(key, 'ProcessorNameString')[0].strip()
        names = []
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                           r'SYSTEM\CurrentControlSet\Control\Video') as key:
            for i in range(winreg.QueryInfoKey(key)[0]):
                try:
                    with winreg.OpenKey(key, winreg.EnumKey(key, i) + r'\0000') as child:
                        names.append(winreg.QueryValueEx(child, 'DriverDesc')[0])
                except OSError:
                    pass
        result['registered_display_adapters'] = sorted(set(names))
    versions = {}
    for package in ('numpy', 'onnx', 'onnxruntime', 'onnxruntime-directml', 'openvino',
                    'onnxruntime-ep-openvino', 'psutil'):
        try:
            versions[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            pass
    result['packages'] = versions
    return result


def make_cases(inputs):
    """Deterministic NCHW; rotate input and restore prediction independently of display."""
    by_name = {v['name']: v for v in inputs}
    shape = by_name['rgb']['shape']
    if shape != [1, 3, 504, 672] or set(by_name) != {'rgb'}:
        raise ValueError('Probe only supports the current [1,3,504,672] UniDepth RGB contract')
    if by_name['rgb']['dtype'] != 'uint8':
        raise ValueError('Current UniDepth requires uint8 RGB')
    height, width = shape[-2:]
    y, x = np.mgrid[:height, :width]
    cases = []
    for variant in range(3):
        rgb = np.stack(((x * 3 + y + variant * 47) % 256,
                        (y * 5 + variant * 73) % 256,
                        (x + y * 2 + variant * 29) % 256))[None].astype(np.uint8)
        feed = {'rgb': rgb}
        if set(feed) != set(by_name):
            raise ValueError('Unsupported input contract: ' + str(sorted(by_name)))
        for rotation in (0, 180):
            oriented = {k: np.ascontiguousarray(v if rotation == 0 else v[..., ::-1, ::-1])
                        for k, v in feed.items()}
            hashes = {k: hashlib.sha256(v.tobytes()).hexdigest() for k, v in oriented.items()}
            cases.append(dict(name=f'image{variant}_r{rotation}', rotation=rotation,
                              inputs=oriented, input_sha256=hashes))
    return cases


def create_backend(args, report):
    begin = time.perf_counter()
    if args.backend in ('cpu', 'directml', 'openvino-ep-cpu'):
        import onnxruntime as ort
        options = ort.SessionOptions()
        options.intra_op_num_threads = args.threads
        options.inter_op_num_threads = 1
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        options.enable_mem_pattern = args.backend != 'directml'
        options.enable_profiling = True
        options.profile_file_prefix = str(args.output / 'ort-profile')
        options.log_severity_level = 3
        providers = ['CPUExecutionProvider']
        if args.backend == 'directml':
            if 'DmlExecutionProvider' not in ort.get_available_providers():
                raise RuntimeError('DirectML execution provider is unavailable')
            providers = [('DmlExecutionProvider', {'device_id': args.device_id}), 'CPUExecutionProvider']
        if args.backend == 'openvino-ep-cpu':
            import onnxruntime_ep_openvino as plugin
            ort.register_execution_provider_library('host_probe_openvino', plugin.get_library_path())
            devices = ort.get_ep_devices()
            report['ort_ep_devices'] = [dict(name=d.ep_name, metadata=d.ep_metadata) for d in devices]
            selected = [d for d in devices if d.ep_name == plugin.get_ep_name()
                        and d.ep_metadata.get('ov_device') == 'CPU']
            if not selected:
                raise RuntimeError('OpenVINO plugin did not enumerate a CPU device')
            config = {'CPU': {'PERFORMANCE_HINT': 'LATENCY', 'NUM_STREAMS': '1',
                              'INFERENCE_NUM_THREADS': str(args.threads),
                              'INFERENCE_PRECISION_HINT': 'f32'}}
            options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
            options.add_provider_for_devices(selected[:1], {'load_config': json.dumps(config)})
            report['openvino_plugin_config'] = config
            report['ort_graph_optimization'] = 'disabled; OpenVINO optimizes the standard ONNX subgraph'
        model_input = args.model.read_bytes() if args.memory_model else str(args.model)
        if args.backend == 'openvino-ep-cpu':
            # Passing providers= here overrides add_provider_for_devices().
            session = ort.InferenceSession(model_input, sess_options=options)
        else:
            session = ort.InferenceSession(model_input, sess_options=options, providers=providers)
        report['model_input_kind'] = 'memory_bytes' if args.memory_model else 'file'
        del model_input
        session.disable_fallback()
        report['providers'] = session.get_providers()
        if args.backend == 'directml' and 'DmlExecutionProvider' not in report['providers']:
            raise RuntimeError('DirectML initialization fell back to another provider')
        names = [v.name for v in session.get_outputs()]
        inputs = [dict(name=v.name, shape=v.shape, dtype={'tensor(uint8)': 'uint8',
                       'tensor(float)': 'float32', 'tensor(bool)': 'bool'}.get(v.type, v.type))
                  for v in session.get_inputs()]

        def infer(feed):
            return dict(zip(names, session.run(names, feed)))

        def stop_profile():
            filename = session.end_profiling()
            events = json.loads(Path(filename).read_text(encoding='utf-8'))
            nodes = [v for v in events if v.get('cat') == 'Node' and v.get('args', {}).get('provider')]
            counts = Counter(v['args']['provider'] for v in nodes)
            duration = {}
            for v in nodes:
                p = v['args']['provider']
                duration[p] = duration.get(p, 0.) + v.get('dur', 0.)
            report['profile'] = dict(path=filename, kernel_events=dict(counts),
                                     kernel_time_us=duration,
                                     note='Profiling covers validation only; disabled during benchmark.')
            if not nodes:
                raise RuntimeError('No executed kernels found in the provider profile')
            if args.backend == 'cpu' and set(counts) != {'CPUExecutionProvider'}:
                raise RuntimeError('CPU-only test executed on another provider')
            if args.backend == 'directml' and not counts.get('DmlExecutionProvider'):
                raise RuntimeError('No DirectML kernels executed; cannot call this a GPU test')
            if args.backend == 'openvino-ep-cpu' and not counts.get('OpenVINOExecutionProvider'):
                raise RuntimeError('No OpenVINO plugin kernels executed')
        execution = f'DirectML adapter index {args.device_id}' if args.backend == 'directml' else 'CPU'
    else:
        if args.memory_model:
            raise ValueError('--memory-model is for ONNX Runtime backends')
        import openvino as ov
        core = ov.Core()
        report['openvino_available_devices'] = list(core.available_devices)
        report['openvino_device_inventory'] = {d: core.get_property(d, 'FULL_DEVICE_NAME')
                                              for d in core.available_devices}
        device = 'CPU' if args.backend == 'openvino-cpu' else args.gpu_device
        if not any(v == device or (device == 'GPU' and v.startswith('GPU')) for v in core.available_devices):
            report['status'] = 'unavailable'
            raise RuntimeError(f'No OpenVINO {device} device; detected {core.available_devices}')
        config = {'PERFORMANCE_HINT': 'LATENCY', 'NUM_STREAMS': '1'}
        if args.precision != 'default':
            config['INFERENCE_PRECISION_HINT'] = args.precision
        if device == 'CPU':
            config['INFERENCE_NUM_THREADS'] = args.threads
        model = core.read_model(str(args.model))
        report['openvino_device_name'] = core.get_property(device, 'FULL_DEVICE_NAME')
        if len(model.outputs) != 1 or 'depth' not in model.outputs[0].get_names():
            raise ValueError('Expected the current UniDepth depth output')
        compiled = core.compile_model(model, device, config)
        report['openvino_execution_devices'] = list(compiled.get_property('EXECUTION_DEVICES'))
        if device == 'CPU' and report['openvino_execution_devices'] != ['CPU']:
            raise RuntimeError('OpenVINO CPU test selected a different execution device')
        if device == 'CPU':
            report['openvino_effective_threads'] = compiled.get_property('INFERENCE_NUM_THREADS')
            report['openvino_effective_streams'] = str(compiled.get_property('NUM_STREAMS'))
        report['openvino_precision'] = str(compiled.get_property('INFERENCE_PRECISION_HINT'))
        report['openvino_runtime_types'] = dict(Counter(
            str(node.get_rt_info()['runtimePrecision']) for node in compiled.get_runtime_model().get_ops()
            if 'runtimePrecision' in node.get_rt_info()))
        request = compiled.create_infer_request()
        inputs = [dict(name=v.get_any_name(), shape=list(v.get_shape()),
                       dtype={'u8': 'uint8', 'f32': 'float32', 'boolean': 'bool'}.get(
                           v.get_element_type().get_type_name(), v.get_element_type().get_type_name()))
                  for v in compiled.inputs]

        def infer(feed):
            request.infer(feed)
            # OpenVINO retains both the public name and internal Cast aliases;
            # get_any_name() may choose an alias. Keep the ONNX public contract.
            return {'depth': request.get_output_tensor(0).data.copy()}

        def stop_profile():
            pass
        execution = device
    report.update(session_create_ms=(time.perf_counter() - begin) * 1000,
                  execution_device=execution, inputs=inputs)
    return infer, stop_profile, inputs


def compare_predictions(actual, reference):
    rows = []
    for key, value in actual.items():
        if key not in reference or reference[key].shape != value.shape:
            raise ValueError('Reference output missing or shape mismatch: ' + key)
        expected = reference[key].astype(np.float64)
        delta = np.abs(value.astype(np.float64) - expected)
        mask = np.abs(expected) > 1e-6
        relative = delta[mask] / np.abs(expected[mask])
        rows.append(dict(key=key, mean_abs_m=float(delta.mean()), max_abs_m=float(delta.max()),
                         mean_relative=float(relative.mean()) if relative.size else 0.,
                         max_relative=float(relative.max()) if relative.size else 0.,
                         zero_mask_equal=bool(np.array_equal(value == 0, expected == 0))))
    return rows


def run(args, report):
    infer, stop_profile, inputs = create_backend(args, report)
    cases = make_cases(inputs)
    report['cases'] = [{k: v for k, v in c.items() if k != 'inputs'} for c in cases]
    predictions = {}
    for case in cases:
        output = infer(case['inputs'])
        if set(output) != {'depth'} or output['depth'].shape != (1, 1, 480, 640):
            raise ValueError('Current UniDepth requires depth [1,1,480,640]')
        for name, value in output.items():
            if not np.isfinite(value).all():
                raise ValueError('Nonfinite output: ' + case['name'] + '/' + name)
            if name == 'depth' and not (value > 0).all():
                raise ValueError('UniDepth prediction is not strictly positive')
            restored = value if case['rotation'] == 0 else value[..., ::-1, ::-1]
            predictions[case['name'] + '__' + name] = np.ascontiguousarray(restored)
    repeated = infer(cases[0]['inputs'])
    for name, value in repeated.items():
        if not np.array_equal(value, predictions[cases[0]['name'] + '__' + name]):
            raise ValueError('A-B-A input sequence changed the repeated A result')
    # A different orientation is not counted as new content: compare distinct patterns.
    names = list(repeated)
    if all(np.array_equal(predictions['image0_r0__' + name], predictions['image1_r0__' + name]) for name in names):
        raise ValueError('Different inputs produced identical predictions')
    stop_profile()
    np.savez(args.output / 'predictions.npz', **predictions)
    report['output_shapes'] = {k: list(v.shape) for k, v in repeated.items()}
    report['validation'] = dict(cases=len(cases), finite=True, input_output_orientation_restored=True,
                                new_content_changes_output=True, repeated_input_identical=True,
                                numerical_comparison_performed=bool(args.reference))
    if args.reference:
        with np.load(args.reference, allow_pickle=False) as reference:
            report['comparison'] = compare_predictions(predictions, reference)
        report['reference_sha256'] = digest(args.reference)
        report['numerical_gate'] = dict(mean_relative_limit=args.mean_relative_limit,
                                       max_relative_limit=args.max_relative_limit,
                                       passed=all(r['mean_relative'] <= args.mean_relative_limit and
                                                  r['max_relative'] <= args.max_relative_limit
                                                  for r in report['comparison']),
                                       physical_accuracy_verified=False)
    for i in range(args.warmup):
        infer(cases[i % len(cases)]['inputs'])
    process = psutil.Process()
    before = process.cpu_times()
    wall = time.perf_counter()
    samples = []
    peak_rss = process.memory_info().rss
    while len(samples) < args.max_iterations and (len(samples) < args.min_iterations or time.perf_counter() - wall < args.seconds):
        feed = cases[len(samples) % len(cases)]['inputs']
        begin = time.perf_counter()
        infer(feed)
        samples.append((time.perf_counter() - begin) * 1000)
        peak_rss = max(peak_rss, process.memory_info().rss)
    elapsed = time.perf_counter() - wall
    after = process.cpu_times()
    memory = process.memory_info()
    report['benchmark'] = dict(iterations=len(samples), elapsed_seconds=elapsed,
        mean_ms=float(np.mean(samples)), median_ms=float(np.median(samples)),
        p95_ms=float(np.percentile(samples, 95)), min_ms=min(samples), max_ms=max(samples),
        forward_fps=1000 / float(np.mean(samples)), loop_fps=len(samples) / elapsed,
        cpu_percent_one_logical_core_is_100=100 * ((after.user + after.system) - (before.user + before.system)) / elapsed,
        observed_peak_rss_mib=peak_rss / 2**20,
        process_lifetime_peak_working_set_mib=getattr(memory, 'peak_wset', memory.rss) / 2**20,
        samples_ms=samples, scope='Prepared synthetic input -> model -> host output; no capture, H fusion, IPC or GUI')
    report['status'] = 'passed' if report.get('numerical_gate', {}).get('passed', True) else 'numerical_mismatch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--backend', choices=('cpu', 'directml', 'openvino-cpu', 'openvino-gpu',
                                            'openvino-ep-cpu'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--min-iterations', type=int, default=8)
    parser.add_argument('--max-iterations', type=int, default=300)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--device-id', type=int, default=0)
    parser.add_argument('--gpu-device', default='GPU')
    parser.add_argument('--precision', choices=('default', 'f32', 'f16', 'bf16'), default='f32')
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--memory-model', action='store_true', help='ORT creates its session from model bytes')
    parser.add_argument('--mean-relative-limit', type=float, default=.0025)
    parser.add_argument('--max-relative-limit', type=float, default=.01)
    args = parser.parse_args()
    if args.threads < 1 or args.seconds <= 0 or not 0 < args.min_iterations <= args.max_iterations or args.warmup < 0:
        parser.error('Invalid thread count or benchmark bounds')
    args.model = args.model.resolve()
    args.output = args.output.resolve()
    if args.output == args.model.parent or args.model.is_relative_to(args.output):
        parser.error('Output must not contain or replace the source model')
    args.output.mkdir(parents=True, exist_ok=False)
    original_hash = digest(args.model)
    report = dict(schema=1, status='failed', backend=args.backend, model=str(args.model),
                  model_sha256=original_hash, hardware=hardware(), threads=args.threads,
                  hardware_capture_started=False, synthetic_only=True,
                  physical_accuracy_verified=False, product_integration_verified=False)
    try:
        run(args, report)
    except Exception as error:
        report['error'] = str(error)
        (args.output / 'exception.txt').write_text(traceback.format_exc(), encoding='utf-8')
    report['source_model_unchanged'] = digest(args.model) == original_hash
    if not report['source_model_unchanged']:
        report['status'] = 'failed'
        report['error'] = 'Source model changed during experiment'
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    compact = {k: v for k, v in report.items() if k in
               ('status', 'backend', 'model', 'error', 'session_create_ms', 'numerical_gate')}
    if 'benchmark' in report:
        compact['benchmark'] = {k: v for k, v in report['benchmark'].items()
                                if k in ('iterations', 'mean_ms', 'p95_ms', 'forward_fps', 'observed_peak_rss_mib')}
    print(json.dumps(compact), flush=True)
    return 0 if report['status'] == 'passed' else 2


if __name__ == '__main__':
    raise SystemExit(main())
