# Tiny NV12 128×80 板端产品包

独立新增 `board_tiny_nv12_128x80`（mode18），不覆盖 LiteSR。名称为 H×W=128×80，实际图像和米制数组 W×H=80×128。屏幕上的 RGB 和深度按 ISP 上游裁剪 `1248×1296`（26:27）显示，以还原非等比采样前的画面比例；原始像素、米制数据和保存数组仍为 80×128。身份与源文件哈希见 [manifest.json](manifest.json)，接入结果见 [验证记录](../../docs/TINY_NV12_INTEGRATION.md)。

模型入口：`E:/WORK/dToF/models/tiny_nv12_128x80/manifest.json`。沿用接受的双输入 NV12 权重及 `output/tiny_nv12_holes_20261106_v1/build_v2` 保守补洞 V2；没有重新训练、重新量化或重跑冻结 test。产品派生只改生命周期、模型文件名和回执，不改变数值计算。

## 输入、几何与输出

- 原验证设备 `REDACTED_DEVICE_SERIAL`；同 F1 装配的新设备可登记，不锁序列号。准入标定 `rv20261008-f1`，安装 180°／模型 0°。原生 16×16 F1 径向 UQ2 → 单位射线／M 一次／R、t／K、D → camera-Z；不能 resize ToF 网格替代投影。
- 传感器 2304×1296，原图 ROI `(560,0,1248,1296)`，标定参考 ROI `(496,0,1248,1296)`。ISP 输出 80×128 Full Range NV12，K 按裁剪和半像素缩放同步变换。
- 双输入 `[1,4,128,80]`：Y/127.5−1、base/3、support、min(distance/16,1)；`[1,2,64,40]`：原始 U、V 分别 `(byte−128)/127.5`。
- 板端输出 `base_depth_m + 0.1 * signed_residual`，没有 ±1 clamp。原有效门 0.01–3 m，输出补洞 V2 与实测覆盖分开。上位机仅将无效值表达为 0，并保留输出有效掩码。
- 同帧原生深度和真实 Signal 随 TNY6 传输；RGB 最近配对限 50 ms，ToF 不复用。NV12→RGB 仅用于同帧预览，安装角度仅补偿一次。

## 加密及构建

`payload/model.dctsm` 为从原 RKNN 副本生成的独立 AES-256-GCM 密文，48275 字节；加载时认证完整信封、核对原文 SHA256，内存创建 RKNN 上下文并清零临时明文。每帧复用上下文，错误、缺失或明文均拒绝，不回退明文。

内部构建输入在 `E:/WORK/dToF/output/tiny_nv12_host_integration_20261106_v1/private/`，含私有 `Key.h`、模型副本及 ARM libcrypto 链接输入；不得随产品交付。`runtime/` 不含密钥头或原始模型。ARM SDK 与工具链使用 `/opt/rv1103-sdk/`，可由 `RKNN_SDK`／`RV1103_CROSS_PREFIX` 覆盖。

```bash
bash tools/tiny_nv12_board/build.sh /absolute/private /absolute/new-output
```

输出目录必须不存在。构建 `tiny_nv12` 与 `tiny_nv12_camera`，二者和对应密文成组更新，并同步部署哈希和本清单。当前交叉构建采用 C++17、Cortex-A7/NEON hard-float、RKNN 2.3.2 与固件 OpenSSL 1.1 ABI。

## 部署与切换

`tools/rv_control/build.ps1` 将密文、两份 ARM 程序和 `camera-session.sh` 复制到原生 worker 载荷并登记 SHA256；`tools/installer/build.ps1` 已加入内容检查。v1.1.6 安装版已包含本包，见 [安装记录](../../docs/INSTALLER_1_1_6.md)。

设备管理的“安装板端推理与自动切换”使用相同受信任原生 worker：核验空闲、身份、原文件哈希，保留 SHA 命名备份，认证加载后启用 `/root/dtof_candidates_20261009/dcts-tiny-nv12-128x80-v1/`。同时更新协调器 `control.sh`、`session.sh`、`litesr_gui.sh`；`dtof-switch.sh` 增加 mode18 的 16×16／33 ms／Signal 开／Noise 关会话预设，保留 v1/v2/v3 精确迁移模板。Tiny 原始相机由同一会话所有权管理，停止后恢复完整相机格式/crop，主机模式恢复 H.265，LiteSR 384 保持相机关闭。长期参数不写入。

现有 Tiny 只适配上述固定设备；其他标定或输入方向在主机禁用该条目。全量程物理精度及已有 INT8 数值限制仍沿用研究记录，本次功能接入不重新宣称精度验收。
