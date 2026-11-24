"""Check host worker parity with synthetic RGB and ToF; no hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import numpy as np

from unidepth.native48_contract import VGA, PORTRAIT, SIZES, metadata, native_plane, check_batch, exchange
from run_offline_tests import test_environment

def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(4*1024*1024),b''):
            digest.update(block)
    return digest.hexdigest()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--reference-executable',type=Path,required=True)
    parser.add_argument('--qt-root',type=Path,required=True)
    parser.add_argument('--runtime',type=Path,help='Model runtime for the candidate executable')
    parser.add_argument('--reference-runtime',type=Path,help='Defaults to the candidate runtime')
    parser.add_argument('--backend',default='tensorrt',help='Explicit device, e.g. cpu, directml:0, openvino:CPU, openvino:INT8')
    parser.add_argument('--reference-backend',help='Defaults to --backend; exact output comparison remains mandatory')
    parser.add_argument('--compatible-runtime',type=Path)
    parser.add_argument('--reference-compatible-runtime',type=Path)
    parser.add_argument('--model',choices=(PORTRAIT, VGA),default=PORTRAIT)
    parser.add_argument('--comparison',choices=('all','monocular'),default='all',
                        help='Compare the frozen graph against a Native16 reference when migrating output grids')
    parser.add_argument('--scheme',choices=('G','B'),default='G',help='Portrait pairing mode to exercise')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    model=args.model
    if args.comparison=='monocular' and model!=PORTRAIT:
        parser.error('Monocular migration comparison requires a portrait candidate')
    folder='UniDepthRuntimeRgb672VgaFP16RTX'
    candidate_runtime=args.runtime.resolve() if args.runtime else root/folder
    runtime=args.reference_runtime.resolve() if args.reference_runtime else candidate_runtime
    args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    for directory in {runtime,candidate_runtime}:
        manifest=json.loads((directory/'runtime.json').read_text())
        for name,expected in manifest['files'].items():
            target=(directory/name).resolve()
            assert directory.resolve() in target.parents, ('Runtime path outside directory',name)
            assert sha256(target)==expected,('Runtime hash mismatch',name)
    temporary=args.output/'tmp';temporary.mkdir()
    env=test_environment(args.qt_root.resolve(),temporary)
    env['RV_INFERENCE_BACKEND']=args.backend
    env['RV_ASSIST_COMPAT_ROOT']=str((args.compatible_runtime or root/'UniDepthRuntimeCompatible').resolve())
    reference_env=dict(env)
    reference_env['RV_INFERENCE_BACKEND']=args.reference_backend or args.backend
    reference_env['RV_ASSIST_COMPAT_ROOT']=str((args.reference_compatible_runtime or args.compatible_runtime or root/'UniDepthRuntimeCompatible').resolve())
    profile=json.loads((candidate_runtime/'model.json').read_text())
    width,height,_,_=SIZES[model]
    y,x=np.mgrid[:height,:width]
    images=[np.stack(((x*3+y+v*47)%256,(y*5+v*73)%256,(x+y*2+v*29)%256)).astype(np.uint8).tobytes() for v in range(3)]
    cases=[]
    cols,rows,limit=(48,32,165.) if model==PORTRAIT else (16,16,100.)
    mode=args.scheme if model==PORTRAIT else 'H'
    def packet(index, rotation, epoch):
        return metadata(index,6,rotation,mode,cols,rows,model,profile,epoch)
    def add(name,m,native=None,rgb=None,accepted=True,epoch=None,**extra):
        cases.append(dict(name=name,metadata=m,native=native if native is not None else native_plane(root,cols,rows,m['dtof_fov_correction']),
                          rgb=rgb if rgb is not None else images[0],accepted=accepted,epoch=epoch if epoch is not None else m['generation'],**extra))
    for rotation in (0,180):
        for variant in range(3):
            i=len(cases)+1
            add(f'f6_r{rotation}_image{variant}',packet(i,rotation,i),
                native_plane(root,cols,rows,6,(.25,.6,1.2)[variant]),images[variant])
    i=len(cases)+1
    m=packet(i,0,100)
    native=native_plane(root,cols,rows,6)
    add('H_first_native',m,native)
    fresh_limit=packet(len(cases)+1,0,101)
    for delta in (limit,limit+.001):
        fresh=dict(fresh_limit,frame_id=len(cases)+1, pair_delta_ms=delta,
                   rgb_timestamp_ns=fresh_limit['tof_timestamp_ns']-round(delta*1e6))
        add('fresh_'+str(delta),fresh,native,accepted=delta<=limit)
    # Reuse checks belong to the same measurement/epoch as their first packet.
    m=packet(len(cases)+1,0,102)
    add('first_for_reuse',m,native)
    for delta in (20.,50.,limit,-limit,limit+.001,-limit-.001):
        reused=dict(m,tof_reused=True,pair_delta_ms=delta,rgb_timestamp_ns=m['tof_timestamp_ns']-round(delta*1e6),frame_id=len(cases)+1)
        add('reuse_'+str(delta),reused,native,accepted=mode!='G' and abs(delta)<=limit)
    reused=dict(m,tof_reused=True,pair_delta_ms=50.,rgb_timestamp_ns=m['tof_timestamp_ns']-50_000_000,frame_id=len(cases)+1)
    add('H_changed_native_rejected',reused,bytes([native[0]^1])+native[1:],accepted=False)
    add('H_false_reuse_rejected',dict(reused,tof_reused=False),native,accepted=False)
    for fault in ('wrong_grid','missing_fov','conflicting_fov','bad_rotation','invalid_bit','low_snr'):
        i=len(cases)+1;m=packet(i,0,i+200);native=native_plane(root,cols,rows,6)
        if fault=='wrong_grid':m['dtof_width']=16 if cols==48 else 47
        if fault=='missing_fov':m.pop('dtof_fov_correction')
        if fault=='conflicting_fov':m['tof_fov_correction']=1
        if fault=='bad_rotation':m['assist_input_rotation_degrees']=90
        if fault=='invalid_bit':native=native[:3]+b'\x02'+native[4:]
        if fault=='low_snr':native=b''.join(native[j:j+2]+b'\x05'+native[j+3:j+4] for j in range(0,len(native),4))
        add(fault,m,native,accepted=fault=='low_snr',uncorrected=fault=='low_snr')
    i=len(cases)+1;m=packet(i,0,500)
    for key in ('tof_sequence','tof_timestamp_ns','rgb_timestamp_ns','tof_reused','pair_delta_ms','dtof_width','dtof_height','dtof_bytes','dtof_format','dtof_range_unit','dtof_source','dtof_coordinate','dtof_fov_correction','tof_warning_flags','pair_clock_uncertainty_ms','pair_clock_source'):
        m.pop(key,None)
    m.update(dtof_present=False,work_mode='host_rgb');add('H_rgb_only_no_invented_points',m,b'',uncorrected=True)
    for folder in ('candidate','reference','restart'):(args.output/folder).mkdir()
    current,hashes=check_batch(args.executable,root,candidate_runtime,model,cases,env,args.output/'candidate')
    if args.comparison=='monocular':
        reference_cases=[]
        ref_profile=json.loads((runtime/'model.json').read_text())
        for i, case in enumerate(cases[:6], 1):
            legacy=metadata(i,6,case['metadata']['assist_input_rotation_degrees'],'H',16,16,VGA,ref_profile,i)
            reference_cases.append(dict(case,metadata=legacy,epoch=i,native=native_plane(root,16,16,6)))
        _,before=check_batch(args.reference_executable,root,runtime,VGA,reference_cases,reference_env,args.output/'reference')
        assert all(hashes[name]['model_mono']==item['mono'] for name,item in before.items()),'Frozen graph changed before crop'
    else:
        _,before=check_batch(args.reference_executable,root,runtime,model,cases,reference_env,args.output/'reference')
        assert before==hashes,'Frozen depth/monocular/diagnostic output differs from source software'
    _,restarted=check_batch(args.executable,root,candidate_runtime,model,cases[:3],env,args.output/'restart')
    assert all(restarted[c['name']]==hashes[c['name']] for c in cases[:3]),'Stop/restart changed output'
    assert len({hashes[c['name']]['mono'] for c in cases[:3]})==3,'New RGB inputs were replayed'
    code,packets,_=exchange(args.executable,root,candidate_runtime,model,[],env,override=False)
    assert code!=0 and not packets,'Explicit worker model identity is required'
    removed=dict(cases[0],name='A_removed',metadata=dict(cases[0]['metadata'],assist_experiment='A'),accepted=False)
    code,packets,_=exchange(args.executable,root,candidate_runtime,model,[removed],env)
    assert code==0 and packets[-1][0].get('error') and not packets[-1][1],'Removed correction mode accepted'
    summary=dict(passed=True,model=model,scheme=mode,backend=args.backend,model_loading=current["ready"]["model_loading"],cases=len(cases),
                 comparison=args.comparison, bitwise_identical_outputs=len(before),
                 stop_restart_identical=True,varied_rgb_not_replayed=True,model_manifest_verified=True,
                 removed_modes_rejected=True,hardware_started=False,physical_accuracy_verified=False,
                 executable_sha256=sha256(args.executable),source_executable_sha256=sha256(args.reference_executable),results=current)
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k!='results'}))

if __name__=='__main__':main()
