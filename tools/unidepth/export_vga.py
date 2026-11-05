"""Export the pinned Small weights and a centred VGA camera profile."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import warnings

os.environ.setdefault("HF_HUB_OFFLINE", "1")
sys.dont_write_bytecode = True

WEIGHTS = "93705cb3295dd7476b44911b8a55f5215bf74e8d5eccd27cecdb1b338270a648"
INTRINSICS = "8198bdfde1916c72e4d472e73448ebf7341fc080e38c14733e479ce6bc11b9ba"
WIDTH, HEIGHT, LEFT, TOP = 672, 504, 16, 12
FOCAL = 260 * 480 / 324


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-root", type=Path, required=True)
    parser.add_argument("--intrinsics", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True)
    sys.path.insert(0, str(args.model_root / "source"))
    import cv2
    import numpy as np
    import onnx
    import torch
    from onnx import TensorProto, numpy_helper
    from onnxconverter_common import float16
    from safetensors.torch import load_file
    from unidepth.models.unidepthv2.export import UniDepthV2ONNXcam
    from unidepth.utils.camera import Pinhole

    weight = out / "model.safetensors"
    shutil.copyfile(args.model_root / "weights/model.safetensors", weight)
    if digest(weight) != WEIGHTS or digest(args.intrinsics) != INTRINSICS:
        raise ValueError("Pinned weights or physical camera calibration changed")
    physical = json.loads(args.intrinsics.read_text(encoding="utf-8"))
    y, x = np.indices((HEIGHT, WIDTH), dtype=np.float64)
    rays = np.stack(((x - (WIDTH-1)/2) / FOCAL, (y - (HEIGHT-1)/2) / FOCAL, np.ones_like(x)), axis=-1)
    raw = cv2.projectPoints(rays.reshape(-1, 3), np.zeros(3), np.zeros(3),
                            np.array(physical["camera_matrix"]), np.array(physical["distortion_coefficients"]))[0]
    raw = raw.reshape(HEIGHT, WIDTH, 2)
    ij, frac = cv2.convertMaps(raw.astype(np.float32), None, cv2.CV_16SC2)
    u, v = ij[..., 0].astype(np.int32), ij[..., 1].astype(np.int32)
    if not (np.isfinite(raw).all() and u.min() >= 0 and u.max() < 2303 and v.min() >= 0 and v.max() < 1295):
        raise ValueError("672x504 remap uses pixels outside the real source")
    remap = out / "rgb672_rectify.bin"
    remap.write_bytes(struct.pack("<8s2I", b"NVMAP1\0\0", WIDTH, HEIGHT)
                      + (v * 2304 + u).astype("<u4").tobytes() + frac.astype("<u2").tobytes())

    torch.set_num_threads(4)
    torch.manual_seed(17)
    config = json.loads((args.model_root / "weights/config.json").read_text())
    config["training"]["export"] = True
    config["model"]["pixel_encoder"]["pretrained"] = None
    model = UniDepthV2ONNXcam(config).eval()
    model.load_state_dict(load_file(str(weight)), strict=True)
    k = torch.tensor([[[FOCAL, 0, WIDTH/2], [0, FOCAL, HEIGHT/2], [0, 0, 1]]])

    class HostGraph(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.model = model
            self.register_buffer("rays", Pinhole(K=k).get_rays((1, HEIGHT, WIDTH)).float())
            self.register_buffer("mean", torch.tensor([.485, .456, .406]).reshape(1, 3, 1, 1))
            self.register_buffer("std", torch.tensor([.229, .224, .225]).reshape(1, 3, 1, 1))

        def forward(self, rgb):
            points, _, _ = self.model((rgb.float() / 255.0 - self.mean) / self.std, self.rays)
            return points[:, 2:3, TOP:TOP+480, LEFT:LEFT+640]

    graph = HostGraph().eval()
    rgb = torch.randint(0, 256, (1, 3, HEIGHT, WIDTH), dtype=torch.uint8)
    source = out / "unidepthv2-small-rgb672-fp32.onnx"
    with torch.inference_mode():
        (out / "parity_rgb.bin").write_bytes(rgb.numpy().tobytes())
        (out / "parity_depth.bin").write_bytes(graph(rgb).numpy().astype("<f4").tobytes())
        torch.onnx.export(graph, (rgb,), str(source), input_names=["rgb"], output_names=["depth"],
                          opset_version=17, dynamo=False, do_constant_folding=True)

    original = onnx.load(str(source))
    # Repair the legacy atan2 symbolic's FP64 constants before mixed-precision conversion.
    def repair_tensor(tensor):
        if tensor.data_type == TensorProto.DOUBLE:
            tensor.CopyFrom(numpy_helper.from_array(numpy_helper.to_array(tensor).astype(np.float32), tensor.name))

    for tensor in original.graph.initializer:
        repair_tensor(tensor)
    for node in original.graph.node:
        for attribute in node.attribute:
            if attribute.type == onnx.AttributeProto.TENSOR:
                repair_tensor(attribute.t)
            if node.op_type == "Cast" and attribute.name == "to" and attribute.i == TensorProto.DOUBLE:
                attribute.i = TensorProto.FLOAT
    for value in (*original.graph.input, *original.graph.output, *original.graph.value_info):
        if value.type.tensor_type.elem_type == TensorProto.DOUBLE:
            value.type.tensor_type.elem_type = TensorProto.FLOAT
    onnx.checker.check_model(original, full_check=True)
    onnx.save(original, str(source))
    first = next(i for i, node in enumerate(original.graph.node)
                 if node.name == "/model/pixel_decoder/depth_layer/to_depth_hr/to_depth_hr.2/Conv")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        converted = float16.convert_float_to_float16(original, keep_io_types=True,
            node_block_list=[node.name for node in original.graph.node[first:]])
    types = {v.name: v.type.tensor_type.elem_type for v in (*converted.graph.value_info, *converted.graph.output)}
    for node in converted.graph.node:
        if node.op_type == "Cast" and types.get(node.output[0]) == TensorProto.FLOAT16:
            next(attr for attr in node.attribute if attr.name == "to").i = TensorProto.FLOAT16
    depth = converted.graph.output[0]
    if depth.type.tensor_type.elem_type != TensorProto.FLOAT:
        producer = next(node for node in converted.graph.node if depth.name in node.output)
        if producer.op_type != "Cast":
            raise ValueError("Expected explicit FP32 depth cast")
        next(attr for attr in producer.attribute if attr.name == "to").i = TensorProto.FLOAT
        depth.type.tensor_type.elem_type = TensorProto.FLOAT
        for value in converted.graph.value_info:
            if value.name == depth.name:
                value.type.tensor_type.elem_type = TensorProto.FLOAT
    onnx.checker.check_model(converted, full_check=True)
    candidate = out / "unidepthv2-small-rgb672-vga.onnx"
    onnx.save(converted, str(candidate))
    info = dict(schema=1, model="UniDepthV2-Small", weights_sha256=WEIGHTS,
                input=[1, 3, HEIGHT, WIDTH], input_dtype="uint8", output=[1, 1, 480, 640],
                output_dtype="float32", depth_unit="camera-Z_m_float32", camera_profile="wide",
                network_profile="rgb672_vga_crop_v1", network_shape=[HEIGHT, WIDTH],
                output_crop_ltrb=[LEFT, TOP, LEFT, TOP], rgb_padding_ltrb=[0, 0, 0, 0],
                rectified_intrinsics_integer_centres=[FOCAL, FOCAL, 319.5, 239.5],
                rectify_sha256=digest(remap), onnx_sha256=digest(candidate), source_onnx_sha256=digest(source),
                physical_intrinsics_sha256=INTRINSICS,
                source_bounds_xyxy=[*raw.min(axis=(0, 1)).tolist(), *raw.max(axis=(0, 1)).tolist()],
                precision="fp16-mixed-trial", conversion="ort-fp16-rgb672-vga-head-fp32-v1")
    (out / "model.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(info), flush=True)


if __name__ == "__main__":
    main()
