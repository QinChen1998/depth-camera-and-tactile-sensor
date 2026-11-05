"""Frozen synthetic FP16/H parity. No sockets, cameras, serial ports or recording."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import numpy as np

sys.path.insert(0,str(Path(__file__).parent/'unidepth'))
from check_native16_worker import ALIAS, metadata, native_plane, check_batch, exchange

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
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1];runtime=root/'UniDepthRuntimeWide576FP16'
    candidate_runtime=args.runtime.resolve() if args.runtime else runtime
    args.output.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((runtime/'runtime.json').read_text())
    for name,expected in manifest['files'].items():
        assert sha256(runtime/name)==expected,('Runtime hash mismatch',name)
    env={k:v for k,v in os.environ.items() if not k.startswith(('PYTHON','CONDA','RV_','DCTS_'))}
    env.update(PATH=str(args.qt_root/'bin')+';'+os.environ.get('SystemRoot','C:/Windows')+'/System32',
               QT_PLUGIN_PATH=str(args.qt_root/'plugins'),RV_TEST_OFFLINE_ONLY='1')
    profile=json.loads((runtime/'model.json').read_text())
    y,x=np.mgrid[:324,:576]
    images=[np.stack(((x*3+y+v*47)%256,(y*5+v*73)%256,(x+y*2+v*29)%256)).astype(np.uint8).tobytes() for v in range(3)]
    cases=[]
    def add(name,m,native=None,rgb=None,accepted=True,epoch=None,**extra):
        cases.append(dict(name=name,metadata=m,native=native if native is not None else native_plane(root,16,16,m['dtof_fov_correction']),
                          rgb=rgb if rgb is not None else images[0],accepted=accepted,epoch=epoch if epoch is not None else m['generation'],**extra))
    for fov in (1,2):
        for rotation in (0,180):
            for variant in range(3):
                i=len(cases)+1
                m=metadata(i,fov,rotation,'H',16,16,ALIAS,profile,i)
                add(f'f{fov}_r{rotation}_image{variant}',m,native_plane(root,16,16,fov,(.25,.6,1.2)[variant]),images[variant])
    i=len(cases)+1
    m=metadata(i,2,0,'H',16,16,ALIAS,profile,100)
    native=native_plane(root,16,16,2)
    add('H_first_native',m,native)
    for delta in (20.,50.,100.,-100.,100.001,-100.001):
        reused=dict(m,tof_reused=True,pair_delta_ms=delta,rgb_timestamp_ns=m['tof_timestamp_ns']-round(delta*1e6),frame_id=len(cases)+1)
        add('H_reuse_'+str(delta),reused,native,accepted=abs(delta)<=100.)
    reused=dict(m,tof_reused=True,pair_delta_ms=50.,rgb_timestamp_ns=m['tof_timestamp_ns']-50_000_000,frame_id=len(cases)+1)
    add('H_changed_native_rejected',reused,bytes([native[0]^1])+native[1:],accepted=False)
    add('H_false_reuse_rejected',dict(reused,tof_reused=False),native,accepted=False)
    for fault in ('wrong_grid','missing_fov','conflicting_fov','bad_rotation','invalid_bit','low_snr'):
        i=len(cases)+1;m=metadata(i,2,0,'H',16,16,ALIAS,profile,i+200);native=native_plane(root,16,16,2)
        if fault=='wrong_grid':m.update(dtof_width=48,dtof_height=32,dtof_bytes=6144);native=native_plane(root,48,32,2)
        if fault=='missing_fov':m.pop('dtof_fov_correction')
        if fault=='conflicting_fov':m['tof_fov_correction']=1
        if fault=='bad_rotation':m['assist_input_rotation_degrees']=90
        if fault=='invalid_bit':native=native[:3]+b'\x02'+native[4:]
        if fault=='low_snr':native=b''.join(native[j:j+2]+b'\x05'+native[j+3:j+4] for j in range(0,len(native),4))
        add(fault,m,native,accepted=fault=='low_snr',uncorrected=fault=='low_snr')
    i=len(cases)+1;m=metadata(i,2,0,'H',16,16,ALIAS,profile,500)
    for key in ('tof_sequence','tof_timestamp_ns','rgb_timestamp_ns','tof_reused','pair_delta_ms','dtof_width','dtof_height','dtof_bytes','dtof_format','dtof_range_unit','dtof_source','dtof_coordinate','dtof_fov_correction','tof_warning_flags','pair_clock_uncertainty_ms','pair_clock_source'):
        m.pop(key,None)
    m.update(dtof_present=False,work_mode='host_rgb');add('H_rgb_only_no_invented_points',m,b'',uncorrected=True)
    current,hashes=check_batch(args.executable,root,candidate_runtime,ALIAS,cases,env,args.output)
    protected_load=None
    if args.runtime:
        code,packets,stderr=exchange(args.executable,root,candidate_runtime,ALIAS,[],env)
        assert code==0 and len(packets)==1, (code,stderr)
        protected_load=packets[0][0].get('model_loading',{})
        assert protected_load.get('kind')=='aes256-gcm-memory-v1', protected_load
        assert protected_load.get('plaintext_sha256_verified') is True, protected_load
        assert protected_load.get('plaintext_file_used') is False, protected_load
    reference,before=check_batch(args.reference_executable,root,runtime,ALIAS,cases,env,args.output)
    assert before==hashes,'Frozen depth/monocular/diagnostic output differs from source software'
    _,restarted=check_batch(args.executable,root,candidate_runtime,ALIAS,cases[:3],env,args.output)
    assert all(restarted[c['name']]==hashes[c['name']] for c in cases[:3]),'Stop/restart changed output'
    assert len({hashes[c['name']]['mono'] for c in cases[:3]})==3,'New RGB inputs were replayed'
    code,packets,_=exchange(args.executable,root,candidate_runtime,ALIAS,[],env,override=False)
    assert code!=0 and not packets,'Explicit native16 worker identity is required'
    removed=dict(cases[0],name='A_removed',metadata=dict(cases[0]['metadata'],assist_experiment='A'),accepted=False)
    code,packets,_=exchange(args.executable,root,candidate_runtime,ALIAS,[removed],env)
    assert code==0 and packets[-1][0].get('error') and not packets[-1][1],'Removed correction mode accepted'
    summary=dict(passed=True,protected_load=protected_load,cases=len(cases),bitwise_identical_outputs=len(hashes),
                 stop_restart_identical=True,varied_rgb_not_replayed=True,model_manifest_verified=True,
                 removed_modes_rejected=True,hardware_started=False,physical_accuracy_verified=False,
                 executable_sha256=sha256(args.executable),source_executable_sha256=sha256(args.reference_executable),results=current)
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k!='results'}))

if __name__=='__main__':main()
