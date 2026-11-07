# 第一轮源码精简记录（2026-11-05）

本文为第一轮历史记录。用户已确认第 1–7 类后续清理并执行，当前结果及行数见 [第二轮报告](SOURCE_CLEANUP_PHASE2.md)；第 8 类 IDE 文件不处理。

进度入口：[根目录清单](../REFACTOR_CHECKLIST.md)。本轮检查了主工程、原生控制/推理、触觉、板端源与载荷、测试、资源和维护脚本。改动集中在确定不可达的移植遗留逻辑；所有当前成像入口和数据合同保留。最终验证状态见下表。

## 备份与交付范围

- 根目录没有 `.git`。原样本地快照在 `x64/validation/source_cleanup_20261105/before/`，322 份源码/资源，逐文件 SHA256 在 `before-sha256.json`；同时保留改前 Release/Debug EXE。
- 经用户明确批准，脱敏的 252 份公开源码通过 `http://127.0.0.1:7897` 推送至 [GitHub 备份分支](https://github.com/QinChen1998/depth-camera-and-tactile-sensor/tree/backup/pre-cleanup-20261105)，远端核对 commit 为 `3e4ee52f1d3aabb9fb734fe38070dcc285bd3213`。
- 公开备份排除专有模型、密文、密钥二进制、设备配置、CAD 源、AGENTS 和新增板端 runtime；设备公钥/序列号已脱敏，known_hosts 为空。范围见 `public-backup-audit.json`。公开副本不是可直接交付的安装包，也不能替代本地原样快照。
- 本轮改后代码保存在当前工作区，没有再次公开推送或制作安装包。原始移植项目只读，未更改；没有连接真实硬件。

下文报告名均相对于 `x64/validation/source_cleanup_20261105/`。完整逐文件变更和哈希见 [changed-files.json](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/changed-files.json)，可审阅的源码差异见 [source-changes.patch](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/source-changes.patch)。

## 已完成的精简

| 范围 | 结果 |
|---|---|
| 深度界面 | 删除无调用的模拟/离线热图页面、33 ms 轮询、旧 source/resolution 控件及第四个主机 LiteSR 页面。保留共享采集、三个有效显示视图、切换遮罩、色标、旋转、米制快照与保存 |
| 会话管理 | 去除恒不可用的主机 LiteSR 对象/信号/取消链和旧进程内 G 优化入口，收窄板端方案分派；保留摄像头安装方向、旧设备方向偏好迁移、同帧身份、100 ms 复用和停止等待 |
| 主机推理 | 删除 DEPTHOR、Color LiteSR 推理和 CUDA CSPN 专用资源、无注册入口的旧性能/实机测试、进程内测试 transport；保留当前 FP16 H、RTX/CUDA 后端分支、AES-GCM 加载和诊断合同 |
| 运行路径发现 | `PythonResult` 改为 `NativeWorkerResult`，`handPoseNativeEnvironment` 改为 `nativeWorkerEnvironment`；删除恒真组件检查与恒空 LiteSR 路径包装，四处调用同步 |
| 测试 | 补充已退役控件不存在的 UI 检查；Native16 TCP 负例必须收到实际协议拒绝，网络失败不能冒充通过；离线 runner 增加白名单、报告/断言/FAIL 检查与工作区 TEMP/TMP |
| 原生验证工具 | 合并旧脚本为 `tools/verify_port.py` + `tools/unidepth/native16_contract.py`，支持两种当前 H 模型、分别指定参考/候选 runtime，并按清单核验明文或受保护加载 |
| 工程与目录 | 同步 vcxproj/filters，清除空白格式噪声；在导入 Qt props 前设置 IntDir，新的 qmake/MOC/RCC 均在 x64。原根目录 `Depth Ca.F64F6C2A/` 的 22 份生成文件归档到 `obsolete-qmake-cache/`，未移动产品源码/模型 |
| 文档与资源 | 更新 README、AGENTS、架构、保护实现和板端部署版本入口；新增 TESTING.md 和 audit_source.py；删除旧类 21 条翻译，lrelease 生成 829 条已完成翻译 |

## 已删除的文件

以下九份文件已从工作区移除，原样副本仍在本地快照；工程和调用引用已同步清理。

| 文件 | 删除依据／替代入口 |
|---|---|
| `sensors/RvLiteSrDepth.cpp` | 模型准入恒 false，产品没有主机 LiteSR 运行包；板端 LiteSR 保留 |
| `sensors/RvLiteSrDepth.h` | 同上，移除 MOC 项及全部对象/信号引用 |
| `sensors/Tmf8829Frame.cpp` | 仅由已移除的独立热图使用；真实设备仍使用 RvTofFrame |
| `sensors/Tmf8829Frame.h` | 同上 |
| `sensors/SensorTypes.h` | 仅为旧深度模拟/source UI 提供类型 |
| `sensors/DepthorV3Geometry.h` | 仅由被产品准入排除的 DEPTHOR 推理引用 |
| `sensors/RvColorSurface.h` | 仅由被产品准入排除的 Color LiteSR 推理引用 |
| `tools/unidepth/check_native_worker.py` | 默认要求已不存在的模型/worker 参数，当前合同已由统一验证入口覆盖 |
| `tools/unidepth/check_native16_worker.py` | 保留所需协议与样本到 native16_contract.py；删除把 A/B/G 和旧模型当作产品成功场景的重复入口 |

## 第一轮提出的后续清单（历史记录）

以下为第一轮结束时的判断依据。第 1–7 类现已按用户确认执行，实际删除、归档与保留边界见第二轮报告；第 8 类继续保留。

| 文件／目录 | 可以考虑的处理 | 保留原因与前提 |
|---|---|---|
| `sensors/RvAssistFusion.*`、`tests/RvAssistRecoveryTests.cpp` | 后续继续缩减 A–G 专项或拆分纯几何与 H 实现 | H 与旧专项共享校准、恢复和诊断函数；需要独立数值回归，不能整文件删掉 |
| `sensors/RvCameraView.h`、`sensors/RvDepthView.cpp`、`sensors/RvDtofBoardStream.h` | 继续移除旧模型几何/显示兼容分支 | 标准/广角 profile 仍是共享几何测试的输入，8×8、512、原始径向预览等须先划定工程支持范围 |
| `tools/dtof_m0/`、`tools/dtof_followup/`、`tools/litesr_board/Contour512.h` | 在取消相应历史算法验证后精简 | 仍被源码 include 或调用，不是单凭无菜单就能删除的文件 |
| `tools/litesr_board/*/runtime/` 的重复实现 | 以后抽取共有代码并重建对应板端程序 | 当前部署载荷与源码绑定；本轮未实机验收，不能仅在主机编译通过后就替换板端包 |
| `translations/hand_zh_CN.ts` 的重复 context 和历史消息 | 继续合并 context、清除确认不再使用的条目 | 动态 tr/translate 仍需保留；本轮仅移除已删除类的完整 context |
| `models/tactile/*/source/*.STEP` | 可移到用户选定的 CAD 归档位置 | 是表面模型来源，不是缓存；归档后须维护追溯入口 |
| `x64/validation/` 的历史构建、实验、安装包 | 用户选择保留日期/交付版本后归档或删除 | 文档、模型证据、安装包及本轮原样备份有引用，不能批量清空 |
| `.vs/`、`*.vcxproj.user` | 不需要旧 IDE 状态时可删除/重建 | 属于用户本机状态，已有忽略规则，清理会丢失相应配置 |

明确继续保留：原始 256 模型与权重、当前三种 UniDepth runtime、三种加密 LiteSR 包及原生采集载荷、用户 `config/`、工程 dToF 调参控制器/测试。`config/first-run-settings.ini` 是 RCC 输入，忽略规则已专门放行，不能与设备档案一起排除。

## 本次验证

| 项目 | 状态 | 报告 |
|---|---|---|
| Release／Debug 主工程及原生控制 worker 构建 | 通过 | `build-final-Release.log`、`build-final-Debug.log` |
| 控制 worker 自检 | 两配置各 33 项通过；board_connected=false | `worker-self-test-Release.json`、`worker-self-test-Debug.json` |
| Python 工具回归 | 4 项测试通过，含多个失败输入子场景 | `python-tests.txt` |
| Release 八组离线专项 | 通过，1368 项断言 | `final-release/summary.json` |
| Debug 八组离线专项 | 通过，1368 项断言 | `final-debug/summary.json` |
| release wide H 模型 | 通过，28 场景，19 份有效输出逐字节一致 | `final-parity-release-wide/summary.json` |
| release vga H 模型 | 通过，28 场景，19 份有效输出逐字节一致 | `final-parity-release-vga/summary.json` |
| debug wide H 模型 | 通过，28 场景，19 份有效输出逐字节一致 | `final-parity-debug-wide/summary.json` |
| debug vga H 模型 | 通过，28 场景，19 份有效输出逐字节一致 | `final-parity-debug-vga/summary.json` |
| Release 主程序异步推理通道 | 通过 | `worker-path-Release.json` |
| Debug 主程序异步推理通道 | 通过 | `worker-path-Debug.json` |
| 工程、filters、RCC、文档本地入口 | 通过，无缺失路径 | `source-inventory.json` |
| 模型及板端载荷 | 14 份的 SHA256、长度、修改时间与改前一致 | `model-fingerprints-after.json` |

修改前现有二进制八组基线按新口径重计后，两配置各 1,358 项断言。旧脚本报告的 1,366 将测试名中的 BYPASS 和 RESULT PASS 汇总行也计入，旧证据原样保留；统一口径见 `baseline-normalized.json`。

首次受限沙箱中的 Qt 注册表/TCP/临时目录问题已用正常工具链、本机回环权限及工作区 TEMP/TMP 解决。直接用中文 EXE 名驱动 TensorRT IPC 触发的失败也保留；正式启动与模型对照采用 ASCII 的 DctsInferenceWorker.exe。当前构建仍有既存的 Qt 弃用接口和数值转换警告，本轮没有通过关闭警告掩盖它们。

本轮均为离线测试和本机 GPU 合成推理，不包含真实设备重新标定、光学/物理精度或安装包交付验收。没有生成新模型、修改权重、部署到设备或连接串口。

## 有效代码行数

统计工具：[tools/audit_source.py](../tools/audit_source.py)。口径和命令见 [测试文档](TESTING.md)。统计物理非空、非纯注释行，排除第三方、生成文件、模型、资源数据、运行包和 x64；不是可达性证明。Python docstring 按字符串计入。

| 范围 | 改前 | 当前 | 差值 |
|---|---:|---:|---:|
| 主机应用与原生支持源码 | 19,638 | 17,638 | -2,000 |
| 板端 runtime 源码 | 3,670 | 3,670 | +0 |
| C++／Python 测试 | 4,936 | 5,004 | +68 |
| 构建、安装、验证等脚本 | 2,791 | 2,270 | -521 |
| **合计** | **31,035** | **28,582** | **-2,453** |

当前共 196 份第一方代码文件，净减少 2,453 行（7.90%）。主机应用与原生支持减少 2,000 行；测试增加覆盖。若不计测试和工具脚本，主机与板端源码合计 **21,308 行**。
