"""Run selected desktop regressions with isolated settings and no hardware."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import time

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--configuration',choices=('Release','Debug'),required=True)
    parser.add_argument('--qt-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--local-runtime',action='store_true')
    parser.add_argument('--flags',nargs='*')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    executable=root/'x64'/args.configuration/'Depth Camera and Tactile Sensor.exe'
    args.output.mkdir(parents=True,exist_ok=False)
    env={k:v for k,v in os.environ.items() if not k.startswith(('PYTHON','CONDA','RV_','DCTS_'))}
    env.update(PATH=str(args.qt_root/'bin')+';'+os.environ.get('SystemRoot','C:/Windows')+'/System32',
               QT_PLUGIN_PATH=str(args.qt_root/'plugins'),QML_IMPORT_PATH=str(args.qt_root/'qml'),
               QML2_IMPORT_PATH=str(args.qt_root/'qml'),RV_TEST_OFFLINE_ONLY='1')
    if args.local_runtime:
        env.update(PATH=str(executable.parent)+';'+os.environ.get('SystemRoot','C:/Windows')+'/System32',
                   QT_PLUGIN_PATH=str(executable.parent),QML_IMPORT_PATH=str(executable.parent/'qml'),
                   QML2_IMPORT_PATH=str(executable.parent/'qml'))
    flags=args.flags or ['port-ui-smoke-test','rv-tuner-test','tactile-serial-test','tactile-visualization-test',
                        'tactile-3d-test','tactile-render-test','rv-assist-recovery-test','rv-usb-enrollment-test']
    results=[]
    for flag in flags:
        output=args.output/(flag+'.txt');started=time.monotonic()
        print('RUN '+args.configuration+' '+flag,flush=True)
        try:
            process=subprocess.run([str(executable),'--'+flag,str(output)],cwd=root,env=env,
                                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=150)
            (args.output/(flag+'.log')).write_bytes(process.stdout)
            text=output.read_text(encoding='utf-8-sig',errors='replace') if output.exists() else ''
            row=dict(flag=flag,exit_code=process.returncode,seconds=round(time.monotonic()-started,2),
                     passed=process.returncode==0,checks=text.count('PASS'),
                     failures=[line for line in text.splitlines() if re.search(r'\bFAIL\b',line)])
        except subprocess.TimeoutExpired as error:
            (args.output/(flag+'.log')).write_bytes(error.stdout or b'')
            row=dict(flag=flag,passed=False,timeout=True,seconds=150)
        results.append(row);print(json.dumps(row),flush=True)
    summary=dict(passed=all(row['passed'] for row in results),configuration=args.configuration,local_runtime=args.local_runtime,
                 hardware_started=False,results=results)
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    raise SystemExit(0 if summary['passed'] else 1)

if __name__=='__main__':main()
