# 公开源码备份范围

公开仓库为 [QinChen1998/depth-camera-and-tactile-sensor](https://github.com/QinChen1998/depth-camera-and-tactile-sensor)。本次源码位于 `backup/release-v1.1.6-20261106`；安装包通过 [v1.1.6 Release](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/releases/tag/v1.1.6) 发布。

沿用此前公开备份范围，上传主机源码、工程文件、显示资源、测试、安装与部署脚本及公开技术文档。仓库原有 LICENSE、第三方声明与 ONNX Runtime 头文件许可保持。

以下内容仅在本地保留，不进入公开源码：

- AGENTS.md 等协作说明，IDE/个人配置，设备档案，凭据和私有密钥材料。
- 明文模型、模型密文、包含密钥的板端程序及模型运行包。
- 新增板端私有 runtime/common 实现及其原始 CAD、历史实验和构建资料。

公开版本中的真实设备序列号、公钥和预登记 known_hosts 经过脱敏；本地源码和实际运行资源保留原值。公开备份没有完整的授权模型、板端运行材料和开发机工具链，构建时需另行提供这些本地依赖。安装器包含三项成像方案所需的受保护运行资源，源文件排除规则不会删改安装载荷。
