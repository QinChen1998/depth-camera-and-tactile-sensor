"""Synthetic native16/legacy inference verification. Never contacts a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

import numpy as np

ALIAS = 'unidepth_wide_576_fp16_tof16'
BASE = 'unidepth_wide_576_fp16'
PIXELS = 576 * 324
DEPTH_BYTES = PIXELS * 4
REASON_BYTES = 1536
PAYLOAD_BYTES = DEPTH_BYTES + 2 * REASON_BYTES


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(header, rgb, native):
    body = json.dumps(header, separators=(',', ':')).encode()
    return struct.pack('<III', len(body), len(rgb), len(native)) + body + rgb + native


def decode(data):
    packets = []
    while data:
        if len(data) < 12:
            raise ValueError('Truncated result header')
        h, b, c = struct.unpack('<III', data[:12])
        if not 0 < h <= 65536 or b not in (0, DEPTH_BYTES) or c not in (0, PAYLOAD_BYTES) or len(data) < 12+h+b+c:
            raise ValueError('Invalid result length')
        packets.append((json.loads(data[12:12+h]), data[12+h:12+h+b], data[12+h+b:12+h+b+c]))
        data = data[12+h+b+c:]
    return packets


def metadata(index, fov, rotation, experiment, cols, rows, model, profile, epoch):
    stamp = 1_000_000_000 + index * 67_000_000
    result = dict(source_protocol='HDA1', source_transport='rtsp_h265', model='Native',
        source_rgb_width=2304, source_rgb_height=1296, rgb_width=576, rgb_height=324,
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
    e = json.loads((root/'assets/camera/extrinsics.json').read_text())
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
    if model == ALIAS and override:
        command.append(ALIAS)
    wire = b''.join(encode(dict(protocol=1, type='infer', token=token, request=i,
        epoch=case['epoch'], host_model=model, metadata=case['metadata']), case['rgb'], case['native'])
        for i, case in enumerate(cases, 1))
    child = subprocess.Popen(command, cwd=root, env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = child.communicate(wire, timeout=240)
    return child.returncode, decode(stdout), stderr.decode(errors='replace')


def check_batch(executable, root, runtime, model, cases, environment, output):
    status, packets, stderr = exchange(executable, root, runtime, model, cases, environment)
    (output/f'{model}_stderr.log').write_text(stderr, encoding='utf-8')
    if status or len(packets) != len(cases)+1:
        raise AssertionError(f'{model}: exit {status}, packets {len(packets)}, expected {len(cases)+1}: {stderr[-500:]}')
    identity = json.loads((runtime/'model.json').read_text())
    ready = packets[0][0]
    assert ready['type'] == 'ready' and ready['host_model'] == model
    assert ready['graph_sha256'] == identity['onnx_sha256'] and ready['weights_sha256'] == identity['weights_sha256']
    rows, fingerprints = [], {}
    for request, (case, (header, depth, diagnostic)) in enumerate(zip(cases, packets[1:]), 1):
        assert header['request'] == request and header['epoch'] == case['epoch'] and header['token'] == 'offline-native16-contract'
        accepted = bool(depth)
        assert accepted == case.get('accepted', True), (case['name'], header.get('error'))
        result = dict(case=case['name'], passed=True, accepted=accepted)
        if accepted:
            m = header['metadata']
            z = np.frombuffer(depth, '<f4')
            assert np.isfinite(z).all() and (z >= 0).all() and (z > 0).any()
            assert len(diagnostic) == PAYLOAD_BYTES
            mono = np.frombuffer(diagnostic[:DEPTH_BYTES], '<f4')
            assert np.isfinite(mono).all() and (mono > 0).all()
            assert m['assist_host_model'] == model and m['assist_graph_sha256'] == identity['onnx_sha256']
            assert m['assist_input_orientation_checked'] is True and m['assist_output_restored_to_calibrated_grid'] is True
            assert m['assist_input_rotation_degrees'] == case['metadata']['assist_input_rotation_degrees']
            for key in ('dtof_present', 'tof_sequence', 'tof_timestamp_ns', 'rgb_timestamp_ns', 'pair_delta_ms',
                        'tof_reused', 'dtof_width', 'dtof_height', 'dtof_bytes'):
                assert m.get(key) == case['metadata'].get(key), (case['name'], key)
            fit = m['assist_fusion']
            n = 256 if model == ALIAS else 1536
            for key in ('used_native_indices', 'global_used_native_indices', 'local_used_native_indices'):
                assert all(type(i) is int and 0 <= i < n for i in fit.get(key, []))
            if fit.get('dtof_corrected'):
                assert fit.get('tof_total_zones') == n
            if model == ALIAS:
                assert m['assist_diagnostic_native_count'] == 256
                reasons = np.frombuffer(diagnostic[DEPTH_BYTES:], np.uint8).reshape(2, REASON_BYTES)
                assert not reasons[:, 256:].any()
            if case.get('uncorrected'):
                assert fit.get('dtof_corrected') is False and not fit.get('used_native_indices')
            fingerprints[case['name']] = dict(depth=digest(depth), diagnostic=digest(diagnostic), mono=digest(diagnostic[:DEPTH_BYTES]))
            result.update(fusion=fit.get('fusion_mode'), adopted=len(fit.get('used_native_indices', [])),
                          native_measurements=n if m['dtof_present'] else 0, **fingerprints[case['name']])
        else:
            assert header.get('error') and not diagnostic
            result['error'] = header['error']
        rows.append(result)
    return dict(passed=True, ready=ready, cases=rows), fingerprints


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--reference-executable', type=Path)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--qt-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    env = {k:v for k,v in os.environ.items() if not k.startswith(('PYTHON', 'CONDA', 'RV_'))}
    env.update(PATH=str(a.qt_root/'bin')+';'+os.environ.get('SystemRoot', 'C:/Windows')+'/System32',
               QT_PLUGIN_PATH=str(a.qt_root/'plugins'), RV_TEST_OFFLINE_ONLY='1')
    y, x = np.mgrid[:324, :576]
    rgb = np.stack(((x*3+y)%256, (y*5)%256, (x+y*2)%256)).astype(np.uint8).tobytes()
    runtime = a.root/'UniDepthRuntimeWide576FP16'
    profile = json.loads((runtime/'model.json').read_text())
    cases = []
    def add(name, m, native=None, accepted=True, epoch=None, **extra):
        index = len(cases)+1
        cases.append(dict(name=name, metadata=dict(m, frame_id=index), native=native if native is not None else native_plane(a.root,16,16,m['dtof_fov_correction']),
                          rgb=rgb, accepted=accepted, epoch=epoch if epoch is not None else index, **extra))
    for fov in (1, 2):
        for rotation in (0, 180):
            for experiment in ('A', 'B', 'G', 'H'):
                i = len(cases)+1
                add(f'f{fov}_r{rotation}_{experiment}', metadata(i,fov,rotation,experiment,16,16,ALIAS,profile,i))
    for experiment in ('G', 'H'):
        i = len(cases)+1
        m = metadata(i,2,0,experiment,16,16,ALIAS,profile,i)
        for key in ('tof_sequence','tof_timestamp_ns','rgb_timestamp_ns','tof_reused','pair_delta_ms',
                    'dtof_width','dtof_height','dtof_bytes','dtof_format','dtof_range_unit','dtof_source','dtof_coordinate',
                    'dtof_fov_correction','tof_warning_flags','pair_clock_uncertainty_ms','pair_clock_source'):
            m.pop(key,None)
        m.update(dtof_present=False,work_mode='host_rgb')
        add('rgb_only_'+experiment,m,b'',uncorrected=True)
    i = len(cases)+1
    m = metadata(i,2,0,'H',16,16,ALIAS,profile,100)
    native = native_plane(a.root,16,16,2)
    add('H_first_native',m,native,epoch=100)
    for delta in (20.,50.,100.,-100.,100.001,-100.001):
        reused=dict(m,tof_reused=True,pair_delta_ms=delta,rgb_timestamp_ns=m['tof_timestamp_ns']-round(delta*1e6))
        add('H_reuse_'+str(delta),reused,native,abs(delta)<=100.,epoch=100)
    reused=dict(m,tof_reused=True,pair_delta_ms=50.,rgb_timestamp_ns=m['tof_timestamp_ns']-50_000_000)
    add('H_changed_bytes',reused,bytes([native[0]^1])+native[1:],False,epoch=100)
    add('H_false_reuse_flag',dict(reused,tof_reused=False),native,False,epoch=100)
    for fault in ('wrong_grid','missing_fov','conflicting_fov','bad_rotation','invalid_bit','low_snr'):
        i=len(cases)+1; m=metadata(i,2,0,'G',16,16,ALIAS,profile,i+100)
        native=native_plane(a.root,16,16,2)
        if fault=='wrong_grid':m.update(dtof_width=48,dtof_height=32,dtof_bytes=6144);native=native_plane(a.root,48,32,2)
        if fault=='missing_fov':m.pop('dtof_fov_correction')
        if fault=='conflicting_fov':m['tof_fov_correction']=1
        if fault=='bad_rotation':m['assist_input_rotation_degrees']=90
        if fault=='invalid_bit':native=native[:3]+b'\x02'+native[4:]
        if fault=='low_snr':native=b''.join(native[j:j+2]+b'\x05'+native[j+3:j+4] for j in range(0,len(native),4))
        add(fault,m,native,fault=='low_snr',epoch=i+100,uncorrected=fault=='low_snr')
    report, fingerprints = check_batch(a.executable,a.root,runtime,ALIAS,cases,env,a.output)
    _, restarted = check_batch(a.executable,a.root,runtime,ALIAS,cases[:1],env,a.output)
    assert restarted[cases[0]['name']] == fingerprints[cases[0]['name']], 'Restart changed the same native16 input'
    # Explicit alias binding cannot be acquired by sending a request to a
    # normal FP16 worker, and cannot bind a different graph/runtime package.
    code, packets, _ = exchange(a.executable,a.root,runtime,ALIAS,[cases[0]],env,override=False)
    alias_rejected = code != 0 or not packets[-1][1]
    code, packets, _ = exchange(a.executable,a.root,a.root/'UniDepthRuntime',ALIAS,[],env)
    wrong_runtime_rejected = code != 0 and packets[0][0].get('type') == 'fatal'
    assert alias_rejected and wrong_runtime_rejected
    parity=[]
    for model, folder in ((BASE,'UniDepthRuntimeWide576FP16'), ('unidepth','UniDepthRuntime'),
                           ('unidepth_wide','UniDepthWideRuntime'),('unidepth_wide_576','UniDepthRuntimeWide576')):
        if not a.reference_executable and model != BASE:
            continue
        path=a.root/folder; identity=json.loads((path/'model.json').read_text()); legacy=[]
        for fov in (1,2):
            for rotation in (0,180):
                for experiment in ('A','B','G','H'):
                    i=len(legacy)+1
                    legacy.append(dict(name=f'f{fov}_r{rotation}_{experiment}',metadata=metadata(i,fov,rotation,experiment,48,32,model,identity,i),
                                       native=native_plane(a.root,48,32,fov),rgb=rgb,epoch=i))
        current, hashes=check_batch(a.executable,a.root,path,model,legacy,env,a.output)
        if model==BASE:
            for name, hashes48 in hashes.items():
                assert hashes48['mono']==fingerprints[name]['mono'], ('Model tensor/restoration changed with ToF grid',name)
        if a.reference_executable:
            reference, before=check_batch(a.reference_executable,a.root,path,model,legacy,env,a.output)
            assert before==hashes, ('Original 48x32 depth or diagnostic changed',model)
            parity.append(dict(model=model,passed=True,bitwise_outputs=len(hashes)))
    result=dict(passed=True, native16=report, legacy_bitwise_parity=parity,
        original_fp16_mono_identical_to_native16=True, restart_identical=True, alias_without_override_rejected=alias_rejected,
        incompatible_runtime_rejected=wrong_runtime_rejected, real_device_connected=False,
        physical_accuracy_verified=False, executable_sha256=digest(a.executable.read_bytes()))
    (a.output/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(dict(passed=True,native16_cases=len(cases),legacy_identical_outputs=sum(r['bitwise_outputs'] for r in parity),output=str(a.output))))


if __name__=='__main__':
    main()
