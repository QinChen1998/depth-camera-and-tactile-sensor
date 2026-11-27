# 公开源码与安装载荷

公开仓库为 [QinChen1998/depth-camera-and-tactile-sensor](https://github.com/QinChen1998/depth-camera-and-tactile-sensor)。`main` 通过 Pull Request 合并。Windows SDK 接入及主机推理变更见 [SDK 同步记录](../SDK_SYNC_CHECKLIST.md)。

沿用此前公开备份范围，上传主机源码、工程文件、显示资源、测试、安装与部署脚本及公开技术文档。仓库原有 LICENSE、第三方声明与 ONNX Runtime 头文件许可保持。

以下内容仅在本地保留，不进入公开源码：

- AGENTS.md 等协作说明，IDE/个人配置，设备档案，凭据和私有密钥材料。
- 明文模型、模型密文、包含密钥的板端程序及模型运行包。
- 新增板端私有 runtime/common 实现及其原始 CAD、历史实验和构建资料。
- 本机解包的安装运行库、安装版许可副本、依赖与 SDK 压缩包。源码内原有 LICENSE、第三方许可和来源声明继续保留。

公开版本对设备序列号、公钥和预登记 known_hosts 进行脱敏。本地源码和运行资源保留原值。公开安装程序使用脱敏源码构建，并检查交付载荷中的已知设备身份。开发环境提供 Windows SDK 3.1.0 和工具链。

当前安装构建收集 USB 直连 Windows SDK 和桌面依赖。Depth Pro 2-S 使用单独的本地研究运行包。安装器和 SHA-256 文件的生成及校验步骤见 [当前安装包](MODEL_PROTECTION.md#当前安装包)。

## 历史发布

- v1.1.6 安装包保留当时的三模型版本。源码备份引用 `backup/release-v1.1.6-20261106` 记录于 `x64/validation/branch_cleanup_20261124/removed-branches.bundle`，可以按原路径从 本地归档索引 `archives/README.md` 查询。安装包见 [v1.1.6 Release](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/releases/tag/v1.1.6)。
- v1.1.7 发布于 2026-11-09，包含当时的 UniDepth 多后端、可选混合 INT8、F6 原生 ToF 和 USB 接入修复。2026-11-10 的脱敏源码提交包含 GUI 减载优化，v1.1.7 Release 保留该版本原始产物。见 [v1.1.7 发布记录](INSTALLER_1_1_7.md) 和 [历史性能验证](TESTING.md#gui-减载与实机延迟对照2026-11-10)。
