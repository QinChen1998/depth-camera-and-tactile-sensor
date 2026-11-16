# RV1103 原生采集与构建

「上位机辅助推理」和「板端推理」使用 mode24 的 48×32 普通 ToF。「板端推理高精度」使用 mode25 的 48×32 高精度 ToF。mode14 提供工程用 Native16。会话为单目标，关闭 Signal 和 Noise，周期、SNR 和积分沿用保存值。

`bash tools/rv_capture/build.sh /absolute/new-output` 使用 `RV1103_CROSS_PREFIX` 指定的 ARM 工具链，默认位于 `/opt/rv1103-sdk/`。输出目录必须不存在，生成的 `native-fov6.bin` 与 F6 标定和 `tools/rv_control/build_inputs.json` 的 SHA256 配套。

`common/` 和 `native/runtime/` 实现原生 IPC、SPI 和 H.265 进度。`payload/native_gui.sh` 验证并启动采集程序。USB 导入时将 `payload/dtof-host.sh` 嵌入会话脚本，并核对标定、会话预设、相机配置及完整运行载荷。

候选协调器通过 `capabilities` 报告 `clean-candidate-restart-v1`。它验证同 boot 的上一采集代次已正常退出，确认没有故障锁、占用进程或监听端口后，再开始新一代采集。

TMF8829 驱动通过 [prepare_spi_driver.py](../rv_control/prepare_spi_driver.py) 复制厂商源码后应用 F6 配置和 SPI 时钟修复。修复使用 SPI 结果头传输的单调时间估计传感器时钟，按完整帧锁定校正值。补丁和估计器位于本地的 `common/`。运行脚本生成的 `build.sh` 可交叉编译 `tof_driver` 并执行 FOV 和时钟回归。将验证过的驱动放在 `tools/rv_control/bootstrap/tof_driver` 后，重新构建桌面程序会同步更新板端载荷摘要。

本次修复对应的 `tof_driver` SHA-256 为 `f272722063a8d5f123c9b6dcc933e1836cc87323b8d7ff1e4164af6e83d14d23`。
