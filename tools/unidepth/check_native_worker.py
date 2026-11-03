"""Developer-only end-to-end native test using synthetic RGB and a 2 m plane.

Python only sends/validates test packets. The EXE performs model inference and
fusion. This test does not open a camera or connect to the board.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import uuid
import numpy as np

FP32_SHA = '46488b81c0fbf75c6277fd9d4da5d29bbf7dcd6f499a67a87ff7e7aa33594368'
FP16_SHA = '5b2df8c511708e890d5922ed31fd552aa2526e4a82a76173b3fe0c9faa5f328b'


def parity_difference(actual, reference, mean_limit=.001, max_limit=.01):
    relative = np.abs(actual - reference) / np.maximum(np.abs(reference), 1e-6)
    return dict(mean_relative_error=float(relative.mean()), max_relative_error=float(relative.max()),
                within_existing_parity_tolerance=bool(relative.mean() < .001 and relative.max() < .01),
                mean_limit=mean_limit, max_limit=max_limit,
                within_profile_parity_tolerance=bool(relative.mean() < mean_limit and relative.max() < max_limit))


def check_rejected_contracts(executable, runtime, env, directory, fp32_runtime, source_sha):
    model = json.loads((runtime / 'model.json').read_text(encoding='utf-8-sig'))
    manifest = json.loads((runtime / 'runtime.json').read_text(encoding='utf-8-sig'))
    checks = []
    wrong_precision = 'fp16-mixed' if model['precision'] == 'fp32' else 'fp32'
    cases = [('wrong_precision', 'precision', wrong_precision, True), ('missing_precision', 'precision', None, True),
             ('wrong_source', 'source_onnx_sha256', 'f' * 64, True), ('wrong_conversion', 'conversion', 'unknown', True),
             ('wrong_graph', 'onnx_sha256', '0' * 64, True),
             ('wrong_camera_profile', 'camera_profile', 'standard' if model.get('camera_profile') == 'wide' else 'wide', True),
             ('wrong_virtual_intrinsics', 'rectified_intrinsics_integer_centres', [1,1,287.5,161.5], True),
             ('old_camera_binding', 'rectify_sha256', '681b622d8e0ee2bc84b63b089c8da294c7f68e2ea2516878ada68803820c46b3', True),
             ('manifest_tamper', 'precision', wrong_precision, False)]
    if model.get('network_profile') == 'rgb576_pad6_v1':
        cases += [('missing_network_profile', 'network_profile', None, True),
                  ('wrong_padding', 'rgb_padding_ltrb', [0, 0, 0, 0], True),
                  ('wrong_crop', 'output_crop_ltrb', [0, 0, 0, 0], True),
                  ('wrong_network_shape', 'network_shape', [574, 1008], True),
                  ('wrong_resolution_level', 'resolution_level', 9, True)]
    if model.get('provisional') is True:
        cases += [('missing_provisional', 'provisional', None, True), ('false_provisional', 'provisional', False, True),
                  ('unapproved_mean_limit', 'candidate_parity_mean_limit', .01, True),
                  ('unapproved_max_limit', 'candidate_parity_max_limit', .1, True)]
    for name, field, value, rehash in cases:
        root = directory / name
        root.mkdir(parents=True)
        # Only immutable payloads are hardlinked; each test has separate writable
        # model/manifest files and cannot mutate the source runtime.
        for relative in manifest['files']:
            if relative == 'model.json':
                continue
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(runtime / relative, target)
        altered = dict(model)
        if value is None:
            del altered[field]
        else:
            altered[field] = value
        data = (json.dumps(altered, indent=2)+'\n').encode()
        (root / 'model.json').write_bytes(data)
        closure = dict(manifest, files=dict(manifest['files']))
        if rehash:
            closure['files']['model.json'] = hashlib.sha256(data).hexdigest()
        (root / 'runtime.json').write_text(json.dumps(closure)+'\n', encoding='utf-8')
        report = root / 'rejection.json'
        result = subprocess.run([str(executable), '--rv-assist-runtime-test', str(root), str(report)],
                                env=env, capture_output=True, timeout=35, creationflags=subprocess.CREATE_NO_WINDOW)
        response = json.loads(report.read_text())
        expected = 'fingerprint mismatch' if not rehash else ('precision contract' if field in ('precision', 'source_onnx_sha256', 'conversion') else 'calibration binding mismatch')
        # RGB576 precision selects a distinct immutable graph/profile. A
        # changed/missing precision can therefore fail graph binding first.
        if rehash and field == 'precision' and model.get('provisional') is True:
            expected = 'calibration binding mismatch'
        if rehash and field in ('rgb_padding_ltrb', 'output_crop_ltrb'):
            expected = 'padding and output grid binding mismatch'
        if rehash and field in ('provisional', 'candidate_parity_mean_limit', 'candidate_parity_max_limit'):
            expected = 'temporary candidate contract mismatch'
        assert result.returncode == 2 and response['pass'] is False and expected in response['error'], response
        checks.append(dict(case=name, pass_=True, error=response['error']))
    if fp32_runtime:
        report = directory / 'fp32_rejection.json'
        result = subprocess.run([str(executable), '--rv-assist-runtime-test', str(fp32_runtime.resolve()), str(report)],
                                env=env, capture_output=True, timeout=35, creationflags=subprocess.CREATE_NO_WINDOW)
        response = json.loads(report.read_text())
        assert result.returncode == 2 and response['pass'] is False and 'calibration binding mismatch' in response['error'], response
        checks.append(dict(case='fp32_runtime_rejected_without_fallback', pass_=True, error=response['error']))
    return checks


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--executable", type=Path, required=True)
    p.add_argument("--runtime", type=Path, required=True)
    p.add_argument("--geometry", type=Path, required=True)
    p.add_argument("--qt-root", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--test-fallback", action="store_true", help="Alternate valid, missing and low-SNR ToF then recover")
    p.add_argument("--test-orientation", action="store_true", help="Verify per-channel rotation, native-grid output, scale reset and invalid orientation rejection")
    p.add_argument("--test-support", action="store_true", help="Verify current ToF output support, zero unknowns, retained numeric parity and policy rejection")
    p.add_argument("--test-support-review", action="store_true", help="Exercise bounded opt-in support image generation in the headless worker")
    p.add_argument("--test-fov", action="store_true", help="Verify continuous wide geometry across native validity changes and RGB-only frames")
    p.add_argument("--expected-precision", choices=('fp16-mixed', 'fp32', 'fp32-tf32', 'fp16-mixed-trial'), default='fp16-mixed',
                   help="FP32 is only for an explicitly preserved baseline executable, not a GUI fallback")
    p.add_argument("--benchmark-frames", type=int, default=0, help="After five warmups, time 60-600 synthetic frames")
    p.add_argument("--compare-depths", type=Path, help="FP32 synthetic .npz written by a prior benchmark")
    p.add_argument("--test-contract", action='store_true', help="Reject rehashed precision/source/conversion/binding mismatches before DLL load")
    p.add_argument("--fp32-runtime", type=Path, help="Inactive FP32 baseline package for fail-closed validation")
    p.add_argument('--camera-profile', choices=('standard','wide'), default='standard')
    p.add_argument('--rgb576-pad6', action='store_true', help='Validate the independent pad-only wide runtime')
    p.add_argument('--fp16-candidate', action='store_true', help='Use only the explicitly provisional RGB576 FP16 profile and its separate 0.25% mean gate')
    p.add_argument('--model-size', choices=('small',), default='small')
    p.add_argument('--test-experiment', action='store_true', help='Exercise B/G contracts and retired correction rejection')
    p.add_argument('--test-diagnostic', action='store_true', help='Validate bounded current-frame monocular/reason payloads without saving arrays')
    a = p.parse_args()
    parity_mean_limit, parity_max_limit = .001, .01
    if a.fp16_candidate and not a.rgb576_pad6:
        p.error('fp16-candidate requires rgb576-pad6')
    source_sha = FP32_SHA if a.camera_profile == 'standard' else '63941069172b0095fac8a70b1c387c02c2dca63f3b376906b2a0bd15a1e46c7e'
    graph_sha = FP16_SHA if a.camera_profile == 'standard' else '1ce04189bf527a6dc016ff9af4f61776d4a03c887a26301187ea25ecda87234a'
    map_sha = '00de89f4e6d95f09758b0d8274c2694168a27b5f601bdfeda667b0b179126b5d' if a.camera_profile == 'standard' else '0317f36006702df342d78a0ff19ed8250d58b91424b4b09f11327d81f31c6191'
    conversion = 'ort-fp16-cast-fixed-v1' if a.camera_profile == 'standard' else 'ort-fp16-cast-fixed-head-fp32-v1'
    host_model = 'unidepth' if a.camera_profile == 'standard' else 'unidepth_wide'
    if a.rgb576_pad6:
        precision = 'fp16-mixed-trial' if a.fp16_candidate else 'fp32-tf32'
        if a.camera_profile != 'wide' or a.expected_precision != precision:
            p.error('rgb576-pad6 requires wide and its explicitly selected precision')
        lock = json.loads(Path(__file__).with_name('rgb576_fp16_trial.lock.json' if a.fp16_candidate else 'rgb576.lock.json').read_text())
        source_sha, graph_sha = lock['source_onnx_sha256'], lock['onnx_sha256']
        conversion, host_model = lock['conversion'], 'unidepth_wide_576_fp16' if a.fp16_candidate else 'unidepth_wide_576'
        if a.fp16_candidate:
            parity_mean_limit, parity_max_limit = lock['candidate_parity_mean_limit'], lock['candidate_parity_max_limit']
    if a.benchmark_frames and not 60 <= a.benchmark_frames <= 600:
        p.error('benchmark-frames must be between 60 and 600')
    # Windows AddDllDirectory requires an absolute native runtime path.
    for field in ("executable", "runtime", "geometry", "qt_root", "report"):
        setattr(a, field, getattr(a, field).resolve())
    data = a.geometry.read_bytes()
    translation = np.frombuffer(data, "<f8", count=3, offset=84)
    rays = np.frombuffer(data, "<f8", count=1536*3, offset=124).reshape(1536, 3)
    ranges = np.round((2.0-translation[2])/rays[:, 2]/.00025).astype("<u2")
    packed = np.zeros((1536, 4), np.uint8)
    packed[:, :2] = ranges.view(np.uint8).reshape(1536, 2)
    packed[:, 2] = 20
    packed[:, 3] = 1
    rgb = (a.runtime / "parity_rgb.bin").read_bytes()
    token = str(uuid.uuid4())
    env = os.environ.copy()
    env["PATH"] = str(a.qt_root / "bin") + os.pathsep + env.get("PATH", "")
    review_directory = a.report.parent / (a.report.stem + '_review')
    if a.test_support_review:
        if not a.test_support:
            p.error('test-support-review requires test-support')
        review_directory.mkdir()
        env['RV_UNIDEPTH_SUPPORT_AUDIT'] = '1'
        env['RV_HOST_HD_REVIEW_DIRECTORY'] = str(review_directory)
    process = subprocess.Popen([str(a.executable), "--rv-assist-worker", str(a.runtime), token],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    def exact(n):
        out = bytearray()
        while len(out) < n:
            part = process.stdout.read(n-len(out))
            if not part:
                raise RuntimeError("Native worker exited before completing a packet")
            out.extend(part)
        return bytes(out)
    diagnostic_checks = []
    def packet():
        h, b, aux = struct.unpack("<III", exact(12))
        pixels = 576*324
        assert 0 < h <= 65536 and b in (0, pixels*4) and aux in (0, pixels*4+2*1536)
        response, depth, extra = json.loads(exact(h)), exact(b), exact(aux)
        meta = response.get('metadata', {})
        if b and meta.get('assist_diagnostic_capture') is True:
            assert a.test_diagnostic and meta['assist_diagnostic_payload'] == 'mono_reasons_v1'
            assert aux == pixels*4+2*1536
            mono = np.frombuffer(extra[:pixels*4], '<f4')
            reasons = np.frombuffer(extra[pixels*4:], np.uint8).reshape(2, 1536)
            assert np.all(np.isfinite(mono) & (mono > 0)) and np.all(reasons <= 11)
            fit = meta['assist_fusion']
            for group, key in enumerate(('global_used_native_indices', 'local_used_native_indices')):
                ids = fit.get(key, [])
                assert np.all(reasons[group, ids] == 9), (key, fit)
            diagnostic_checks.append(dict(frame=meta['frame_id'], mode=meta.get('assist_experiment', 'A'),
                rotation=meta['assist_input_rotation_degrees'], bytes=aux, reason_trace_matches_roles=True))
        else:
            assert aux == 0
        return response, depth
    executor = concurrent.futures.ThreadPoolExecutor(1)
    try:
        ready, _ = executor.submit(packet).result(35)
        assert ready["type"] == "ready" and ready["token"] == token, ready
        expected_graph = source_sha if a.expected_precision == 'fp32' else graph_sha
        assert ready['graph_sha256'] == expected_graph, ready
        if a.expected_precision in ('fp16-mixed', 'fp32-tf32', 'fp16-mixed-trial'):
            assert ready['precision'] == a.expected_precision and ready['source_graph_sha256'] == source_sha
            assert ready['conversion'] == conversion
        records = []
        model_reference = None
        fixture_difference = None
        for i in range(1, 9):
            measurements = packed.copy()
            fallback = a.test_fallback and i in (1, 3, 4, 5)
            held = fallback and i != 1
            if fallback:
                measurements[:, 3 if i != 4 else 2] = 0
            metadata = {"dtof_width":48,"dtof_height":32,"dtof_format":"range_uq2_u16le_snr_u8_valid_u8",
                        "dtof_range_unit":"0.25_mm","dtof_source":"tmf8829_ipc_first_peak","dtof_bytes":6144,
                        "dtof_coordinate":"sensor_radial_native","frame_id":i,"generation":7,"tof_sequence":i,
                        "tof_timestamp_ns":500000000+i*67000000,"tof_reused":False,"work_mode":"fusion",
                        "pair_delta_ms":0.0,"rectify_map_sha256":map_sha,
                        "rgb_width":576,"rgb_height":324,"width":144,"height":81,"model":"B0",
                        "simulation":True,"input_origin":"SIMULATED","assist_input_rotation_degrees":0}
            if a.test_diagnostic: metadata['assist_diagnostic_capture'] = True
            header = json.dumps({"protocol":1,"type":"infer","host_model":host_model,"token":token,"request":i,"epoch":3,"metadata":metadata}).encode()
            start = time.perf_counter()
            process.stdin.write(struct.pack("<III", len(header), len(rgb), measurements.nbytes)+header+rgb+measurements.tobytes())
            process.stdin.flush()
            response, depth = executor.submit(packet).result(2)
            assert response["type"] == "result" and response["token"] == token and response["request"] == i and response["epoch"] == 3
            assert not response.get("error"), response
            m = response["metadata"]
            assert m["frame_id"] == i and m["tof_sequence"] == i and m["generation"] == 7
            z = np.frombuffer(depth, "<f4")
            assert z.size == 576*324 and np.all(np.isfinite(z) & (z > 0))
            assert m['assist_graph_sha256'] == expected_graph
            if a.expected_precision in ('fp16-mixed', 'fp32-tf32', 'fp16-mixed-trial'):
                assert m['assist_precision'] == a.expected_precision and m['assist_source_graph_sha256'] == source_sha
                assert m['assist_conversion'] == conversion
            expected_modes = ("held_scale",) if held else (("monocular",) if fallback else ("global_scale", "scale_local"))
            assert m["assist_fusion"]["fusion_mode"] in expected_modes
            assert m["assist_fusion"]["dtof_corrected"] == (not fallback)
            expected_source = "host_unidepthv2_held_scale" if held else ("host_unidepthv2_monocular" if fallback else "host_unidepthv2_dtof")
            assert m["depth_source"] == expected_source
            if fallback:
                assert m["depth_method"] == ("monocular_held_dtof_scale" if held else "monocular_unadjusted")
                assert m["assist_calibration_status"] == ("historical_dtof_scale_only" if held else "monocular_not_dtof_corrected")
                if model_reference is None:
                    model_reference = z.copy()
                    fixture_difference = parity_difference(z, np.fromfile(a.runtime / 'parity_depth.bin', dtype='<f4'), parity_mean_limit, parity_max_limit)
                    assert fixture_difference['within_profile_parity_tolerance'], fixture_difference
                assert np.allclose(z, model_reference*m["assist_fusion"]["scale"], rtol=.001, atol=1e-5), "Fallback did not scale current monocular values"
                if held:
                    assert m["assist_fusion"]["scale"] == records[1]["fusion"]["scale"]
                    assert m["assist_fusion"]["scale_reference_frame_id"] == 2
            records.append({"frame":i,"inference_ms":m["assist_inference_ms"],"total_ms":m["assist_total_ms"],
                            "round_trip_ms":(time.perf_counter()-start)*1000,"fusion":m["assist_fusion"],
                            "simulated_plane_mae_m":float(np.mean(np.abs(z-2)))})
        orientations = []
        if a.test_orientation:
            reference = None
            rotated_rgb = np.frombuffer(rgb, np.uint8).reshape(3, 324, 576)[:, ::-1, ::-1].copy().tobytes()
            for i, (rotation, valid) in enumerate(((0, False), (180, False), (180, True), (0, False), (90, False), (0, False)), 9):
                metadata.update(frame_id=i, tof_sequence=i, tof_timestamp_ns=500000000+i*67000000,
                                assist_input_rotation_degrees=rotation)
                payload = rotated_rgb if rotation == 180 else rgb
                measurements = packed.copy() if valid else np.zeros_like(packed)
                header = json.dumps({"protocol":1,"type":"infer","host_model":host_model,"token":token,"request":i,"epoch":4,"metadata":metadata}).encode()
                process.stdin.write(struct.pack("<III",len(header),len(payload),measurements.nbytes)+header+payload+measurements.tobytes())
                process.stdin.flush()
                response, depth = executor.submit(packet).result(5)
                assert response["request"] == i and response["metadata"]["assist_input_rotation_degrees"] == rotation
                if rotation == 90:
                    assert response.get("error") and not depth
                else:
                    assert not response.get("error"), response
                    z = np.frombuffer(depth, "<f4")
                    assert z.size == 576*324 and np.all(np.isfinite(z) & (z > 0))
                    if reference is None:
                        reference = z.copy()
                        assert parity_difference(reference, np.fromfile(a.runtime / 'parity_depth.bin', dtype='<f4'), parity_mean_limit, parity_max_limit)['within_profile_parity_tolerance']
                    if not valid:
                        expected = reference[::-1] if rotation == 180 else reference
                        assert response["metadata"]["assist_fusion"]["fusion_mode"] == "monocular", "Old orientation scale leaked"
                        assert np.allclose(z, expected, rtol=.001, atol=1e-5), "Output not restored to native camera grid"
                    else:
                        assert response["metadata"]["assist_fusion"]["dtof_corrected"], "Restored depth no longer aligns with ToF"
                orientations.append({"rotation":rotation,"valid_tof":valid,"rejected":bool(response.get("error")),"pass":True})
        profile_checks = []
        if a.camera_profile == 'wide':
            for i, case in enumerate(('other_map', 'other_model'), 25):
                changed = dict(metadata, assist_input_rotation_degrees=0)
                if case == 'other_map':
                    changed['rectify_map_sha256'] = '00de89f4e6d95f09758b0d8274c2694168a27b5f601bdfeda667b0b179126b5d'
                header = json.dumps(dict(protocol=1,type='infer',host_model='unidepth' if case == 'other_model' else host_model,
                                         token=token,request=i,epoch=5,metadata=changed)).encode()
                process.stdin.write(struct.pack('<III',len(header),len(rgb),packed.nbytes)+header+rgb+packed.tobytes())
                process.stdin.flush()
                response, body = executor.submit(packet).result(5)
                assert 'camera profile mismatch' in response.get('error','') and not body, response
                profile_checks.append(dict(case=case, rejected=True))
        support_checks = []
        if a.test_support:
            policy = 'dtof_supported_mesh_v1'
            def support_request(frame, rotation, measured, output_policy=None, rgb_only=False, native_fov=None, measured_fov=None):
                changed = dict(metadata, frame_id=frame, tof_sequence=frame, tof_timestamp_ns=500000000+frame*67000000,
                               assist_input_rotation_degrees=rotation)
                if output_policy is not None:
                    changed['assist_output_policy'] = output_policy
                if native_fov is not None:
                    changed['dtof_fov_correction'] = native_fov if measured_fov is None else measured_fov
                    if output_policy == 'dtof_geometric_fov_v1':
                        changed['assist_tof_fov_correction'] = native_fov
                if rgb_only:
                    changed.update(source_protocol='HDA1', source_transport='rtsp_h265', model='Native', work_mode='host_rgb',
                                   source_rgb_width=2304, source_rgb_height=1296, dtof_present=False,
                                   boot_id=str(uuid.uuid4()), host_decoded_monotonic_ns=1000000000)
                    if a.camera_profile == 'wide':
                        changed['assist_camera_profile'] = 'wide'
                    for key in ('tof_sequence','tof_timestamp_ns','pair_delta_ms','tof_reused','dtof_fov_correction'):
                        changed.pop(key, None)
                payload = np.frombuffer(rgb, np.uint8).reshape(3,324,576)[:,::-1,::-1].copy().tobytes() if rotation == 180 else rgb
                header = json.dumps(dict(protocol=1,type='infer',host_model=host_model,token=token,request=frame,epoch=frame,metadata=changed)).encode()
                body = b'' if rgb_only else measured.tobytes()
                process.stdin.write(struct.pack('<III',len(header),len(payload),len(body))+header+payload+body)
                process.stdin.flush()
                return executor.submit(packet).result(5)
            for i, rotation in enumerate((0,180)):
                original, original_body = support_request(40+i*2, rotation, packed)
                limited, limited_body = support_request(41+i*2, rotation, packed, policy)
                assert not original.get('error') and not limited.get('error'), (original,limited)
                z = np.frombuffer(limited_body, '<f4')
                reference = np.frombuffer(original_body, '<f4')
                assert z.size == 576*324 and np.all(np.isfinite(z) & (z >= 0))
                count = int(np.count_nonzero(z))
                fit = limited['metadata']['assist_fusion']
                assert 0 < count < z.size and fit['support_pixels'] == count and fit['output_policy'] == policy, fit
                assert fit['support_triangles'] > 0 and fit['dtof_corrected']
                assert np.array_equal(z[z>0], reference[z>0]), 'Support clipping changed retained depth values'
                if a.camera_profile == 'wide':
                    assert np.all(z.reshape(324,576)[[0,0,-1,-1],[0,-1,0,-1]] == 0)
                support_checks.append(dict(rotation=rotation, retained_pixels=count, remaining_values_bit_exact=True))
            empty, body = support_request(50, 0, np.zeros_like(packed), policy)
            assert not empty.get('error') and not np.any(np.frombuffer(body,'<f4'))
            assert empty['metadata']['assist_fusion']['support_pixels'] == 0
            support_checks.append(dict(case='no_current_correction_is_all_unknown', pass_=True))
            for frame, name, requested, rgb_only in ((51,'unknown_policy','unknown',False),(52,'rgb_only',policy,True)):
                response, body = support_request(frame,0,packed,requested,rgb_only)
                assert 'output support contract' in response.get('error','') and not body, response
                support_checks.append(dict(case=name, rejected=True))
            for i, rotation in enumerate((0,180)):
                response, body = support_request(54+i, rotation, packed, rgb_only=True)
                assert not response.get('error') and body, response
                absent = ('tof_sequence','tof_timestamp_ns','rgb_timestamp_ns','pair_delta_ms','tof_reused')
                assert all(key not in response['metadata'] for key in absent), {key:response['metadata'][key] for key in absent if key in response['metadata']}
                support_checks.append(dict(case='rgb_only_identity_absence', rotation=rotation, pass_=True))
            if a.test_support_review:
                for frame in range(60,80):
                    response, body = support_request(frame,0,packed,policy)
                    assert not response.get('error') and body, response
                assert (review_directory/'tof_support_same_frame.png').is_file()
                support_checks.append(dict(case='headless_worker_bounded_review_image',pass_=True))
        fov_checks = []
        if a.test_fov:
            if not a.test_support or a.camera_profile != 'wide':
                p.error('test-fov requires test-support and camera-profile wide')
            policy = 'dtof_geometric_fov_v1'
            frame = 200
            for code in (1,2):
                geometry_path=a.geometry.with_name('dtof_m48_geometry'+('_f2' if code==2 else '')+'.bin')
                raw=geometry_path.read_bytes();tr=np.frombuffer(raw,'<f8',count=3,offset=84)
                directions=np.frombuffer(raw,'<f8',count=1536*3,offset=124).reshape(1536,3)
                native=packed.copy();radial=np.round((2-tr[2])/directions[:,2]/.00025).astype('<u2')
                native[:,:2]=radial.view(np.uint8).reshape(1536,2)
                for rotation in (0,180):
                    missing_reference=None
                    for kind in ('paired','rgb_only','low_snr'):
                        measurements=native.copy()
                        if kind=='low_snr':measurements[:,2]=0
                        original,original_body=support_request(frame,rotation,measurements,rgb_only=kind=='rgb_only',native_fov=code);frame+=1
                        limited,limited_body=support_request(frame,rotation,measurements,policy,rgb_only=kind=='rgb_only',native_fov=code);frame+=1
                        assert not original.get('error') and not limited.get('error'), (original,limited)
                        z=np.frombuffer(limited_body,'<f4');reference=np.frombuffer(original_body,'<f4')
                        fit=limited['metadata']['assist_fusion'];count=int(np.count_nonzero(z))
                        assert z.size==576*324 and np.all(np.isfinite(z)&(z>=0)) and 0<count<z.size
                        assert fit['output_policy']==policy and fit['fov_pixels']==count and fit['fov_geometry_only'] is True
                        assert np.array_equal(z[z>0],reference[z>0]), 'Geometric clipping changed original inference'
                        if kind=='rgb_only':
                            # The display chooses native FOV only when that
                            # field exists. Even a newly inserted null hides
                            # the explicit RGB-only geometry and drops its box.
                            for response in (original, limited):
                                assert not any(key in response['metadata'] for key in ('dtof_fov_correction', 'tof_fov_correction')), 'RGB-only result fabricated a native FOV field'
                            assert limited['metadata']['assist_tof_fov_correction']==code
                            missing_reference=z.copy()
                        if kind=='low_snr':assert np.array_equal(z,missing_reference), 'Missing returns carved geometry holes'
                        fov_checks.append(dict(native_fov=code,rotation=rotation,input_kind=kind,retained_pixels=count,remaining_values_bit_exact=True))
            for name,code,measured_code in (('missing_fov',None,None),('unknown_fov',3,None),('wrong_fov',1,2)):
                response,body=support_request(frame,0,packed,policy,native_fov=code,measured_fov=measured_code);frame+=1
                assert 'output support contract' in response.get('error','') and not body, response
                fov_checks.append(dict(case=name,rejected=True))
        experiment_checks = []
        dynamic_pair_checks = []
        if a.test_experiment:
            if model_reference is None:
                reference_meta = dict(metadata, frame_id=450, tof_sequence=450, assist_input_rotation_degrees=0)
                header = json.dumps(dict(protocol=1,type='infer',host_model=host_model,token=token,request=450,epoch=450,metadata=reference_meta)).encode()
                empty = np.zeros_like(packed)
                process.stdin.write(struct.pack('<III',len(header),len(rgb),empty.nbytes)+header+rgb+empty.tobytes())
                process.stdin.flush()
                reference_reply, reference_body = executor.submit(packet).result(5)
                assert not reference_reply.get('error'), reference_reply
                model_reference = np.frombuffer(reference_body, '<f4').copy()
            frame = 500
            for code in (1, 2):
                raw = a.geometry.with_name('dtof_m48_geometry'+('_f2' if code == 2 else '')+'.bin').read_bytes()
                tr = np.frombuffer(raw, '<f8', count=3, offset=84)
                directions = np.frombuffer(raw, '<f8', count=1536*3, offset=124).reshape(1536, 3)
                native = packed.copy()
                native[:, :2] = np.round((2-tr[2])/directions[:, 2]/.00025).astype('<u2').view(np.uint8).reshape(1536, 2)
                for mode in ('C', 'B_OLD', 'D', 'E', 'F', 'B_FAST', 'B', 'G', 'H'):
                    for rotation in (0, 180):
                        for valid in (False, True):
                            frame += 1
                            changed = dict(metadata, model='Native', source_protocol='HDA1', source_transport='rtsp_h265',
                                source_rgb_width=2304, source_rgb_height=1296, dtof_present=True, work_mode='rgb_tof',
                                boot_id=str(uuid.uuid4()), source_generation=uuid.uuid4().hex, generation_source='host_presentation_epoch',
                                source_candidate_mode=14, source_engine='native-hd-capture-v1', generation=frame, frame_id=frame,
                                tof_sequence=frame, tof_timestamp_ns=2000000000, rgb_timestamp_ns=1992000000,
                                pair_delta_ms=8., pair_clock_uncertainty_ms=35, pair_clock_source='rtcp_prft_board_realtime_monotonic',
                                host_decoded_monotonic_ns=3000000000, assist_input_rotation_degrees=rotation,
                                assist_experiment=mode, dtof_fov_correction=code)
                            if a.camera_profile == 'wide':
                                changed.update(assist_camera_profile='wide', assist_output_policy='dtof_geometric_fov_v1', assist_tof_fov_correction=code)
                            if mode == 'H':
                                changed.update(pairing_policy='latest_native_reuse_100ms_global_scale_v1', tof_warning_flags=0)
                            measured = native.copy()
                            if not valid: measured[:, 3] = 0
                            payload = np.frombuffer(rgb, np.uint8).reshape(3,324,576)[:,::-1,::-1].copy().tobytes() if rotation == 180 else rgb
                            header = json.dumps(dict(protocol=1,type='infer',host_model=host_model,token=token,request=frame,epoch=frame,metadata=changed)).encode()
                            process.stdin.write(struct.pack('<III',len(header),len(payload),measured.nbytes)+header+payload+measured.tobytes())
                            process.stdin.flush()
                            response, body = executor.submit(packet).result(30)
                            if mode not in ('B', 'G', 'H'):
                                assert 'Invalid ToF experiment request' in response.get('error', '') and not body, response
                                experiment_checks.append(dict(mode=mode, retired_rejected=True, fov=code, rotation=rotation, valid_tof=valid))
                                continue
                            assert not response.get('error'), response
                            z = np.frombuffer(body, '<f4'); fit = response['metadata']['assist_fusion']
                            assert z.size == 576*324 and np.all(np.isfinite(z) & (z >= 0 if a.camera_profile == 'wide' else z > 0))
                            assert response['metadata']['assist_experiment'] == mode
                            revision = 'global_scale_latest_native_rgb_v2' if mode == 'H' else 'global_scale_rgb_v2' if mode == 'G' else 'local_recovery_v3'
                            assert response['metadata']['assist_fusion_revision'] == revision
                            assert response['metadata']['assist_output_restored_to_calibrated_grid'] is True
                            if not valid:
                                assert fit['dtof_corrected'] is False and not fit['used_native_indices']
                                # Compare orientation with the SAME production
                                # graph's current output. The frozen FP32 export
                                # fixture is checked separately with its existing
                                # mean/max mixed-precision parity tolerances.
                                expected = model_reference
                                if rotation == 180: expected = expected[::-1]
                                assert np.allclose(z[z > 0], expected[z > 0], rtol=.001, atol=1e-5), 'Experiment orientation restoration moved pixels'
                            experiment_checks.append(dict(mode=mode,fusion_revision=revision,fov=code,rotation=rotation,valid_tof=valid,
                                positive_pixels=int(np.count_nonzero(z)),total_ms=response['metadata']['assist_total_ms']))
                            # An identical native sequence is not a second inference.
                            process.stdin.write(struct.pack('<III',len(header),len(payload),measured.nbytes)+header+payload+measured.tobytes())
                            process.stdin.flush()
                            duplicate, duplicate_body = executor.submit(packet).result(5)
                            assert 'reus' in duplicate.get('error','') and not duplicate_body
            if a.camera_profile == 'wide':
                # One session alternates new native measurements with RGB-only
                # predictions, matching the 30/15 FPS G-mode publication path.
                frame = 800
                for code in (1, 2):
                    raw = a.geometry.with_name('dtof_m48_geometry'+('_f2' if code == 2 else '')+'.bin').read_bytes()
                    tr = np.frombuffer(raw, '<f8', count=3, offset=84)
                    directions = np.frombuffer(raw, '<f8', count=1536*3, offset=124).reshape(1536, 3)
                    native = packed.copy()
                    native[:, :2] = np.round((2-tr[2])/directions[:, 2]/.00025).astype('<u2').view(np.uint8).reshape(1536, 2)
                    for rotation, mode in ((r, m) for r in (0, 180) for m in ('G', 'H')):
                        boot, generation, epoch = str(uuid.uuid4()), uuid.uuid4().hex, frame
                        last_scale, last_sequence = None, None
                        for tick in range(9):
                            frame += 1
                            paired = tick < 8 if mode == 'H' else tick % 2 == 0
                            native_tick = (0, 0, 2, 2, 2, 5, 6, 6, 6)[tick] if mode == 'H' else tick
                            sequence = (1, 1, 2, 2, 2, 3, 4, 4, 4)[tick] if mode == 'H' else tick//2+1
                            tof_ns, rgb_ns = 2000000000+native_tick*33333333, 1992000000+tick*33333333
                            reused = paired and sequence == last_sequence
                            changed = dict(metadata, model='Native', source_protocol='HDA1', source_transport='rtsp_h265',
                                source_rgb_width=2304, source_rgb_height=1296, dtof_present=paired,
                                work_mode='rgb_tof' if paired else 'host_rgb', boot_id=boot, source_generation=generation,
                                generation_source='host_presentation_epoch', source_candidate_mode=14, source_engine='native-hd-capture-v1',
                                generation=epoch, frame_id=frame, tof_sequence=sequence, tof_timestamp_ns=tof_ns, tof_reused=reused,
                                rgb_timestamp_ns=rgb_ns, pair_delta_ms=(tof_ns-rgb_ns)/1e6, pair_clock_uncertainty_ms=35,
                                pair_clock_source='rtcp_prft_board_realtime_monotonic', host_decoded_monotonic_ns=3000000000+tick*33333333,
                                assist_input_rotation_degrees=rotation, assist_experiment=mode, dtof_fov_correction=code,
                                assist_camera_profile='wide', assist_output_policy='dtof_geometric_fov_v1', assist_tof_fov_correction=code)
                            if mode == 'H':
                                changed.update(pairing_policy='latest_native_reuse_100ms_global_scale_v1', tof_warning_flags=0)
                            if not paired:
                                for key in ('tof_sequence','tof_timestamp_ns','tof_reused','rgb_timestamp_ns','pair_delta_ms',
                                            'pair_clock_uncertainty_ms','pair_clock_source','dtof_fov_correction',
                                            'dtof_width','dtof_height','dtof_bytes','dtof_format','dtof_range_unit','dtof_source','dtof_coordinate'):
                                    changed.pop(key, None)
                            payload = np.frombuffer(rgb, np.uint8).reshape(3,324,576)[:,::-1,::-1].copy().tobytes() if rotation == 180 else rgb
                            header = json.dumps(dict(protocol=1,type='infer',host_model=host_model,token=token,request=frame,epoch=epoch,metadata=changed)).encode()
                            body = native.tobytes() if paired else b''
                            process.stdin.write(struct.pack('<III',len(header),len(payload),len(body))+header+payload+body)
                            process.stdin.flush()
                            response, body = executor.submit(packet).result(30)
                            assert not response.get('error'), response
                            result = response['metadata']; fit = result['assist_fusion']; z = np.frombuffer(body, '<f4')
                            assert result['assist_tof_fov_correction']==code and fit['fov_pixels']==np.count_nonzero(z)
                            assert fit['dtof_corrected'] is paired
                            if paired:
                                assert fit['fusion_mode']=='global_scale' and fit['used_native_indices']
                                assert result['tof_reused'] is reused and result['tof_sequence']==sequence
                                assert not fit['local_used_native_indices']
                                last_scale = fit['scale']; reference_frame = frame; last_sequence = sequence
                            else:
                                assert fit['fusion_mode']=='held_scale' and fit['scale']==last_scale and fit['scale_reference_frame_id']==reference_frame
                                assert not any(key in result for key in ('dtof_fov_correction','tof_fov_correction','tof_sequence','tof_timestamp_ns'))
                                assert not any(fit.get(key, []) for key in ('used_native_indices','global_used_native_indices','local_used_native_indices'))
                            dynamic_pair_checks.append(dict(mode=mode,fov=code,rotation=rotation,paired=paired,reused=reused,frame=frame,
                                fusion_mode=fit['fusion_mode'],geometry_preserved=True,current_points_only=True))
        benchmark = None
        if a.benchmark_frames:
            def infer_synthetic(payload, frame, epoch):
                metadata.update(frame_id=frame, tof_sequence=frame, tof_timestamp_ns=500000000+frame*67000000,
                                assist_input_rotation_degrees=0)
                header = json.dumps(dict(protocol=1, type='infer', host_model=host_model, token=token, request=frame, epoch=epoch, metadata=metadata)).encode()
                measurements = np.zeros_like(packed)
                process.stdin.write(struct.pack('<III', len(header), len(payload), measurements.nbytes)+header+payload+measurements.tobytes())
                process.stdin.flush()
                response, body = executor.submit(packet).result(5)
                assert not response.get('error') and response['request'] == frame and response['epoch'] == epoch, response
                m = response['metadata']
                assert m['assist_graph_sha256'] == expected_graph and m['assist_fusion']['fusion_mode'] == 'monocular'
                z = np.frombuffer(body, '<f4').copy()
                assert z.size == 576*324 and np.all(np.isfinite(z) & (z > 0))
                return z, m
            timed = []
            for i in range(5+a.benchmark_frames):
                _, m = infer_synthetic(rgb, 100+i, 100)
                if i >= 5:
                    timed.append(m['assist_inference_ms'])
            y, x = np.indices((324, 576))
            rng = np.random.default_rng(20261008)
            cases = dict(original_parity_noise=np.frombuffer(rgb, np.uint8).reshape(3, 324, 576))
            cases.update({name: np.full((3, 324, 576), value, np.uint8) for name, value in [('black', 0), ('gray', 128), ('white', 255)]})
            cases['color_gradient'] = np.stack([x*255//575, y*255//323, (x+y)*255//898]).astype(np.uint8)
            cases['checkerboard'] = np.tile((((x//24+y//24)%2)*255).astype(np.uint8)[None], (3, 1, 1))
            cases['dark_noise'] = rng.integers(0, 64, size=(3, 324, 576), dtype=np.uint8)
            cases['bright_noise'] = rng.integers(192, 256, size=(3, 324, 576), dtype=np.uint8)
            depths = {}
            for i, (name, values) in enumerate(cases.items()):
                depths[name], _ = infer_synthetic(values.tobytes(), 1000+i, 1000+i)
            depth_path = a.report.with_suffix('.npz')
            np.savez_compressed(depth_path, **depths)
            comparisons = []
            if a.compare_depths:
                with np.load(a.compare_depths, allow_pickle=False) as baseline:
                    for name, z in depths.items():
                        comparisons.append(dict(case=name, **parity_difference(z, baseline[name])))
            benchmark = dict(samples=len(timed), warmups=5, mean_ms=float(np.mean(timed)), median_ms=float(np.median(timed)),
                             p95_ms=float(np.percentile(timed, 95)), inference_fps=1000/float(np.mean(timed)),
                             synthetic_depths=str(depth_path), fp32_comparisons=comparisons,
                             quality_scope='Numerical agreement only; not real-scene or ground-truth accuracy')
        report = {"pass":True,"input_origin":"SIMULATED","camera_or_board_connected":False,"frames":records,"orientation_checks":orientations,
                  'profile_checks':profile_checks,'support_checks':support_checks,'fov_checks':fov_checks,'experiment_checks':experiment_checks,
                  'diagnostic_checks':diagnostic_checks,
                  'dynamic_pair_checks':dynamic_pair_checks,
                  'camera_profile':a.camera_profile,'precision':a.expected_precision,'graph_sha256':expected_graph,'original_fixture_parity':fixture_difference,'benchmark':benchmark}
        report.update(provisional_model=a.fp16_candidate, profile_mean_limit=parity_mean_limit, profile_max_limit=parity_max_limit)
        if a.test_contract:
            assert a.expected_precision in ('fp16-mixed', 'fp32-tf32', 'fp16-mixed-trial')
            report['contract_checks'] = check_rejected_contracts(a.executable, a.runtime, env, a.report.parent / (a.report.stem+'_contracts'), a.fp32_runtime, source_sha)
        a.report.write_text(json.dumps(report, indent=2)+"\n")
        print(json.dumps({"pass":True,"frames":len(records),"inference_median_ms":float(np.median([r["inference_ms"] for r in records[1:]])),
                          "total_median_ms":float(np.median([r["total_ms"] for r in records[1:]]))}), flush=True)
    except BaseException:
        process.kill()
        diagnostic = process.stderr.read().decode('utf-8',errors='replace')[-2000:]
        if diagnostic:
            print('NATIVE_WORKER_STDERR: '+diagnostic, flush=True)
        raise
    finally:
        process.kill()
        process.wait(2)
        executor.shutdown(wait=True)


if __name__ == "__main__":
    main()
