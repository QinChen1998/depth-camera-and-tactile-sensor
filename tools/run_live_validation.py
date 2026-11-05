"""Explicit two-mode GUI validation with authorized saved-parameter restoration."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--serial',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--allow-saved-tof-switch',action='store_true',required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    executable=root/'x64/Release/触想工作台.exe'
    if args.output.exists():raise SystemExit('Use a new report filename')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    env={k:v for k,v in os.environ.items() if not k.startswith(('PYTHON','CONDA','RV_','DCTS_','QT_','QML'))}
    env.update(PATH=str(executable.parent)+';'+os.environ.get('SystemRoot','C:/Windows')+'/System32',
               QT_PLUGIN_PATH=str(executable.parent),QML_IMPORT_PATH=str(executable.parent/'qml'),
               QML2_IMPORT_PATH=str(executable.parent/'qml'),DCTS_LIVE_ENABLE='1',
               DCTS_ALLOW_SAVED_TOF_SWITCH='1',DCTS_LIVE_SERIAL=args.serial)
    with args.output.with_suffix('.process.log').open('wb') as log:
        child=subprocess.Popen([str(executable),'--live-validation',str(args.output)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
        print(json.dumps(dict(pid=child.pid,started_unix_ms=round(time.time()*1000),seconds_per_mode=300)),flush=True)
        code=child.wait()
    if args.output.exists():
        result=json.loads(args.output.read_text(encoding='utf-8-sig'))
        print(json.dumps({k:result.get(k) for k in ('passed','restore_succeeded','saved_sensor_files_restored_identically','final_owner_idle')},ensure_ascii=True))
    raise SystemExit(code)

if __name__=='__main__':main()
