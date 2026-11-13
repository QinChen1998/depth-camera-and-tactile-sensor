# 测试与源码统计

2026-11-09 后续变更：新增两档 48×32 原生 ToF，固定使用报告标定并完整导入板端，见 [默认标定与原生 ToF 模式](DEFAULT_CALIBRATION_NATIVE_TOF.md)。
从项目根目录执行。工具输出使用新的 `x64/validation/<本轮目录>/`；不覆盖已有报告。历史实机报告不能代替当前改动的验收。2026-11-08 已将既有验证历史按原路径归档，恢复与 SHA256 索引见 [本地归档](../archives/README.md)。

USB 启动与新板接入修复的证据目录为 `x64/validation/usb_startup_20261109/`。`python -m unittest discover -s tests -p test_usb_network.py -v` 在临时文件系统和模拟 Windows 网卡上运行实际探测/配置脚本，覆盖断开、迟到枚举、错误父设备、固件临时地址、旧地址保留及失败回滚；不访问硬件。C++ 专项继续通过统一 runner 的 `rv-usb-enrollment-test` 执行。USB 实机验证在临时设置中选择 H，避免用户保存的原生档位影响深度统计，正常启动仍保留用户选择。

两档原生 ToF 水平镜像与设备管理角度控件移除的证据目录为 `x64/validation/native_tof_mirror_20261109/`。`port-ui-smoke-test` 用不对称测点验证自定义角度及四个全局旋转下的屏幕水平镜像、悬停原始索引和距离、两档快照不变、UniDepth 不镜像，以及设备保存仍保留既有方向。此次为主机显示改动，使用离线测试，不更新板端文件。

当前成像入口为主机 H，以及不使用神经网络的 48×32 原生 ToF 普通／高精度两档。LiteSR 普通／高精度、Tiny 的模型、推理程序和切换模板已移除。下文板端推理实测章节均为对应日期的历史结果，不能据此运行已删除目标。模型移除记录见 [仅保留上位机辅助推理](HOST_ASSIST_ONLY.md)，当前三档见 [默认标定与原生 ToF 模式](DEFAULT_CALIBRATION_NATIVE_TOF.md)。

## 构建与离线专项

推理设备新增专项见 [UniDepth 推理设备选择](MULTI_BACKEND_INFERENCE.md)。`tools/test_inference_backends.ps1` 负责实际模型检测、选项策略、完整深度输出、加密加载及设置界面；`tools/verify_port.py` 可对同后端的开发／安装运行包作同输入对照。该专项不扩展下述八组标志的白名单。本轮输出为 `x64/validation/multibackend_20261109/`。

```powershell
./tools/build_desktop.ps1 -Configuration Release -QtRoot E:/Qt/6.11.1/msvc2022_64
./tools/build_desktop.ps1 -Configuration Debug -QtRoot E:/Qt/6.11.1/msvc2022_64
python tools/run_offline_tests.py --configuration Release --qt-root E:/Qt/6.11.1/msvc2022_64 --output x64/validation/my_check/release
python tools/run_offline_tests.py --configuration Debug --qt-root E:/Qt/6.11.1/msvc2022_64 --output x64/validation/my_check/debug
python -m unittest discover -s tests -p 'test_*.py' -v
```

默认构建原生控制 worker，并同步部署 Qt、QML、视频插件。主工程在导入 Qt props 前设置 `IntDir`，qmake/MOC/RCC 中间文件进入 `x64/obj_<配置>/`。主推理进程使用同一构建复制的 `DctsInferenceWorker.exe`；不要用中文主程序文件名直接驱动 TensorRT 二进制 IPC。

离线脚本仅允许以下八组，按需用 `--flags <标志> ...` 选择子集：

| 标志 | 覆盖范围 |
|---|---|
| `port-ui-smoke-test` | 四个模块、成像入口、方向、米制快照、点云坐标、独立点径和色标、停靠和清屏 |
| `rv-tuner-test` | 调参协议、互斥、异步停止、板端模式、Native16 分片与同帧校验 |
| `tactile-serial-test` | 串口协议、型号、零点、失败与恢复 |
| `tactile-visualization-test` | 触觉显示、面板可见性、选择、曲线和断开 |
| `tactile-3d-test` | QML 三维数据与生命周期 |
| `tactile-render-test` | 三维渲染、交互、显示刷新和关闭 |
| `rv-assist-recovery-test` | 当前 640×480 H 模型/F1/F2 的米制几何、100 ms 边界、原生点有效性、尺度连续性与 schema 1/2 诊断 |
| `rv-usb-enrollment-test` | USB 身份、地址规划、已有设备复用、完整检查回执、设置交接身份及登记准入 |

脚本清除继承的 RV/DCTS/Python/Qt 覆盖，强制 `RV_TEST_OFFLINE_ONLY=1`，TEMP/TMP 指向本轮输出的 `tmp/`。调参测试只使用本机 TCP 回环；需要允许本机回环连接，不会打开真实设备。连接失败/超时不能冒充协议拒绝通过负例。QtMsBuild 需要读取本机 Qt 注册表；访问被沙箱拒绝时应修正运行环境，不改模型或设备参数。

`--point-cloud-live-validation <绝对报告.json>` 使用已登记的 RV1103 依次运行 640×480 上位机辅助推理、48×32 普通和高精度档，核对辅助深度帧的标定网格、原生同帧有效点数、Qt Quick 3D 绘制、点径、色标、滚轮和停采清屏。它使用与成像切换专项相同的设备身份及授权环境变量，保存计数、状态及 640×480 点云视口截图。

判定同时检查退出码、报告存在、有独立 PASS 断言且无 FAIL。统计不把 `BYPASS` 字符串或 `RESULT PASS` 汇总行当作新断言。Python 测试覆盖缺报告、空报告、仅汇总、FAIL、非零退出、环境隔离和非离线命令拒绝。

## USB 自动发现与免填参数（2026-11-09）

当前自动发现不再通过单独的 SSH 端口探测绕过完整文件检查。已保存设备仅在完整资源检查回执通过后自动选择并启动 H；新设备、文件更新或缺少 USB 网络配置时，确认后自动打开设备管理并调用同一个 USB 连接入口。新设备使用安装 180°／推理 0°，已有方向与临时 FOV 设置保留；USB 登录复用保存凭据或工厂 root 登录，不弹出密码或角度输入框。

USB 专项覆盖完整检查回执的真／假／缺失／错误类型、错误序列号和旧 boot ID，以及确认后的换板、公钥变化、重插和重启拒绝。UI 专项实际操作中英文添加／更新提示，验证取消、确认后自动进入设置与排队前停止撤销；所有入口继续受离线保护。原生 worker 自检验证检查与安装共用完整文件计划、长期参数保留、缺凭据的免提示分支及非工厂账号拒绝。Python 测试继续在映射的临时文件系统中执行安装与回滚脚本。

Release/Debug 构建均通过，两配置各八组离线专项、各 1320 个断言通过，原生 worker 各 61 项自检通过，Python 44 项测试通过。当前源码路径审计为 0 个问题，14 个已有历史归档路径警告与上轮一致。

本轮证据在 `x64/validation/usb_auto_connect_20261109/`，最终结果见该目录 `verification.json`。全部操作仅限离线构建和模拟测试，未连接、重启或修改 RV1103，也未制作安装包或上传源码。离线通过不代表实机深度出帧、物理精度或 FPS 验收。

## USB 一键检查、修复与重复连接（2026-11-07）

本轮证据保存在 `x64/validation/usb_reconcile_20261107/`。Release/Debug 构建及各 3 组相关离线专项通过：USB 26、界面 144、调参 341，共每配置 511 个检查；两个原生 worker 各 46 项自检通过。临时文件系统中的 21 项安装/相机接管测试覆盖任意旧驱动替换、缺失文件补装、完整上传摘要、加密初始化失败、F2 能力、切换失败回滚和回滚未完成时保留原文件及故障。

随后在同步 `main` 前重新运行 Release/Debug 全部八组离线专项，每配置 1336 项检查通过，两个原生 worker 各 46 项自检通过。工作区与实际脱敏上传快照的 Python 测试各 40 项通过。源码路径审计与前次保持相同的 24 项私有源码、外部归档及 CAD 引用缺项，公开范围继续排除这些本地依赖。本次源码、脱敏清单和测试证据保存在本地 `x64/validation/github_main_20261107/`，不发布新安装包或 Release。

已在当前 F1 实机通过普通主窗口的 USB 按钮验证首次登记与已有设备重复连接。首次在隔离的本地设备档案中登记，并暂时移开受管日志脚本以触发完整补装；板端安装保持生效。首次连接收到 30 份有效 384×256 径向米制深度并实际绘制 25 帧，重复点击后选中同一档案、跳过文件写入，再收到 30 份并绘制 25 帧。两次均正常停止并核对设备空闲，本地原配置、已保存传感器参数和本次启动身份保持，未保存实景图像或测量数组。报告为 `live-release-new-repair/summary.json`。源码审计仍为既有 24 个私有源码/归档/CAD 路径问题。

## 主机模型同输入对照

需要 NumPy、匹配的 Qt DLL 和兼容 NVIDIA GPU。先保存待比较版本的 EXE（ASCII 文件名），对候选/参考执行相同合成输入；两者的模型 ID、后端和运行包必须匹配。

```powershell
python tools/verify_port.py --executable x64/Release/DctsInferenceWorker.exe --reference-executable x64/validation/reference/before-Release.exe --qt-root E:/Qt/6.11.1/msvc2022_64 --output x64/validation/my_check/parity-default
python tools/verify_port.py --executable x64/Release/DctsInferenceWorker.exe --reference-executable x64/validation/reference/before-Release.exe --qt-root E:/Qt/6.11.1/msvc2022_64 --model unidepth_rgb672_vga_fp16_tof16 --output x64/validation/my_check/parity-vga
```

默认使用 `UniDepthRuntimeRgb672VgaFP16RTX/`，仅接受 VGA 模型 ID。`--runtime` 指定候选包，`--reference-runtime` 单独指定参考包，未指定时使用候选包。受保护包按清单要求 AES-GCM 内存加载、原 SHA 校验和不读明文；开发明文包验证 `plaintext-file`，不能因传了目录参数就假定加密。

每个模型覆盖 28 个场景：F1/F2、0°/180°、不同 RGB、±100 ms 接受和超界拒绝、复用身份/字节一致、错误网格/FOV/方向/有效位、低 SNR 和 RGB-only。比较有效输出的米制深度、原始预测及诊断字节，检查停止重启、不重放旧 RGB，并拒绝已移除的 A 模式。包清单逐文件 SHA256 也须通过。协议和合成样本共用 `tools/unidepth/native16_contract.py`；不再运行已删除的旧模型测试脚本。

这类测试验证代码与数据合同，不证明实际物理精度、客户显卡性能或设备安装标定。连续尺度平滑使用单调时间；逐字节比较采用固定场景，不能把不同墙钟节奏下的长流输出直接当作同一帧比较。

原生控制 worker 可单独运行 `x64/rv_control/<配置>/runtime/RvControlWorker.exe --self-test`。主机异步启动/结果回传入口为主程序 `--rv-assist-path-test <绝对报告.json>`，须同时设置 `RV_TEST_OFFLINE_ONLY=1`；报告 `pass=true` 且 `depth_bytes` 正确才通过。

首次板端部署脚本由 `tests/test_board_bootstrap.py` 在临时文件系统中实际执行，覆盖其他版本旧驱动、没有旧交付目录、备份、身份绑定、上传损坏拒绝、安装中断回退、固件依赖缺失、已有残留部署和采集占用。Windows 使用 Git Bash，Linux 使用 bash；测试中的系统路径映射到临时目录，不访问真实板卡。与上述 Python 测试入口一起运行。

真实首次导入入口为主程序 `--usb-enrollment-live-validation <绝对报告.json>`，显式要求 `DCTS_LIVE_ENABLE=1`、`DCTS_LIVE_SERIAL=<实际设备序列号>`，并拒绝离线模式。该专项通过普通设备管理界面的「USB 一键连接」、同序列号确认框和原生 worker 完成部署，使用 F1、安装 180°、推理输入 0°。本地设备档案在临时配置中隔离，成功后将档案导出到 `<报告>.devices.json`；测试结束停止采集并核对空闲状态。此入口会实际写入板端文件，必须取得对应设备的测试授权，不纳入离线八组。

仅验证上位机辅助推理的实机入口为主程序 `--host-assist-live-validation <绝对报告.json>`。先登记实际 USB 设备、核对公钥与标定并保存登录，在运行该进程的环境中显式设置 `DCTS_LIVE_ENABLE=1` 和与登记设备一致的 `DCTS_LIVE_SERIAL`，清除 `RV_TEST_OFFLINE_ONLY`。通过等待进程退出的方式启动，检查退出码与报告的 `passed=true`；PowerShell 直接调用 GUI EXE 时不能把命令立即返回当作测试完成。

该专项只选择 `host_unidepth_rgb672_vga_fp16_tof16`，进行两轮各 30 秒的普通开始、停止及重启。按 `RV_INFERENCE_BACKEND`（默认 `tensorrt`）检查实际后端、模型摘要及精度身份，并检查 640×480 米制输出、16×16 原生测量、输入方向、H 矫正、100 ms 配对上限、相机与深度实际绘制、停止后的晚到结果，以及两份板端长期设置/脚本保持不变。`DCTS_LIVE_CONFIG_SOURCE` 可指定只读的已登记设备配置来源，复制进临时配置后执行。批量入口为 `tools/test_live_inference_backends.ps1`，要求显式 `-AllowLiveDevice`，首次失败即停止。2026-11-09 五后端结果见 [推理设备选择](MULTI_BACKEND_INFERENCE.md)。报告只保存计数、耗时、身份和设置，不保存真实图像或测量数组，不运行板端推理。它不属于离线八组，也不使用调参持久化操作；实景精度仍须用独立的物理基准评估。

2026-11-07 本轮首次部署及主机实测证据位于 `x64/validation/fresh_board_setup_20261107/`。当前设备通过普通 USB 一键连接完成全量安装（`usb-live-REDACTED_DEVICE.json`），不再要求旧 V1.2.0 驱动哈希。原厂摄像头退出后遗留的 DHCP 进程仅在身份、参数、启动时间、RTSP 监听和摄像头 DMA 句柄均匹配时接管；原始摄像头配置及驱动已备份。

按用户明确要求，当前设备档案开启临时 F1，模型输入 0°、相机安装 180°；传感器独立读回仍为 F=6。新版驱动使用 F1 射线表实际重建 XYZ，并保留真实 FOV 回执。`host-live-REDACTED_DEVICE-software-fov1.json` 两轮 30 秒均通过：深度 24.04 / 24.56 fps，视频约 29.94 / 29.96 fps，最大配对差 96.33 / 93.64 ms，正常停止、重新启动、无晚到结果，长期设置和脚本未改。该结果只证明临时参数下的部署与运行流程，不替代后续正式标定。

本轮最终 Release/Debug 均构建通过；各配置相关四组离线专项合计 569 个断言通过（界面 146、调参 341、H 恢复 61、USB 21），两个原生控制 worker 各 46 项通过。隔离安装/相机接管 Python 测试 13 项、离线运行器 Python 测试 4 项，以及软件 FOV/回执/告警策略 C 测试 1803 个断言通过。源码审计仍为原有 24 处缺少私有板端源码/归档/CAD 路径，本轮没有新增路径问题。当前临时标定的开关、真实/处理 FOV 区分、过期会话拒绝和隐藏差异拒绝均纳入专项。

## 三模型切换与界面提示清理（2026-11-07）

按用户要求，深度图移除整段临时标定提示栏，主机推理运行状态也不再追加同类文字；设备管理保留“使用所选 FOV 标定”配置项。实际 FOV、处理 FOV、会话身份及标定回执校验仍保留在数据链路中。

本次 Release/Debug 重新构建并更新翻译，两配置的界面、调参协议、H 恢复专项各 546 项检查通过（144／341／61），两个原生控制 worker 各 46 项自检通过。源码审计仍有既有的 24 处私有板端源码、归档及 CAD 路径问题，相对前次审计没有新增问题。

使用当前受信任设备和实际 Release 主窗口，按 LiteSR → Tiny → H → LiteSR → H → Tiny → LiteSR → H 运行八段，各测量 30 秒，覆盖三个模型之间全部六个切换方向。八段全部通过，共接收 5300 份新深度；同时检查模型/单位/输出尺寸、原生网格、输入方向、有效米制值、相机显隐和实际绘制。

| 模型 | 深度接收 FPS | 实际绘制 FPS | 相机策略 |
|---|---:|---:|---|
| LiteSR 384×256 | 11.90–12.10 | 11.87–12.07 | 关闭并隐藏 |
| Tiny NV12 128×80 | 30.11 | 29.74–29.87 | 恢复同帧 80×128 NV12 |
| 主机 640×480 H | 25.76–27.54 | 25.13–26.97 | 恢复 2304×1296 H.265，约 30 FPS |

Tiny 两轮共 60 次颜色检查均有可见颜色，不符像素为 0，RGB/深度裁剪画幅保持。H 最大测量配对差 95.21 ms，未超过 100 ms；历史尺度帧未携带旧原生校正点。三个模型的实际界面截图均确认无临时标定提示。最终正常停止，板端空闲，停止后的晚到结果为 0；两份板端长期参数/服务脚本和本地设备档案、用户设置的 SHA256 均保持不变。

本轮记录位于 `x64/validation/model_switch_20261107/`：`live_three_models.json` 为实机明细，`live_summary.json` 核对退出码和本地配置，`offline_release/`、`offline_debug/` 为离线报告，`live_three_models.json.stage_*.png` 为真实主窗口画面。本次验证运行和切换功能，不增加物理精度结论。

## 板端自然温升与降频观察

用户授权后，从自然冷却、重新上电的设备开始。先关闭普通上位机，避免热插拔自动成像提前加热。保持同一供电、摆放、外壳与散热条件：空闲采样 30 秒，运行板端 LiteSR 384×256 连续 600 秒，再正常停止并观察 60 秒。LiteSR 会关闭摄像头；本测试不恢复其他模型入口，也不改频率 governor、温控阈值、散热状态、EEPROM 或板端长期参数。当前临时 F1、模型输入 0°、安装 180° 的档案继续适用，传感器实际 F=6 单独保留；本专项评估吞吐与温升，不评估标定精度。

```powershell
python tools/thermal/run.py --serial <实际受信任序列号> --output x64/validation/thermal_<本轮唯一名称>
python tools/thermal/run.py --mode host --serial <实际受信任序列号> --output x64/validation/thermal_host_<本轮唯一名称>
```

脚本通过只读 ADB shell 命令流采样，每秒记录热区温度、CPU 当前/允许最高频率、频率驻留计数、CPU 使用率与 I/O 等待、devfreq/NPU 可用节点、cooling device 状态、内存及相关进程计数。当前原厂 adbd 仅支持旧版 PTY，不支持 exec-out 或 shell -T，因此将脚本作为正确引用的非交互命令传递，避免从 stdin 输入后被终端回显污染数据。时钟树每 10 秒读取一次；按实际内核表头解释列，不假定旧内核也有 protect_count。静态元数据保存传感器类型、实际温控 trip point、governor 和可用频率，另保留测试前后的内核日志。脚本不在板端写文件，不挂载 debugfs，不用额外 CPU 压力工具。缺失节点标为缺失，频率计数回退及设备重启不得计算为零负载。

上位机 `--board-thermal-live-validation` 入口要求 `DCTS_LIVE_ENABLE=1`、准确的 `DCTS_LIVE_SERIAL`、非离线状态及普通设备身份/文件校验。通过既有共享会话启动、接收、显示、停止，只保存标量统计。每秒区分原生测量序号速率、板端 publish_count 速率（固件提供时）、PC 接收深度速率和实际绘制速率，并记录 NPU、板端整帧、主机轮廓耗时。当前 LiteSR 384 程序提供 NPU/整帧时间，但没有独立 publish_count；这时板端计数速率保持缺失，用有效深度包的接收速率作为“PC 观测出帧率”，不得冒称板端生产计数。没有新帧的一秒窗口保留为观测，不因短暂无输出直接截断温升曲线；原有故障/超时准入仍生效。断线、故障、重启或未运行满 10 分钟须标记未完成，不能作完整测试通过。

输出包括 `board.jsonl`、`board-metadata.json`、`inference.json` 及其逐秒进度、`board.csv`、合并的 `timeline.csv`、`timeline.png`、`summary.json`。比较前后各 60 秒，吞吐下降 10% 仅作为筛查标志；结合真实温度、CPU/NPU 频率/上限和温控降频状态判断。CPU 低负载下正常调频、PC 绘制变慢或缺少温控节点均不能直接证明热降频。单次工况没有出现降速也不代表其他环境温度、外壳或散热条件下永远不会发生。频率节点语义参照 [Linux CPUFreq](https://docs.kernel.org/admin-guide/pm/cpufreq.html)、[热管理 sysfs](https://docs.kernel.org/driver-api/thermal/sysfs-api.html)、[频率驻留统计](https://docs.kernel.org/cpu-freq/cpufreq-stats.html)。

`tests/test_thermal_analysis.py` 离线验证累计计数差分、guest 计数不重复、缺失/回退数据、旧时钟列布局、过期采样不复用、板端/PC 帧率区分及只读脚本的临时文件系统执行。实际硬件只在上述显式实机入口运行。

`--mode host` 使用 `--host-thermal-live-validation`：仅选当前 RGB672 输入／640×480 输出的 H 模型运行 600 秒，恢复 2304×1296 H.265，继续严格检查 100 ms 配对、复用身份、无历史校正点冒用、模型输入方向和停止后晚到结果。电脑端每 5 秒通过只读 nvidia-smi 记录 GPU 温度、SM/显存频率、利用率、功耗、硬件/软件热限制状态和累计时间，并记录主机 CPU/内存与供电状态。累计热限制计数只比较本轮增量，历史非零值、N/A 或缺失不能冒充本轮限频。GPU 记录与板端及逐秒推理窗口按同一主机时间对齐，过期读数不复用。此模式的板端发热包含摄像头、ISP、编码和视频传输，不能因神经网络在电脑端运行就假定板端负载很低。

2026-11-07 当前设备两轮均完整运行 600 秒并正常停止，晚到结果为 0，长期参数和脚本前后相同。板端记录在 `x64/validation/thermal_20261107_REDACTED_DEVICE_run2/`，辅助推理记录在 `x64/validation/thermal_host_20261107_REDACTED_DEVICE_run1/`，汇总与对比图在 `x64/validation/thermal_comparison_20261107/`。板端 run1 仅为 exec-out 通道不兼容的启动失败，未启动推理，不计为实测。辅助轮未再次断电，记录实际起始温度；外界温度及散热片/风扇情况未独立确认。

| 实际工况 | RV1103 峰值温度 | 全程深度输出均速 | 前／后各一分钟均速 | 模型与整帧平均耗时 |
|---|---|---|---|---|
| LiteSR 384×256，摄像头关闭 | 62.425°C | 11.733 fps | 11.931 → 11.699 fps（-1.94%） | NPU 42.853 ms，板端整帧 84.457 ms |
| 主机 H 640×480，完整 H.265 链路 | 92.647°C | 27.060 fps | 26.948 → 27.268 fps（+1.19%） | 主机模型 15.266 ms，主机整帧 21.198 ms |

两轮原生 dToF 序号速率约 15.05 fps，CPU 允许最高频率始终 1104 MHz，采用原有 ondemand。辅助轮原视频约 29.96 fps，最大配对差 98.36 ms，16191 帧含当前有效 dToF、其中 7554 帧为明确复用；9407 帧实际接受 dToF 矫正，其他帧沿用既有兜底及历史尺度规则。电脑 GPU 最高 68°C，硬件/软件热限制累计时间本轮增量均为 0，也未观察到 GPU 功耗封顶标志。该结果验证运行/热性能与协议，临时 F1 仍不代表完成正式几何标定。

没有观察到与升温一致的持续限频或吞吐下降，但辅助轮板温已超过固件配置的 75/85°C 被动点。只读核查显示 thermal_zone0 为 enabled/step_wise，标准 cooling_device 不存在，设备树 soc-thermal 没有 cooling-maps；rockchip-system-monitor 驱动已绑定，内核 config 不可读。因此不能据此宣称设备没有任何热保护，也不能把可读的温控点当作已验证的 CPU 限频保护；仍需结合实际固件源码/绑定核对。检查原件位于辅助轮的 `thermal-policy-inspection.txt` 与 `system-monitor-inspection.txt`，未修改任何温控/调频参数。两轮结束后推理和采样进程均退出，设备保持空闲降温。

## 有效代码行数与入口检查

```powershell
python tools/audit_source.py --root . --output x64/validation/my_check/source-inventory.json
```

范围为根目录、`sensors/`、`tests/`、`qml/`、`tools/` 的第一方 C++/头文件/inc/QML/Python/PowerShell/shell/awk/cmd。按物理非空、非纯注释行计数，大括号、预处理和字符串算代码；Python docstring 算字符串。排除第三方、生成文件、模型、资源数据、运行包和 `x64/`。报告分别列出主机与原生支持、板端 runtime、测试、脚本及 SHA256。这是源码规模口径，不表示每行都在普通产品入口可达。

同时检查项目本地编译/MOC/RCC/资源/None 路径、filters 对应关系、RCC 内容及根目录/docs/tools/归档索引的 Markdown 本地链接，并核对触觉 CAD 来源路径及 SHA256。当前入口缺失会返回非零退出码；忽略目录中的历史验证报告缺失单独列为提示。删除文件前检查调用和资源引用，不以行数下降代替功能验收。

## 板端公共源码

当前交付使用报告标定对应的 F6 原生采集程序，入口见 [原生采集构建](../tools/rv_capture/README.md)。离线验证覆盖 Native16 分片、网格／身份／告警校验、拒绝旧板端深度载荷、H 与原生 ToF 会话预设及安装回滚。旧 F1/F2、多模型切换和 NPU 一致性记录属于历史。真实设备需另行验证当前 H 启停、USB 修复、视频及配对，不因离线报告或历史报告通过而视为本次已实机验证。

## GUI 减载与实机延迟对照（2026-11-10）

统一离线运行器可用 `--executable <绝对 EXE 路径>` 检查主工程的隔离构建，仍仅接受原有八组标志。Release 全八组和 Debug 相关四组分别保存报告；高 DPI 像素断言按抓图的 devicePixelRatio 换算物理像素，不改变产品镜像、旋转或渲染。

模型精度对照使用 `tools/verify_port.py`，候选和参考均指定各自的 ASCII 命名 `DctsInferenceWorker.exe` 及匹配运行包，覆盖 0°／180°、同帧身份、100 ms 边界、无效输入拒绝和停止重启，要求预测、H 深度和诊断字节一致。RGB 张量转换另检查正常／带行填充输入、180°图像及 ARGB32 来源。色表缓存对照原始 Qt 转换，遍历全部色相状态的取整边界，并检查百万组范围／像素组合及 NaN、无穷、零值和超范围黑色规则。

显式授权的 `--host-assist-live-validation` 可设置进程环境 `DCTS_LIVE_SECONDS=120`：先预热 10 秒，再各测两轮，每轮限制 30–300 秒；默认行为仍为原两轮各 30 秒。扩展测量按秒记录接收与实际绘制 FPS，按绘制帧汇总 RGB 准备、父进程校验、着色排队／计算、绘制及 `source_to_paint_ms`。后者从主机解码完成时间起算，不包含传输和摄像头曝光延迟。报告只保存计数、耗时、身份及参数，不保存真实图像或测量数组。

本轮同一受信任设备、相同临时配置的基线及优化回测证据位于 `x64/validation/gui_optimization_20261110_01/`。真实 FPS 对照期间暂停「推理设备」测速和其他上位机实例；重测另记录相关进程的出现及退出，核查是否有并发测速。用户指出可能同时启动测速的一轮保留为疑似受干扰证据，排除在最终性能结论之外。每轮继续验证米制网格、实际推理后端、输入方向、原生捕获配对、复用标记、正常停止、停采后无晚到结果及板端长期文件不变。FPS 和延迟以各次实测报告为准。

最终独占对照使用 `baseline-live-06-final-exclusive`（两轮各 30 秒）和 `optimized-live-05-palette-exclusive`（两轮各 120 秒），两个进程日志均确认没有其他上位机或设备测速。按实际绘制帧数及样本数加权汇总如下：

| 指标 | 基准版 | 最终优化版 |
|---|---:|---:|
| 实际绘制 FPS | 28.89 | 29.99 |
| 解码完成至绘制平均延迟 | 84.37 ms | 56.65 ms |
| 各轮显示延迟 P95 | 95.998 / 92.937 ms | 62.358 / 65.258 ms |
| GUI 主线程单逻辑核占用（同样的 24 秒采样） | 83.47% | 71.71% |
| 后台深度着色平均耗时 | 14.47 ms | 3.35 ms |

优化版两轮分别为 30.00、29.98 FPS。240 个约一秒窗口最低 28.97 FPS；补充的五秒滚动窗口最低 29.60 FPS，P5 为 29.83 FPS，显示在本次独占实测中持续接近视频源 30 FPS。滚动窗口相互重叠，仅描述运行稳定性，不作为独立统计试验。各轮 P95 单独保留，未对分位数作错误平均。汇总为该目录的 `performance-summary.json`。

最终 Release 八组 1396 项、Debug 相关四组 632 项离线断言通过；两种构建均通过 22 用例／13 组有效深度、预测和诊断数组与基准逐字节对照，父子进程链路及输入张量校验通过。正常停止、重启、无晚到结果和板端长期文件不变均通过。已更新本地 Release／Debug 程序并保留旧二进制备份。

## 当前 UniDepth 的 CPU／集显可行性实验

[host_fallback_probe.py](../tools/host_fallback_probe.py) 直接测试当前 RGB672×504 → 640×480 UniDepth ONNX 的执行后端。只生成三组确定性 RGB、分别检查 0°／180°输入及输出逆恢复，不启动上位机普通主窗口、相机、串口或板端连接。它记录实际执行 provider、纯模型延迟、内存、输入变化及 A→B→A 重复一致性；应用层专项仍由本页的统一离线运行器执行。

可选后端为 `cpu`、`directml`、`openvino-cpu`、`openvino-gpu`、`openvino-ep-cpu`。其中最后一项使用 ORT 插件 API 显式选择 CPU，可配合 `--memory-model` 检查从模型字节创建会话。它验证内存加载可用性，尚未验证产品加密 DLL、安装构建或自动回退。

开发依赖采用独立 Python 3.11 环境。本次 ORT/OpenVINO 插件环境为 `onnxruntime==1.31.1`、`onnxruntime-ep-openvino==1.8.1`、`numpy==2.4.6`、`psutil==7.2.2`；独立 OpenVINO 运行时为 `openvino==2026.4.1`。DirectML 另用 `onnxruntime-directml==1.24.4` 的环境，避免不同 ORT wheel 共用同一个 Python 包目录。

```powershell
# 使用已安装上述依赖的隔离环境 Python；输出必须是尚不存在的目录。
python -B tools/host_fallback_probe.py --model UniDepthRuntimeRgb672VgaFP16RTX/unidepthv2-small-rgb672-vga.onnx --backend openvino-ep-cpu --threads 8 --memory-model --output x64/validation/my_cpu_probe
```

用 `--reference <RTX同输入预测.npz>` 执行数值对照；未给参考时只报告运行功能和速度。参考须使用同一模型、六组同输入和实际方向，当前基准由 `DctsInferenceWorker.exe` 生成，并先通过 `verify_port.py`。参考输出键为 `image{0,1,2}_r{0,180}__depth`，值为恢复到标定网格的 `[1,1,480,640]` float32 预测。平均／最大相对数值门槛沿用当前模型的 0.25%／1%；超限返回非零并保留测速，不据此宣称替换后端验收通过。

2026-11-09 实验原件位于 `x64/validation/cpu_igpu_20261109_01/`，结论见该目录 `REPORT.md` 和 `summary.json`。仅当前 UniDepth 纳入结论；FP32 计算副本保留既有 FP16 权重的数值并提高计算精度，不恢复丢失的原始 FP32 权重，不改变网络结构或输入／输出尺寸。纯推理 FPS 不等于 GUI FPS；合成数值差异不等于物理测距误差。测试机只具备 RTX 3080 Ti，DirectML 运行结果不能视为 Intel／AMD 集显验收。
