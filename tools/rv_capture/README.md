# RV1103 原生采集与 LiteSR

“上位机辅助推理”和“板端推理”使用 mode24、48×32 普通。“板端推理高精度”使用 mode25、48×32 高精度。mode14 的 Native16 用于工程采集。这些会话运行纯采集程序。“板端 LiteSR 近距（INT8，实验）”使用 mode13 和单独的 F6 推理程序，采集 48×32 普通 ToF 与同帧 Signal。所有模式保持单目标、Noise 关闭，周期、SNR 和积分沿用保存值，预设只作用于本次会话。

```bash
bash tools/rv_capture/build.sh /absolute/new-native-output
```

输出目录必须不存在。默认交叉工具链在 `/opt/rv1103-sdk/`，可用 `RV1103_CROSS_PREFIX` 覆盖；不需要 RKNN SDK、模型密文或私有密钥。只生成 `native-fov6.bin`，与 2026-11-09 默认标定成组维护。

`common/` 和 `native/runtime/` 保存原生 IPC、SPI 与 H.265 进度实现。`payload/model-switch.sh` 按模式调用对应 ARM 程序。`payload/dtof-host.sh` 导入时嵌入板端 `session.sh`，只有 mode13 开启 Signal。`Worker.inc` 与 USB bootstrap 共用运行文件检查和安装回滚。

## LiteSR 近距载荷

mode13 对应移植来源的近距 256 INT8 模型。输入沿用 0° 原生测量顺序和白点 512，板端根据同帧距离与 Signal 生成 256×256 径向米。模型发布范围为 0.01–1.4 m，0 表示无效。上位机使用同帧 48×32 支持做轮廓处理，色图透明度只影响显示。2304×1296 H.265 视频独立运行。

构建使用本机 ARM 工具链、RKNN SDK 头文件及匹配的 ARM `librknnmrt.so`、`libcrypto.so.1.1`。`pack_litesr.py` 校验原 RKNN SHA256，向新私有目录输出密文及独立的密钥头。

```bash
python3 tools/rv_capture/pack_litesr.py /absolute/model.rknn /absolute/private-build
# 将经过核验的两份 ARM 库放入 private-build
bash tools/rv_capture/build.sh /absolute/new-litesr-output litesr /absolute/private-build
```

配套私有载荷由 `litesr-fov6.bin`、`litesr-near-256.dctsm`、`librknnmrt.so`、`libcrypto.so.1.1` 组成。控制 worker 从 `litesr/payload/` 读取完整闭包并核验 `tools/rv_control/build_inputs.json` 中的 `litesr_files` 摘要。重新加密时应成组更新四个文件和对应摘要。密钥头和明文模型属于私有构建输入。带齐载荷的 worker 在 USB 一键连接或「安装板端采集支持」时自动导入这些文件及切换脚本，在启用目录之前核验 F6 与解密后的模型摘要。纯采集构建可单独使用，LiteSR 启动需要带模型载荷的 worker。

`common/NativeHoleFill.h` 在 mode24/25 的板端显示请求中估计孤立缺测点。LSG2 请求的偏移 24 为补点请求标志，响应头的偏移 32 为同帧估计载荷长度。估计载荷位于原始帧之后，由 1536 个小端 UQ2 值组成，零表示该格无估计。主机辅助模式使用原始载荷。

`native/runtime/HoleFillTests.cpp` 可用 C++17 编译器单独构建并离线运行，覆盖空间孤点、移动边缘和异常历史帧。

详见 [默认标定与原生 ToF 模式](../../docs/DEFAULT_CALIBRATION_NATIVE_TOF.md)。
