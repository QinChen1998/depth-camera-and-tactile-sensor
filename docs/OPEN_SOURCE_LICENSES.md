# Model rights and third-party materials

This project retains the provenance and license material belonging to its sensor modules.

| Component | Local material |
|---|---|
| Depth Pro 2-S local research runtime | `DepthPro2Runtime/Apple-Model-LICENSE.txt` and `runtime.json`. The runtime contains the local TensorRT plan and NVIDIA runtime libraries. |
| Historical native RV worker, libssh2 and OpenSSL | `tools/rv_control/upstream.lock.json`, `x64/rv_control/<Configuration>/runtime/licenses/`, `implementation.json` |
| dToF tuner payload | `tools/dtof_tuner/payload/LICENSE-MIT.TXT`, `provenance.json`; the same files are hashed in the deployed worker manifest |
| FFmpeg 7.1.3 headers and provenance | `third_party/ffmpeg_7_1_3/` |
| Qt 6.11.1 Widgets, multimedia and serial runtime | `windeployqt` collects the required DLLs and plugins. The installer includes the corresponding notices and SBOM files. |
| CXWL Depth Camera SDK for Windows x64, 3.1.0 | The app dynamically links `CxwlDepthSdk.dll`. The Windows runtime, product license and third-party notices come from the official SDK package. |
| Adapted CXWL SDK Qt Viewer examples | The MIT notice in `third_party/cxwl-depth-sdk-example/LICENSE` applies to the adapted capture client, device manager and point-cloud canvas. |
| Orbbec SDK v2 (Gemini 335 measurement) | Uses the separately installed Windows SDK and its own license materials |

Retired UniDepth model, ONNX Runtime, DirectML and OpenVINO notices remain with their historical runtime copies. The desktop installer stages SDK capture components. The local Depth Pro 2-S engine is subject to the Apple research model license.

USB enrollment uses the camera's preinstalled device service through SDK 3.1.0. The SDK product license and third-party notices travel with the dynamically linked Windows runtime in `cxwl-depth-sdk/`.

The SPI driver build helper `tools/rv_control/prepare_spi_driver.py` copies the local vendor source into an isolated build directory and retains its notices and source SHA256 manifest. The wrapper adds explicit software ray-table selection and an atomic per-generation calibration receipt, preserving the independently read sensor FOV. It does not modify vendor originals, model weights, sensor EEPROM or saved settings. The product driver accepts the informational bits `0x08` and `0x10` individually and together under `observe-0x08`.

The local Depth Pro 2-S research runtime manifest records the SHA-256 identities of its engine, worker, bundled libraries and model license. The installer collects notices for the SDK, Qt, FFmpeg and Visual C++ runtime. The board camera stack and standard C/C++ runtime come from the firmware.

The former experimental LiteSR port and its RKNN/OpenSSL payload were retired from the product on 2026-11-12.

Qt/FFmpeg and Microsoft runtime binaries retain the terms of the respective installed dependencies. No new application-code license is assigned by this migration. Mechanical-hand, MediaPipe and retargeting components are not part of this application.

## Retired Tiny NV12 128×80 deployment (2026-11-06)

Removed completely from the current host source/runtime on 2026-11-08, including dedicated camera migration code; historical v1.1.6 packages and their notices remain unchanged in the local validation archive. This section records historical provenance only. The independent Tiny package was derived from the locally maintained `E:/WORK/dToF` TinyNV12 implementation and the pinned checkpoint/RKNN identities in `E:/WORK/dToF/models/tiny_nv12_128x80/manifest.json`. Its dataset/training provenance remains recorded in `E:/WORK/dToF/docs/TINY_NV12_128X80.md`; adding encryption does not change or independently verify that provenance. The ARM runtime used the firmware RKNN/OpenSSL libraries and the locally deployed RKNN runtime already documented above. It did not add a downloaded model or redistribute training data.
