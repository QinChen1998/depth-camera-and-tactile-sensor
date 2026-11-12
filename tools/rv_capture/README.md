# RV1103 原生采集与构建

「上位机辅助推理」和「板端推理」使用 mode24 的 48×32 普通 ToF。「板端推理高精度」使用 mode25 的 48×32 高精度 ToF。mode14 提供工程用 Native16。会话为单目标，关闭 Signal 和 Noise，周期、SNR 和积分沿用保存值。

`bash tools/rv_capture/build.sh /absolute/new-output` 使用 `RV1103_CROSS_PREFIX` 指定的 ARM 工具链，默认位于 `/opt/rv1103-sdk/`。输出目录必须不存在，生成的 `native-fov6.bin` 与 F6 标定和 `tools/rv_control/build_inputs.json` 的 SHA256 配套。

`common/` 和 `native/runtime/` 实现原生 IPC、SPI 和 H.265 进度。`payload/native_gui.sh` 验证并启动采集程序。USB 导入时将 `payload/dtof-host.sh` 嵌入会话脚本，并核对标定、会话预设、相机配置及完整运行载荷。

候选协调器通过 `capabilities` 报告 `clean-candidate-restart-v1`。它验证同 boot 的上一采集代次已正常退出，确认没有故障锁、占用进程或监听端口后，再开始新一代采集。
