# Windows SDK 同步及接口整理

目标 PR 为 `fix/SDK Sync and Interface Cleanup`，目标分支为 `main`。

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

当前验证使用本机离线环境。SDK 的 USB 接入、1200K 积分实读以及双绿网格写入需要板端实机验证。
