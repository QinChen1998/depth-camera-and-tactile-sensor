# 三模型接入、LiteSR 纯深度与上位机清理

> 历史记录：2026-11-09 当前产品已移除全部 LiteSR 板端推理，仅保留上位机 H。本文的旧入口、源码路径、资源及实测结果对应当时版本；当前说明见 [仅保留上位机辅助推理](HOST_ASSIST_ONLY.md)。

2026-11-08 当前开发版已移除 Tiny，保留 LiteSR 普通／高精度和主机 H 三项入口。见 [Tiny 退役记录](TINY_NV12_INTEGRATION.md) 和 [LiteSR 高精度记录](LITESR384_HIGH_ACCURACY.md)。用户取消本轮实机采集；下文全部为 2026-11-06 历史基线，其菜单、准备步骤和性能数据不适用于当前版本。

更新：2026-11-06。本轮修改上位机及其配套板端导入资源；后续已制作包含三项方案的 v1.1.6 安装包，见 [安装记录](INSTALLER_1_1_6.md)。**清理仅限 `E:/WORK/Depth Camera and Tactile Sensor`；`E:/WORK/dToF`、`E:/WORK/RV1103-1106` 的研究模型、权重、训练与实验记录均保留。**

## 2026-11-06 行为与验证基线

| 用户可见模型 | 会话预设 | 板端相机和上位机显示 |
|---|---|---|
| LiteSR 384×256（mode16） | 48×32 普通，保存周期，单目标，Signal 开、Noise 关 | 关闭相机采集服务；ARM 程序不打开相机；上位机清空缓存并隐藏相机面板，只显示深度 |
| 上位机辅助推理 640×480（mode14） | 16×16 普通，保存周期，单目标，Signal/Noise 关 | 恢复 H.265 相机和相机面板，RGB672→640×480，保持 H 矫正 |
| Tiny NV12 128×80（mode18） | 16×16 普通，33 ms，单目标，Signal 开、Noise 关 | 使用参与同次推理的 80×128 NV12，相机面板恢复；RGB/深度按实际裁剪的 26:27 比例显示 |

菜单仅有上述三项。旧 RGB576 选择迁移为 640×480，旧 LiteSR 256/128 选择迁移为 384×256。旧独立模型、运行包和加载入口已经移除。旧协议编号及参数脚本 v1/v2/v3 精确升级模板用于拒绝旧入口、升级已有设备，不携带旧模型。

LiteSR 384 密文和权重未改，保留原径向米制、同帧原生 D/I、有效掩码及上位机轮廓处理。新 ARM 包位于 `dcts-litesr-384x256-v3-depth-only`，不覆盖板上旧包。Tiny 仍为独立 AES-256-GCM 包，既有有效掩码和画幅修复保持。

## 新设备准备

1. 使用兼容 V1.2.0 的 RV1103＋SC3336＋SPI ToF 交付设备，确保 ADB/RNDIS 可用。运行本项目 `START_RELEASE.cmd`，在「设置 → 设备管理」使用「USB 一键连接」。
2. 核对设备序列号、SSH 指纹及标定。要使用全部三项，设备须采用与 Tiny 匹配的 F1 装配及 `rv20261008-f1` 标定；确认安装角 180°、模型输入 0°。不再绑定开发板序列号，但不能把固定标定套到不同机械装配。
3. 首次准备自动携带原生采集、驱动、标定、协调脚本、LiteSR 384 和 Tiny 两组加密模型及运行程序。已登记设备先停采，再点击「安装板端推理与自动切换」。程序按 F1/F2 选择相应 LiteSR/native 程序，检查两组密文认证和依赖后启用；错误会保留诊断与回退文件。
4. 验证连接显示模型及自动切换支持已就绪后，即可在三个成像方案间切换。选择方案自动切换本次会话参数，不覆盖保存的 SPI 时钟、积分、SNR 等长期设置。不同 FOV/安装方向仍按实际能力禁用 Tiny。

不需要用户单独挑选模型文件。交付准备入口为 `tools/rv_control/build.ps1`、`tools/rv_control/bootstrap/install.sh`、`tools/litesr_board/protected/payload/dtof-switch.sh` 和 `litesr_gui.sh`，对应载荷位于 `tools/litesr_board/protected384/payload/`、`tools/litesr_board/native/payload/`、`tools/tiny_nv12_board/payload/`。Release/Debug 原生 worker 的 `implementation.json` 分别列出全部依赖和 SHA256。

[已部署板端文件清单](../../dToF/output/litesr_depth_only_20261106_v1/board/prepared_files/manifest.json)保留开发板上实际生效的 9 份文件，供核对与恢复。新设备应使用上面的导入流程，让程序绑定自己的序列号和标定；不要直接复制开发板的 control/session 脚本。

## 本轮验证

- Release/Debug 构建成功；两配置各八组离线专项累计 **1323 项**通过（按每组最终结果统计），覆盖界面、参数切换、触觉、渲染、诊断恢复和 USB 登记。最后三组界面/参数/诊断回归每配置 538 项通过。
- Release/Debug 原生 worker 各 **46 项**自检通过，包含新板 F1/F2 资源闭合、加密包哈希、首次准备幂等、旧脚本升级和回退守卫。实际 POSIX 参数切换脚本 **7 项**执行检查通过。
- 现有 RV1103 开发板实机主窗口完成 **8 个各约 30 秒阶段**，覆盖三模型全部 **6 个有向切换**。LiteSR 深度 11.57–11.67 FPS；Tiny 约 29.07 FPS；主机 640×480 约 29.88–29.90 FPS。该数据为本次 GUI 接收频率，不混同 NPU 或物理精度。
- LiteSR 三个阶段视频帧数均为 **0**；相机面板均隐藏。独立进程监测共 **126 份**：LiteSR 43 份无 H.265/Tiny 相机进程，Tiny 28 份仅独立 NV12 相机，主机模式 55 份仅 H.265 相机；监测错误 0。切回两种 RGB 模型后相机恢复。
- Tiny 两段颜色逐像素检查共 58 份，颜色不符 0；RGB/深度裁剪比例均正确。正常停采、无迟到结果；长期参数与文件不变，相机格式/裁剪恢复全幅 NV12。最后主机模式读回 M16/F1、66 ms、short100、格式1，设备空闲无故障。
- 最终板端审计 **439 项**通过；40 份原文件中仅 control/session/launcher/native 四份按预期更新，每份有精确 SHA256 备份。现有板上历史模型原件保持，未把上位机清理范围扩大到板端研究资产。

[完整机器汇总](../../dToF/output/litesr_depth_only_20261106_v1/final_summary.json)、[实机报告](../../dToF/output/litesr_depth_only_20261106_v1/live_v1/report.json)、[进程监测](../../dToF/output/litesr_depth_only_20261106_v1/live_v1/camera_ownership.json)、[最终审计](../../dToF/output/litesr_depth_only_20261106_v1/audit_final_v2/report.json)。早期失败记录保留：诊断仍按 576 尺寸导致回归失败，已改为当前 640×480 并验证保存/回放/篡改拒绝；安装占位符和审计枚举/模板比较问题均已修正，最终报告以上述链接为准。

当前实机为 `REDACTED_DEVICE_SERIAL`。**尚未拿到另一台全新 RV1103 做现场验收**；首次导入资源和迁移已通过离线专项，现有板已完成实际导入与切换。沿用固定实采标定：Tiny 原图 ROI `(560,0,1248,1296)`→80×128，径向 range 经 M/R/t/K/D 转 camera-Z；未重训、重标定或改变投影。物理精度和全量程 INT8 原有缺口保持，不以切换通过代替精度验收。

## 清理与运行

从上位机移出旧 RGB576、LiteSR 256/128、旧 512 网络源码、旧部署副本及包含旧模型的历史交付/备份，共 **59 个目标、18,933 个文件、17,389,542,218 字节**。移动前后逐文件 SHA256 一致；清理清单与原件在 历史项目外归档路径 `../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/`（当前路径未找到） 和 [移动回执](../../dToF/output/litesr_depth_only_20261106_v1/retirement_result.json)。当前 640×480、LiteSR 384、Tiny 和磁触觉资源保留。dToF 原模型目录和原训练产物没有移除。

历史 1.1.3/1.1.4/1.1.5 安装程序原字节保留在项目外归档。当前开发入口为 [START_RELEASE.cmd](../START_RELEASE.cmd) / [Release 主程序](../x64/Release/触想工作台.exe)，三模型安装版为 v1.1.6。
