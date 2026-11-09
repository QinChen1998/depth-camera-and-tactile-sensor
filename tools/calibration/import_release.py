"""Import the pinned 20261109 calibration and derive the existing H virtual camera.

No hardware access and no model conversion. The frozen graph still receives the
same centred 672x504 pinhole camera and returns the same 640x480 camera-Z grid.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

import cv2
import numpy as np

RELEASE = "RV1103_SC3336_TMF8829_20261109"
CALIBRATION_SHA = "c7836f67c2d4645389634b03e7e93518b13c6ecda3292c8d90319d24136d3dfa"
GEOMETRY_SHA = "4ca13f0d66987cfd84453a2aacf7ae086b7a730025fe3c90cdf3c6bc88782cc7"
FILES = ("calibration.json", "intrinsics.json", "extrinsics.json", "projection.bin", "M48_geometry.bin", "manifest.json")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def generate(source, output):
    source, output = Path(source), Path(output)
    model = json.loads((source / "calibration.json").read_text())
    if model["release_id"] != RELEASE or sha(source / "calibration.json") != CALIBRATION_SHA or sha(source / "M48_geometry.bin") != GEOMETRY_SHA:
        raise ValueError("The release differs from the user-selected calibration")
    manifest = json.loads((source / "release_index.json").read_text())
    for name in FILES:
        if sha(source / name) != manifest["files"][name]["sha256"]:
            raise ValueError("Release checksum mismatch: " + name)
    output.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        shutil.copyfile(source / name, output / name)
    width, height, focal = 672, 504, 260.0 * 480 / 324
    y, x = np.indices((height, width), dtype=np.float64)
    rays = np.stack(((x - 335.5) / focal, (y - 251.5) / focal, np.ones_like(x)), -1)
    raw = cv2.projectPoints(rays.reshape(-1, 3), np.zeros(3), np.zeros(3),
                            np.array(model["camera"]["K"]), np.array(model["camera"]["D"]))[0].reshape(height, width, 2)
    ij, frac = cv2.convertMaps(raw.astype(np.float32), None, cv2.CV_16SC2)
    u, v = ij[..., 0].astype(np.int32), ij[..., 1].astype(np.int32)
    if not (np.isfinite(raw).all() and u.min() >= 0 and v.min() >= 0 and u.max() < 2303 and v.max() < 1295):
        raise ValueError("The H virtual camera samples outside the physical camera")
    mapping = output / "rgb672_rectify.bin"
    mapping.write_bytes(struct.pack("<8s2I", b"NVMAP1\0\0", width, height)
                       + (v * 2304 + u).astype("<u4").tobytes() + frac.astype("<u2").tobytes())
    contract = dict(release_id=RELEASE, calibration_sha256=CALIBRATION_SHA,
                    physical_intrinsics_sha256=sha(output / "intrinsics.json"),
                    geometry_sha256=GEOMETRY_SHA, rectify_sha256=sha(mapping),
                    source_size=[2304, 1296], input_size=[672, 504], output_size=[640, 480],
                    rectified_intrinsics_integer_centres=[focal, focal, 319.5, 239.5],
                    source_bounds_xyxy=[*raw.min(axis=(0, 1)).tolist(), *raw.max(axis=(0, 1)).tolist()],
                    native_fov=6, native_order=True, mapping_applied_once=True,
                    graph_changed=False, depth_unit="camera-Z_m_float32",
                    files={name: sha(output / name) for name in (*FILES, "rgb672_rectify.bin")})
    (output / "host_contract.json").write_text(json.dumps(contract, indent=2) + "\n", encoding="utf-8")
    return contract


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(generate(args.source, args.output), indent=2))
