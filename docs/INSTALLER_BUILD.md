# DCTS 1.1.2 trial build

The application, control worker and installer scripts in this archive are the source used for the 1.1.2 trial setup. The application uses GPL-3.0-only. See the root LICENSE and the notices shipped with the setup. Install Windows and Qt dependencies as described in README.md.

The open application and worker can be built with tools/build_desktop.ps1. The trial setup uses the installer configuration in tools/installer/build.ps1. Use PowerShell 7, VS 2022 v143, Windows SDK 10.0.22621.0, Qt 6.11.1 MSVC x64 with its matching Src and SBOM files, and Inno Setup 6.7.3. Prepare the worker SDK with tools/rv_control/prepare_sdk.ps1.

For the protected trial configuration, obtain an authorized RGB576 FP16 model pack and the matching ONNX Runtime 1.31.1 / TensorRT-RTX runtime and SDK. Place the model inputs in UniDepthRuntimeWide576FP16, then pass the verified dependency directories to the installer script. The script creates new model-encryption material for that build.

```powershell
./tools/installer/build.ps1 -Version 1.1.2 `
  -BuildDirectory (Join-Path (Get-Location) 'x64/validation/trial') `
  -QtRoot $qtRoot -PlatformTools $adbRoot -VlcRoot $vlcRoot `
  -TensorRtRuntime $rtxRuntime -TensorRtSdk $rtxSdk `
  -Iscc $iscc -ChineseMessages $chineseMessages
```

The installer is written to the build directory's delivery folder. The application source archive excludes the proprietary model inputs and production key material. The encrypted model and separately licensed runtime are covered by the notices installed with the trial package. Versioned dependency-source locations are recorded in tools/installer/Corresponding-Source.txt and in the installed third-party notices.

The Windows installer stages the board import bundle from
`tools/litesr_board/protected/payload/`. The two shell scripts are in this
source archive. Supply the matching protected `model.dctsm` and the two
ARM executables `litesr-fov1.bin` and `litesr-fov2.bin` as one set. The
board runtime source and build script are under
`tools/litesr_board/protected/`. Rebuilding that set requires the board
cross toolchain, RKNN SDK, firmware-compatible libcrypto, and authorized
model build inputs. Use the same protected model envelope and key material
for both FOV variants. The one-click installer chooses the executable by
the registered board calibration and verifies every shipped file.
