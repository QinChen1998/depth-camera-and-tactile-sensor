# RV1103 原生采集

板端只采集 dToF 和 H.265 视频，不含模型或 NPU 推理。H 使用 mode14、Native16 普通；“板端推理”使用 mode24、48×32 普通；“板端推理高精度”使用 mode25、48×32 高精度。均为单目标、Signal/Noise 关闭；周期、SNR 和积分沿用保存值，预设只作用于本次会话。

```bash
bash tools/rv_capture/build.sh /absolute/new-native-output
```

输出目录必须不存在。默认交叉工具链在 `/opt/rv1103-sdk/`，可用 `RV1103_CROSS_PREFIX` 覆盖；不需要 RKNN SDK、模型密文或私有密钥。只生成 `native-fov6.bin`，与 2026-11-09 默认标定成组维护。

`common/` 和 `native/runtime/` 保存原生 IPC、SPI 与 H.265 进度实现。`Contract.h` 接受 mode14/24/25；保留 LSG1/LSG2 传输标识用于兼容。`payload/dtof-host.sh` 提供三档切换，导入时嵌入板端 `session.sh`。`Worker.inc` 与 USB bootstrap 共用完整采集资源检查和安装回滚。

详见 [默认标定与原生 ToF 模式](../../docs/DEFAULT_CALIBRATION_NATIVE_TOF.md)。
