# Tiny NV12 128×80 上位机接入（2026-11-06）

> 后续变更：LiteSR 384 已关闭相机，Tiny 允许相同 F1 装配的新序列号；其他模型已移出上位机项目。当前行为与验证以[三模型变更记录](THREE_MODEL_DEPTH_ONLY.md)为准。本文保留首次接入时的历史证据。

Tiny 以独立第三项加入，原 LiteSR 384×256 和上位机 640×480 均保留。菜单仅创建这三项；RGB576、LiteSR 256、LiteSR 128 继续隐藏。已完成 Release/Debug 构建、加密包及板端部署、实机全部六个方向的切换验证。本次没有制作安装包，历史 1.1.5 安装程序未修改。

用户随后发现 Tiny 画面近乎全黑，定位为上位机接收端将有效掩码写成 `1`，但共享绘图器按 `0–255` 不透明度解释，导致有效像素亮度降至约 `1/255`。已修正 Tiny 显示掩码为有效 `255`／无效 `0`；板端模型、深度值、有效区域和用户色阶均未改变。新增像素亮度回归先复现失败，再验证修复；下表为修复后的实机重测，不再仅凭帧数判断显示正确。

用户继而指出画面瘦长：此前按 `80:128` 采样网格直接显示，忽略了 ISP 对 `1248×1296` 原图裁剪的非等比采样。RGB 和深度现统一按裁剪比例 `26:27` 显示，与已认可研究预览一致（其显示为 `420×436`／`370×384`）。四种显示旋转都按该比例适配；模型输入、80×128 原始像素/深度、保存数据和悬停取值保持。切回旧模型恢复其原有画幅。Tiny 保留实际裁剪视场，不把它虚构为主机 640×480 档的完整视场。

## 模型位置与合同

模型主入口为 `E:/WORK/dToF/models/tiny_nv12_128x80/manifest.json`，说明为 `E:/WORK/dToF/docs/TINY_NV12_128X80.md`。原 RKNN 位于 `output/tiny_nv12_20261106_v1/rknn/nv12/model.rknn`，SHA256 `034bb260a7751c24ec133ec01ea6343f9ecf9102684162e0efa55aaef7056434`；保守补洞 V2 运行来源为 `output/tiny_nv12_holes_20261106_v1/build_v2`。名称按 H×W=128×80，实际图像/深度为 W×H=80×128。

本次阅读并按 `RGB_dToF标定参数与投影接入说明_20260920.md`、`REAL_CALIBRATION_READINESS.md` 完成接入检查。真实运行沿用固定设备 `REDACTED_DEVICE_SERIAL` 的 `rv20261008-f1`，没有把原文旧 R/t 覆盖当前绑定：16×16 原生径向测量经 M 一次、R/t、K/D 投影得到 camera-Z，安装 180°／模型 0°。原图 ROI `(560,0,1248,1296)`，参考 ROI `(496,0,1248,1296)`，ISP 输出 Full Range NV12 80×128，K 做匹配的裁剪与半像素变换。没有用 resize ToF 网格替代投影。

权重、输入通道/归一化、量化、signed residual scale 0.1 和补洞 V2 保持。实测覆盖和输出有效性分别处理；上位机禁用 Tiny 的 LiteSR 轮廓后处理与径向画幅转换，切回旧模型恢复原规则。没有训练、重新转换或重跑冻结 test。当前仅允许该固定标定设备/方向；其他设备仍显示条目但禁用。

## 文件与部署

| 内容 | 位置 |
|---|---|
| 加密模型、ARM 引擎、原始相机程序 | [Tiny 产品包](../tools/tiny_nv12_board/README.md) 的 `payload/` |
| 完整源文件与载荷身份 | [manifest.json](../tools/tiny_nv12_board/manifest.json) |
| 新主机协议及身份校验 | `sensors/RvTinyNv12Protocol.h`、`tools/tiny_nv12_board/Receipt.h` |
| 模型菜单、会话与显示 | `DepthMapWidget`、`RvSessionController`、`RvDtofBoardStream`、`RvDepthView` |
| 板端参数切换 | `tools/litesr_board/protected/payload/dtof-switch.sh`（v3，保留 v1/v2 精确升级模板） |
| 板端启动和相机所有权 | `litesr_gui.sh`、`tiny_nv12_board/payload/camera-session.sh`，由 native worker 注入/校验协调器 |
| 安装资源收集 | `tools/rv_control/build.ps1`、`tools/installer/build.ps1` |
| 已部署的六份完整板端文件及清单 | `E:/WORK/dToF/output/tiny_nv12_host_integration_20261106_v1/board/prepared_files/`（绑定本次设备的已保存参数；普通升级使用设备管理流程） |
| 本轮原始记录和回退副本 | `E:/WORK/dToF/output/tiny_nv12_host_integration_20261106_v1/` |

Tiny 密文由只读原 RKNN 的副本生成独立密钥，SHA256 `46f20a4f167c2d33d6c8a01d49e6d3670167630c1a99504cbb717be21793780d`，48275 字节。密钥/明文只留在内部 `private/`，不进入交付载荷。ARM 内存解密、认证并核对原文模型身份后初始化 RKNN，临时明文清零；缺失或损坏拒绝，明文不回退。

实际通过上位机随附的 `RvControlWorker` 安装至 `/root/dtof_candidates_20261009/dcts-tiny-nv12-128x80-v1/`。原 LiteSR 模型/程序和既有长期参数保留；仅协调器的 `control.sh`、`session.sh`、`litesr_gui.sh` 做已知哈希升级，原文件留有精确 SHA 命名备份。Tiny 会话使用 16×16／33 ms／Signal 开／Noise 关；切到 LiteSR 恢复 48×32／保存周期，切到 H 恢复 16×16／保存周期／Signal 关。Tiny 停止后恢复相机全幅格式/crop，旧模式恢复 H.265。

## 本轮验证

- Release/Debug 构建成功。两配置各八组离线专项累计 1364／1364 检查通过，其中 Tiny 相关 23 项覆盖数值/掩码、源身份、错误模型/方向/尺寸拒绝、过期数据、分片网络接收、旧视图恢复、有效像素完整亮度／无效像素保持黑色、RGB/深度裁剪比例、四方向旋转和原始像素保持。最终界面专项为 `aspect_fixed_release`、`aspect_fixed_debug`，各 133 项界面与 107 项恢复检查通过；其他受影响专项仍见 `offline_release_final`、`offline_debug_final`。
- 原生 worker 的 33 项自检通过，覆盖会话预设幂等、保存参数保持和含糊脚本拒绝。
- 实际 ARM 加密加载 7 项通过：正常加载、损坏密文、损坏 tag、错误身份头、截断、缺失、明文输入；所有拒绝场景同时保留明文替代文件仍拒绝。临时测试文件已清理。
- 下表由实际 Release 主窗口、模型下拉框和共享成像会话驱动，各阶段 30 秒。顺序覆盖三个选项之间全部六个方向；核对模型哈希、单位、源网格、同帧 Signal、相机尺寸、递增帧及真实绘制。未以离线样本替代实机。

| 次序 | 模型 | 测量秒数 | 新深度帧 | 接收 FPS | 绘制 FPS | 含有效深度帧（含准备期） | 结果 |
|---|---|---:|---:|---:|---:|---:|---|
| 1 | LiteSR 384×256 | 29.995 | 191 | 6.368 | 6.368 | 194 | 通过 |
| 2 | Tiny NV12 128×80 | 29.996 | 900 | 30.004 | 29.770 | 903 | 通过 |
| 3 | 主机 640×480 | 29.995 | 901 | 30.038 | 29.638 | 904 | 通过 |
| 4 | LiteSR 384×256 | 30.005 | 195 | 6.499 | 6.499 | 198 | 通过 |
| 5 | 主机 640×480 | 30.003 | 900 | 29.997 | 29.564 | 903 | 通过 |
| 6 | Tiny NV12 128×80 | 29.992 | 898 | 29.941 | 29.708 | 901 | 通过 |
| 7 | LiteSR 384×256 | 29.987 | 194 | 6.470 | 6.470 | 197 | 通过 |
| 8 | 主机 640×480 | 29.997 | 898 | 29.936 | 29.603 | 901 | 通过 |

两段 Tiny 实机分别采样 30／30 份颜色图，逐像素与实际米制深度对应的色阶核对，亮度/颜色不符像素均为 0，至少 10 次采样有可见颜色。实际控件画面：`E:\WORK\dToF\output\tiny_nv12_host_integration_20261106_v1\live_display_geometry_fixed_v1\report.json.tiny_stage_1.png`、`E:\WORK\dToF\output\tiny_nv12_host_integration_20261106_v1\live_display_geometry_fixed_v1\report.json.tiny_stage_5.png`。`live_v2` 和用户目标就位后的 `live_scene_confirmed_v1` 保留为传输、有效深度及切换证据；其旧帧数/绘制计数不证明显示亮度正确。

RGB 与深度的实际显示矩形均通过 `26:27` 几何检查；原始视频/深度仍为 80×128。RGB 画面：`E:\WORK\dToF\output\tiny_nv12_host_integration_20261106_v1\live_display_geometry_fixed_v1\report.json.tiny_camera_stage_1.png`、`E:\WORK\dToF\output\tiny_nv12_host_integration_20261106_v1\live_display_geometry_fixed_v1\report.json.tiny_camera_stage_5.png`。`live_color_fixed_v1` 保留为亮度修复后的切换证据，其画幅仍是修正前的 80:128。

最终正常停采，无停采后迟到结果；保存的传感器参数和服务文件不变。独立板端审计 179 项通过，核对原文件/模型、升级备份、新包哈希、无残留 Tiny 进程、无故障锁、相机格式与物理读回恢复。本轮八个原始研究资产 SHA256 全部保持。

完整机器记录：`E:/WORK/dToF/output/tiny_nv12_host_integration_20261106_v1/final_summary.json`；实机记录 `E:\WORK\dToF\output\tiny_nv12_host_integration_20261106_v1\live_display_geometry_fixed_v1\report.json`；板端审计 `E:\WORK\dToF\output\tiny_nv12_host_integration_20261106_v1\display_geometry_final_audit\report.json`。早期发现的菜单构造空指针已修复；首次场景仅 9 帧有效深度的失败记录 `live_v1` 原样保留，没有降低有效输出门。

这次验证模型接入、运行和切换，不新增全量程物理精度结论；原研究记录中的 INT8/远距误差限制仍然存在。安装构建资源已准备，但本次没有生成或验收新的安装程序。
