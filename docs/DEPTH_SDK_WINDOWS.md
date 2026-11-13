# Windows x64 深度图 SDK

首版包含深度采集、原视频和 RV1103 接入。一个进程打开一个 SDK 会话。设备库可登记多台受信任设备，停止当前采集后可按设备 ID 切换。深度与原视频使用同一采集会话。

## 构建与运行包

根目录解决方案在 Release 和 Debug 下构建 SDK DLL、导入库、独立推理 worker、C 示例和上位机。上位机通过 SDK 的 Qt 适配接口获取会话。公开 ABI 由 `include/DepthSdk.h` 定义，调用方链接 `lib/DctsDepthSdk.lib`。

使用安装构建的 `-StageOnly` 生成运行载荷，再组装 SDK。此步骤复用已准备的板端文件、视频依赖和受保护的模型运行时。

```powershell
./tools/build_desktop.ps1 -Configuration Release -QtRoot E:/Qt/6.11.1/msvc2022_64
./tools/package_depth_sdk.ps1 -BinaryDirectory ./x64/Release -RuntimePayload './x64/validation/installer/payload' -OutputDirectory './x64/validation/depth-sdk'
```

将 SDK 的 `bin` 放到 Windows x64 程序可加载的目录，并把该目录的绝对路径传给 `DctsDepth_Open`。资源路径在同目录的 `runtime-paths.json` 中指定。设备档案和 `depth-sdk.ini` 使用调用方提供的可写配置位置。已有的受信任设备档案可复用，同一 Windows 用户继续通过凭据管理器使用已保存的 SSH 凭据。

在 SDK 目录运行示例。仅提供路径时枚举已登记设备。提供成像档位时，示例订阅视频和深度，执行启动、停止、重连和关闭。最后的设备 ID 参数可选，默认使用档案中的当前设备。

```powershell
$bin = (Resolve-Path ./bin).Path
& (Join-Path $bin DctsDepthSdkExample.exe) $bin 'C:\path\to\devices.json'
& (Join-Path $bin DctsDepthSdkExample.exe) $bin 'C:\path\to\devices.json' board
```

运行包内的 `examples/DepthSdkExample.c` 可用 VS 2022 x64 工具链编译。

```powershell
cl /TC /std:c17 /I include examples\DepthSdkExample.c /link lib\DctsDepthSdk.lib /OUT:bin\DctsDepthSdkExample.exe
```

## 会话与帧

1. 调用 `DctsDepth_Open`，用 `DctsDepth_ListDevices` 读取档案，按需调用 `DctsDepth_SelectDevice`。设备列表的 `modes` 是产品档位，启动时还会校验设备身份、标定和板端载荷。
2. 用 `DctsDepth_SetMode` 选择 `board`、`high` 或 `host` 对应的枚举值。使用 `DctsDepth_Subscribe` 接收状态事件和帧。主机档启动时可返回 `DCTS_DEPTH_BUSY`，收到推理候选变更事件后查询状态并重试。候选枚举、选择、测速及兼容故障回退沿用现有推理运行时。
3. 调用 `DctsDepth_Start` 后，用状态和帧确认采集已开始。停止后 SDK 撤销当前帧。重新启动、重连和切换设备前，可通过 `DctsDepth_GetState` 检查 `busy`。板端恢复仍检查同一启动会话的采集退出与释放回执。
4. 调用 `DctsDepth_Close` 释放会话。USB 操作期间关闭会返回 `DCTS_DEPTH_BUSY`。可继续派发事件并在操作结束后关闭。

会话 API 在打开它的线程上调用。普通 Win32/C 调用方用 `DctsDepth_Poll` 派发事件。已经运行 Qt 的调用方从 Qt 主线程打开并使用现有事件循环。回调中可读状态和设备信息，控制操作在回调返回后执行。帧和字符串的地址在回调期间有效，需跨线程或在之后保存时由调用方复制。

| 流或档位 | 数据 |
| --- | --- |
| 原视频 | 2304×1296 RGB8，使用帧给出的行跨度和主机单调时间戳。 |
| 板端普通／高精度 | 48×32 float32 小端米制径向距离，使用板端单调时间戳。`estimated_mask` 标记补点，空掩码表示当前帧无补点。 |
| 上位机辅助 | 480×640 float32 小端 camera-Z 米制深度、同次输出的配对 RGB，以及可用时的单目预测与 48×32 全局／局部原因数组。时间戳来自对应 RGB 的主机解码时钟。 |

米制数组的无效位置为 0。按设备 ID、会话代次和板端采集代次关联数据。深度帧的 JSON 元数据携带标定、配对与尺度状态、当前校正点等信息。补显预测供预览使用，普通保存和测距使用米制数组。上位机的深度快照在相应档位保存 `.f32`、PNG、JSON 及选中的配对 RGB。

## USB 登记与修复

停止采集后调用 `DctsDepth_CheckUsb` 进行只读检查。通过 `DctsDepth_GetUsbStatus` 查看进度。状态为 `ready`、`registration_required`、`update_required` 或 `network_required` 时，结果带有效的 `check_id`。调用方确认后将该 ID 传给 `DctsDepth_EnrollUsb`，并指定是否在登记成功后启动。确认绑定设备身份、USB transport、boot ID、设备库版本及停止修订。插拔、换板、停止或设备档案变化后应重新检查。多 USB 候选会报告歧义。

上位机保留已有的自动发现、确认提示、进度和诊断报告，调用同一 USB 登记实现。离线模式 `RV_TEST_OFFLINE_ONLY=1` 禁用 USB 操作。
