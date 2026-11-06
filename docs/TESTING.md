# 测试与源码统计

从项目根目录执行。工具输出使用新的 `x64/validation/<本轮目录>/`；不覆盖已有报告。历史实机报告不能代替当前改动的验收。

## 构建与离线专项

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
| `port-ui-smoke-test` | 三模块、成像入口、默认配置、方向、米制快照与显示分离 |
| `rv-tuner-test` | 调参协议、互斥、异步停止、板端模式、Native16 分片与同帧校验 |
| `tactile-serial-test` | 串口协议、型号、零点、失败与恢复 |
| `tactile-visualization-test` | 触觉显示、面板可见性、选择、曲线和断开 |
| `tactile-3d-test` | QML 三维数据与生命周期 |
| `tactile-render-test` | 三维渲染、交互、显示刷新和关闭 |
| `rv-assist-recovery-test` | 当前 640×480 H 模型/F1/F2 的米制几何、100 ms 边界、原生点有效性、尺度连续性与 schema 1/2 诊断 |
| `rv-usb-enrollment-test` | USB 身份、地址规划及登记准入 |

脚本清除继承的 RV/DCTS/Python/Qt 覆盖，强制 `RV_TEST_OFFLINE_ONLY=1`，TEMP/TMP 指向本轮输出的 `tmp/`。调参测试只使用本机 TCP 回环；需要允许本机回环连接，不会打开真实设备。连接失败/超时不能冒充协议拒绝通过负例。QtMsBuild 需要读取本机 Qt 注册表；访问被沙箱拒绝时应修正运行环境，不改模型或设备参数。

判定同时检查退出码、报告存在、有独立 PASS 断言且无 FAIL。统计不把 `BYPASS` 字符串或 `RESULT PASS` 汇总行当作新断言。Python 测试覆盖缺报告、空报告、仅汇总、FAIL、非零退出、环境隔离和非离线命令拒绝。

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

## 有效代码行数与入口检查

```powershell
python tools/audit_source.py --root . --output x64/validation/my_check/source-inventory.json
```

范围为根目录、`sensors/`、`tests/`、`qml/`、`tools/` 的第一方 C++/头文件/inc/QML/Python/PowerShell/shell/awk/cmd。按物理非空、非纯注释行计数，大括号、预处理和字符串算代码；Python docstring 算字符串。排除第三方、生成文件、模型、资源数据、运行包和 `x64/`。报告分别列出主机与原生支持、板端 runtime、测试、脚本及 SHA256。这是源码规模口径，不表示每行都在普通产品入口可达。

同时检查项目本地编译/MOC/RCC/资源/None 路径、filters 对应关系、RCC 内容及根目录/docs/tools/归档索引的 Markdown 本地链接，并核对触觉 CAD 来源路径及 SHA256。当前入口缺失会返回非零退出码；忽略目录中的历史验证报告缺失单独列为提示。删除文件前检查调用和资源引用，不以行数下降代替功能验收。

## 板端公共源码

统一构建命令、工具链覆盖和私有输入见 [板端源码与构建](../tools/litesr_board/README.md)。改动公共头文件时构建 native、protected384 的 F1/F2 四个目标；Tiny 独立构建。仅提取重复源码的改动可对照相同工具链、参数与私有输入的 SHA256；若产物变化，还需检查模型身份、预处理、协议和对应设备功能。旧八目标一致性记录属于历史；本轮关闭相机及纯采集改动改变 ARM 程序，按当前资源重新导入并实测六个切换方向，见 [记录](THREE_MODEL_DEPTH_ONLY.md)。
