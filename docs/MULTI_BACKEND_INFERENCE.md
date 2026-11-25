# UniDepth 多后端历史记录

本文记录 v1.1.7 时期的 UniDepth 多后端实现和安装包。运行示例使用当时版本的应用和模型包。当前 Depth Pro 2-S 的验证见 [主机推理与离线回放](TESTING.md#主机推理与离线回放)，当前安装构建见 [安装包说明](MODEL_PROTECTION.md#当前安装包)。

当时的「推理设备」设置支持 RGB672 UniDepth。启动时由独立进程枚举硬件、检查运行条件并显示候选。枚举最多运行 5 秒，超时使用已检测到的候选。执行推理时由选定后端加载并校验模型。

启动时按设备身份恢复可用的已保存选项。默认优先顺序为 TensorRT-RTX、DirectML、OpenVINO GPU、OpenVINO CPU、ONNX Runtime CPU。混合 INT8 由用户手动选择。

| 设备 | 候选路径 | 失败处理 |
|---|---|---|
| 兼容 NVIDIA RTX | TensorRT-RTX、DirectML | 可用 CPU 路径 |
| 其他支持 DirectX 12 的 NVIDIA GPU | DirectML | 可用 CPU 路径 |
| AMD 独显／集显 | DirectML | 可用 CPU 路径 |
| Intel 集显／独显 | OpenVINO GPU、DirectML | 可用 CPU 路径 |
| 无可用 GPU | OpenVINO CPU、OpenVINO CPU 混合 INT8、ONNX Runtime CPU | 依序尝试原精度 CPU 候选 |

TensorRT-RTX 候选检查 CUDA 驱动和架构。DirectML 候选来自支持 D3D12 的硬件适配器，推理时禁用静默 CPU 算子回退。OpenVINO 枚举插件识别的 Intel GPU，并检查 CPU 的 AVX2 和操作系统支持。

## 速度、精度与数据契约

在「设置 → 推理设备」点击「开始测速」后，程序逐个后端加载模型，用合成 RGB 预热、验证输出并计时。列表中的 FPS 衡量模型推理。此前本机配对实验中 OpenVINO CPU 原模型为 2.25 FPS，decoder 混合 INT8 为 2.70 FPS，提升约 20%。相对原模型输出的平均差异为 1.34%，像素 P95 为 5.40%，最差帧平均约 6.66%。实验输入、校准和报告保留在 `x64/validation/unidepth_int8_20261109_01/`。

原 ONNX 保留不动。额外的 INT8 模型只量化 decoder，独立记录源图、转换版本、精度及 SHA256。诊断保存／回放区分原模型和量化模型。各后端在输出逆恢复后使用同一个 480×640 标定网格和 G/B 矫正。

兼容路径使用测速结果或默认估算设定有上限的处理和显示超时。原生 dToF／RGB 按捕获时间配对，实际时间差加时钟裕量最多 200 ms。仅保留最新待处理帧。

后端发生兼容性故障时，程序按候选顺序切换当前设备，保存的偏好仍用于下次启动。模型缺失、身份不符或密文认证失败时拒绝加载。手动测速期间暂停主机推理，结束后用选定设备恢复。

## 原生运行环境与交付

兼容包为 `UniDepthRuntimeCompatible/`，与原 `UniDepthRuntimeRgb672VgaFP16RTX/` 隔离。不同 ORT 版本只在独立推理进程加载，避免同名 DLL 冲突。

- TensorRT-RTX：原 ORT 1.31.1／TensorRT-RTX 运行环境。
- DirectML 与通用 CPU：ORT 1.24.4，DirectML 1.15.4。
- OpenVINO CPU／Intel GPU：OpenVINO 2026.4.1 原生 C API、CPU/GPU 插件、模型前端及 TBB。
- 安装版同时携带 Qt、VC++、视频、ADB 和采集控制所需的现有运行库，不需要 Python、Conda、PyTorch、CUDA Toolkit 或 OpenVINO SDK。

目标系统为 Windows 10 2004（19041）及以上／Windows 11 x64。显卡内核驱动由 Windows／硬件厂商提供，安装包不代装各型号驱动；没有可用驱动时仍尝试 CPU。安装包不承诺任意旧 CPU、Windows ARM 或内存不足设备可用。

两个模型均通过 AES-256-GCM 和原生加载 DLL 保护。原图用于 TensorRT、DirectML、ORT CPU 和 OpenVINO；INT8 XML／BIN 先封装再加密。OpenVINO 加载 DLL 认证并核对模型身份后在内存中编译，返回编译模型句柄并清零临时明文。交付目录不携带明文 ONNX、IR 或 Python 环境。详细约定见 [模型保护](MODEL_PROTECTION.md) 和 [依赖许可](OPEN_SOURCE_LICENSES.md)。

## 历史构建与验证入口

`tools/prepare_compatible_runtime.py` 从指定版本的 wheel 目录收集原生 DLL、许可证和 C 头文件，并生成文件摘要清单。v1.1.7 的安装构建对两个模型的副本分别生成新密钥、密文和 DLL。以下命令需要恢复历史版本的对应运行包或安装目录。

```powershell
$legacyExe = Read-Host 'v1.1.7 安装版触想工作台.exe 的绝对路径'
$run = Join-Path $PWD ('x64/validation/unidepth-offline-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
.\tools\test_inference_backends.ps1 -Executable $legacyExe -Output $run -Ui -Protected
```

这套专项使用实际模型检测、默认和手动选择、GPU 失效的 CPU 回退、各可用后端的完整父进程／worker 深度输出与设置截图。报告写入新的目录，PATH 仅保留被测程序和 Windows，强制 `RV_TEST_OFFLINE_ONLY=1`。通用八组离线回归仍由 `tools/run_offline_tests.py` 运行。

同输入对照继续用 `tools/verify_port.py`，指定 `--backend`、`--reference-backend`、`--compatible-runtime` 和 `--reference-compatible-runtime`；只在相同后端／精度之间要求逐字节一致。不同后端的浮点输出不可按完全一致作兼容判据。

本轮证据目录为 `x64/validation/multibackend_20261109/`。本机 i7-12700KF／RTX 3080 Ti 可验证 TensorRT、DirectML、OpenVINO CPU、混合 INT8 和通用 CPU；无 Intel GPU、AMD GPU，相关设备由后续设备验收补齐。Windows Sandbox 使用关闭网络、关闭 vGPU 的配置验证清洁环境下实际 CPU 推理、安装、启动和卸载。

## Windows 实机结果（2026-11-09）

当时在上述电脑连接已登记 RV1103，使用 H 的真实 RGB＋16×16 ToF。每个后端运行两轮各 30 秒，并正常停止、重启。下表为完整深度链路的实际出帧率，包含视频、传输和处理影响。

| 路径 | 第一轮 FPS | 第二轮 FPS |
|---|---:|---:|
| TensorRT-RTX | 30.00 | 29.97 |
| DirectML（RTX 3080 Ti） | 16.30 | 16.44 |
| OpenVINO CPU | 2.33 | 2.33 |
| OpenVINO CPU 混合 INT8 | 2.53 | 2.77 |
| ONNX Runtime CPU | 0.80 | 0.77 |

五条路径均通过实际后端／模型身份、米制输出、输入方向、100 ms 捕获配对、绘制、停采后无晚到结果及板端长期参数不变检查。混合 INT8 两轮均值比原 OpenVINO CPU 高约 14%；此前交错离线实验约高 20%，测试负载和场景不同，不能视为所有设备的固定收益。原件在 `live-windows/summary.json`；包含设备身份的完整报告仅本地保留。

有明确实机授权时，可通过下列入口复现。它使用临时上位机配置，不改写设备长期参数；`ConfigSource` 需要本机已登记且已保存登录的设备配置。实际成像会启动板端采集，不属于离线专项。

```powershell
$legacyExe = Read-Host 'v1.1.7 程序的绝对路径'
$legacyConfig = Read-Host '该历史版本匹配的已登记设备配置目录'
$run = Join-Path $PWD ('x64/validation/unidepth-live-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
.\tools\test_live_inference_backends.ps1 -Executable $legacyExe -ConfigSource $legacyConfig -Output $run -AllowLiveDevice
```

其默认测试本机五条路径；其他机器须以设置检测成功的设备 ID 传入 `-Backends`。Intel GPU 和 AMD GPU 仍待相应实机验收。

随后对脱敏后的最终安装载荷完成相同五路径实机复核：OpenVINO CPU 两轮均约 2.30 FPS，混合 INT8 为 2.80／2.77 FPS，均速提升约 21%；其余路径与前轮接近。最终交付文件、帧率和验证范围见 [v1.1.7 安装记录](INSTALLER_1_1_7.md)。
