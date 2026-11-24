# Windows SDK 同步及接口整理

目标 PR 为 `fix/SDK Sync and Interface Cleanup`，目标分支为 `main`。

- [x] 确认原工作区在 `main`，初始 HEAD 为 `4aba887`。已有未跟踪的 `Log/`。
- [x] 核对 GitHub `main` 同为 `4aba887`，已创建 `fix/sdk-sync-and-interface-cleanup` 分支。
- [ ] 创建以 `main` 为目标的 GitHub PR。
- [x] 从本机 `release` 目录选定 `CXWLDepthSDK-2.2.2-win64.zip`，已核对 ZIP 的 SHA-256。
- [ ] 核对 SDK API、源码和样例，确认与项目数据链路的对接点。
- [ ] 接入 SDK 的设备枚举、连接和采集，简化 RV1103 设备管理。
- [ ] 接通 SDK 的 RGB、原生深度和同帧元数据，保留 UniDepthV2 及保留的 Depth Pro 2 方案。
- [ ] 板端普通和高精度档均固定为 1200K，迁移两个旧 600K 选项。
- [ ] 按 SDK 实现同步双绿网格计算及其界面入口。
- [ ] 核对「显示 ToF 测点」「ToF / RGB 对照」「录制诊断」及已有保存和测量采集。
- [ ] 迁移依赖 Qt Quick 3D 的显示，更新工程和安装依赖。
- [ ] 移除 `Depth Pro 2-B (ToF G)`、`Depth Pro 2-B (ToF B, reuse)` 及其共用的 `DepthPro2BRuntime`、代码和构建引用。
- [ ] 构建 Release 和 Debug，运行相关离线测试并检查安装输入。
- [ ] 审查提交范围和脱敏，推送并更新 PR 验证记录。
