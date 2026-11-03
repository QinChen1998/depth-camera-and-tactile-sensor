# 触想工作台

触想工作台是一款面向磁触觉传感器和 RV1103 深度相机的 Windows 开源上位机。

- 磁触觉支持串口采集、模拟、三维力显示和曲线。
- 摄像头原视频支持 H.265 播放。
- 深度图支持板端 LiteSR，以及使用 RGB576 FP16 和 16×16 ToF 的上位机辅助推理（方案 H）。

## 构建

需要 Windows x64、Visual Studio 2022（MSVC v143、Windows SDK 10.0.22621.0）、Qt 6.11.1 MSVC x64（含 Qt Quick 3D）和 Qt VS Tools 提供的 QtMsBuild。

在 PowerShell 中运行，按实际安装位置填写 Qt 路径：

```powershell
./tools/build_desktop.ps1 -Configuration Release -QtRoot 'C:/Qt/6.11.1/msvc2022_64' -SkipWorker
```

产物位于 `x64/Release/`。上述命令可构建上位机并验证界面。相机实机功能还需要 RV 控制 worker。准备 OpenSSL 3.6.4 x64 开发文件后，用 `tools/rv_control/prepare_sdk.ps1 -OpenSslRoot <目录>` 准备 libssh2 SDK，再去掉 `-SkipWorker` 构建。设备连接需在设备管理中登记设备。离线启动可在运行前设置 `RV_TEST_OFFLINE_ONLY=1`。

## 模型与许可

上位机源码以 [GPL-3.0-only](LICENSE) 发布。第三方组件保留各自的版权和许可，详见[第三方许可记录](docs/OPEN_SOURCE_LICENSES.md)。

RGB576 FP16 模型通过单独授权提供。方案 H 需要兼容的 NVIDIA GPU 和获授权的模型运行包。板端 LiteSR 使用设备侧部署的模型。公开仓库提供上位机代码、模型接入代码、构建脚本和所需的项目资源。
