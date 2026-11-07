"""Prepare a vendor-source copy with explicit software ray-table calibration."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import shlex


def linux_path(path):
    value = str(path.resolve()).replace('\\', '/')
    return '/mnt/' + value[0].lower() + value[2:] if len(value) > 1 and value[1] == ':' else value


def prepare(source, output):
    if output == source or source in output.parents:
        raise ValueError('Build output must be outside the read-only vendor source')
    if output.exists():
        raise ValueError('Use a new output directory; vendor input is never overwritten')
    required = ('driver_main.c', 'main.c', 'tmf8829.c', 'tmf8829_driver.c',
                'tmf8829_shim.c', 'tmf8829_frameparser.c', 'tmf8829_ipc.c', 'parent_watch.h')
    for name in required:
        if not (source / name).is_file():
            raise ValueError('Missing vendor source: ' + name)
    output.mkdir(parents=True)
    copied = output / 'source'
    shutil.copytree(source, copied)
    wrapper = copied / 'driver_main.c'
    text = wrapper.read_text(encoding='utf-8')
    marker = '    unsigned char *c = driver->config;'
    if text.count(marker) != 1:
        raise ValueError('Unrecognized vendor register readback wrapper')
    text = text.replace(marker, marker + '\n'
        '    const unsigned sensor_fov=c[0x78-0x22];\n'
        '    const int processing_fov=dcts_processing_fov(sensor_fov,getenv("DCTS_PROCESSING_FOV"));\n'
        '    if (processing_fov<0) return -1;\n'
        '    chip->frameParser.fovCorrection=(unsigned char)processing_fov;\n'
        '    fprintf(stderr,"HOST_CALIBRATION sensor_fov=%u processing_fov=%d temporary=%d\\n",\n'
        '            sensor_fov,processing_fov,sensor_fov!=(unsigned)processing_fov);')
    marker = '    if (fclose(f) != 0 || rename(tmp, path) != 0) return -1;'
    if text.count(marker) != 1:
        raise ValueError('Unrecognized vendor readback publication')
    text = text.replace(marker, marker + '\n'
        '    if (dcts_write_calibration(path,getenv("CANONICAL_RUN_ID"),sensor_fov,\n'
        '                               (unsigned)processing_fov)!=0) return -1;')
    wrapper.write_bytes(text.encode('utf-8'))
    project = Path(__file__).resolve().parents[2]
    for name in ('SessionFov.h', 'DriverWarningPolicy.h'):
        shutil.copyfile(project / 'tools/rv_control' / name, copied / name)
    quote = shlex.quote
    files = ('driver_main.c', 'tmf8829.c', 'tmf8829_driver.c', 'tmf8829_shim.c',
             'tmf8829_frameparser.c', 'tmf8829_ipc.c')
    command = ('"$cc" -std=gnu11 -O2 -Wall -Wextra -mcpu=cortex-a7 -mfpu=neon-vfpv4 '
               '-mfloat-abi=hard -DTMF8829_DIAG08 -DTMF_HOST_SIGNAL '
               '-include "$src/DriverWarningPolicy.h" -include "$src/SessionFov.h" -I "$src" '
               + ' '.join('"$src/' + name + '"' for name in files)
               + ' -lm -pthread -o "$out/tof_driver"\n')
    script = ('#!/bin/bash\nset -euo pipefail\n'
              'cc=${RV1103_CC:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc}\n'
              'src=' + quote(linux_path(copied)) + '\nout=' + quote(linux_path(output)) + '\n'
              + command
              + '/usr/bin/cc -std=c11 -D_DEFAULT_SOURCE -I "$src" ' + quote(linux_path(project / 'tests/SessionFovTests.c'))
              + ' -o "$out/session-fov-tests"\nCANONICAL_WARNING_POLICY=observe-0x08 "$out/session-fov-tests"\n'
              + 'file "$out/tof_driver"\nsha256sum "$out/tof_driver"\n')
    (output / 'build.sh').write_bytes(script.encode('utf-8'))
    hashes = {str(p.relative_to(source)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in source.rglob('*') if p.is_file()}
    (output / 'provenance.json').write_text(json.dumps({
        'vendor_source_sha256': hashes, 'changes': ['explicit per-session software ray-table selection',
        'actual sensor FOV retained in verified readback',
        'atomic generation-bound calibration receipt before publishing', 'existing exact warning policy retained'],
        'models_changed': False, 'persistent_sensor_settings_written': False,
    }, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source.resolve(), args.output.resolve())
