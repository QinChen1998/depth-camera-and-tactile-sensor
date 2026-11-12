# 第二轮源码精简（2026-11-05）

用户确认按建议执行第一轮列出的第 1–7 类，第 8 类 IDE 文件不处理。本轮已完成，以开始时的工作区为基线，保留期间并行出现的触觉状态行布局修复及测试，没有回退其他工作。进度见 [清单](../REFACTOR_CHECKLIST.md)，证据见 [验证汇总](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/verification-summary.json)。

## 执行范围与行为

| 类别 | 已执行 | 保留与影响 |
|---|---|---|
| 1 旧融合 | 删除 A–G 局部恢复、残差融合、旧全局拟合与影子对照；直接调用当前 H | 两个 H 模型、100 ms 配对、原生点、尺度连续性、米制输出和 IPC 原因位保持；退役算法的研究对照不再提供 |
| 2 几何/显示兼容 | 删除旧 Standard/Wide、DEPTHOR、Color profile 和主机 512 轮廓/显示分支 | 保留五个当前成像入口、各模型独立输入/输出几何、方向、原生工程预览；工程调参仍支持其他传感器网格 |
| 3 旧 dToF | 删除 M0、V1Depth、旧 Options/Wire 及关联编译项；候选入口只接受 mode13/14/16/17 | 保留当前板端身份、原生网格、同帧光子、故障与会话准入；拒绝退役 mode9/10/11/12/15 |
| 4 板端重复源码 | 28 份重复副本归并至公共文件；三个重复 build.sh 合并为一个入口 | 模型差异和私有输入独立；四类 F1/F2 共八个程序在改前、改后、统一入口和当前载荷之间 SHA256 完全相同 |
| 5 翻译 | 合并 6 组同名 context，删除 1 条退役 DEPTHOR 消息，重新 lrelease | 当前 19 个 context、827 条完整译文；动态消息保留，中文控件断言验证 |
| 6 CAD | 将三份 STEP 移至 `archives/cad/tactile/` | 字节与哈希保持，更新布局 JSON 相对来源路径；表面、点位与权重保持 |
| 7 历史产物 | 232 份构建日志归档；删除 195 份历史编译中间文件/调试符号和 7 个旧安装包 | 保留 1.1.4 交付与 1.1.3 回退、报告、历史源码、模型、私有构建输入、安装工具和两轮备份；释放约 5.19 GiB |
| 8 IDE | 未执行清理、移动或编辑 | `.vcxproj.user` 哈希保持；观察到 `.vs` 文件内容索引变化，未对这些索引做回滚或清理 |

主机诊断包升级为 schema 2，删除 `legacy.f32/png` 和 A/B 对照数据；当前 H 的 schema 1 可回放，其他旧模型/旧算法明确拒绝。回放使用记录的实际尺度，不虚构此前的时间序列。图像保存、原生点 CSV、同帧身份与篡改拒绝继续保留。现有诊断快照文件的几何仍为 RGB576，此次未扩大到 VGA 快照。

## 删除与路径调整

彻底删除的旧算法文件：

- `tools/dtof_m0/M0.cpp`、`M0.h`
- `tools/dtof_followup/V1Depth.cpp`、`V1Depth.h`
- `tools/dtof_board/Options.h`、`Wire.h`
- `tools/litesr_board/Contour512.h`

`tools/litesr_board/native/runtime/Contour512.h` 是冻结板端采集源码的内部编译依赖，继续保留；它不恢复主机 512 方案。

板端 28 份副本的原路径与唯一来源见 [移动对应表](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/board-source-moves.json)。新增公共目录 `tools/litesr_board/common/`，现有根目录 `Contour.h`、`ContourGrid.inc` 复用为唯一来源。历史四个构建目标对应本节记录。现行入口见 [原生采集与构建](../tools/rv_capture/README.md)。

三份 STEP 的原/新路径与哈希见 [CAD 移动记录](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/cad-moves.json)。来源入口为 [CAD 说明](../archives/cad/tactile/README.md) 和 [布局来源索引](../models/tactile/surface_layouts.json)，目录相对 JSON 所在目录解析。

历史日志 ZIP 和每份被删产物的原路径、大小、时间、SHA256 见 [归档索引](../archives/README.md)。过时安装程序包括 1.0.0 的两个构建、1.1.0、1.1.1、1.1.2 以及两份被最终 1.1.3 取代的早期构建。被删文件都是历史输出，原始模型和构建私有输入不在删除范围内。

## 验证及代码规模

本轮改前为 196 份第一方代码、28,586 有效行；改后 **167 份、25,240 行**，净减少 **3,346 行（11.71%）**。改前与第一轮结束的 28,582 行略有不同，来自本轮开始前已存在的其他工作区改动。并行的触觉布局修复新增了 14 行测试，已计入当前总数；纯精简部分减少 3,360 行。

| 当前类别 | 文件数 | 有效代码行 |
|---|---:|---:|
| 主机与原生支持 | 97 | 15,734 |
| 板端 runtime | 30 | 2,422 |
| 测试 | 11 | 4,818 |
| 构建和维护脚本 | 29 | 2,266 |
| 合计 | 167 | 25,240 |

口径为第一方源码的物理非空、非纯注释行，包含测试和脚本，排除模型、第三方依赖、资源、生成文件及 `x64/archives`。不将这个数值解释为每行代码都可从产品界面到达。统计与入口检查见 [最终清单](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/inventory-final.json)，复现命令见 [测试文档](TESTING.md)。

| 验证 | 最终结果 |
|---|---|
| Release/Debug 主工程与原生 worker | 构建通过；删除过时头文件工程条目后，两配置 worker 再次独立重建通过 |
| 八组离线专项 | 两配置各 1,338 项通过，含中文入口、UI、触觉、调参、同帧及异步停止 |
| 当前 H 数值与诊断专项 | 各配置 107 项；两个模型/F1/F2、米制平面、100 ms、复用、历史尺度、篡改和覆盖保护 |
| 两种 H 模型 × 两种配置 | 四组各 28 场景通过，每组 19 份有效深度/预测/诊断与本轮改前逐字节一致；停止重启、不重放旧 RGB 和模型清单通过 |
| 主程序异步推理通道 | Release/Debug 均通过，返回 746,496 字节深度数据 |
| 原生控制 worker | 两配置各 33 项自检通过；各 48 个运行文件/依赖/载荷指纹通过 |
| schema 1 兼容和退役拒绝 | 每配置 1 份当前 H 合成 schema 1 样本通过，9 份旧模型/算法包明确拒绝 |
| Python 工具回归 | 4 项通过 |
| 板端构建与等价性 | 四种程序 × F1/F2，改前/改后/统一入口/当前载荷四方完全一致 |
| 路径与资源 | 工程、filters、RCC、文档本地链接、CAD 来源与哈希全部通过，无缺失路径 |

八组断言从改前各 1,371 项变为 1,338 项：退役调参兼容断言净减 52，当前 H 专项净增 19；其余组保持。初次回归中失败的 DEPTHOR 提示断言已移除，工程调参的所有保存网格继续测试。附加 schema 1 验证的初版误把旧模型 ID 当成当前模型，修正样本判断后通过。收尾发现 Release 在验证后再次生成，已对当前 EXE 补跑八组及两种模型对照，并核对期间 SHA256 不变。原始报告保留，最终以 `final-release-3/`、`final-debug/`、`final-parity-Release-2-*/`、`final-parity-Debug-*/`、`extra-verification-2.json` 及汇总为准。

板端八个程序已完成重建及一致性验证，当前载荷无需替换；本轮没有重新部署或运行硬件，不新增实机精度、吞吐或安装包交付结论。16 份模型/板端文件及 3 份配置的 SHA256、大小、修改时间保持。收尾观察到用户偏好 `config/preferences/DepthCameraAndTactileSensor/DepthCameraAndTactileSensor.ini` 于 15:15 更新，已保留当前内容，没有执行编辑或回滚；本任务离线专项使用隔离设置目录。最终指纹记录明确保留这一差异，不将 20 项都报告为未变。

完整逐文件修改、删除和新增记录见 [文件清单](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/changed-files.json)，文本差异见 [源码补丁](../../dToF/output/litesr_depth_only_20261106_v1/retired_host_assets/x64/validation/source_cleanup_phase2_20261105/source-changes.patch)。共移除 38 个旧代码路径并新增 9 个公共代码/构建路径，净减 29 份；CAD 归档及文档不计入代码文件数。清单也记录工作区同期的两份触觉文件差异，标记为 `concurrent_change_preserved`，不归为本次精简修改。

第一轮脱敏 GitHub 备份及两轮本地源码快照保留。本轮没有再次提交或公开推送，也没有修改移植来源项目。
