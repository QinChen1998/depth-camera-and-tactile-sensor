# 项目架构

主机推理设备层见 [UniDepth 推理设备选择](MULTI_BACKEND_INFERENCE.md)。`RvInferenceSettings` 管理启动候选检测、手动测速、用户选择和按候选顺序回退。`RvInferenceRuntime` 管理硬件候选、固定模型身份、运行库指纹和原生 OpenVINO 会话。`RvAssistDepth` 承担预处理、全局尺度校正与独立 worker 生命周期。TensorRT ORT 与兼容 ORT／OpenVINO 在子进程中隔离。

本项目是独立的 Windows x64 传感器上位机，主窗口提供磁触觉、深度图、点云和摄像头原视频。dToF 运行预设由成像方案选择。本文描述当前日常版本的架构。当前保护实现及安装包约定见 [模型保护](MODEL_PROTECTION.md)，维护约束见工作区的 AGENTS.md（仅本地保存）。

## 产品边界

主窗口提供磁触觉、深度图、点云和原视频四个模块。深度图提供以下三项，旧设置中的退役选项迁移到上位机辅助推理。

| 显示名称 | 产品入口 ID | 推理和处理位置 |
|---|---|---|
| 上位机辅助推理 | `host_unidepth_rgb672_vga_fp16_tof48_b` | 主机 UniDepth 以预算内最新的合格 48×32 ToF 拟合全局尺度，允许多次使用 |
| 板端推理 | `board_native_tof48` | 48×32 普通原生 ToF 径向距离，无神经网络 |
| 板端推理高精度 | `board_native_tof48_high_accuracy` | 48×32 高精度原生 ToF 径向距离，无神经网络 |

主机 worker 的模型 ID 为 `unidepth_rgb672_vga_fp16_tof48`，输出 480×640。成像入口见 `DCTS_APP.json`，安装构建核对菜单、板端采集模式及载荷清单。

移植来源 `E:/WORK/Dexterous Hand Visualization` 只读。当前工程的源码、模型包、RV worker、资源和配置均独立；日常运行不依赖该来源项目目录。

## 模块与进程

```mermaid
flowchart TD
    Main[main.cpp] --> Window[SensorMainWindow]
    Window --> Tactile[磁触觉显示与串口源]
    Window --> Session[RvSessionController 共享成像会话]
    Session --> Camera[原视频页面]
    Session --> Depth[深度页面]
    Session --> PointCloud[辅助深度和原生 ToF 点云]
    Session --> Tuner[dToF 调参控制器]
    Session --> Control[RvControlWorker.exe]
    Tuner --> Control
    Control --> Board[设备端服务与传感器]
    Board --> Streams[视频与原生 dToF 接收]
    Streams --> Session
    Session --> Assist[主程序的 rv-assist-worker 子进程]
    Assist --> ORT[ORT TensorRT-RTX 模型推理与全局尺度校正]
    ORT --> Session
```

`SensorMainWindow` 创建一个共享 `RvSessionController`，深度页面、原视频页面和 dToF 调参模块围绕同一个设备会话工作。不能为每个页面各自启动一套采集服务。板端仅运行受信任档案对应的 F6 原生 dToF 采集、状态服务和 H.265 视频。原生程序接收 TMF9/SPI 数据；不加载模型，不依赖 RKNN 或模型解密库。

设备档案的可选布尔值 `temporary_fov_calibration` 默认关闭，仅在明确选择时向采集进程传递 `DCTS_PROCESSING_FOV`。SPI 驱动先读取并保留真实寄存器值，再用指定射线表重建 XYZ，IPC 的处理 FOV 与这些坐标一致。原 `sensor_readback.json` 始终记录传感器原值，另以原子写入的 `.calibration.json` 保存真实 FOV、处理 FOV、临时标志及 `dtof_<generation>`。控制 worker 与共享会话均验证该回执的本次身份；主机元数据保留此区别，界面不显示临时标定状态提示。未开启时不覆盖处理 FOV，继续严格拒绝不匹配的标定。

| 入口或文件 | 职责 |
|---|---|
| `main.cpp` | 应用初始化、独立配置、离线测试分发、主机推理 worker 分发 |
| `SensorMainWindow.*` | 模块与菜单、布局恢复、异步停止和退出等待 |
| `TactileDockWidget.*`、`sensors/TactileVisualizationWidget.*` | 磁触觉操作、曲线、三维显示与串口连接 |
| `sensors/TactileSerialSource.*` | 串口测量、设备型号识别及有界自动置零 |
| `sensors/TactileSceneAdapter.*`、`TactileSurfaceModel.*`、`qml/` | 三维场景数据与表面资源 |
| `sensors/RvSessionController.*` | 设备身份、成像状态机、配对、结果时效、共享采集和停止 |
| `sensors/RvHdVideoStream.h`、`RvCameraStream.h`、`RvDtofBoardStream.*` | 高清视频、RGB 处理及原生 dToF 接收 |
| `sensors/RvAssistDepth.*` | 原生模型会话、worker IPC、模型输入方向、预测逆恢复和融合调用 |
| `sensors/RvAssistFusion.*`、`RvDepthDiagnostic.*`、`RvDepthCoverage.*` | 米制融合、尺度连续性、校正点及显示用预测 |
| `sensors/RvDepthView.*`、`DepthMapWidget.*`、`RtspCameraPage.*` | 深度与视频显示、悬停数值、显示设置和快照 |
| `sensors/RvPointCloudWidget.*`、`qml/RvPointCloudView.qml` | 辅助深度和原生 ToF 反投影、点云显示和视角操作 |
| `sensors/RvDeviceStore.*`、`RvDeviceManagerDialog.*` | 受信任设备档案、固定默认标定和输入方向 |
| `sensors/RvUsbEnrollment.*` | USB ADB 发现、用户确认、板端与 RNDIS 网卡配网、受信任档案登记 |
| `sensors/RvTunerController.*`、`RvTunerWidget.*`、`RvTunerProtocol.*` | 独占调参、异步操作、停止、长期参数保存 |
| `RvControlWorker.cpp`、`RvControlProtocol.*` | 原生 libssh2/OpenSSL 控制、身份校验与板端服务操作 |
| `RuntimeDiscovery.*` | 应用根目录、运行时覆盖路径、worker 清单和依赖校验 |

点云与深度图共享成像会话。48×32 档订阅 `tofFrameReady`，通过 F6 GEO2 射线和相机平移得到原生测点的 XYZ。上位机辅助推理档订阅 `assistDepthSnapshotReady`，校验 480×640 竖向输出标识，并用 `outputProfile` 的内参将相机 Z 逐像素反投影。后台一次转换一帧，转换期间保留最新待处理帧。Qt Quick 3D 用实例化圆片绘制有效像素，着色器在屏幕空间按设定点径展开，Z 色标使用点云自己的设置。两种数据源都使用 X 向右、Y 向上、前方为负 Z 的场景坐标。停采、切档或过期时清空数据并保留视角。

主机模型 worker 使用同一构建产出的 ASCII 文件名 `DctsInferenceWorker.exe`，以 `--rv-assist-worker` 启动 `QCoreApplication` 和二进制 IPC。`RuntimeDiscovery::NativeWorkerResult` 返回原生 `RvControlWorker.exe` 的路径和校验错误，`nativeWorkerEnvironment()` 限制设备控制进程的 DLL/Qt 环境；没有 Python 回退。

深度页面绑定共享成像会话。上位机辅助推理使用真实的 48×32 ToF 射线表和 RGB672 UniDepth 模型，输出 480×640 camera-Z 深度。两档原生 ToF 显示径向测量并保持原始索引和有效位。工程调参保留其支持的网格、独占及保存流程。

普通启动由 `main.cpp` 显式请求一次磁触觉默认串口连接，待模块可见和 QML 就绪后执行。控件构造、显示、离线专项及实机深度专项均不会自行打开磁触觉串口。默认参数优先恢复 `tactile/serial` 中上次经有效回包确认的端口、波特率、地址和模块。已保存端口缺失时保持待连接。自动、手动和重连均先以 0x03 读取 0x44C0，严格校验地址、功能码、长度、CRC 和型号值 1/2/3。优先使用当前地址，失败后在当前波特率尝试公共地址 0。公共地址恢复适用于单设备总线，需读出 0x466C 的实际地址并用该地址复读同一型号。未知型号或识别失败显示错误状态，不改变设置面板的显隐。界面按同次 session/generation 的已确认型号同步更新选中模块、硬件类型和实际地址，首帧使用实际型号的 20/16 点布局。手动修改通信参数后连接也走相同流程。

首份 CRC 正确、量程有效且身份匹配的数据到达后，停止连接响应计时并进入有界的自动 delta 置零。`TactileAutoZero` 在串口线程逐帧处理原生测量，先舍弃 150 ms 的稳定等待段，再检查最近 300 ms 的有效帧：至少 6 帧、跨度至少 200 ms、相邻有效帧间隙不超过 120 ms，每点每轴极差不超过 150 counts，前后半段均值差不超过 40 counts。最早 500 ms 完成稳定均值置零；900 ms 到期仍未形成稳定均值时，以本次连接最新一份 CRC 正确、量程有效且身份匹配的原生帧自动置零一次。兜底与正常均值置零使用同一个完成流程，不显示未完成或要求手动置零的提示。计时从首份有效测量开始；尚无有效测量、断开或显式取消不会生成零点。均值按原始单 count 精度取整并重算合力，只用作显示基线，不进入原始帧或采集历史。完成结果随最新帧持续携带，GUI 合并采样不会丢失零点。等待期间隐藏力箭头和曲线，自动置零完成后隐藏状态行并回收其布局空间，不显示完成提示、帧数或耗时，通信诊断开关也不会恢复该提示；均值置零和到期单帧置零均不改变设置面板的显隐，手动置零按钮继续可用。默认约 1.5 秒未收到首份有效数据则异步停止，较长通信超时会相应增加连接等待时间。每个连接会话只进行一次有界自动置零，完成后不跟随受力更新基线。运行中结合 Windows 串口到达与移除通知及端口列表轮询，检测到记住的端口新出现时，按当前界面上的波特率和从站 ID 尝试连接。没有记住的端口时，仅在单个串口新出现时尝试。热插拔快速探测窗口至少为 10 秒。失败后保留当前设置面板显隐，端口仍存在且被选中时，等待 3 秒后再执行一轮型号识别，当前地址和公共地址各尝试一次。识别成功且首帧有效后继续使用同一个串口采集，失败则在等待期间释放串口。端口缺失时只监测插拔。用户主动断开后，同一次插接保持停止。

“连接设置”每次启动默认展开，“显示设置”默认收起，之后只由用户手动显隐。两个面板不随连接、置零、故障、断开、重试或模块启停自动展开或收起；不保存展开状态，也不读取旧的 `tactile/display/expanded` 偏好。其余显示参数继续持久保存。

磁触觉请求后间隔默认及下限均为 10 ms，可在采集中修改。串口线程在一笔请求完整结束后用 Windows 高精度等待定时器计时，停止、配置操作和间隔编辑可唤醒等待；系统无法创建该定时器时退回有界睡眠。自动置零期间临时使用 10 ms 请求后间隔，成功、失败或取消后恢复当前用户请求间隔，界面数值和持久偏好不被临时采样覆盖。置零开始后单次响应等待受剩余 900 ms 窗口约束，避免较长通信超时拖住本次尝试。单次请求的传输和响应耗时另计，GUI 仍按独立的 33 ms 定时器读取最新数据。轻微读数抑制默认开启，用户保存的显示偏好继续生效；原始数组和串口协议不受显示置零或抑制影响。

磁触觉“显示方式”保留 gridSize 2/3/4 档，名称按当前模块的实际点数生成：F/G 为 20/65/136，S 为 16/49/100。首次启动选择最高点数档和流畅渲染，后续恢复用户选择。`TactileSurfaceModel` 的采样函数复用既有空间插值权重；列表对当前帧、零点基线采样，曲线对原始历史逐帧采样，不向 `Frame` 或 `TactileHistory` 填入伪造的原生测点。列表和曲线分别在 100 ms 和 66 ms 内合并刷新请求，三维发布和历史采样沿用原有节奏。点选、置零和密度切换即时刷新相关视图。选择超出较低密度范围时收敛到有效行。

列表仅输出从原始基线计算的 delta，轴表头为 X、Y、Z；基线或数据无效时显示“--”，不回退显示当前读数。共享 `zeroSmallComponents` 仅对 X、Y 应用原有的包容边界阈值，Z 原样通过；模型、曲线、列表及插值点均采用此策略。三维模型保持原有的基线相减及插值顺序。过滤仅影响显示，不改原始帧、串口诊断、跳变准入或历史样本。

通信状态每次启动时关闭，隐藏的状态行不占用空间。开启后，串口栏显示端口、ID、接收计数和通信错误，列表下方只在有可显示的过滤错误时出现提示。QML 测点详情显示点号与数值。模拟数据源、模拟场景和底部恢复模拟/场景下拉/暂停按钮均已删除，硬件断开使用连接设置中的异步停止入口。窗口启停与可见性只控制显示刷新和串口挂起，不产生读数。无设备、断开、错误型号页面或数据失效时保留模型并显示“设备未连接/数据不可用”；其他型号不会补充模拟帧。固定原生格式样本仅存在于测试目录，用于离线渲染回归，不提供产品入口。

## 主机深度数据链路

输入方向在「设置 → 设备管理 → 上位机推理输入朝向」调整。已有设备沿用旧版深度图按设备及安装角度保存的方向，设备管理显示该生效值。保存设备时，当前选中的方向写入设备档案，并移除对应的旧偏好。

上位机在 RGB 入队前确定对应的 ToF 配对。单个后台线程负责 RGB672 校正与 CHW 张量转换，最多保留一个待处理任务、一个完成结果和一条待派发通知。停止、会话切换与视频重连会取消待处理结果。结果提交前复核会话代次与设备身份，推理时效仍从原始解码时间起算。

深度视图在一次遍历中解码米制数组、检查有限值并统计视场内正值，按当前 480×640 网格校验模型身份、FOV 和有效像素数。着色线程缓存现有色带的 24,001 个 Qt RGB 值。离线测试覆盖 Qt 色相取整边界、有效范围和无效值，并检查后台处理前后的 RGB 与 ToF 身份。

1. 原视频和原生 dToF 数据进入共享会话，携带设备身份、generation、frame、原生序号和时间戳等元数据。
2. RGB 使用对应标定图处理到模型输入网格。模型输入为 `[1,3,504,672]` 的 `uint8`，唯一输出 `depth` 为 `[1,1,480,640]` 的 `float32` camera-Z 米制深度。
3. 根据设备档案中的实际模型输入方向，按 0°或 180°处理输入；180°输入按每个平面通道反转。
4. ORT 1.31.1 TensorRT-RTX EP 执行模型，预测结果先逆恢复到原标定网格，再使用原始 dToF 网格做全局尺度校正。显示旋转独立，不能根据 GUI 是否正立判断模型输入是否正确。
5. worker 返回米制深度、同帧元数据及 `mono_reasons_v1` 显示载荷；会话检查身份、方向和时效后交给显示层。

原生控制 worker 启动 mode24 普通采集，并校验板端能力及实际 48×32 网格。共享会话区分 mode24 的主机推理与原生显示。切换成像方案时，会话协调器处理采集 generation、推理和尺度参考。预设为单目标，关闭 Signal 和 Noise，沿用已保存的周期、积分和 SNR。

上位机从 2304×1296 解码帧直接校正取样 672×504，模型内裁去左右各 16、上下各 12 像素，原始神经网络预测仍为 640×480。2026-11-10 起，主机从该预测裁取 `[140,0,360,480]` 并以方形像素等比采样为 480×640，然后在竖向网格拟合全局尺度并应用 ToF 几何掩码。配对 RGB 从输入 `[156,12,360,480]` 采用同一变换；输出 K 为 `[513.5802469135803,513.5802469135803,239.5,319.5]`。原输入映射、模型权重和输出逆旋转保持，显示旋转独立。

`RvCameraView::profile` 表示冻结的模型相机，`outputProfile` 表示当前交付网格，显示层可读取显式网格身份。实时 IPC 要求 `assist_output_grid=tof_portrait_crop_v1`、模型尺寸及裁剪矩形完全匹配。480×640 与 640×480 字节数相同，不能仅按长度推断行列。`host_contract.json` 的 `output_size` 继续描述冻结图输出；实际产品输出由上述显式帧契约定义。原生 48×32 数组不旋转、拉伸或改写。

上位机推理由 RGB 触发，选取已收到且满足时间预算的最新无告警 ToF。配对总预算为 200 ms，包含 35 ms 时钟裕量。复用同一序号时，worker 核对测量时间戳、原生数组与 FOV。全局拟合接受 0.02–50 倍的尺度，按 350 ms 时间常数在对数尺度上平滑，每次更新限于 ×1.10 或其倒数。两次有效拟合之间沿用最近应用的倍率，尚无参考时使用单目预测。输出修订为 `global_scale_reuse_latest_rgb_portrait_v1`。

`mono_reasons_v1` 在同帧 IPC 中提供预测和校正原因位，供视场外补显与离线契约检查使用。米制数组的几何未知区域记为零，普通保存使用米制结果。

## 原生采集与板端准入

USB 一键连接和「安装板端采集支持」共用事务式文件计划。板端运行文件、SPI 驱动、标定内外参、相机 ISP 配置脚本、调参及会话脚本逐项校验 SHA256。安装检查新 F6 程序和当前固件配置的兼容性，保留可读取的长期参数及原运行目录供回滚。旧模型残留随活动目录替换退出运行路径。

自动 USB 发现先核对 ADB 身份与对应 USB 父设备的在线 RNDIS 网卡；缺席、枚举延迟及拔出均不弹导入失败。板端探测等待固件地址稳定，手动接入可补充受管地址，保持其他地址；复核仍要求同一序列号、公钥和 boot ID。随后复用上述完整文件计划，通过 `--check-usb-enrollment` 只读核对全部安装文件及保留长期参数后的脚本。已保存且完全匹配的设备直接设为当前设备，走 `--automatic-start` 启动当前选择的成像方案；已移除仅凭 SSH 端口可达便自动启动的独立路径。新设备或文件需要更新时只询问是否前往设置，确认后打开设备管理并排队调用与 USB 按钮相同的入口。该入口自动获取身份、登录和标定，新设备使用原始相机 0° 标定输入／0° 推理方向；已保存方向继续保留。USB 登录复用保存凭据或工厂 root 登录，不显示密码框。确认绑定 USB transport、序列号、公钥和 boot ID，停止修订号及设备库修订检查防止异步结果在停止、换板或修改档案后启动采集。

产品使用 mode24（上位机辅助推理与普通原生显示）及 mode25（高精度原生显示）。工程采集使用 mode14 的 16×16 网格。三个产品档位通过 LSG2 接收 48×32 测量、时钟和实际 VENC 进度。上位机辅助推理请求原始测距，两档原生显示可额外请求同帧孤立缺测估计。

登记时推理设备仍在检测，自动启动保留同一 USB transport、设备档案修订和 Stop 修订。检测完成后复核 USB、RNDIS、SSH 身份及 boot ID。整套断流恢复使用板端协同能力和同 boot 的上一 generation 干净退出回执。所有者、进程、设备句柄及监听端口释放后才重开，故障锁和不匹配的身份会阻止恢复。

## 显示和数值契约

原视频底栏的画面旋转联动原视频与深度图。两个原生 ToF 档位另有独立的“输入角度”按钮，默认额外顺时针 90°，允许 0–359°并保存；该角度不改变 UniDepth、模型输入、标定、原生数组或保存图像。`RvDepthView` 仅对启用原生角度的画布做任意角度适配与悬停逆变换。深度图底栏的色标范围需点击「应用」才生效，范围为 0.01–11 m，且最小值小于最大值。

`RvDepthView::presentationImage()` 可以使用经过同帧验证的预测补充 dToF 几何视场外的显示。`snapshotImage()`、`depthAt()`、浮点数组和普通保存继续使用原有米制与掩码规则。显示补充不产生虚构的 dToF 支持，也不改变校正点的来源。诊断缺失、身份不符、非有限数或过期时，显示回到原有受限图像。

原视频和深度的启停与重连调用同一个设备会话。自动启动只在受信任设备、原生采集程序和所选会话预设校验通过后执行；缺少主机 GPU 或推理运行文件时明确报告原因。USB 首次部署及修复继续校验同次启动身份、标定、驱动摘要、采集占用和 Native16 能力，并保留原厂启动配置与回滚。主窗口不注册调参窗口；工程调参控制器的独占、异步停止和退出等待保持。

## 目录和资源

| 位置 | 内容及处理方式 |
|---|---|
| 根目录 `.sln`、`.vcxproj`、`.filters` | 主构建工程；不要改为以临时项目替代主工程 |
| `sensors/` | 数据源、会话、融合、显示和调参实现 |
| `assets/camera/` | 标定、几何、校正图及来源信息 |
| `assets/tactile/`、`models/tactile/`、`qml/` | 磁触觉布局、表面和场景 |
| `archives/cad/tactile/` | 三种触觉的 STEP 原件；由 `models/tactile/surface_layouts.json` 相对定位并记录 SHA256，不参与运行 |
| `archives/validation/` | 历史构建日志，以及 2026-11-08 验证历史 ZIP、逐文件 SHA256 清单和原路径索引；报告、历史安装包、备份与私有输入在归档中完整保留 |
| `SensorResources.qrc`、`Component_depth.qrc`、`Component_tactile.qrc` | 编译进程序的 QML、翻译、相机校正图及触觉表面等资源 |
| `translations/` | TS 源和 lrelease 生成的 QM；QM 随资源编译 |
| `UniDepthRuntimeRgb672VgaFP16RTX/` | 日常明文模型、ORT/TensorRT-RTX DLL、模型身份与完整文件哈希清单、许可材料 |
| `config/` | 当前软件自己的设备档案和偏好，需要当前运行账户可写 |
| `tools/`、`tests/` | 构建、原生依赖准备、离线和实机验证入口 |
| `tools/rv_capture/` | Native16/Native48/SPI/H.265 纯采集源码、协议、部署资源和 F6 构建入口 |
| `x64/Release/`、`x64/Debug/` | 主程序及 Qt/FFmpeg 部署目录 |
| `x64/rv_control/<配置>/runtime/` | 原生设备控制 worker、SSH/加密依赖、调参载荷及 `implementation.json` |
| `x64/validation/` | 当前忽略的测试输出、归档阅读入口及保留的安装工具；2026-11-08 以前的历史材料见 `archives/README.md`，不代表当前验收状态 |

新增源码、头文件和资源时同步维护编译、MOC、RCC 和 filters。修改翻译必须运行 lrelease。

## 配置和运行时定位

`RuntimeDiscovery::applicationRoot()` 从 EXE 目录向上查找主工程文件或 `DCTS_APP.json`。安装包在 EXE 目录提供相对运行路径，不需要工程文件或产品声明文件。

EXE 旁的 `runtime-paths.json` 使用相对 EXE 目录的路径；同名环境变量优先。当前主要覆盖项为 `RV_CONTROL_CPP`、`RV_ASSIST_RGB672_VGA_FP16_ROOT`。日常默认指向本项目内部的 worker 和明文模型包，实验启动器只为自身进程覆盖模型根目录。

`main.cpp` 将设备库定向到 `config/devices.json`，安装版的 `config/` 位于 `%LOCALAPPDATA%/DepthCameraAndTactileSensor/`，开发版位于项目根目录。INI 偏好位于 `config/preferences/DepthCameraAndTactileSensor/`。普通首次启动从资源内的 `first-run-settings.ini` 补齐可移植默认值，初始化后沿用用户配置。旧注册表及旧 INI 的兼容导入由显式 `--import-settings` 入口执行。设备密码由 Windows 凭据管理器管理；拷贝设备 JSON 不会迁移密码，已有 SSH 身份校验也不等于模型许可证绑定。

## 构建和验证

构建环境为 VS 2022 v143、Windows SDK 10.0.22621.0、Qt 6.11.1 MSVC x64 和 QtMsBuild。根目录构建入口：

```powershell
./tools/build_desktop.ps1 -Configuration Release -QtRoot E:/Qt/6.11.1/msvc2022_64
./tools/build_desktop.ps1 -Configuration Debug -QtRoot E:/Qt/6.11.1/msvc2022_64
```

默认同时重建 RV worker，校验其依赖闭包，并部署 Qt/QML/视频插件。保持 `NativeWorkersBuild=true`；主机应用构建通过不能替代 worker、模型清单和运行时依赖的验证。

离线入口为 `tools/run_offline_tests.py`，包含 UI、调参、触觉串口协议、触觉可视化、三维渲染、融合恢复及 USB 登记八组专项。`tools/verify_port.py` 核对上位机推理的模型方向、配对边界、逐字节输出和停止重启。直接使用 EXE 专项时，标志后传绝对报告路径；主机二进制 IPC 由脚本驱动。命令与统计口径见 [测试与源码统计](TESTING.md)。

测试报告放入新的 `x64/validation/` 子目录，防止覆盖旧证据。GPU 合成推理属于离线验证，不连接相机或串口；真实设备验证由 `tools/run_live_validation.py` 单独执行，需要针对当前任务的明确授权。README 中的历史实机结果不代表设备当前状态或新改动的实机验收。

模型保护实验曾遇到沙箱内 QtMsBuild 注册表访问被拒和 VS PDB 服务错误，使用正常工具链权限后构建通过。此类错误应先按工具链访问问题核对，不据此修改模型、标定或传感器参数。

## 运行和客户环境

日常运行是原生 C++/Qt、ORT/TensorRT-RTX 与原生 RV 控制，不需要客户安装 Python、Conda、PyTorch、Visual Studio 或 Qt SDK。开发验证脚本使用 Python，部分需要 NumPy。

运行包需要完整 Qt DLL/QML/插件、FFmpeg DLL、VLC 3 x64 H.265/RTSP 运行库和插件、RV worker 闭包和模型运行时，以及匹配 v143 构建的 x64 VC++ 运行库。主机 FP16 模型需要兼容的 NVIDIA RTX GPU 和驱动。日常模型包携带 TensorRT-RTX 与所需的 CUDA 用户态运行库。板端需要配套原生采集服务和受信任档案。串口设备需要对应硬件驱动，配置目录需要可写。

安装包要求经认证的加密模型。构建入口、性能证据、原始权重保护和交付目录规则见 [模型保护与安装包约定](MODEL_PROTECTION.md)。


2026-11-09 移除 LiteSR 板端推理的范围及本轮验证见 [仅保留上位机辅助推理](HOST_ASSIST_ONLY.md)。
