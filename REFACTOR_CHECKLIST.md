# 源码精简与回归检查清单（2026-11-05）

## 第二轮：执行用户确认的第 1–7 类（已完成）

第 8 类 `.vs/` 和 `*.vcxproj.user` 不处理。本轮保留第一轮备份，另存当前源码和二进制快照，以当前工作区为准，不覆盖期间的其他改动。

- [x] 保存第二轮基线、模型/配置/IDE 文件指纹，盘点历史产物与板端构建条件。
- [x] 1：精简旧融合分支，保留 H 共用算法并更新有效回归测试。
- [x] 2：清理退出支持范围的模型几何、尺寸及显示兼容分支。
- [x] 3：解除旧 dToF 算法依赖，移除退出使用的源码和编译项。
- [x] 4：抽取相同板端 runtime 源码，完成各目标构建及等价性验证；记录实机验收状态。
- [x] 5：合并同名翻译 context，清理已退役消息，生成 QM 并验证中文入口。
- [x] 6：将 STEP 归档至独立 CAD 资料目录，保留哈希与来源索引，更新路径。
- [x] 7：按保留策略清理历史构建产物；保留关键报告、有效交付/回退版本及两轮源码备份。
- [x] 完成 Release/Debug、当前模型同输入对照、worker、UI、数据契约和路径检查。（Release 重新生成后已补跑，报告与当前 EXE 指纹对应。）
- [x] 更新文档、最终有效代码行数、移除/归档明细与本轮结果。

本轮输出：`../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/`，说明见 [第二轮报告](docs/SOURCE_CLEANUP_PHASE2.md)。当前 167 份第一方代码、25,240 有效行，较本轮开始减少 3,346 行（11.71%）。两配置八组离线专项各 1,338 项通过；四组 H 模型对照各 28 场景、19 份有效输出逐字节一致；主程序异步通道和控制 worker 均通过。四种板端程序的 F1/F2 重建与当前载荷全部逐字节一致，未部署或启动硬件。已归档 232 份构建日志，删除 195 份历史中间文件和 7 个旧安装包，释放约 5.19 GiB。第 8 类不处理。以下为第一轮已完成记录。

## 第一轮

目标：在保留现有功能、数据契约和原始模型的前提下，检查全项目源码、更新测试、精简移植遗留逻辑与目录，并修正文档入口。

- [x] 阅读 `README.md` 与 `docs/ARCHITECTURE.md`，建立本清单。
- [x] 核对 Git 仓库、现有改动和上传范围，通过 `127.0.0.1:7897` commit/push 到 GitHub，并验证远端备份；完成前不改产品源码。
- [x] 盘点主工程、worker、资源、脚本和测试，记录有效代码行数统计口径与 Release/Debug 基线。
- [x] 按模块检查过度设计、重复或不可达逻辑、过时测试与失效路径，确定可验证的精简范围。
- [x] 实施精简；明确过时文件直接移除，不确定文件列入待判断清单。
- [x] 更新必要测试，完成 Release/Debug、离线专项、worker/模型及数据契约验证；不启动真实硬件。
- [x] 更新 `AGENTS.md`、`README.md`、架构与测试文档，核对构建、资源和文档路径。
- [x] 汇总修改、验证结果、当前有效代码行数和待用户判断的删除/修改文件清单。

## 当前进度

- 当前根目录不含 Git 元数据；已在 `../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/before/` 保存 322 份原样源码/资源，哈希见同目录上级 `before-sha256.json`。
- 用户明确批准具体脱敏文件集后，已通过 7897 代理推送至 `QinChen1998/depth-camera-and-tactile-sensor` 的 `backup/pre-cleanup-20261105` 分支，并通过 `ls-remote` 验证远端 commit 为 `3e4ee52f1d3aabb9fb734fe38070dcc285bd3213`。源码精简和本轮全部验证已完成；改后代码保存在本地，没有再次公开推送。
- 公开备份含 252 份文件，排除专有模型、密文、带密钥二进制、设备配置、CAD 源、AGENTS 和新增板端 runtime 源；实际设备公钥/序列号脱敏，`known_hosts` 为空。审查明细：`../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/public-backup-audit.json`。
- 所有本次临时报告和验证输出使用新的 `../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/` 子目录。
- 原始移植项目只读，原始权重保留；未经本任务明确授权不连接硬件。

## 完成记录

- 最终结果、九份已删除文件、八类待判断清单与验证边界见 [源码精简报告](docs/SOURCE_CLEANUP_REVIEW.md)，完整证据汇总见 [verification-summary.json](../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/verification-summary.json)。
- 改前第一方代码共 202 份、31,035 有效行；第一轮结束时 196 份、28,582 行，净减少 2,453 行。工程、filters、RCC 与文档入口静态检查通过。
- 改前 Release/Debug 各八组离线专项通过，按新的独立断言口径各 1,358 项；原生控制 worker 自检 33 项通过。
- 最初沙箱内调参/USB 失败报告保留，不覆盖；旧基线见 `../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_20261105/baseline-summary.json`。旧计数曾将测试名中的 `BYPASS` 及末尾 `RESULT PASS` 计入，已在 `baseline-normalized.json` 用新的断言口径统一重计。
- 已移除旧独立热图、主机 LiteSR 空实现、DEPTHOR/Color LiteSR 推理、无入口的进程内 G 测试及两份旧脚本；新模型对照入口为 `tools/verify_port.py`。
- 最终 Release/Debug 构建及两配置控制 worker 各 33 项自检通过；两配置八组离线专项各 1,368 项断言全部通过；Python 工具四项回归通过。
- 两种 H 模型 × 两种配置的四组 GPU 对照均通过 28 个场景，每组 19 份有效输出与改前逐字节一致，停止重启和不同 RGB 不重放检查通过；Release/Debug 主程序异步推理通道均通过，返回 746,496 字节深度数据。
- 14 份模型/板端载荷的哈希、大小、修改时间保持不变；21 条退役类翻译已清理并 lrelease。22 份旧根目录 qmake 缓存已归档，后续中间输出统一进入 x64。未连接硬件。

本轮清单已全部完成。验证为离线专项及本机 GPU 合成推理，不代表实际设备、物理精度或安装包交付验收。
