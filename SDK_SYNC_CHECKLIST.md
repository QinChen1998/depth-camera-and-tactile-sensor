# Windows SDK 同步及接口整理

本文记录 Windows SDK 2.2.2 接入、Depth Pro 2-S 单模型和相关验证。[GitHub PR #3](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/pull/3) 已合并到 `main` 的 `9e2c055`。当前操作入口见 [测试文档](docs/TESTING.md)。

- [x] 确认原工作区在 `main`，初始 HEAD 为 `4aba887`。已有未跟踪的 `Log/`。
- [x] 核对 GitHub `main` 同为 `4aba887`，已创建 `fix/sdk-sync-and-interface-cleanup` 分支。
- [x] 已创建以 `main` 为目标的 [GitHub PR #3](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/pull/3)。
- [x] 从本机 `release` 目录选定 `CXWLDepthSDK-2.2.2-win64.zip`，已核对 ZIP 的 SHA-256。
- [x] 核对 SDK ABI v2、设备样例、分流回调与 F6 几何。官方离线 SelfTest 通过，报告位于 `x64/validation/sdk-sync-20261124/sdk-self-test.json`。
- [x] 接入 SDK 的设备枚举、连接和采集，设备管理已使用 SDK 样例的操作流程。
- [x] 接通 SDK RGB、原生深度和同帧质量图，沿用 UniDepthV2 与 Depth Pro 2-S 的 F6 校正和推理链路。
- [x] 已移除两个 600K 菜单选项。旧普通和高精度方案分别迁移至对应 1200K 方案。
- [x] 接入 SDK 的 SC3336 双绿网格检查、计算、部署与结果反馈。
- [x] 保留「显示 ToF 测点」「ToF / RGB 对照」「录制诊断」及保存和测量采集，Release 首轮界面专项通过。
- [x] 点云和磁触觉三维视口使用 Qt Widgets，更新工程、翻译、资源和安装依赖。
- [x] 移除 `Depth Pro 2-B (ToF G)`、`Depth Pro 2-B (ToF B, reuse)` 及其共用的本地运行包、代码和构建入口。
- [x] 构建 Release 和 Debug。两个配置各通过 8 组离线测试、1,547 项检查。报告位于 `x64/validation/sdk-sync-20261124/` 下的 `Release-final` 和 `Debug-final`。
- [x] 检查 Windows SDK 从空目录解包、安装运行依赖和许可文件。核对两个配置均导入 `CxwlDepthSdk.dll`，部署目录均无 Qt Quick 或 QML 运行文件。
- [x] 核对提交范围与脱敏，暂存区仅包含本次源码、测试、文档、转换后的触觉网格和翻译资源。

最初提交完成了本机离线验证。USB 接入和 1200K 采集的实机结果见下方。双绿网格写入尚未进行本轮实机验收。

## Visual Studio 构建入口复核

- [x] 在找不到 `pwsh` 的构建环境复现 `MSB3073` 和退出码 `9009`。
- [x] 切换到 Windows 自带的 PowerShell。Visual Studio 已在空 SDK 目录完成 Debug 构建，错误的 SDK ZIP 摘要仍会被拒绝。
- [x] 仅提供 Debug 控制程序且没有路径覆盖文件时，离线调参专项通过 338 项检查。
- [x] 在找不到 `pwsh` 的环境中使用 Visual Studio 重新生成 `Debug|x64` 和 `Release|x64`。两个配置各通过 8 组离线测试、1,547 项检查。

复核日志和测试报告位于 `x64/validation/vs-sdk-powershell-20261124/`。

## 实机采集故障复核

- [x] SDK 2.2.2 原版 QuickStart 在当前 USB 相机收到 2304×1296 RGB 和 48×32 ToF，重连后两条流均恢复。
- [x] 在 Visual Studio 运行的 Release 上位机上复现 B 档无数据。SDK 按绝对路径在 `cxwl-depth-sdk/` 中加载 FFmpeg，构建之前只将五个 DLL 放在程序目录。
- [x] 修复部署脚本，在干净输出目录核对两个位置的五个 DLL 均与已锁定的 SDK 包哈希相符。
- [x] 在原有 Release 窗口确认 2304×1296 RGB 约 30 FPS、B 档 480×640 深度约 29 FPS 和点云输出。两个板端档均显示 48×32 深度约 14.9 FPS，实际寄存器回读均为 `iterations=1200`、`verified=true`。原始回读同时记录 `exact_match=false`。
- [x] 在 Release 上用「立即重连」恢复 RGB、B 档深度及点云。按「停止」后，窗口清空且板端的采集会话已释放。已恢复原来的 B 档选项。
- [x] Visual Studio 生成的 Debug 实机点云检查返回 0。G 档收到 304 帧 RGB、25 帧辅助深度。板端普通档收到 34 帧 RGB、18 帧 ToF，高精度档收到 30 帧 RGB、18 帧 ToF。三档均确认实际绘制，视角保持，停止后没有迟到的深度帧。
- [x] 修正实机检查的切档判据。G 切到普通档应复用板端采集代次。切到高精度应获得新的采集代次。两次切档均须更新上位机的显示代次并清除旧点云。

实机读数与日志位于 `x64/validation/sdk-live-capture-20261124/`。Release 窗口读数见 `release-ui-observations.json`，Debug 完整结果见 `debug-point-cloud-live-verified.json`。

## 上位机单模型与 480×640 输入

- [x] 核对现有 640×480 引擎、Depth Pro 2-S 权重指纹和 F6 校正网格。竖幅输入采用 F6 现有的 360×480 ToF 重叠区域，等比采样为 480×640。
- [x] 生成 480×640 的 Depth Pro 2-S 引擎，统一 RGB 取样、模型输出与 ToF 投影的网格。30 帧同输入对照覆盖 0° 和 180°，worker 与直接运行同一 TensorRT 引擎的深度逐像素一致。
- [x] 只保留复用 ToF 的上位机辅助推理。更新菜单名称、旧设置迁移、运行依赖和交付入口。旧运行包移入本机验证目录下的 `retired-runtimes/`。
- [x] 核对原视频、深度、点云、ToF 测点及诊断的帧身份和标定。Release 和 Debug 各通过 8 组离线专项、1,531 项检查，含竖幅诊断数据写入和单帧点云替换。
- [x] Visual Studio 成功构建 Release 和 Debug。在当前 RV1103 上实测三个档位均收到视频、深度和点云。Release 辅助档采集 30 秒，900 帧 RGB 对应 900 帧经 ToF 配对校正的 480×640 深度。通过主窗口的「立即重连」恢复三路显示，停止后窗口清空，实机专项未收到迟到帧。
- [x] 移除点云叠加按钮和多帧缓存。界面专项通过单帧点状、网格替换及超时清除检查。
- [x] 验证安装负载的 SDK、五个 FFmpeg DLL 和板端采集文件。负载不包含旧模型及推理进程。Depth Pro 2-S 使用本机工作区的运行包。

本轮验证位于 `x64/validation/depthpro2-portrait-20261124/`。实机报告为 `debug-point-cloud-live.json`、`release-host-live-final.json` 与 `release-ui-observations.json`。

## 上位机辅助推理分段计时

- [x] 在本地验证目录用同一 480×640 引擎构建计时 worker 副本，分别记录 CPU 预处理、CUDA 双向传输、TensorRT 流及输出整理耗时。
- [x] 当前 RV1103 实测 60 秒，输出 1,800 帧深度，平均 30.00 FPS。1,800 帧匹配到 ToF，748 帧应用了有效校正。实机专项确认停采和板端配置一致。
- [x] 同一 worker 按 30 FPS 输入和连续输入各测 900 帧。推理平均耗时依次为实机 11.84 ms、30 FPS 对照 11.31 ms、连续对照 5.01 ms。TensorRT 流区间依次为 10.09、9.42、3.34 ms。

计时报告和逐帧记录位于 `x64/validation/depthpro2-stage-profile-20261124/`，汇总为 `stage-summary.json`。

## 辅助推理不可用时的默认档位

- [x] 统一启动默认档位、设备检查和辅助档置灰逻辑。辅助推理初始化失败时使用同一采集会话切换到普通板端档。
- [x] 核对无兼容显卡的预检入口。离线验证运行包不可用时自动选中普通板端档并置灰辅助档，保留已保存的板端高精度与辅助档偏好。
- [x] Visual Studio 的 Debug 和 Release 均构建成功，各通过 8 组离线测试、1,539 项检查。
- [x] 在当前 RV1103 实测缺少运行包、引擎初始化失败及辅助推理可用三种情况。前两种无需手动切档，自动用普通板端档出图。辅助档可用时三个档位均收到原视频、深度和点云。三个场景均正常停采。

报告位于 `x64/validation/depthpro2-board-fallback-20261124/`。无辅助推理的实机验收分别使用空运行包目录和失效引擎副本。
