"""Stage the pinned native UniDepth fallback runtimes; never modify source weights."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA = '50d22cd33e126832793d905788a1efadeb4c9298de72cbfa634d49fdcdbc2dc1'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openvino-site', type=Path, required=True)
    parser.add_argument('--directml-site', type=Path, required=True)
    parser.add_argument('--int8-model', type=Path, required=True)
    parser.add_argument('--directml-notice', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'UniDepthRuntimeCompatible')
    parser.add_argument('--resume-incomplete', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    if (out / 'runtime.json').exists():
        raise SystemExit('A completed runtime already exists; use a new output directory.')
    out.mkdir(parents=True, exist_ok=args.resume_incomplete)
    files = {}

    def copy(source, name):
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        files[name] = digest(target)

    ov = args.openvino_site / 'openvino'
    for name in ('openvino.dll', 'openvino_c.dll', 'openvino_intel_cpu_plugin.dll',
                 'openvino_intel_gpu_plugin.dll', 'openvino_ir_frontend.dll',
                 'openvino_onnx_frontend.dll', 'tbb12.dll', 'tbbbind_2_5.dll',
                 'tbbmalloc.dll', 'tbbmalloc_proxy.dll', 'cache.json'):
        copy(ov / 'libs' / name, 'openvino/' + name)
    ort = args.directml_site / 'onnxruntime'
    for name in ('onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll'):
        copy(ort / 'capi' / name, 'directml/' + name)
    for name in ('LICENSE', 'ThirdPartyNotices.txt', 'Privacy.md'):
        copy(ort / name, 'licenses/onnxruntime/' + name)
    copy(args.directml_notice, 'licenses/DirectML/LICENSE.txt')
    for name in ('LICENSE-CODE.txt', 'ThirdPartyNotices.txt'):
        if (args.directml_notice.parent / name).is_file():
            copy(args.directml_notice.parent / name, 'licenses/DirectML/' + name)
    copy(ROOT / 'UniDepthRuntimeRgb672VgaFP16RTX/licenses/Proprietary-Model-NOTICE.txt',
         'licenses/model/Proprietary-Model-NOTICE.txt')
    for info in args.openvino_site.glob('*.dist-info'):
        if not info.name.startswith(('openvino-', 'tbb-')):
            continue
        for source in info.rglob('*'):
            if source.is_file() and ('licenses' in source.relative_to(info).parts or 'LICENSE' in source.name.upper() or 'NOTICE' in source.name.upper()):
                copy(source, 'licenses/' + info.name + '/' + source.relative_to(info).as_posix())
    headers = ROOT / 'dependency_sources/openvino/include/openvino/c'
    headers.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ov / 'include/openvino/c', headers, dirs_exist_ok=True)
    xml = (args.int8_model / 'model.xml').read_bytes()
    weights = (args.int8_model / 'model.bin').read_bytes()
    packed = out / 'decoder-int8.ovm'
    packed.write_bytes(b'DCTOVIR1' + struct.pack('<QQ', len(xml), len(weights)) + xml + weights)
    files[packed.name] = digest(packed)
    manifest = dict(schema=1, source_graph_sha256=ORIGINAL_SHA,
                    openvino_version='2026.4.1', ort_version='1.24.4',
                    int8=dict(file=packed.name, sha256=files[packed.name],
                              precision='int8_fp32_decoder',
                              conversion='nncf_3.4.0_decoder_128calibration_v1',
                              xml_sha256=hashlib.sha256(xml).hexdigest(),
                              weights_sha256=hashlib.sha256(weights).hexdigest()),
                    files=files)
    (out / 'runtime.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(output=str(out), int8=manifest['int8'], files=len(files))))


if __name__ == '__main__':
    main()
