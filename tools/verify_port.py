"""Frozen G parity and B ToF reuse tests without device or network access."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from run_offline_tests import test_environment
from unidepth.native48_contract import GRAPH_SIZE, MODEL, NATIVE_GRID, check_batch, exchange, metadata, native_plane


def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(4*1024*1024),b''):
            digest.update(block)
    return digest.hexdigest()


def remove_native(m):
    for key in ('tof_sequence','tof_timestamp_ns','rgb_timestamp_ns','tof_reused','pair_delta_ms',
                'dtof_width','dtof_height','dtof_bytes','dtof_format','dtof_range_unit','dtof_source',
                'dtof_coordinate','dtof_fov_correction','tof_warning_flags','pair_clock_uncertainty_ms','pair_clock_source'):
        m.pop(key,None)
    m.update(dtof_present=False,work_mode='host_rgb',pairing_policy='no_current_native_measurement')


def build_cases(root, profile):
    width,height,_,_=GRAPH_SIZE
    cols,rows=NATIVE_GRID
    y,x=np.mgrid[:height,:width]
    images=[np.stack(((x*3+y+v*47)%256,(y*5+v*73)%256,(x+y*2+v*29)%256)).astype(np.uint8).tobytes()
            for v in range(3)]
    cases=[]

    def add(name, rotation=0, native=None, rgb=None, accepted=True, epoch=None, **extra):
        index=len(cases)+1
        m=metadata(index,6,rotation,profile,epoch if epoch is not None else index)
        case=dict(name=name,metadata=m,native=native if native is not None else native_plane(root,cols,rows,6),
                  rgb=rgb if rgb is not None else images[0],accepted=accepted,epoch=m['generation'],**extra)
        cases.append(case)
        return case

    for rotation in (0,180):
        for variant,z in enumerate((.25,.6,1.2)):
            add(f'f6_r{rotation}_image{variant}',rotation,native_plane(root,cols,rows,6,z),images[variant])
    for delta in (165.,-165.,165.001,-165.001):
        m=add(f'G_pair_delta_{delta}',accepted=abs(delta)<=165.)['metadata']
        m.update(pair_delta_ms=delta,rgb_timestamp_ns=m['tof_timestamp_ns']-round(delta*1e6))

    first=add('G_fresh_native',epoch=100)['metadata']
    for explicit_reuse in (False,True):
        m=add(f'G_duplicate_native_rejected_{explicit_reuse}',epoch=100,accepted=False)['metadata']
        for key in ('tof_sequence','tof_timestamp_ns','rgb_timestamp_ns'):
            m[key]=first[key]
        m['tof_reused']=explicit_reuse

    remove_native(add('G_rgb_only_uses_last_scale',native=b'',epoch=100,uncorrected=True,
                      held_scale=True,reference_frame=first['frame_id'])['metadata'])
    remove_native(add('G_rgb_only_new_epoch',native=b'',uncorrected=True)['metadata'])

    for fault in ('wrong_grid','missing_fov','conflicting_fov','bad_rotation','invalid_bit','warning','low_snr'):
        case=add(fault,accepted=fault=='low_snr',uncorrected=fault=='low_snr')
        m,native=case['metadata'],case['native']
        if fault=='wrong_grid':
            m.update(dtof_width=16,dtof_height=16,dtof_bytes=1024)
            case['native']=native_plane(root,16,16,6)
        if fault=='missing_fov':m.pop('dtof_fov_correction')
        if fault=='conflicting_fov':m['tof_fov_correction']=1
        if fault=='bad_rotation':m['assist_input_rotation_degrees']=90
        if fault=='invalid_bit':case['native']=native[:3]+b'\x02'+native[4:]
        if fault=='warning':m['tof_warning_flags']=8
        if fault=='low_snr':
            case['native']=b''.join(native[j:j+2]+b'\x05'+native[j+3:j+4] for j in range(0,len(native),4))
    return cases


def build_reuse_cases(profile, g_cases):
    first_rgb, next_rgb = (case['rgb'] for case in g_cases[:2])
    native = g_cases[0]['native']
    cases = []

    def add(name, *, sequence=1, reused=True, delta=-33., epoch=1, rgb=next_rgb,
            samples=native, timestamp_shift=0, accepted=True, **expectations):
        m = metadata(len(cases)+1, 6, 0, profile, epoch)
        tof_ns = 1_000_000_000 + sequence*67_000_000 + timestamp_shift
        m.update(assist_experiment='B', tof_sequence=sequence, tof_reused=reused,
                 tof_timestamp_ns=tof_ns, rgb_timestamp_ns=tof_ns-round(delta*1e6), pair_delta_ms=delta)
        cases.append(dict(name=name,metadata=m,native=samples,rgb=rgb,accepted=accepted,epoch=epoch,**expectations))
        return m

    add('B_first_measurement', reused=False, delta=5., rgb=first_rgb, corrected=True)
    add('B_reuse_for_new_rgb', corrected=True)
    add('B_reuse_at_negative_limit', delta=-165., corrected=True)
    add('B_reuse_beyond_negative_limit', delta=-165.001, accepted=False)
    add('B_reuse_cannot_change_native_bytes', samples=bytes([native[0]^1])+native[1:], accepted=False)
    add('B_reuse_cannot_change_tof_timestamp', timestamp_shift=1_000_000, accepted=False)
    add('B_reuse_must_be_labeled', reused=False, accepted=False)
    add('B_new_sequence_cannot_claim_reuse', sequence=6, accepted=False)
    add('B_new_sequence_at_positive_limit', sequence=6, reused=False, delta=165., corrected=True)
    latest = add('B_reuse_at_positive_limit', sequence=6, delta=165., corrected=True)
    add('B_reuse_beyond_positive_limit', sequence=6, delta=165.001, accepted=False)
    add('B_old_sequence_rejected', delta=-165., accepted=False)
    remove_native(add('B_rgb_only_uses_last_scale', samples=b'', held_scale=True,
                      uncorrected=True, reference_frame=latest['frame_id']))
    add('B_reuse_cannot_cross_epoch', epoch=2, accepted=False)
    add('B_new_epoch_starts_fresh', epoch=2, reused=False, delta=5., rgb=first_rgb, corrected=True)
    return cases


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--reference-executable',type=Path,required=True)
    parser.add_argument('--qt-root',type=Path,required=True)
    parser.add_argument('--runtime',type=Path,help='Model runtime for the candidate executable')
    parser.add_argument('--reference-runtime',type=Path,help='Defaults to the candidate runtime')
    parser.add_argument('--backend',default='tensorrt',help='Explicit device, e.g. cpu, directml:0, openvino:CPU, openvino:INT8')
    parser.add_argument('--reference-backend',help='Defaults to --backend')
    parser.add_argument('--compatible-runtime',type=Path)
    parser.add_argument('--reference-compatible-runtime',type=Path)
    parser.add_argument('--model',choices=(MODEL,),default=MODEL)
    parser.add_argument('--comparison',choices=('all','monocular'),default='all',
                        help='Select monocular when comparing only raw model tensor identity')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    model=args.model
    candidate_runtime=args.runtime.resolve() if args.runtime else root/'UniDepthRuntimeRgb672VgaFP16RTX'
    runtime=args.reference_runtime.resolve() if args.reference_runtime else candidate_runtime
    args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    for directory in {runtime,candidate_runtime}:
        manifest=json.loads((directory/'runtime.json').read_text())
        for name,expected in manifest['files'].items():
            target=(directory/name).resolve()
            assert directory.resolve() in target.parents, ('Runtime path outside directory',name)
            assert sha256(target)==expected,('Runtime hash mismatch',name)
    temporary=args.output/'tmp'
    temporary.mkdir()
    env=test_environment(args.qt_root.resolve(),temporary)
    env['RV_INFERENCE_BACKEND']=args.backend
    env['RV_ASSIST_COMPAT_ROOT']=str((args.compatible_runtime or root/'UniDepthRuntimeCompatible').resolve())
    reference_env=dict(env)
    reference_env['RV_INFERENCE_BACKEND']=args.reference_backend or args.backend
    reference_env['RV_ASSIST_COMPAT_ROOT']=str((args.reference_compatible_runtime or args.compatible_runtime or root/'UniDepthRuntimeCompatible').resolve())

    profile=json.loads((candidate_runtime/'model.json').read_text())
    cases=build_cases(root,profile)
    for folder in ('candidate','reference','restart','reuse_b'):
        (args.output/folder).mkdir()
    current,hashes=check_batch(args.executable,root,candidate_runtime,model,cases,env,args.output/'candidate')
    assert all(r.get('output_grid')=='tof_portrait_crop_v1' and r.get('output_wh')==[480,640]
               for r in current['cases'] if r['accepted']), 'Candidate did not deliver the current portrait grid'
    reference,before=check_batch(args.reference_executable,root,runtime,model,cases,reference_env,args.output/'reference')
    (args.output/'comparison.json').write_text(json.dumps(dict(candidate=current,reference=reference),indent=2),encoding='utf-8')
    if args.comparison=='all':
        assert before==hashes,'Frozen depth/monocular/diagnostic output differs from the reference'
    else:
        assert before.keys()==hashes.keys()
        assert all(before[name]['model_mono']==hashes[name]['model_mono'] for name in hashes),'Original monocular model tensor changed'
    _,restarted=check_batch(args.executable,root,candidate_runtime,model,cases[:3],env,args.output/'restart')
    assert all(restarted[c['name']]==hashes[c['name']] for c in cases[:3]),'Stop/restart changed output'
    assert len({hashes[c['name']]['mono'] for c in cases[:3]})==3,'New RGB inputs were replayed'

    code,packets,_=exchange(args.executable,root,candidate_runtime,model,[],env,override=False)
    assert code!=0 and not packets,'Explicit worker model identity is required'
    for retired_model in ('unidepth_rgb672_vga_fp16_tof16','unidepth_rgb672_vga_fp16_tof48_i'):
        code,packets,_=exchange(args.executable,root,candidate_runtime,retired_model,[],env)
        assert code!=0 and not packets,('Removed worker model accepted',retired_model)
    removed=[dict(cases[0],name=f'{experiment}_removed',
                  metadata=dict(cases[0]['metadata'],assist_experiment=experiment),accepted=False)
             for experiment in ('A','H','I')]
    code,packets,_=exchange(args.executable,root,candidate_runtime,model,removed,env)
    assert code==0 and len(packets)==len(removed)+1
    assert all(header.get('error') and not depth and not diagnostic for header,depth,diagnostic in packets[1:]), \
        'Removed correction mode accepted'

    reuse_cases=build_reuse_cases(profile,cases)
    reuse,b_hashes=check_batch(args.executable,root,candidate_runtime,model,reuse_cases,env,args.output/'reuse_b')
    first_b=b_hashes['B_first_measurement']
    assert first_b==hashes[cases[0]['name']], 'B first pair differs from G for the same input'
    assert first_b==b_hashes['B_new_epoch_starts_fresh'], 'B reused the previous epoch scale'
    assert first_b['model_mono']!=b_hashes['B_reuse_for_new_rgb']['model_mono'], 'B replayed the previous RGB'
    fits={result['case']:result.get('measured_scale') for result in reuse['cases'] if result['accepted']}
    assert fits['B_first_measurement']!=fits['B_reuse_for_new_rgb'], 'B did not refit the scale for new RGB'
    (args.output/'reuse_b'/'summary.json').write_text(json.dumps(reuse,indent=2),encoding='utf-8')

    summary=dict(passed=True,comparison=args.comparison,model=model,reference_model=model,backend=args.backend,
                 model_loading=current['ready']['model_loading'],cases=len(cases),
                 bitwise_identical_outputs=sum(before[name]==hashes[name] for name in hashes),
                 bitwise_identical_model_monocular_outputs=len(hashes),
                 stop_restart_identical=True,varied_rgb_not_replayed=True,model_manifest_verified=True,
                 b_cases=len(reuse_cases),b_accepted_outputs=len(b_hashes),
                 b_reused_pairs=sum(case['metadata'].get('tof_reused') is True and case['accepted'] for case in reuse_cases),
                 removed_modes_rejected=True,hardware_started=False,physical_accuracy_verified=False,
                 executable_sha256=sha256(args.executable),source_executable_sha256=sha256(args.reference_executable),results=current)
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k!='results'}))


if __name__=='__main__':
    main()
