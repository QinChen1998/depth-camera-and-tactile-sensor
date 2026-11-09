"""Native16/H synthetic packet fixtures; run tools/verify_port.py. No devices."""
import hashlib
import json
import struct
import subprocess
import time

import numpy as np

VGA = 'unidepth_rgb672_vga_fp16_tof16'
SIZES = {VGA: (672, 504, 640, 480)}
REASON_BYTES = 1536


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(header, rgb, native):
    body = json.dumps(header, separators=(',', ':')).encode()
    return struct.pack('<III', len(body), len(rgb), len(native)) + body + rgb + native


def decode(data, depth_bytes):
    payload_bytes = depth_bytes + 2 * REASON_BYTES
    packets = []
    while data:
        if len(data) < 12:
            raise ValueError('Truncated result header')
        h, b, c = struct.unpack('<III', data[:12])
        if not 0 < h <= 65536 or b not in (0, depth_bytes) or c not in (0, payload_bytes) or len(data) < 12+h+b+c:
            raise ValueError('Invalid result length')
        packets.append((json.loads(data[12:12+h]), data[12+h:12+h+b], data[12+h+b:12+h+b+c]))
        data = data[12+h+b+c:]
    return packets


def metadata(index, fov, rotation, experiment, cols, rows, model, profile, epoch):
    width, height, _, _ = SIZES[model]
    stamp = 1_000_000_000 + index * 67_000_000
    result = dict(source_protocol='HDA1', source_transport='rtsp_h265', model='Native',
        source_rgb_width=2304, source_rgb_height=1296, rgb_width=width, rgb_height=height,
        dtof_present=True, work_mode='rgb_tof', frame_id=index, generation=epoch,
        host_decoded_monotonic_ns=time.monotonic_ns(), boot_id='12345678-1234-1234-1234-123456789abc',
        tof_sequence=index, tof_timestamp_ns=stamp, rgb_timestamp_ns=stamp-5_000_000,
        tof_reused=False, pair_delta_ms=5., pair_clock_uncertainty_ms=35.,
        pair_clock_source='rtcp_prft_board_realtime_monotonic', source_candidate_mode=14,
        source_engine='native-hd-capture-v1', generation_source='host_presentation_epoch', source_generation='a'*32,
        dtof_width=cols, dtof_height=rows, dtof_bytes=cols*rows*4,
        dtof_format='range_uq2_u16le_snr_u8_valid_u8', dtof_range_unit='0.25_mm',
        dtof_source='tmf8829_ipc_first_peak', dtof_coordinate='sensor_radial_native',
        dtof_fov_correction=fov, tof_warning_flags=0, rectify_map_sha256=profile['rectify_sha256'],
        assist_camera_profile=profile.get('camera_profile', 'standard'),
        assist_input_rotation_degrees=rotation, assist_experiment=experiment, assist_diagnostic_capture=True)
    if result['assist_camera_profile'] == 'wide':
        result.update(assist_output_policy='dtof_geometric_fov_v1', assist_tof_fov_correction=fov)
    if experiment == 'H':
        result['pairing_policy'] = 'latest_native_reuse_100ms_global_scale_v1'
    return result


def native_plane(root, cols, rows, fov, z=.6):
    # Independent reference from the unchanged source extrinsics, not a resized
    # M48 measurement. Each actual cell supplies exactly one radial sample.
    e = json.loads((root/'assets/camera/calibration_20261109/extrinsics.json').read_text())
    y, x = np.mgrid[:rows, :cols]
    q = np.stack(((x-cols/2+.5)/(.75*cols)-((fov&3)-1.5)/40,
                  (y-rows/2+.5)/rows-(((fov>>2)&3)-1.5)/40, np.ones_like(x)), -1)
    q /= np.linalg.norm(q, axis=-1, keepdims=True)
    rays = (q @ np.asarray(e['ray_mapping']).T) @ np.asarray(e['R']).T
    radial = np.rint((z-e['t_m'][2])/rays[..., 2]/.00025).astype('<u2').ravel()
    return b''.join(struct.pack('<HBB', int(value), 20, 1) for value in radial)


def exchange(executable, root, runtime, model, cases, environment, override=True):
    token = 'offline-native16-contract'
    command = [str(executable), '--rv-assist-worker', str(runtime), token]
    if override:
        command.append(model)
    wire = b''.join(encode(dict(protocol=1, type='infer', token=token, request=i,
        epoch=case['epoch'], host_model=model, metadata=case['metadata']), case['rgb'], case['native'])
        for i, case in enumerate(cases, 1))
    child = subprocess.Popen(command, cwd=root, env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        stdout, stderr = child.communicate(wire, timeout=240)
    except subprocess.TimeoutExpired:
        child.kill()
        child.communicate()
        raise
    _, _, width, height = SIZES[model]
    return child.returncode, decode(stdout, width * height * 4), stderr.decode(errors='replace')


def check_batch(executable, root, runtime, model, cases, environment, output):
    _, _, width, height = SIZES[model]
    depth_bytes = width * height * 4
    payload_bytes = depth_bytes + 2 * REASON_BYTES
    status, packets, stderr = exchange(executable, root, runtime, model, cases, environment)
    (output/f'{model}_stderr.log').write_text(stderr, encoding='utf-8')
    if status or len(packets) != len(cases)+1:
        raise AssertionError(f'{model}: exit {status}, packets {len(packets)}, expected {len(cases)+1}: {stderr[-500:]}')
    identity = json.loads((runtime/'model.json').read_text())
    backend=environment.get('RV_INFERENCE_BACKEND','tensorrt')
    expected_graph=identity['onnx_sha256']
    protected_manifest=json.loads((runtime/'runtime.json').read_text())
    if backend=='openvino:INT8':
        from pathlib import Path
        compatible=json.loads((Path(environment['RV_ASSIST_COMPAT_ROOT'])/'runtime.json').read_text(encoding='utf-8-sig'))
        assert compatible['source_graph_sha256']==identity['onnx_sha256']
        assert compatible['int8']['sha256']=='7dc50150c210b75e72fac35b45967ef874d85e9a883040223dbd5ebd4e01d63d'
        expected_graph=compatible['int8']['sha256']
        protected_manifest=compatible['int8']
    ready = packets[0][0]
    assert ready['type'] == 'ready' and ready['host_model'] == model
    assert ready['graph_sha256'] == expected_graph and ready['weights_sha256'] == identity['weights_sha256']
    protection = protected_manifest.get('model_protection')
    loading = ready.get('model_loading', {})
    if protection:
        assert loading.get('kind') == 'aes256-gcm-memory-v1', loading
        assert loading.get('plaintext_sha256_verified') is True, loading
        assert loading.get('plaintext_file_used') is False, loading
    else:
        assert loading.get('kind') == 'plaintext-file' and loading.get('plaintext_file_used') is True, loading
    rows, fingerprints = [], {}
    for request, (case, (header, depth, diagnostic)) in enumerate(zip(cases, packets[1:]), 1):
        assert header['request'] == request and header['epoch'] == case['epoch'] and header['token'] == 'offline-native16-contract'
        accepted = bool(depth)
        assert accepted == case.get('accepted', True), (case['name'], header.get('error'))
        result = dict(case=case['name'], passed=True, accepted=accepted)
        if accepted:
            m = header['metadata']
            assert (m['width'], m['height']) == (width, height)
            z = np.frombuffer(depth, '<f4')
            assert np.isfinite(z).all() and (z >= 0).all() and (z > 0).any()
            assert len(diagnostic) == payload_bytes
            mono = np.frombuffer(diagnostic[:depth_bytes], '<f4')
            assert np.isfinite(mono).all() and (mono > 0).all()
            assert m['assist_host_model'] == model and m['assist_graph_sha256'] == expected_graph
            if 'assist_execution_device' in m:
                assert m['assist_execution_device']==backend
            assert m['assist_input_orientation_checked'] is True and m['assist_output_restored_to_calibrated_grid'] is True
            assert m['assist_input_rotation_degrees'] == case['metadata']['assist_input_rotation_degrees']
            for key in ('dtof_present', 'tof_sequence', 'tof_timestamp_ns', 'rgb_timestamp_ns', 'pair_delta_ms',
                        'tof_reused', 'dtof_width', 'dtof_height', 'dtof_bytes'):
                assert m.get(key) == case['metadata'].get(key), (case['name'], key)
            fit = m['assist_fusion']
            n = 256
            for key in ('used_native_indices', 'global_used_native_indices', 'local_used_native_indices'):
                assert all(type(i) is int and 0 <= i < n for i in fit.get(key, []))
            if fit.get('dtof_corrected'):
                assert fit.get('tof_total_zones') == n
            assert m['assist_diagnostic_native_count'] == 256
            reasons = np.frombuffer(diagnostic[depth_bytes:], np.uint8).reshape(2, REASON_BYTES)
            assert not reasons[:, 256:].any()
            if case.get('uncorrected'):
                assert fit.get('dtof_corrected') is False and not fit.get('used_native_indices')
            fingerprints[case['name']] = dict(depth=digest(depth), diagnostic=digest(diagnostic), mono=digest(diagnostic[:depth_bytes]))
            result.update(fusion=fit.get('fusion_mode'), adopted=len(fit.get('used_native_indices', [])),
                          native_measurements=n if m['dtof_present'] else 0, **fingerprints[case['name']])
        else:
            assert header.get('error') and not diagnostic
            result['error'] = header['error']
        rows.append(result)
    return dict(passed=True, ready=ready, cases=rows), fingerprints
