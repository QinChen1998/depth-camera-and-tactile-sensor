# Model rights and third-party materials

This project retains the provenance and license material belonging to its sensor modules.

| Component | Local material |
|---|---|
| Provider-declared proprietary RGB576 FP16 model and local ORT/CUDA runtime | `UniDepthRuntimeWide576FP16/model.json`, `runtime.json`, `licenses/` |
| ONNX Runtime 1.31.1, TensorRT-RTX EP 0.5.0 and NVIDIA runtime | Development `UniDepthRuntimeWide576FP16RTX/licenses/` and installed `UniDepthRuntimeWide576FP16/licenses/` contain ORT and EP licenses/notices, NVIDIA acknowledgements and the model notice |
| Native RV worker, libssh2 and OpenSSL | `tools/rv_control/upstream.lock.json`, `x64/rv_control/<Configuration>/runtime/licenses/`, `implementation.json` |
| dToF tuner payload | `tools/dtof_tuner/payload/LICENSE-MIT.TXT`, `provenance.json`; the same files are hashed in the deployed worker manifest |
| VideoLAN VLC 3 managed RTSP/H.265 runtime | Installed `licenses/VLC/` contains the license, authors and matching release source location |
| FFmpeg 7.1.3 headers and provenance | `third_party/ffmpeg_7_1_3/` |
| Qt 6.11.1 and its QML/runtime dependencies | The matching Qt installation; `windeployqt` copies the binaries and plugins required by this application |
| Android SDK Platform-Tools (USB enrollment) | External ADB runtime in `platform-tools/` alongside the executable, with the distribution's `NOTICE.txt` and `source.properties` |

The deployed model is recorded as `LicenseRef-Proprietary`, following the model provider's declaration of architecture redesign and retraining. Its usage notice is `UniDepthRuntimeWide576FP16/licenses/Proprietary-Model-NOTICE.txt`. Existing model names, source/conversion identifiers and hashes remain as technical traceability records; this notice update does not alter or independently verify the model's training provenance. Any incorporated third-party source code, pretrained weights or training data remain subject to their applicable terms.

The installer runtime manifest preserves the SHA256 identities of the model notice and bundled third-party materials. Before external delivery, confirm the packaged NVIDIA binaries against the [TensorRT for RTX agreement](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/reference/sla.html) and [CUDA 12.9 redistribution terms](https://docs.nvidia.com/cuda/archive/12.9.1/eula/index.html). Device Manager installs the encrypted LiteSR RKNN payload and the matching F1/F2 executable. The board executable links to the firmware-provided RKNN runtime and OpenSSL 1.1 libcrypto. The model payload contains the encrypted model and matching ARM programs. The V1.2.0 USB setup additionally carries the previously validated ARM/uClibc RKNN runtime as an application-local dependency under `/root/r144_depth/`. OpenSSL and the camera stack continue to come from the firmware.

Qt/FFmpeg and Microsoft runtime binaries retain the terms of the respective installed dependencies. No new application-code license is assigned by this migration. Mechanical-hand, MediaPipe and retargeting components are not part of this application.
