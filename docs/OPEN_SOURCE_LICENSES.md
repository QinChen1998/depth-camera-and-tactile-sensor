# Model rights and third-party materials

This project retains the provenance and license material belonging to its sensor modules.

| Component | Local material |
|---|---|
| Provider-declared proprietary RGB672 VGA FP16 model and local ORT runtime | `UniDepthRuntimeRgb672VgaFP16RTX/model.json`, `runtime.json`, `licenses/` |
| ONNX Runtime 1.31.1, TensorRT-RTX EP 0.5.0 and NVIDIA runtime | Development and installed `UniDepthRuntimeRgb672VgaFP16RTX/licenses/` contain ORT and EP licenses/notices, NVIDIA acknowledgements and the model notice |
| ONNX Runtime 1.24.4 and DirectML 1.15.4 | `UniDepthRuntimeCompatible/licenses/onnxruntime/` and `licenses/DirectML/`; DirectML NuGet license, code license, third-party notices and signed-binary provenance are retained |
| OpenVINO 2026.4.1, oneTBB and oneDNN | `UniDepthRuntimeCompatible/licenses/openvino-2026.4.1.dist-info/` retains the wheel's complete license materials; pinned C headers and Apache notice are in `dependency_sources/openvino/` |
| Optional decoder INT8 derivative of the current UniDepth model | `UniDepthRuntimeCompatible/runtime.json` records the source and derivative hashes and NNCF conversion; the provider model notice is preserved in `licenses/model/`; this does not change or independently verify model ownership |
| Native RV worker, libssh2 and OpenSSL | `tools/rv_control/upstream.lock.json`, `x64/rv_control/<Configuration>/runtime/licenses/`, `implementation.json` |
| dToF tuner payload | `tools/dtof_tuner/payload/LICENSE-MIT.TXT`, `provenance.json`; the same files are hashed in the deployed worker manifest |
| VideoLAN VLC 3 managed RTSP/H.265 runtime | Installed `licenses/VLC/` contains the license, authors and matching release source location |
| FFmpeg 7.1.3 headers and provenance | `third_party/ffmpeg_7_1_3/` |
| Qt 6.11.1 and its QML/runtime dependencies | The matching Qt installation; `windeployqt` copies the binaries and plugins required by this application |
| Android SDK Platform-Tools (USB enrollment) | External ADB runtime in `platform-tools/` alongside the executable, with the distribution's `NOTICE.txt` and `source.properties` |

The deployed model is recorded as `LicenseRef-Proprietary`, following the model provider's declaration of architecture redesign and retraining. Its usage notice is `UniDepthRuntimeRgb672VgaFP16RTX/licenses/Proprietary-Model-NOTICE.txt`. Existing model names, source/conversion identifiers and hashes remain as technical traceability records; this notice update does not alter or independently verify the model's training provenance. Any incorporated third-party source code, pretrained weights or training data remain subject to their applicable terms.

USB enrollment also provisions the pinned dToF inspection/engineering runtime when it is absent and the verified board is idle, including its MIT license and provenance file. Existing matching runtime assets and their original provenance records are retained. The ordinary main window exposes host-assisted H imaging and two native 48x32 ToF modes; provisioning these tools does not start an experiment or write saved sensor settings.

The SPI driver build helper `tools/rv_control/prepare_spi_driver.py` copies the local vendor source into an isolated build directory and retains its notices and source SHA256 manifest. The wrapper adds explicit software ray-table selection and an atomic per-generation calibration receipt, preserving the independently read sensor FOV. It does not modify vendor originals, model weights, sensor EEPROM or saved settings. The existing exact warning policy is retained.

The installer runtime manifest preserves the SHA256 identities of the model notice and bundled third-party materials. Before external delivery, confirm the packaged NVIDIA binaries against the [TensorRT for RTX agreement](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/reference/sla.html) and [CUDA 12.9 redistribution terms](https://docs.nvidia.com/cuda/archive/12.9.1/eula/index.html). Device Manager now provisions native dToF/H.265 capture only. The LiteSR ciphertext, ARM model executables and bundled RKNN runtime were removed on 2026-11-09; host H protection and its notices are unchanged. The board camera stack and standard C/C++ runtime continue to come from the firmware. Historical LiteSR/Tiny delivery records describe their original packages and do not imply current redistribution.

Qt/FFmpeg and Microsoft runtime binaries retain the terms of the respective installed dependencies. No new application-code license is assigned by this migration. Mechanical-hand, MediaPipe and retargeting components are not part of this application.

## Retired Tiny NV12 128×80 deployment (2026-11-06)

Removed completely from the current host source/runtime on 2026-11-08, including dedicated camera migration code; historical v1.1.6 packages and their notices remain unchanged in the local validation archive. This section records historical provenance only. The independent Tiny package was derived from the locally maintained `E:/WORK/dToF` TinyNV12 implementation and the pinned checkpoint/RKNN identities in `E:/WORK/dToF/models/tiny_nv12_128x80/manifest.json`. Its dataset/training provenance remains recorded in `E:/WORK/dToF/docs/TINY_NV12_128X80.md`; adding encryption does not change or independently verify that provenance. The ARM runtime used the firmware RKNN/OpenSSL libraries and the locally deployed RKNN runtime already documented above. It did not add a downloaded model or redistribute training data.
