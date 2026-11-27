# 测试与源码统计

从项目根目录执行。每次测试使用新的 `x64/validation/` 子目录。下方历史记录中的路径是报告生成时的相对路径，可按 本地归档索引 `archives/README.md` 查询和恢复。

## 构建与离线专项

主工程部署 Qt Widgets、CXWL SDK 3.1.0 和视频插件。主机推理依赖独立的 `DepthPro2Runtime/` 运行包。构建命令见 [README](../README.md#开发)。

```powershell
$qt = 'E:/Qt/6.11.1/msvc2022_64'
$run = Join-Path $PWD ('x64/validation/offline-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
python -B tools/run_offline_tests.py --configuration Release --qt-root $qt --output "$run/Release"
python -B tools/run_offline_tests.py --configuration Debug --qt-root $qt --output "$run/Debug"
```

运行器默认执行八组，`--flags` 可以选择下表中的子集。用 `--executable` 检查同一程序的隔离构建，`--local-runtime` 让测试进程使用 EXE 目录部署的 Qt。

客户版先用 `tools/build_desktop.ps1 -Edition Customer` 构建，再将 `--executable` 指向 `x64/Customer/<配置>/触想工作台.exe`。客户版界面专项检查两个 48×32 档位、旧配置回落、主机推理及诊断入口的禁用、米制深度保存和设备管理。

| 标志 | 覆盖范围 |
|---|---|
| `port-ui-smoke-test` | 四个模块、七个成像入口及其设置恢复、B 策略、单目深度图与点云联动、单帧点云替换、米制快照、点云坐标、色标和清屏 |
| `rv-tuner-test` | 调参协议、互斥、异步停止、板端模式、Native16 分片、8×8 和 16×16 原生 ToF 的设置映射、米制帧与停止释放 |
| `tactile-serial-test` | 串口协议、型号、零点、失败与恢复 |
| `tactile-visualization-test` | 触觉显示、面板可见性、选择、曲线和断开 |
| `tactile-3d-test` | Qt Widgets 三维显示和生命周期 |
| `tactile-render-test` | 三维渲染、交互、显示刷新和关闭 |
| `rv-assist-recovery-test` | 480×640/F6 Native48 米制几何、配对边界、有效测点、尺度连续性及旧 H 诊断回放 |
| `rv-usb-enrollment-test` | SDK 3.1.0 和 ABI v3、六档能力、空设备库、直连档案和离线 USB 禁用 |

运行器清理继承的 RV、DCTS、Python 和 Qt 覆盖，并设置 `RV_TEST_OFFLINE_ONLY=1`。历史调参测试使用离线夹具，SDK 加载测试禁止打开硬件，串口使用测试输入。通过需要退出码为 0、有独立 PASS 断言且无 FAIL。`BYPASS` 和 `RESULT PASS` 汇总行不计作断言。历史板端安装和回滚测试使用临时文件系统。在 Windows 上运行其 shell 夹具需要 Git Bash。

```powershell
python -B -m unittest discover -s tests -p test_board_bootstrap.py -v
```

## 主机推理与离线回放

`--rv-assist-path-test` 加载本机的 `DctsDepthPro2Worker.exe`，核对 480×640 米制输出、单目结果、F6 Native48 和 B 策略。设 `DCTS_TEST_REUSE_TOF=1` 时要求复用合成 ToF 测量。需要匹配的 `DepthPro2Runtime/` 和兼容 NVIDIA GPU。在独立的 PowerShell 会话中运行，判定读取 JSON 的 `pass` 和进程退出码。

```powershell
$run = New-Item -ItemType Directory -Path (Join-Path $PWD ('x64/validation/assist-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
$exe = (Resolve-Path -LiteralPath 'x64/Release/触想工作台.exe').Path
$env:RV_TEST_OFFLINE_ONLY = '1'
$env:DCTS_TEST_REUSE_TOF = '1'
$report = Join-Path $run.FullName 'reuse.json'
$process = Start-Process -FilePath $exe -ArgumentList ('--rv-assist-path-test "{0}"' -f $report) -WindowStyle Hidden -Wait -PassThru
$process.ExitCode
Get-Content -LiteralPath $report
```

`--rv-depth-diagnostic-replay <单帧诊断目录> <报告.json>` 读取已保存的诊断并离线重放。用带引号的绝对路径传入诊断目录和报告。融合专项 `rv-assist-recovery-test` 使用合成平面验证同帧投影、有效点、尺度连续性及诊断格式。

## 实机专项

用受信任的本机设备档案运行。各入口要求 `DCTS_LIVE_ENABLE=1`、与本次设备一致的 `DCTS_LIVE_SERIAL`，并关闭 `RV_TEST_OFFLINE_ONLY`。成像切换、点云和测量采集还需 `DCTS_ALLOW_SAVED_TOF_SWITCH=1`。需要时通过 `DCTS_LIVE_CONFIG_SOURCE` 指定用于隔离测试的已保存配置目录。

| 主程序参数 | 验证内容 |
|---|---|
| `--usb-enrollment-live-validation` | USB 直连登记、同设备复用、采集和停采 |
| `--imaging-switch-validation` | 上位机辅助及 SDK 48×32 两档往返切换。设置 `DCTS_LIVE_NATIVE_SENSOR=1` 也覆盖 SDK 的四个 8×8 和 16×16 档。各阶段核对 RGB、深度和停采 |
| `--point-cloud-live-validation` | 正常版三档或客户版两档点云、有效测点、色标、操作及停采清屏 |
| `--host-assist-live-validation` | Depth Pro 2-S、同帧 ToF 配对、校正、渲染和停采 |
| `--measurement-live-validation` | RV1103 和 Gemini 335 测量文件的帧序号、时长、索引与有效数据 |

例如在有设备测试授权的独立 PowerShell 会话中运行成像切换，等待 GUI 进程退出后核对退出码及报告的 `passed=true`。

```powershell
$run = New-Item -ItemType Directory -Path (Join-Path $PWD ('x64/validation/live-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
$exe = (Resolve-Path -LiteralPath 'x64/Release/触想工作台.exe').Path
Remove-Item Env:RV_TEST_OFFLINE_ONLY -ErrorAction SilentlyContinue
$env:DCTS_LIVE_ENABLE = '1'
$env:DCTS_LIVE_SERIAL = Read-Host '本次已登记 RV1103 的序列号'
$env:DCTS_ALLOW_SAVED_TOF_SWITCH = '1'
$report = Join-Path $run.FullName 'imaging-switch.json'
$process = Start-Process -FilePath $exe -ArgumentList ('--imaging-switch-validation "{0}"' -f $report) -Wait -PassThru
$process.ExitCode
Get-Content -LiteralPath $report
```

原生 ToF 专项核对 SDK 档位、原始网格、有效米制深度、帧顺序、界面绘制及 RGB 收帧。设置 `DCTS_LIVE_PREPARE_BOARD=1` 时，先通过 SDK 检查和修复已登记设备的受管采集文件。离线界面专项核对 8×8 和 16×16 放大后的测区颜色、保存图像尺寸和米制数组。

客户版的同一成像切换入口执行 48×32 普通、高精度、普通三段。点云入口检查两档的同帧板端点、RGB、停采清屏和晚到帧。用正常版的设备档案验证时，将 `DCTS_LIVE_CONFIG_SOURCE` 指向正常版配置目录。

给 `--host-assist-live-validation` 设置 `DCTS_HOST_DIAGNOSTIC_DIRECTORY` 后，专项验证 ToF 测点、配对 RGB 对照、10 秒录制及重放。指定目录会保存 RGB 和测量数据。`DCTS_LIVE_SECONDS` 可设置每轮 30–300 秒的测量段。

测量采集专项默认依次录制 RV1103 普通档 30 秒和 Gemini 335 60 秒。可用 `DCTS_MEASUREMENT_LIVE_SOURCE=rv1103` 或 `gemini335` 单选设备。Gemini 335 使用已安装的 Orbbec SDK v2。设置 `DCTS_MEASUREMENT_LIVE_1200K=1` 验证 RV1103 普通 1200K，设置为 `high_accuracy` 验证高精度 1200K。两档均核对模式、传感器回读主积分及帧序号。录制文件位于报告旁的独立目录。

### 历史 ADB 温升验证入口

`tools/thermal/run.py` 启动 `--host-thermal-live-validation`，对已登记的 RV1103 测量 30 秒空闲、600 秒 Depth Pro 2-S 辅助推理及停采后的 60 秒。板端使用只读 ADB 采样，电脑侧记录 GPU 和 CPU 负载、温度与限频。报告区分测量序号、主机接收和实际绘制速率。该命令在单独的非离线 PowerShell 会话中执行。

```powershell
$serial = Read-Host '本次已登记 RV1103 的序列号'
$run = Join-Path $PWD ('x64/validation/thermal-host-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
python -B tools/thermal/run.py --mode host --serial $serial --output $run
```

离线解析测试为 `python -B -m unittest discover -s tests -p test_thermal_analysis.py -v`。板端采样数据、推理报告及时间线分别保存在 `board.jsonl`、`inference.json` 和 `timeline.csv`，汇总见 `summary.json`。

## 有效代码行数与入口检查

```powershell
$run = Join-Path $PWD ('x64/validation/source-check-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
python -B tools/audit_source.py --root . --output "$run/source-inventory.json"
```

统计范围为根目录、`sensors/`、`tests/`、`qml/`、`tools/` 的第一方 C++/头文件/inc/QML/Python/PowerShell/shell/awk/cmd。按物理非空、非纯注释行计数，大括号、预处理和字符串算代码；Python docstring 算字符串。排除第三方、生成文件、模型、资源数据、运行包和 `x64/`。报告分别列出主机与原生支持、板端 runtime、测试、脚本及 SHA256。这是源码规模口径，不表示每行都在普通产品入口可达。

同时检查项目本地编译/MOC/RCC/资源/None 路径、filters 对应关系、RCC 内容及根目录/docs/tools/归档索引的 Markdown 本地链接，并核对触觉 CAD 来源路径及 SHA256。当前入口缺失会返回非零退出码；忽略目录中的历史验证报告缺失单独列为提示。

## 板端公共源码

当前交付使用报告标定对应的 F6 原生采集程序，入口见 [原生采集构建](../tools/rv_capture/README.md)。离线验证覆盖分片、网格／身份／告警校验、原生 ToF 会话预设及安装回滚。旧 F1/F2、多模型切换和 NPU 一致性记录属于历史。

## 历史验证记录

以下结果对应各自的旧版程序、模型、设备和测试工况。复现 UniDepth 使用当时的程序及匹配的模型包，见 [UniDepth 多后端历史记录](MULTI_BACKEND_INFERENCE.md)。

### USB 自动发现与免填参数（2026-11-09）

当时的自动发现流程使用完整文件检查回执。已保存设备仅在完整资源检查回执通过后自动选择并启动 H；新设备、文件更新或缺少 USB 网络配置时，确认后自动打开设备管理并调用同一个 USB 连接入口。新设备使用安装 180°／推理 0°，已有方向与临时 FOV 设置保留；USB 登录复用保存凭据或工厂 root 登录，不弹出密码或角度输入框。

USB 专项覆盖完整检查回执的真／假／缺失／错误类型、错误序列号和旧 boot ID，以及确认后的换板、公钥变化、重插和重启拒绝。UI 专项实际操作中英文添加／更新提示，验证取消、确认后自动进入设置与排队前停止撤销；所有入口继续受离线保护。原生 worker 自检验证检查与安装共用完整文件计划、长期参数保留、缺凭据的免提示分支及非工厂账号拒绝。Python 测试继续在映射的临时文件系统中执行安装与回滚脚本。

Release/Debug 构建均通过，两配置各八组离线专项、各 1320 个断言通过，原生 worker 各 61 项自检通过，Python 44 项测试通过。当轮源码路径审计为 0 个问题，14 个已有历史归档路径警告与上轮一致。

本轮证据在 `x64/validation/usb_auto_connect_20261109/`，最终结果见该目录 `verification.json`。全部操作仅限离线构建和模拟测试，未连接、重启或修改 RV1103，也未制作安装包或上传源码。离线通过不代表实机深度出帧、物理精度或 FPS 验收。

### USB 一键检查、修复与重复连接（2026-11-07）

本轮证据保存在 `x64/validation/usb_reconcile_20261107/`。Release/Debug 构建及各 3 组相关离线专项通过：USB 26、界面 144、调参 341，共每配置 511 个检查；两个原生 worker 各 46 项自检通过。临时文件系统中的 21 项安装/相机接管测试覆盖任意旧驱动替换、缺失文件补装、完整上传摘要、加密初始化失败、F2 能力、切换失败回滚和回滚未完成时保留原文件及故障。

随后在同步 `main` 前重新运行 Release/Debug 全部八组离线专项，每配置 1336 项检查通过，两个原生 worker 各 46 项自检通过。工作区与实际脱敏上传快照的 Python 测试各 40 项通过。源码路径审计与前次保持相同的 24 项私有源码、外部归档及 CAD 引用缺项，公开范围继续排除这些本地依赖。本次源码、脱敏清单和测试证据保存在本地 `x64/validation/github_main_20261107/`，不发布新安装包或 Release。

已在当前 F1 实机通过普通主窗口的 USB 按钮验证首次登记与已有设备重复连接。首次在隔离的本地设备档案中登记，并暂时移开受管日志脚本以触发完整补装；板端安装保持生效。首次连接收到 30 份有效 384×256 径向米制深度并实际绘制 25 帧，重复点击后选中同一档案、跳过文件写入，再收到 30 份并绘制 25 帧。两次均正常停止并核对设备空闲，本地原配置、已保存传感器参数和本次启动身份保持，未保存实景图像或测量数组。报告为 `live-release-new-repair/summary.json`。源码审计仍为既有 24 个私有源码/归档/CAD 路径问题。

### UniDepth 同输入对照

历史对照使用当时版本的 `DctsInferenceWorker.exe`、配套模型包和 `tools/verify_port.py`。同网格比较时选用相同后端，按需指定参考运行包。迁移对照使用 `--comparison monocular` 核对裁切前的预测。

竖幅方案各覆盖 25 个 F6 场景，包括 0°/180°、不同 RGB、165 ms 捕获时间边界、G/B 复用策略、错误网格/FOV/方向/有效位、低 SNR 和 RGB-only。检查裁切前预测、480×640 输出身份、米制深度、诊断及停止重启。包清单逐文件核对 SHA256。协议和合成样本共用 `tools/unidepth/native48_contract.py`。

这类测试验证代码与数据合同，不证明实际物理精度、客户显卡性能或设备安装标定。连续尺度平滑使用单调时间；逐字节比较采用固定场景，不能把不同墙钟节奏下的长流输出直接当作同一帧比较。

### 首次部署与临时 F1（2026-11-07）

2026-11-07 本轮首次部署及主机实测证据位于 `x64/validation/fresh_board_setup_20261107/`。当时的设备通过普通 USB 一键连接完成全量安装（`usb-live-REDACTED_DEVICE.json`），不再要求旧 V1.2.0 驱动哈希。原厂摄像头退出后遗留的 DHCP 进程仅在身份、参数、启动时间、RTSP 监听和摄像头 DMA 句柄均匹配时接管；原始摄像头配置及驱动已备份。

按用户明确要求，当时的设备档案开启临时 F1，模型输入 0°、相机安装 180°；传感器独立读回仍为 F=6。新版驱动使用 F1 射线表实际重建 XYZ，并保留真实 FOV 回执。`host-live-REDACTED_DEVICE-software-fov1.json` 两轮 30 秒均通过：深度 24.04 / 24.56 fps，视频约 29.94 / 29.96 fps，最大配对差 96.33 / 93.64 ms，正常停止、重新启动、无晚到结果，长期设置和脚本未改。该结果只证明临时参数下的部署与运行流程，不替代后续正式标定。

本轮最终 Release/Debug 均构建通过；各配置相关四组离线专项合计 569 个断言通过（界面 146、调参 341、H 恢复 61、USB 21），两个原生控制 worker 各 46 项通过。隔离安装/相机接管 Python 测试 13 项、离线运行器 Python 测试 4 项，以及软件 FOV/回执/告警策略 C 测试 1803 个断言通过。源码审计仍为原有 24 处缺少私有板端源码/归档/CAD 路径，本轮没有新增路径问题。当时的临时标定的开关、真实/处理 FOV 区分、过期会话拒绝和隐藏差异拒绝均纳入专项。

### 三模型切换与界面提示清理（2026-11-07）

按用户要求，深度图移除整段临时标定提示栏，主机推理运行状态也不再追加同类文字；设备管理保留“使用所选 FOV 标定”配置项。实际 FOV、处理 FOV、会话身份及标定回执校验仍保留在数据链路中。

本次 Release/Debug 重新构建并更新翻译，两配置的界面、调参协议、H 恢复专项各 546 项检查通过（144／341／61），两个原生控制 worker 各 46 项自检通过。源码审计仍有既有的 24 处私有板端源码、归档及 CAD 路径问题，相对前次审计没有新增问题。

使用当时登记的设备和实际 Release 主窗口，按 LiteSR → Tiny → H → LiteSR → H → Tiny → LiteSR → H 运行八段，各测量 30 秒，覆盖三个模型之间全部六个切换方向。八段全部通过，共接收 5300 份新深度；同时检查模型/单位/输出尺寸、原生网格、输入方向、有效米制值、相机显隐和实际绘制。

| 模型 | 深度接收 FPS | 实际绘制 FPS | 相机策略 |
|---|---:|---:|---|
| LiteSR 384×256 | 11.90–12.10 | 11.87–12.07 | 关闭并隐藏 |
| Tiny NV12 128×80 | 30.11 | 29.74–29.87 | 恢复同帧 80×128 NV12 |
| 主机 640×480 H | 25.76–27.54 | 25.13–26.97 | 恢复 2304×1296 H.265，约 30 FPS |

Tiny 两轮共 60 次颜色检查均有可见颜色，不符像素为 0，RGB/深度裁剪画幅保持。H 最大测量配对差 95.21 ms，未超过 100 ms；历史尺度帧未携带旧原生校正点。三个模型的实际界面截图均确认无临时标定提示。最终正常停止，板端空闲，停止后的晚到结果为 0；两份板端长期参数/服务脚本和本地设备档案、用户设置的 SHA256 均保持不变。

本轮记录位于 `x64/validation/model_switch_20261107/`：`live_three_models.json` 为实机明细，`live_summary.json` 核对退出码和本地配置，`offline_release/`、`offline_debug/` 为离线报告，`live_three_models.json.stage_*.png` 为真实主窗口画面。本次验证运行和切换功能，不增加物理精度结论。

### 板端 LiteSR 与主机 H 的温升对照（2026-11-07）

2026-11-07，当时设备的两轮均完整运行 600 秒并正常停止，晚到结果为 0，长期参数和脚本前后相同。板端记录在 `x64/validation/thermal_20261107_REDACTED_DEVICE_run2/`，辅助推理记录在 `x64/validation/thermal_host_20261107_REDACTED_DEVICE_run1/`，汇总与对比图在 `x64/validation/thermal_comparison_20261107/`。板端 run1 仅为 exec-out 通道不兼容的启动失败，未启动推理，不计为实测。辅助轮未再次断电，记录实际起始温度；外界温度及散热片/风扇情况未独立确认。

| 实际工况 | RV1103 峰值温度 | 全程深度输出均速 | 前／后各一分钟均速 | 模型与整帧平均耗时 |
|---|---|---|---|---|
| LiteSR 384×256，摄像头关闭 | 62.425°C | 11.733 fps | 11.931 → 11.699 fps（-1.94%） | NPU 42.853 ms，板端整帧 84.457 ms |
| 主机 H 640×480，完整 H.265 链路 | 92.647°C | 27.060 fps | 26.948 → 27.268 fps（+1.19%） | 主机模型 15.266 ms，主机整帧 21.198 ms |

两轮原生 dToF 序号速率约 15.05 fps，CPU 允许最高频率始终 1104 MHz，采用原有 ondemand。辅助轮原视频约 29.96 fps，最大配对差 98.36 ms，16191 帧含当前有效 dToF、其中 7554 帧为明确复用；9407 帧实际接受 dToF 矫正，其他帧沿用既有兜底及历史尺度规则。电脑 GPU 最高 68°C，硬件/软件热限制累计时间本轮增量均为 0，也未观察到 GPU 功耗封顶标志。该结果验证运行/热性能与协议，临时 F1 仍不代表完成正式几何标定。

没有观察到与升温一致的持续限频或吞吐下降，但辅助轮板温已超过固件配置的 75/85°C 被动点。只读核查显示 thermal_zone0 为 enabled/step_wise，标准 cooling_device 不存在，设备树 soc-thermal 没有 cooling-maps；rockchip-system-monitor 驱动已绑定，内核 config 不可读。因此不能据此宣称设备没有任何热保护，也不能把可读的温控点当作已验证的 CPU 限频保护；仍需结合实际固件源码/绑定核对。检查原件位于辅助轮的 `thermal-policy-inspection.txt` 与 `system-monitor-inspection.txt`，未修改任何温控/调频参数。两轮结束后推理和采样进程均退出，设备保持空闲降温。

### GUI 减载与实机延迟对照（2026-11-10）

当轮统一离线运行器可用 `--executable <绝对 EXE 路径>` 检查主工程的隔离构建，仍仅接受原有八组标志。Release 全八组和 Debug 相关四组分别保存报告；高 DPI 像素断言按抓图的 devicePixelRatio 换算物理像素，不改变产品镜像、旋转或渲染。

模型精度对照使用 `tools/verify_port.py`，候选和参考均指定各自的 ASCII 命名 `DctsInferenceWorker.exe` 及匹配运行包，覆盖 0°／180°、同帧身份、100 ms 边界、无效输入拒绝和停止重启，要求预测、H 深度和诊断字节一致。RGB 张量转换另检查正常／带行填充输入、180°图像及 ARGB32 来源。色表缓存对照原始 Qt 转换，遍历全部色相状态的取整边界，并检查百万组范围／像素组合及 NaN、无穷、零值和超范围黑色规则。

当轮通过 `--host-assist-live-validation` 设置进程环境 `DCTS_LIVE_SECONDS=120`：先预热 10 秒，再各测两轮，每轮限制 30–300 秒；默认行为仍为原两轮各 30 秒。扩展测量按秒记录接收与实际绘制 FPS，按绘制帧汇总 RGB 准备、父进程校验、着色排队／计算、绘制及 `source_to_paint_ms`。后者从主机解码完成时间起算，不包含传输和摄像头曝光延迟。报告只保存计数、耗时、身份及参数，不保存真实图像或测量数组。

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

### 历史 UniDepth 的 CPU／集显可行性实验

[host_fallback_probe.py](../tools/host_fallback_probe.py) 用于测试历史 RGB672×504 → 640×480 UniDepth ONNX 的执行后端。只生成三组确定性 RGB、分别检查 0°／180°输入及输出逆恢复，不启动上位机普通主窗口、相机、串口或板端连接。它记录实际执行 provider、纯模型延迟、内存、输入变化及 A→B→A 重复一致性；应用层专项仍由本页的统一离线运行器执行。

可选后端为 `cpu`、`directml`、`openvino-cpu`、`openvino-gpu`、`openvino-ep-cpu`。其中最后一项使用 ORT 插件 API 显式选择 CPU，可配合 `--memory-model` 检查从模型字节创建会话。它验证内存加载可用性，尚未验证产品加密 DLL、安装构建或自动回退。

开发依赖采用独立 Python 3.11 环境。本次 ORT/OpenVINO 插件环境为 `onnxruntime==1.31.1`、`onnxruntime-ep-openvino==1.8.1`、`numpy==2.4.6`、`psutil==7.2.2`；独立 OpenVINO 运行时为 `openvino==2026.4.1`。DirectML 另用 `onnxruntime-directml==1.24.4` 的环境，避免不同 ORT wheel 共用同一个 Python 包目录。

```powershell
$model = Read-Host '已恢复的历史 UniDepth ONNX 绝对路径'
$run = Join-Path $PWD ('x64/validation/unidepth-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
python -B tools/host_fallback_probe.py --model $model --backend openvino-ep-cpu --threads 8 --memory-model --output $run
```

用 `--reference <RTX同输入预测.npz>` 执行数值对照；未给参考时只报告运行功能和速度。参考须使用同一模型、六组同输入和实际方向，当轮基准由 `DctsInferenceWorker.exe` 生成，并先通过 `verify_port.py`。参考输出键为 `image{0,1,2}_r{0,180}__depth`，值为恢复到标定网格的 `[1,1,480,640]` float32 预测。平均／最大相对数值门槛沿用当前模型的 0.25%／1%；超限返回非零并保留测速，不据此宣称替换后端验收通过。

2026-11-09 实验原件位于 `x64/validation/cpu_igpu_20261109_01/`，结论见该目录 `REPORT.md` 和 `summary.json`。实验结论对应当时的 UniDepth；FP32 计算副本保留既有 FP16 权重的数值并提高计算精度，不恢复丢失的原始 FP32 权重，不改变网络结构或输入／输出尺寸。纯推理 FPS 不等于 GUI FPS；合成数值差异不等于物理测距误差。测试机只具备 RTX 3080 Ti，DirectML 运行结果不能视为 Intel／AMD 集显验收。
