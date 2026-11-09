# UniDepth 推理设备选择

2026-11-09 接入；只使用当前 RGB672／640×480 UniDepth。入口为「设置 → 推理设备」。启动时异步枚举硬件，并在独立进程中加载实际模型、预热和试推理；仅把本次成功的选项加入列表。检测不连接传感器，界面保持响应；首次检测包含模型编译，可能需要几十秒，较慢设备更久。

首次使用优先选择通过测试的 TensorRT-RTX；没有可用 TensorRT 时，在通过测试的原精度路径中按本机短时推理速度选择。用户选择按设备身份保存，下次启动仍先测试可用性，再恢复选择。混合 INT8 需要用户主动选择。检测结果不能由显卡名称或历史配置替代。

| 设备 | 候选路径 | 失败处理 |
|---|---|---|
| 兼容 NVIDIA RTX | TensorRT-RTX、DirectML | 可用 CPU 路径 |
| 其他支持 DirectX 12 的 NVIDIA GPU | DirectML | 可用 CPU 路径 |
| AMD 独显／集显 | DirectML | 可用 CPU 路径 |
| Intel 集显／独显 | OpenVINO GPU、DirectML | 可用 CPU 路径 |
| 无可用 GPU | OpenVINO CPU、OpenVINO CPU 混合 INT8、ONNX Runtime CPU | 保留试运行成功的项 |

OpenVINO GPU 只枚举其实际识别的 Intel GPU；DirectML 按 DXGI 适配器测试，跳过 DXGI 明确标记的软件适配器。DirectML 禁止静默 CPU 算子回退，不能完整运行该模型的选项不会出现在设置中。CPU 缺少 OpenVINO 所需的 AVX2／操作系统寄存器支持时跳过 OpenVINO，仍测试通用 ORT CPU。上述是接入范围，不代表每个品牌、型号和驱动都已完成实机验收。

## 速度、精度与数据契约

列表中的 FPS 是预热后短测的模型估计值，不包括视频解码、传输、H 矫正、显示等全部开销，也不是持续帧率保证。量化前后精确比较应使用交错多轮测量。此前本机配对实验中 OpenVINO CPU 原模型为 2.25 FPS，decoder 混合 INT8 为 2.70 FPS，提升约 20%；相对原模型输出的平均差异为 1.34%，像素 P95 为 5.40%，最差帧平均约 6.66%。这些是模型间输出差异，不是对真实距离的测量误差。实验输入、校准和报告保留在 `x64/validation/unidepth_int8_20261109_01/`。

原 ONNX 保留不动。额外的 INT8 模型只量化 decoder，独立记录源图、转换版本、精度及 SHA256。诊断保存／回放接受经身份验证的原图和该量化图，不把量化结果标记成原 FP16 图。模型输入方向、输出逆恢复、640×480 米制网格、H 矫正和显示旋转保持原契约。

较慢路径使用试推理时间设定有上限的处理和显示超时，不再用 GPU 的固定 1 秒结果期限丢弃 CPU 输出。原生 dToF／RGB 的捕获时间配对与 100 ms 复用限制保持；处理耗时不能让过期测量成为新的校正点。仅保留最新待处理帧，不累计视频延迟。

运行时设备失效会移除该选项并尝试已验证的 CPU 路径；用户保存的偏好不因临时失败被覆盖。模型缺失、身份不符、密文认证失败等完整性问题拒绝加载，不借助回退绕过保护。点击「重新检测」会暂停主机推理，完成后用新选择和新帧恢复。

## 原生运行环境与交付

兼容包为 `UniDepthRuntimeCompatible/`，与原 `UniDepthRuntimeRgb672VgaFP16RTX/` 隔离。不同 ORT 版本只在独立推理进程加载，避免同名 DLL 冲突。

- TensorRT-RTX：原 ORT 1.31.1／TensorRT-RTX 运行环境。
- DirectML 与通用 CPU：ORT 1.24.4，DirectML 1.15.4。
- OpenVINO CPU／Intel GPU：OpenVINO 2026.4.1 原生 C API、CPU/GPU 插件、模型前端及 TBB。
- 安装版同时携带 Qt、VC++、视频、ADB 和采集控制所需的现有运行库，不需要 Python、Conda、PyTorch、CUDA Toolkit 或 OpenVINO SDK。

目标系统为 Windows 10 2004（19041）及以上／Windows 11 x64。显卡内核驱动由 Windows／硬件厂商提供，安装包不代装各型号驱动；没有可用驱动时仍尝试 CPU。安装包不承诺任意旧 CPU、Windows ARM 或内存不足设备可用。

两个模型均通过 AES-256-GCM 和原生加载 DLL 保护。原图用于 TensorRT、DirectML、ORT CPU 和 OpenVINO；INT8 XML／BIN 先封装再加密。OpenVINO 加载 DLL 认证并核对模型身份后在内存中编译，返回编译模型句柄并清零临时明文。交付目录不携带明文 ONNX、IR 或 Python 环境。详细约定见 [模型保护](MODEL_PROTECTION.md) 和 [依赖许可](OPEN_SOURCE_LICENSES.md)。

## 构建与验证入口

`tools/prepare_compatible_runtime.py` 从指定、已固定版本的 wheel 目录收集原生 DLL、许可证和 C 头文件，并生成文件摘要清单；转换输入只读，不覆盖完成的输出。正式安装由 `tools/installer/build.ps1` 对两个模型的副本分别生成新密钥、密文和 DLL。

```powershell
./tools/test_inference_backends.ps1 -Executable x64/Release/触想工作台.exe -Output x64/validation/my_check/native -Ui
./tools/test_inference_backends.ps1 -Executable <安装版目录>/触想工作台.exe -Output x64/validation/my_check/protected -Ui -Protected
```

这套专项使用实际模型检测、默认和手动选择、GPU 失效的 CPU 回退、各可用后端的完整父进程／worker 深度输出与设置截图。报告写入新的目录，PATH 仅保留被测程序和 Windows，强制 `RV_TEST_OFFLINE_ONLY=1`。通用八组离线回归仍由 `tools/run_offline_tests.py` 运行。

同输入对照继续用 `tools/verify_port.py`，指定 `--backend`、`--reference-backend`、`--compatible-runtime` 和 `--reference-compatible-runtime`；只在相同后端／精度之间要求逐字节一致。不同后端的浮点输出不可按完全一致作兼容判据。

本轮证据目录为 `x64/validation/multibackend_20261109/`。本机 i7-12700KF／RTX 3080 Ti 可验证 TensorRT、DirectML、OpenVINO CPU、混合 INT8 和通用 CPU；无 Intel GPU、AMD GPU，相关设备由后续设备验收补齐。Windows Sandbox 使用关闭网络、关闭 vGPU 的配置验证清洁环境下实际 CPU 推理、安装、启动和卸载。

## Windows 实机结果（2026-11-09）

在上述电脑连接已登记 RV1103，使用当前 H 的真实 RGB＋16×16 ToF。每个后端运行两轮各 30 秒，并正常停止、重启。下表为完整深度链路的实际出帧率，包含视频、传输和处理影响；短时测试不是长期性能或物理精度认证。

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
./tools/test_live_inference_backends.ps1 -Executable x64/Release/触想工作台.exe -ConfigSource config -Output x64/validation/my_live_check -AllowLiveDevice
```

其默认测试本机五条路径；其他机器须以设置检测成功的设备 ID 传入 `-Backends`。Intel GPU 和 AMD GPU 仍待相应实机验收。

随后对脱敏后的最终安装载荷完成相同五路径实机复核：OpenVINO CPU 两轮均约 2.30 FPS，混合 INT8 为 2.80／2.77 FPS，均速提升约 21%；其余路径与前轮接近。最终交付文件、帧率和验证范围见 [v1.1.7 安装记录](INSTALLER_1_1_7.md)。
