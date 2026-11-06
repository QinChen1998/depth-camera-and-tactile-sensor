# 1.1.6 安装与发布记录

2026-11-06 制作。包含板端 LiteSR 384×256、上位机辅助 640×480 和板端 Tiny NV12 128×80 三项方案；LiteSR 关闭相机，另外两项恢复各自的视频链路。主机推理后端为 ONNX Runtime 1.31.1 / TensorRT-RTX EP。

## 安装产物

- 文件：`触想工作台-内测版-v1.1.6-x64.exe`。
- 发布附件名：`DCTS-Setup-1.1.6-x64.exe`，与本地中文文件名安装包的字节和 SHA256 相同。
- 大小：288612649 字节。
- SHA256：`0016af4a1ea5dfa46797e35f77c364fcb7fabad37b3c561f6ba4a15ef955f419`。
- 本地构建目录：`x64/validation/installer_release_1.1.6_20261106/`；安装器与校验文件在其 `delivery/` 下。
- 源码分支：[backup/release-v1.1.6-20261106](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/tree/backup/release-v1.1.6-20261106)。
- 安装包发布：[v1.1.6](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/releases/tag/v1.1.6)，沿用内测预发布标记。

主机模型从只读原文件副本重新生成密钥、AES-256-GCM 密文与原生加载 DLL。模型原文件 SHA256 和修改时间不变，加解密逐字节回验通过。LiteSR 和 Tiny 使用原有各自独立的密文及匹配 ARM 程序；交付载荷无明文 ONNX/RKNN/权重、私有密钥头或开发文件。

交付载荷中的三份密文及配套程序共 64 个清单文件已逐项核对 SHA256，全部匹配；无需用户另行寻找或下载模型。

| 方案 | 使用条件 |
|---|---|
| 上位机辅助 640×480 | 受支持的 Ampere 或更新 NVIDIA RTX GPU、兼容 CUDA 12.9 的驱动，以及已登记的 RGB/dToF 设备 |
| 板端 LiteSR 384×256 | 兼容 RV1103 设备，通过首次导入或“安装板端推理与自动切换”部署随包的模型、ARM 程序和会话脚本 |
| 板端 Tiny NV12 128×80 | 完成板端导入，采用匹配的 F1 装配和 `rv20261008-f1` 标定，安装 180°、模型输入 0°；不锁定序列号 |

只有满足对应运行条件时才能启用该项。沙盒验证文件齐全和应用生命周期，本次未对安装版三项推理做新的实机逐项验收。

## 本次验证

- 安装版 Release 主工程、原生控制 worker、翻译及 Inno Setup 编译通过。
- Release / Debug 各八组离线专项最终均通过，每配置 1323 项断言。最初受限环境中的 TCP 回环失败保留在原报告中；UI、调参两组在正常权限环境顺序重测后通过。
- 原生控制 worker 46 项自检、Python 离线工具 4 项测试通过。工程、filters、RCC、本地文档路径和 CAD 来源指纹检查通过。
- Windows Sandbox 禁用网络、vGPU 和剪贴板，仅映射只读安装输入与独立结果目录。安装退出码 0；中文主窗口出现且响应正常，正常关闭退出码 0；卸载退出码 0。
- 卸载后主程序、推理 worker、三项模型、桌面快捷方式及卸载登记均已移除；用户偏好按既定行为保留。

沙盒按用户要求仅检查安装、启动、关闭、卸载，不连接硬件、不做严格功能验收。证据为本地构建目录中的 `sandbox-results-smoke/sandbox-result.json`、`startup.png` 和安装/卸载日志。

## 源码与安装包的范围

源码提交使用中文说明，基于已有历史建立新的 backup 分支；不推送 main。Git、API 和发布附件均通过 7897 代理。公开源码沿用已有脱敏边界，详见 [公开源码说明](SOURCE_DISTRIBUTION.md)。本地协作说明和配置保持本地，安装包中仍包含正常使用所需的受保护运行资源。
