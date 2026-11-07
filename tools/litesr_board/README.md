# 板端源码与构建

仅保留两个共享构建目标：`native` 是主机 640×480 的原生 dToF＋H.265 采集程序，不包含旧网络或权重；`protected384` 是 LiteSR 384×256 加密纯深度程序，不打开相机。Tiny 的独立构建入口见 [Tiny 包](../tiny_nv12_board/README.md)。

```bash
bash tools/litesr_board/build.sh native - /absolute/new-native-output
bash tools/litesr_board/build.sh protected384 /absolute/private384 /absolute/new-384-output
```

输出目录必须不存在。private384 含与原密文匹配的 Key.h 和 ARM libcrypto.so.1.1，不能进入交付载荷。工具链默认 `/opt/rv1103-sdk/`，可用 RKNN_SDK / RV1103_CROSS_PREFIX 覆盖。每个目标生成 F1/F2 程序；Tiny 固定 F1 及其安装方向。

`common/` 保留实际使用的原生封包、SPI 和 H.265 进度检查。旧 protected256/protected128、旧 512 网络/探测程序和重复依赖已移除。`protected/` 当前仅存部署 worker、协调器及旧版参数脚本的精确升级模板，不再存放旧模型。当前重建、导入、切换结果见 [变更记录](../../docs/THREE_MODEL_DEPTH_ONLY.md)。
