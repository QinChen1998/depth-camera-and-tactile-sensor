<?xml version='1.0' encoding='utf-8'?>
<TS version="2.1" language="zh_CN" sourcelanguage="en_US">
<context>
    <name>DepthMapWidget</name>
            <message><source>Cannot open the folder. Please copy the path and open it manually.</source><translation>无法打开文件夹，请复制地址后手动打开。</translation></message>
<message><source>Raw network prediction | radial m | unfiltered | 256 x 256</source><translation>网络原始预测 | 径向米 | 未筛选 | 256 x 256</translation></message>
    <message><source>Display: filtered depth</source><translation>显示：筛选后深度</translation></message>
    <message><source>Display: raw network output</source><translation>显示：网络原始输出</translation></message>
    <message><source>Depth output display</source><translation>深度输出显示</translation></message>
    <message><source>Raw prediction without output masks or range rejection. Color limits saturate; nonfinite values are magenta. Input preprocessing is unchanged.</source><translation>原始预测，不应用输出掩码或量程筛选。超出色标范围使用端点颜色，非有限值显示为品红色。输入预处理保持一致。</translation></message>
    <message><source>Switching imaging scheme...</source><translation>正在切换成像方案…</translation></message>
    <message><source>Waiting for the new depth image...</source><translation>正在等待新深度画面…</translation></message>
    <message><source>Switch complete</source><translation>切换完成</translation></message>
    <message><source>Imaging scheme switch progress</source><translation>成像方案切换进度</translation></message>
    <message><source>Estimated progress. Completion waits for a valid new depth image.</source><translation>预估进度；收到有效的新深度图后完成。</translation></message>
    <message>
        <source>GeoFuse-189K runs on the RV1103 NPU: native M48 dToF, 144 x 81 depth.</source>
        <translation>GeoFuse-189K 在 RV1103 的 NPU 推理：原生 M48 dToF，144×81 深度。</translation>
    </message>
    <message>
        <source>Restoring camera and depth...</source>
        <translation>正在恢复相机和深度…</translation>
    </message>
    <message>
        <source>Raw dToF projection follows native range; depth processing unchanged</source>
        <translation>原始 dToF 投影跟随左侧范围；深度处理保持不变</translation>
    </message>
    <message>
        <source>All visible raw points | yellow: contour | magenta: matching native ID</source>
        <translation>全部可见原始点｜黄色：轮廓｜紫色：同一原生 ID</translation>
    </message>
    <message>
        <source>No fresh dToF data</source>
        <translation>等待新的 dToF 数据</translation>
    </message>
    <message>
        <source>Board depth output:</source>
        <translation>板端深度输出：</translation>
    </message>
    <message>
        <source>Host assisted depth inference</source>
        <translation>上位机辅助深度推理</translation>
    </message>
    <message><source>Host assisted depth inference (wide view, experimental)</source><translation>上位机辅助深度推理 宽视野（试验）</translation></message>



    <message>
        <source>Host assisted depth / camera-Z</source>
        <translation>上位机辅助深度 / 相机 Z</translation>
    </message>
    <message>
        <source>Board depth</source>
        <translation>板端深度</translation>
    </message>
    <message>
        <source>Host assisted depth</source>
        <translation>上位机辅助深度</translation>
    </message>
    <message>
        <source>Invalid host assisted depth result</source>
        <translation>上位机辅助深度结果无效</translation>
    </message>
    <message>
        <source>Depth output:</source>
        <translation>深度输出：</translation>
    </message>
    <message>
        <source>Raw 144×81</source>
        <translation>原始144×81</translation>
    </message>
    <message>
        <source>576×324 (Host)</source>
        <translation>576×324（上位机）</translation>
    </message>
    <message>
        <source>Process on RV1103</source>
        <translation>在RV1103上处理</translation>
    </message>
    <message>
        <source>On RV1103</source>
        <translation>在RV1103上处理</translation>
    </message>
    <message>
        <source>Bilinear</source>
        <translation>双线性</translation>
    </message>
    <message>
        <source>Guided</source>
        <translation>引导候选</translation>
    </message>
    <message>
        <source>Bilinear uses valid depth values. Guided is a candidate requiring a 576×324 camera and processing on this computer.</source>
        <translation>双线性仅使用有效深度值。引导是候选方法，需要576×324相机并在上位机处理。</translation>
    </message>
    <message>
        <source>Depth enhancement method</source>
        <translation>深度增强方法</translation>
    </message>
    <message>
        <source>Depth output size is independent of camera size. The 2304×1296 camera-only mode has no depth; F models support original depth only.</source>
        <translation>深度输出尺寸与相机尺寸独立。2304×1296仅相机模式没有深度；F模型仅支持原始深度。</translation>
    </message>
    <message>
        <source>Process enhanced depth on RV1103 when checked, or on this computer when unchecked. 576×324 depth requires processing on this computer.</source>
        <translation>勾选时在RV1103上增强深度，取消勾选时在上位机处理。576×324深度仅支持上位机处理。</translation>
    </message>
    <message>
        <source>RV session changed</source>
        <translation>RV会话已更换</translation>
    </message>
    <message>
        <source>RV session unavailable</source>
        <translation>RV会话不可用</translation>
    </message>
    <message>
        <source>Unavailable for the current model and processing settings</source>
        <translation>当前模型和处理设置下不可用</translation>
    </message>
    <message>
        <source>Unavailable for the current camera and processing location</source>
        <translation>当前相机和处理位置下不可用</translation>
    </message>
    <message>
        <source>Camera-only mode: no depth</source>
        <translation>仅相机模式：无深度</translation>
    </message>
    <message>
        <source>Requested depth: %1×%2 | %3 | %4 / %5</source>
        <translation>请求深度：%1×%2 | %3 | %4 / %5</translation>
    </message>
    <message>
        <source>Save</source>
        <translation>保存</translation>
    </message>
    <message>
        <source>Save camera image too</source>
        <translation>同时保存图片</translation>
    </message>
    <message>
        <source>Save depth map</source>
        <translation>保存深度图</translation>
    </message>
    <message>
        <source>Select capture folder</source>
        <translation>选择保存目录</translation>
    </message>
    <message>
        <source>Save failed: %1</source>
        <translation>保存失败：%1</translation>
    </message>
    <message>
        <source>Saved to %1</source>
        <translation>已保存到 %1</translation>
    </message>
    <message>
        <source>No current depth or paired camera image. Start a camera + depth mode and wait for a frame.</source>
        <translation>当前没有深度图或配对的相机图片。请启动相机＋深度模式并等待新帧。</translation>
    </message>
    <message>
        <source>Start the camera stream to receive depth</source>
        <translation>请在摄像头原视频中点击开始，以接收深度</translation>
    </message>
    <message>
        <source>Invalid depth packet</source>
        <translation>深度数据包无效</translation>
    </message>
    <message>
        <source>RV1103 inferred depth / camera-Z</source>
        <translation>RV1103推理深度／camera-Z</translation>
    </message>
    <message>
        <source>RV1103 depth: no current data</source>
        <translation>RV1103深度：当前无数据</translation>
    </message>
    <message>
        <source>Disconnected / No data</source>
        <translation>未连接 / 无数据</translation>
    </message>
    <message>
        <source>Row %1, Column %2</source>
        <translation>第 %1 行，第 %2 列</translation>
    </message>
    <message>
        <source>Invalid / no return</source>
        <translation>无效 / 无回波</translation>
    </message>
    <message>
        <source>Distance: %1 mm</source>
        <translation>距离：%1 mm</translation>
    </message>
    <message>
        <source>SNR / confidence: %1</source>
        <translation>信噪比 / 置信度：%1</translation>
    </message>
    <message>
        <source>Signal: %1</source>
        <translation>信号：%1</translation>
    </message>
    <message>
        <source>Noise: %1</source>
        <translation>噪声：%1</translation>
    </message>
    <message>
        <source>Xtalk: %1</source>
        <translation>串扰：%1</translation>
    </message>
    <message>
        <source>Depth Map — MOCK / SIMULATED</source>
        <translation>深度图 — 模拟 / 仿真</translation>
    </message>
    <message>
        <source>Depth Map — OFFLINE PLAYBACK</source>
        <translation>深度图 — 离线回放</translation>
    </message>
    <message>
        <source>Depth Map — CONNECTED</source>
        <translation>深度图 — 已连接</translation>
    </message>
    <message>
        <source>Depth Map — DISCONNECTED / NO DATA</source>
        <translation>深度图 — 未连接 / 无数据</translation>
    </message>
    <message>
        <source>none</source>
        <translation>无</translation>
    </message>
    <message>
        <source>%1–%2 mm</source>
        <translation>%1–%2 mm</translation>
    </message>
    <message>
        <source>%1 x %2 | %3 fps | Frame %4 | %5 | Valid depth %6 | Dropped %7</source>
        <translation>%1 x %2 | %3 fps | 帧 %4 | %5 | 有效深度 %6 | 丢弃 %7</translation>
    </message>
    <message>
        <source>Imaging scheme:</source>
        <translation>成像方案：</translation>
    </message>
    <message>
        <source>UniDepth input: rotate 180 deg</source>
        <translation>推理输入：旋转 180°</translation>
    </message>
    <message>
        <source>UniDepth input: original</source>
        <translation>推理输入：原方向</translation>
    </message>
    <message>
        <source>UniDepth input orientation</source>
        <translation>UniDepth 推理输入方向</translation>
    </message>
    <message>
        <source>Applied after camera mounting correction. Rotate RGB for model inference; restore the prediction before dToF fusion. Display rotation is independent.</source>
        <translation>在摄像头安装角度校正后生效。为模型推理旋转RGB，预测会先恢复到标定坐标再与dToF融合；与显示旋转独立。</translation>
    </message>
    <message>
        <source>Select B0, F_LNCA, GeoFuse-189K, host assisted inference or V1 fast depth.</source>
        <translation>选择 B0、F_LNCA、GeoFuse-189K、上位机辅助推理或 V1 快速深度。</translation>
    </message>
    <message>
        <source>Source</source>
        <translation>数据源</translation>
    </message>
    <message>
        <source>Resolution</source>
        <translation>分辨率</translation>
    </message>
    <message>
        <source>Display</source>
        <translation>显示</translation>
    </message>
    <message>
        <source>Mock</source>
        <translation>模拟</translation>
    </message>
    <message>
        <source>Offline JSON...</source>
        <translation>离线 JSON...</translation>
    </message>
    <message>
        <source>Hardware (Unavailable)</source>
        <translation>硬件（不可用）</translation>
    </message>
    <message>
        <source>Depth</source>
        <translation>深度</translation>
    </message>
    <message>
        <source>SNR / Confidence</source>
        <translation>信噪比 / 置信度</translation>
    </message>
    <message>
        <source>Signal</source>
        <translation>信号</translation>
    </message>
    <message>
        <source>Noise</source>
        <translation>噪声</translation>
    </message>
    <message>
        <source>Xtalk</source>
        <translation>串扰</translation>
    </message>
    <message>
        <source>Pause</source>
        <translation>暂停</translation>
    </message>
    <message>
        <source>Image settings...</source>
        <translation>成像设置…</translation>
    </message>
    <message>
        <source>RV1103 computation | %1 | 384×256</source>
        <translation>RV1103 板端计算 | %1 | 384×256</translation>
    </message>
    <message>
        <source>Latest dToF image | replaced when a new frame arrives</source>
        <translation>最近一帧 dToF 图像 | 新帧到达后更新</translation>
    </message>
    <message>
        <source>V1 fast depth</source>
        <translation>V1 快速深度</translation>
    </message>
    <message>
        <source>DELTAR (needs measured standard deviation)</source>
        <translation>DELTAR（需要实测标准差）</translation>
    </message>
    <message>
        <source>The current stream provides distance and SNR, not the measured standard deviation required by DELTAR.</source>
        <translation>当前数据流仅提供距离和 SNR，缺少 DELTAR 所需的实测标准差。</translation>
    </message>
    <message>
        <source>Model input: rotate 180 deg</source>
        <translation>推理输入：旋转 180°</translation>
    </message>
    <message>
        <source>Model input: original</source>
        <translation>推理输入：原方向</translation>
    </message>
    <message>
        <source>Host depth preview</source>
        <translation>上位机深度预览</translation>
    </message>
    <message>
        <source>LiteSR area full (experimental)</source>
        <translation>LiteSR 区域完整模型（实验）</translation>
    </message>
    <message>
        <source>LiteSR no guidance (experimental)</source>
        <translation>LiteSR 无强度引导（实验）</translation>
    </message>
    <message>
        <source>LiteSR WTA full (experimental)</source>
        <translation>LiteSR WTA 完整模型（实验）</translation>
    </message>
    <message>
        <source>LiteSR interpolation reference</source>
        <translation>LiteSR 插值对照</translation>
    </message>
    <message>
        <source>LiteSR area full + contour (experimental)</source>
        <translation>LiteSR 区域完整＋轮廓优化（实验）</translation>
    </message>
    <message>
        <source>Host refinement of LiteSR area full: optional isolated outlier repair and limited linear hole filling, edge-preserving smoothing and contour antialiasing. Experimental radial depth.</source>
        <translation>在上位机对 LiteSR 区域完整深度进行可选孤立异常修复、限定范围线性填洞、保边平滑和轮廓抗锯齿。实验性径向深度。</translation>
    </message>
    <message><source>Repair settings...</source><translation>修复设置…</translation></message>
    <message><source>Board depth repair settings</source><translation>板端深度修复设置</translation></message>
    <message><source>Fill invalid regions:</source><translation>无效区域填充：</translation></message>
    <message><source>Off</source><translation>关闭</translation></message>
    <message><source>Small regions (up to 4 native cells)</source><translation>小范围（最多 4 个原生格）</translation></message>
    <message><source>Larger regions (up to 64 native cells)</source><translation>较大范围（最多 64 个原生格）</translation></message>
    <message><source>Repair isolated outliers (1-2 native cells)</source><translation>修复孤立异常深度（1–2 个原生格）</translation></message>
    <message><source>Region sizes use the original 48 x 32 ToF grid. Fill from this frame's surrounding board depth with linear interpolation. Mixed foreground/background and unsupported regions stay invalid. Isolated small real objects may also be smoothed by outlier repair.</source><translation>区域大小按原生 48×32 ToF 网格计算。从本帧周围的板端深度做线性插值填充。混合前后景或缺少支持的区域仍保持无效。孤立的小型真实物体也可能被异常修复平滑。</translation></message>
    <message><source>Apply</source><translation>应用</translation></message>
    <message><source>Cancel</source><translation>取消</translation></message>
    <message>
        <source>Host FP32, same-frame native distance and first-peak photon counts. Experimental counts-to-luminance mapping; radial 256 x 256, no RGB or intensity output. Not calibrated accuracy.</source>
        <translation>上位机 FP32 推理，使用同帧原生距离和首峰光子计数。采用实验性计数到亮度代理映射；输出 256×256 径向深度，不接 RGB、不输出强度超分。未经精度标定。</translation>
    </message>
    <message>
        <source>Counts white:</source>
        <translation>计数白点：</translation>
    </message>
    <message>
        <source>Fixed experimental mapping: intensity = min(counts / white, 1). Shared across candidates; not per-frame auto range or physical calibration.</source>
        <translation>固定实验映射：强度 = min（计数 / 白点，1）。所有候选共用；不是逐帧自动范围，也不是物理标定。</translation>
    </message>
    <message>
        <source>Native D/I: original</source>
        <translation>原生 D/I：原方向</translation>
    </message>
    <message>
        <source>Native D/I: rotate 180 deg</source>
        <translation>原生 D/I：旋转 180°</translation>
    </message>
    <message>
        <source>Rotate both native D and I together for inference, then restore output. Independent of RGB and display rotation.</source>
        <translation>推理前同时旋转原生距离和强度，输出再逆向还原。独立于 RGB 和显示旋转。</translation>
    </message>
    <message><source>Select host-assisted or on-board depth inference.</source><translation>选择上位机辅助推理或板端推理。</translation></message>
    <message>
        <source>Host contour optimization</source>
        <translation>上位机轮廓优化</translation>
    </message>
    <message>
        <source>Contour settings...</source>
        <translation>轮廓设置…</translation>
    </message>
    <message>
        <source>Host contour refinement settings</source>
        <translation>上位机轮廓优化设置</translation>
    </message>
    <message>
        <source>RV1103 inference + host contour | %1 | %2×256</source>
        <translation>RV1103 推理＋上位机轮廓优化 | %1 | %2×256</translation>
    </message>
    <message>
        <source>RV1103 runs the full on-board INT8 model. The PC refines its depth with matched native input; no host neural network is started. The separate board contour option keeps all processing on RV1103.</source>
        <translation>RV1103 运行板端完整版 INT8 模型，上位机根据同帧原生数据优化轮廓，无需启动上位机神经网络。另一个板端轮廓选项仍全部在 RV1103 计算。</translation>
    </message>
    <message>
        <source>LiteSR full INT8 inference runs on RV1103. Enable Host contour optimization to run the spatial refinement on the PC; leave it off for the unrefined board output.</source>
        <translation>LiteSR 完整版 INT8 推理在 RV1103 执行。勾选“上位机轮廓优化”后在电脑细化深度；关闭则显示板端完整版原始输出。</translation>
    </message>
    <message>
        <source>Runs on the PC: 7 x 7 edge-preserving smoothing, four tangent passes and contour antialiasing. Smoothing adjusts depth by at most 12 mm after repairs. Positive valid depths below 10 cm remain supported.</source>
        <translation>在上位机执行：7×7 保边平滑、4 次切向迭代与轮廓抗锯齿。修复后的平滑调整不超过 12 mm，保留小于 10 cm 的有效正深度。</translation>
    </message>
    <message>
        <source>On-board inference</source>
        <translation>板端推理</translation>
    </message>
    <message>
        <source>Official near-depth post-training, 1-140 cm input and output. INT8 inference runs on RV1103; matched contour refinement and display run on the PC. Real near-depth accuracy is unverified. RGB is not a model input.</source>
        <translation>官方近距数据后训练，输入和输出范围 1–140 cm。RV1103 运行 INT8 推理，上位机进行同帧轮廓优化与显示。实物近距精度尚未验证，RGB 不参与推理。</translation>
    </message>
    <message>
        <source>RV1103 LiteSR + host contour + HD H.265 (experimental)</source>
        <translation>板端 LiteSR＋上位机轮廓＋高清视频 H.265（实验）</translation>
    </message>
    <message>
        <source>RV1103 runs LiteSR full INT8; the PC refines matched depth while 2304 x 1296 H.265 video updates independently. RGB is not a model input.</source>
        <translation>板端运行 LiteSR 完整 INT8 模型，上位机优化同帧深度轮廓，同时独立显示 2304×1296 H.265 视频。RGB 不参与模型推理。</translation>
    </message>
    <message>
        <source>Host LiteSR near full (1-140 cm, FP32, experimental)</source>
        <translation>上位机 LiteSR 近距完整模型（1–140 cm，FP32，实验）</translation>
    </message>
    <message>
        <source>Host LiteSR near no guidance (1-140 cm, FP32, experimental)</source>
        <translation>上位机 LiteSR 近距无强度引导（1–140 cm，FP32，实验）</translation>
    </message>
    <message>
        <source>Same-frame native ToF and photon counts, host FP32 inference, 1-140 cm. No near-range repair. Experimental signal mapping; physical accuracy is unverified.</source>
        <translation>同帧原始 ToF 和光子计数，上位机 FP32 推理，范围 1–140 cm，不做近距数值修补。光强映射为实验方案，实机精度尚未验证。</translation>
    </message>


                <message>
        <source>Diagnostic clip saved to:
%1</source>
        <translation>诊断短序列已保存到：
%1</translation>
    </message>
    <message>
        <source>Diagnostic clip could not be completed:
%1</source>
        <translation>诊断短序列未能完成：
%1</translation>
    </message>
        <message>
        <source>RV1103 LiteSR near full + host contour (INT8, experimental) High resolution (RGB paused)</source>
        <translation>板端 LiteSR 近距完整模型＋上位机轮廓（INT8，实验） 高分辨率（暂停RGB）</translation>
    </message>
    <message>
        <source>512 x 512 model output using the same near-depth weights. INT8 inference runs on RV1103; matched contour and display run on the PC. Real accuracy, especially the 1 cm boundary, is unverified. RGB video pauses in this mode and resumes when switching to another model.</source>
        <translation>复用近距权重原生输出512×512；RV1103执行INT8推理，上位机处理同帧轮廓并显示。真实精度，尤其1厘米边界，尚未验收。此模式暂停RGB视频，切回其他模型时恢复。</translation>
    </message>
    <message>
        <source>RV1103 inference + host contour | %1 | %2×%3</source>
        <translation>RV1103 推理＋上位机轮廓 | %1 | %2×%3</translation>
    </message>
    <message>
        <source>RV1103 computation | %1 | %2×%3</source>
        <translation>RV1103 计算 | %1 | %2×%3</translation>
    </message>
<message><source>LiteSR RGB 256 (host, experimental)</source><translation>LiteSR RGB 256（上位机，实验）</translation></message>
<message><source>LiteSR NV12 256 (host, experimental)</source><translation>LiteSR NV12 256（上位机，实验）</translation></message>
    <message>
        <source>DEPTHOR Small V3 FP32 (experimental)</source>
        <translation>DEPTHOR Small V3 FP32（实验）</translation>
    </message>
    <message>
        <source>DEPTHOR Small V3 FP16 (experimental)</source>
        <translation>DEPTHOR Small V3 FP16（实验）</translation>
    </message>
    <message>
        <source>DEPTHOR Small V3 INT8 (accuracy degraded, experimental)</source>
        <translation>DEPTHOR Small V3 INT8（精度退化，实验）</translation>
    </message>
<message><source>Host-assisted inference</source><translation>上位机辅助推理</translation></message><message><source>Requires saved 16 x 16 ordinary or high accuracy ToF settings with one target and Noise off. Does not change saved sensor settings.</source><translation>要求设备已保存为16×16普通或高精度ToF模式、单目标、Noise关；不会自动改写已保存的传感器参数。</translation></message>
    <message><source>Host-assisted inference model</source><translation>上位机辅助推理模型</translation></message>

    <message><source>Start</source><translation>开始</translation></message>
    <message><source>Stop</source><translation>停止</translation></message>
    <message><source>Reconnect now</source><translation>立即重连</translation></message>
    <message><source>Start, Stop and Reconnect apply to the shared camera + depth stream.</source><translation>开始、停止和立即重连同时作用于原视频与深度图。</translation></message>
    <message><source>Black outside range</source><translation>超范围黑色</translation></message>
    <message><source>Minimum (m)</source><translation>最小值（m）</translation></message>
    <message><source>Maximum (m)</source><translation>最大值（m）</translation></message>
</context>
<context>
    <name>ImagePresentation</name>
    <message>
        <source>0° (Original)</source>
        <translation>0°（原方向）</translation>
    </message>
    <message>
        <source>90° clockwise</source>
        <translation>顺时针 90°</translation>
    </message>
    <message>
        <source>180°</source>
        <translation>180°</translation>
    </message>
    <message>
        <source>270° clockwise</source>
        <translation>顺时针 270°</translation>
    </message>
    <message>
        <source>Display rotation</source>
        <translation>画面旋转</translation>
    </message>
    <message>
        <source>Rotate both depth and camera displays on this computer. Source data and saved images keep their original orientation.</source>
        <translation>在上位机同步旋转深度图与摄像头画面。源数据和保存的图片保持原方向。</translation>
    </message>
</context>
<context>
    <name>QObject</name>
    <message>
        <source>Rename failed</source>
        <translation>重命名失败</translation>
    </message>
    <message>
        <source>Copy failed</source>
        <translation>另存副本失败</translation>
    </message>
    <message>
        <source> (copy)</source>
        <translation>（副本）</translation>
    </message>
    <message>
        <source>Delete failed</source>
        <translation>删除失败</translation>
    </message>
    <message>
        <source>Delete reference</source>
        <translation>删除参考</translation>
    </message>
    <message>
        <source>Delete %1 permanently?</source>
        <translation>永久删除“%1”？</translation>
    </message>
    <message>
        <source>File changed or could not be deleted</source>
        <translation>文件已被修改或无法删除</translation>
    </message>
    <message>
        <source>Import pose reference</source>
        <translation>导入参考姿态</translation>
    </message>
    <message>
        <source>Import failed</source>
        <translation>导入失败</translation>
    </message>
    <message>
        <source>Export failed</source>
        <translation>导出失败</translation>
    </message>
    <message>
        <source>Export pose reference</source>
        <translation>导出参考姿态</translation>
    </message>
    <message>
        <source>Choose an export location outside the managed pose library</source>
        <translation>请选择姿态库管理目录以外的导出位置</translation>
    </message>
</context>
<context>
    <name>RtspCameraPage</name>
    <message>
        <source>Restoring camera and depth...</source>
        <translation>正在恢复相机和深度…</translation>
    </message>
    <message>
        <source>Start or Reconnect resets a latched sensor fault after preserving diagnostics and verifying capture ownership, then waits for fresh camera + depth data.</source>
        <translation>点击“开始”或“立即重连”将保存故障日志、核对采集所有权后复位传感器，并等待新的相机和深度数据。</translation>
    </message>
                <message>
        <source>Start, Stop and Reconnect apply to the shared camera + depth stream.</source>
        <translation>开始、停止和重连作用于相机与深度的共享数据流。</translation>
    </message>
        <message>
        <source>Start</source>
        <translation>开始</translation>
    </message>
    <message>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message>
        <source>Reconnect now</source>
        <translation>立即重连</translation>
    </message>
            <message>
        <source>Stopped</source>
        <translation>已停止</translation>
    </message>
            <message>
        <source>2304 x 1296 H.265 + LiteSR depth</source>
        <translation>2304×1296 H.265＋LiteSR 深度</translation>
    </message>
    <message>
        <source>2304 x 1296 H.265 + host depth</source>
        <translation>2304 x 1296 H.265＋上位机深度</translation>
    </message>
    <message><source>Resolution: %1 | FPS: %2</source><translation>分辨率：%1 | FPS：%2</translation></message>
    <message><source>Display rotation</source><translation>画面旋转</translation></message>
    <message><source>Rotate the camera video and depth display together.</source><translation>联动旋转摄像头原视频和深度图。</translation></message>
</context>
<context>
    <name>RuntimeDiscovery</name>
    <message>
        <source>The %1 display component is missing, damaged or incompatible. Close the application and update or reinstall the component using MaintenanceTool.</source>
        <translation>%1 显示组件缺失、损坏或不兼容。请关闭程序，使用维护工具更新或重新安装该组件。</translation>
    </message>
</context>
<context>
    <name>RvAssistDepth</name>
    <message><source>Host inference worker identity mismatch</source><translation>上位机推理进程身份校验失败。</translation></message>
    <message><source>No NVIDIA GPU was detected. This version requires a supported NVIDIA RTX GPU.</source><translation>未检测到 NVIDIA 显卡。本版本需要受支持的 NVIDIA RTX 显卡。</translation></message>
    <message><source>The required inference runtime or encrypted model files are missing or invalid.</source><translation>推理运行库或加密模型文件缺失或无效。</translation></message>
    <message><source>The NVIDIA CUDA driver is missing or could not be loaded.</source><translation>NVIDIA CUDA 驱动缺失或无法加载。</translation></message>
    <message><source>The NVIDIA driver is too old for the bundled CUDA 12.9 runtime. Update the driver.</source><translation>NVIDIA 驱动版本过旧，无法使用随软件提供的 CUDA 12.9 运行库。请更新驱动。</translation></message>
    <message><source>The NVIDIA driver could not initialize a CUDA device. Check the driver and whether the GPU is enabled.</source><translation>NVIDIA 驱动未能初始化 CUDA 设备。请检查驱动及显卡是否已启用。</translation></message>
    <message><source>The detected NVIDIA GPU architecture is unsupported. TensorRT-RTX requires a supported RTX GPU from the Ampere generation or later.</source><translation>检测到的 NVIDIA 显卡架构不受支持。TensorRT-RTX 需要受支持的 Ampere 或更新架构 RTX 显卡。</translation></message>
    <message><source>TensorRT-RTX did not accept any GPU. Check the GPU model, driver and Windows environment.</source><translation>TensorRT-RTX 未找到可用的显卡。请检查显卡型号、驱动及 Windows 运行环境。</translation></message>
    <message><source>There is insufficient GPU memory to run the model. Close other GPU-intensive applications and try again.</source><translation>显存不足，无法运行模型。请关闭占用显卡的其他程序后重试。</translation></message>
    <message><source>The encrypted model or a runtime file failed integrity verification. Reinstall the application.</source><translation>加密模型或运行库文件未通过完整性校验。请重新安装软件。</translation></message>
    <message><source>Model initialization or inference timed out.</source><translation>模型初始化或推理超时。</translation></message>
    <message><source>A required inference runtime component could not be loaded or initialized.</source><translation>推理所需的运行库组件无法加载或初始化。</translation></message>
    <message><source>The host inference worker failed to initialize or run the model.</source><translation>上位机推理进程无法初始化或运行模型。</translation></message>
    <message><source>Depth display changed; waiting for a new paired frame</source><translation>板端推理 显示已切换，等待新配对帧</translation></message>
    <message><source>Unsupported host model</source><translation>不支持的主机模型</translation></message>
    <message>
        <source>Host inference input orientation changed; waiting for a new paired frame</source>
        <translation>上位机辅助推理 输入方向已改变，等待新的配对帧</translation>
    </message>
    <message>
        <source>Host depth result belongs to another input orientation</source>
        <translation>上位机深度结果属于另一种输入方向</translation>
    </message>
    <message>
        <source>Loading UniDepthV2-Small (FP16) on the GPU...</source>
        <translation>正在 GPU 上加载 UniDepthV2-Small（FP16）…</translation>
    </message>
    <message>
        <source>Host depth result does not match the selected model</source>
        <translation>上位机深度结果与所选模型不符</translation>
    </message>
    <message>
        <source>Host model ready; waiting for a reliable paired frame</source>
        <translation>上位机模型已就绪，等待可靠的配对帧</translation>
    </message>
    <message>
        <source>Host assisted depth timed out; disable and enable it to retry</source>
        <translation>上位机辅助深度超时，请关闭再开启此选项重试</translation>
    </message>
    <message>
        <source>Host assisted depth process exited (%1): %2</source>
        <translation>上位机辅助深度进程退出（%1）：%2</translation>
    </message>
    <message>
        <source>Host assisted depth is disabled</source>
        <translation>上位机辅助深度已关闭</translation>
    </message>
    <message>
        <source>No paired 576×324 camera and native dToF frame</source>
        <translation>没有配对的 576×324 相机和原生 dToF 帧</translation>
    </message>
    <message>
        <source>UniDepth native runtime is missing; prepare the optional runtime first</source>
        <translation>缺少 UniDepth 原生运行文件，请先准备可选运行环境</translation>
    </message>
    <message>
        <source>Paired frame expired before host inference</source>
        <translation>配对帧在上位机推理前已过期</translation>
    </message>
    <message>
        <source>Cannot send paired frame to host depth process</source>
        <translation>无法将配对帧发送给上位机深度进程</translation>
    </message>
    <message>
        <source>Host depth protocol exceeded its bound</source>
        <translation>上位机深度协议超出大小限制</translation>
    </message>
    <message>
        <source>Invalid host depth packet size</source>
        <translation>上位机深度数据包大小无效</translation>
    </message>
    <message>
        <source>Host depth protocol identity mismatch</source>
        <translation>上位机深度协议身份不匹配</translation>
    </message>
    <message>
        <source>Unexpected host depth result</source>
        <translation>上位机深度结果与当前请求不符</translation>
    </message>
    <message>
        <source>Host depth result belongs to another RGB/dToF pair</source>
        <translation>上位机深度结果属于另一组 RGB/dToF 帧</translation>
    </message>
    <message>
        <source>Host assisted depth result expired</source>
        <translation>上位机辅助深度结果已过期</translation>
    </message>
    <message>
        <source>Invalid host assisted depth result</source>
        <translation>上位机辅助深度结果无效</translation>
    </message>
    <message>
        <source>Host model changed; waiting for a new paired frame</source>
        <translation>推理模型已切换，等待新的配对帧</translation>
    </message>
    <message>
        <source>Selected host model runtime is missing; prepare the optional runtime first</source>
        <translation>所选模型的推理包缺失，请先准备可选推理包</translation>
    </message>
    <message>
        <source>Selected host runtime manifest is invalid</source>
        <translation>所选模型的推理包清单无效</translation>
    </message>
    <message>
        <source>Selected GPU Python environment or model runtime is unavailable</source>
        <translation>所选 GPU Python 环境或模型推理包不可用</translation>
    </message>
    <message>
        <source>Loading %1 on the GPU...</source>
        <translation>正在 GPU 上加载 %1…</translation>
    </message>
    <message>
        <source>Host depth result belongs to another selected model</source>
        <translation>深度结果不属于当前选定模型</translation>
    </message>
    <message>
        <source>Invalid model depth or crop boundary</source>
        <translation>模型深度值或裁剪边界无效</translation>
    </message>
    <message>
        <source>Invalid managed H.265 model input</source>
        <translation>受管 H.265 模型输入无效</translation>
    </message>
    <message>
        <source>Host depth result belongs to another H.265 frame</source>
        <translation>上位机深度结果属于其他 H.265 帧</translation>
    </message>
<message><source>Host depth result belongs to another native grid</source><translation>主机深度结果属于其他原生测量网格</translation></message>
    <message><source>Loading the host-assisted inference model on the GPU...</source><translation>正在 GPU 上加载上位机辅助推理模型…</translation></message>
</context>
<context>
    <name>RvDepthView</name>
    <message><source>(%1, %2) raw prediction: %3 m (radial, unfiltered)</source><translation>(%1, %2) 原始预测：%3 m（径向，未筛选）</translation></message>
    <message>
        <source>Waiting for RV1103 depth</source>
        <translation>等待RV1103深度数据</translation>
    </message>
                <message>
        <source>dToF (%1, %2) -&gt; RGB (%3, %4), radial %5 m</source>
        <translation>dToF (%1, %2) → RGB (%3, %4)，径向 %5 m</translation>
    </message>
    <message>
        <source>Model ready</source>
        <translation>模型已就绪</translation>
    </message>
    <message>
        <source>Model ready; waiting for the first depth frame...</source>
        <translation>模型已就绪，正在生成首帧深度…</translation>
    </message>
    <message>
        <source>Preparing GPU and model. Please wait...</source>
        <translation>正在准备 GPU 和模型，请稍候…</translation>
    </message>
    <message>
        <source>Invalid depth</source>
        <translation>无效深度</translation>
    </message>
    <message>
        <source>Last displayed frame; depth reading expired</source>
        <translation>保留的上一帧画面；深度读数已过期</translation>
    </message>
    <message><source>(%1, %2) prediction: %3 m camera-Z</source><translation>（%1，%2）预测：%3 m camera-Z</translation></message>
</context>
<context>
    <name>RvDtofBoardStream</name>
    <message>
        <source>Invalid candidate stream size</source>
        <translation>候选数据流长度无效</translation>
    </message>
    <message>
        <source>Invalid candidate stream header</source>
        <translation>候选数据流头无效</translation>
    </message>
    <message>
        <source>Unexpected candidate stream payload</source>
        <translation>候选数据流载荷异常</translation>
    </message>
    <message>
        <source>Candidate identity or parameters mismatch</source>
        <translation>候选会话或参数不匹配</translation>
    </message>
    <message>
        <source>Invalid candidate measurement identity</source>
        <translation>候选测量标识无效</translation>
    </message>
    <message>
        <source>Invalid candidate depth values</source>
        <translation>候选深度数值无效</translation>
    </message>
    <message>
        <source>RV1103 candidate stream disconnected</source>
        <translation>RV1103 候选数据流已断开</translation>
    </message>
    <message>
        <source>RV1103 candidate stream timed out</source>
        <translation>RV1103 候选数据流超时</translation>
    </message>
    <message>
        <source>Invalid paired board inference native measurement</source>
        <translation>板端深度与原生测量配对无效</translation>
    </message>
    <message>
        <source>Host contour processing failed</source>
        <translation>上位机轮廓优化失败</translation>
    </message>
    <message>
        <source>HD mode cannot carry a second camera stream</source>
        <translation>高清视频模式收到重复相机数据流</translation>
    </message>
    <message>
        <source>Invalid HD encoder progress or identity</source>
        <translation>高清视频编码进度或会话身份无效</translation>
    </message>
    <message>
        <source>HD encoder status expired in transit</source>
        <translation>高清视频编码状态已过期</translation>
    </message>
    <message>
        <source>Unsupported device calibration</source>
        <translation>不支持此设备标定</translation>
    </message>
</context>
<context>
    <name>RvSessionController</name>
    <message><source>The required host inference files are missing or invalid.</source><translation>上位机推理所需的文件缺失或无效。</translation></message>
    <message><source>Depth 256 | raw prediction (unfiltered, radial m) | inference %1 ms</source><translation>深度 256 | 原始预测（未筛选，径向米）| 推理 %1 ms</translation></message>
    <message><source>Imaging scheme switch was not accepted</source><translation>未接受此成像方案切换</translation></message>
    <message>
        <source>Automatically restoring camera and depth...</source>
        <translation>正在自动恢复相机和深度…</translation>
    </message>
    <message>
        <source>Automatic recovery stopped because the board could not safely restart.</source>
        <translation>板端未能安全重启，已停止自动恢复。</translation>
    </message>
    <message>
        <source>Click Start or Reconnect to attempt a controlled sensor reset. If recovery fails, the fault and ownership remain protected.</source>
        <translation>可点击“开始”或“立即重连”尝试复位传感器。恢复失败时仍保留故障和所有权保护。</translation>
    </message>
    <message>
        <source>Host assisted depth expired; waiting for a new reliable frame</source>
        <translation>上位机辅助深度已过期，等待新的可靠帧</translation>
    </message>
    <message>
        <source>Global scale</source>
        <translation>全局尺度</translation>
    </message>
    <message>
        <source>Using last dToF scale %1 | reference %2 s ago | inference %3 ms</source>
        <translation>沿用最近 dToF 尺度 %1 | 参考距今 %2 秒 | 推理 %3 ms</translation>
    </message>
    <message>
        <source>Monocular estimate, not dToF corrected | inference %1 ms | age %2 ms</source>
        <translation>单目估计，未经 dToF 校正 | 推理 %1 ms | 帧龄 %2 ms</translation>
    </message>
    <message>
        <source>Scale + local correction</source>
        <translation>尺度和局部修正</translation>
    </message>
    <message>
        <source>ToF anchors: %1 | scale %2 | inference %3 ms | age %4 ms</source>
        <translation>ToF 锚点：%1 | 尺度 %2 | 推理 %3 ms | 帧龄 %4 ms</translation>
    </message>
    <message>
        <source>Waiting for a paired 576×324 camera and dToF frame</source>
        <translation>等待配对的 576×324 相机和 dToF 帧</translation>
    </message>
    <message>
        <source>Selected model frames did not arrive; check board status</source>
        <translation>未收到所选模型的新帧，请检查板端状态</translation>
    </message>
    <message>
        <source>Model or preview switch timed out; check board status</source>
        <translation>模型或相机配置切换超时，请检查板端状态</translation>
    </message>
    <message>
        <source>Preview switch failed; check board status</source>
        <translation>分辨率切换失败，请检查板端状态</translation>
    </message>
    <message>
        <source>Model or preview confirmation mismatch; check board status</source>
        <translation>模型或相机配置确认不一致，请检查板端状态</translation>
    </message>
    <message>
        <source>VLC 3 with H.265 support is required for the managed camera</source>
        <translation>受管相机需要支持 H.265 的 VLC 3，请检查 VLC 安装和后端设置</translation>
    </message>
    <message>
        <source>Stopped</source>
        <translation>已停止</translation>
    </message>
    <message>
        <source>Model and preview selected; click Start</source>
        <translation>模型和相机配置已选择，请点击“开始”</translation>
    </message>
    <message>
        <source>Connecting</source>
        <translation>连接中</translation>
    </message>
    <message>
        <source>Opening RTSP stream...</source>
        <translation>正在打开 RTSP 流…</translation>
    </message>
    <message>
        <source>Buffering</source>
        <translation>缓冲中</translation>
    </message>
    <message>
        <source>Waiting for H.265 video data...</source>
        <translation>正在等待 H.265 视频数据…</translation>
    </message>
    <message>
        <source>The stream is temporarily unavailable</source>
        <translation>视频流暂时不可用</translation>
    </message>
    <message>
        <source>Connected</source>
        <translation>已连接</translation>
    </message>
    <message>
        <source>Playing</source>
        <translation>播放中</translation>
    </message>
    <message>
        <source>Low-latency RTSP video</source>
        <translation>低延迟 RTSP 原视频</translation>
    </message>
    <message>
        <source>Media playback error</source>
        <translation>媒体播放错误</translation>
    </message>
    <message>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <source>Camera is not started</source>
        <translation>摄像头未启动</translation>
    </message>
    <message>
        <source>Unsupported depth configuration: board processing requires bilinear 288/384; guided processing requires host and 576 camera.</source>
        <translation>不支持的深度配置：板端仅支持双线性288/384；引导处理需要上位机及576相机。</translation>
    </message>
    <message>
        <source>Switching work mode...</source>
        <translation>工作模式切换中…</translation>
    </message>
        <message>
        <source>Enter a valid nv12:// or rtsp:// address</source>
        <translation>请输入有效的 nv12:// 或 rtsp:// 地址</translation>
    </message>
    <message>
        <source>Managed camera confirmation is missing; click Start</source>
        <translation>缺少受管相机配置回执，请点击“开始”重新确认</translation>
    </message>
    <message>
        <source>NV12: host color conversion and rectification</source>
        <translation>NV12：上位机颜色转换与去畸变</translation>
    </message>
    <message>
        <source>Reconnecting</source>
        <translation>断线重连</translation>
    </message>
    <message>
        <source>%1; reconnect %2 in %3 ms</source>
        <translation>%1；%3 ms 后进行第 %2 次重连</translation>
    </message>
        <message>
        <source>Managed camera status changed or became unavailable</source>
        <translation>受管相机状态已变化或暂时无法查询</translation>
    </message>
    <message>
        <source>Managed camera encoder progress stopped; check board status</source>
        <translation>受管相机编码进度停止，请检查板端状态</translation>
    </message>
    <message>
        <source>Disconnected / No data</source>
        <translation>未连接 / 无数据</translation>
    </message>
    <message>
        <source>Uncompressed 2304 camera frames are no longer supported; select H.265 camera mode</source>
        <translation>不再支持未压缩的 2304 相机帧，请选择 H.265 相机模式</translation>
    </message>
    <message>
        <source>Waiting for selected model frames...</source>
        <translation>正在等待所选模型的新帧…</translation>
    </message>
    <message>
        <source>Camera-only mode; depth inference paused</source>
        <translation>仅相机模式，深度推理已暂停</translation>
    </message>
    <message>
        <source>Managed H.265 source dimensions do not match 2304 x 1296</source>
        <translation>受管 H.265 视频源尺寸与 2304 x 1296 不符</translation>
    </message>
    <message>
        <source>Candidate receipt mismatch</source>
        <translation>板端候选回执不匹配</translation>
    </message>
    <message>
        <source>Candidate confirmation is missing</source>
        <translation>缺少板端候选确认</translation>
    </message>
    <message>
        <source>RV1103 | %1 | 384×256 | compute %2 ms</source>
        <translation>RV1103 | %1 | 384×256 | 计算 %2 ms</translation>
    </message>
    <message>
        <source>Waiting for RV1103 candidate output</source>
        <translation>等待 RV1103 候选结果</translation>
    </message>
    <message>
        <source>RGB and native dToF active; B0 inference stopped</source>
        <translation>RGB 和原生 dToF 采集中；B0 推理已停止</translation>
    </message>
    <message>
        <source>RGB 576x324 | %1 FPS | native dToF active | B0 stopped</source>
        <translation>RGB 576×324 | %1 FPS | 原生 dToF 采集中 | B0 已停止</translation>
    </message>
    <message>
        <source>%1 | ToF %2 x %3 | points %4 | inference %5 ms | age %6 ms</source>
        <translation>%1 | ToF %2×%3 | 测点 %4 | 推理 %5 ms | 帧龄 %6 ms</translation>
    </message>
    <message><source>Select a retained host model or RV1103 on-board inference scheme for depth inference.</source><translation>请选择上位机辅助推理或板端推理。</translation></message>
    <message><source>RGB and native dToF active</source><translation>RGB 与原生 dToF 正在运行</translation></message>
        <message>
        <source>Waiting for the selected board depth settings</source>
        <translation>等待新的 板端推理 处理设置生效</translation>
    </message>
    <message>
        <source>This board runtime does not provide the same-frame native input required for host contour processing</source>
        <translation>板端程序尚未提供上位机轮廓优化所需的同帧原生数据</translation>
    </message>
    <message>
        <source> | host contour %1 ms</source>
        <translation> | 上位机轮廓优化 %1 ms</translation>
    </message>
        <message>
        <source>Device selected; click Start when ready.</source>
        <translation>设备已选择，请按需点击“开始”。</translation>
    </message>
    <message>
        <source>Device changed</source>
        <translation>设备已更换</translation>
    </message>
    <message>
        <source>Device FOV does not match the selected calibration.</source>
        <translation>设备 FOV 与所选标定配置不匹配。</translation>
    </message>
    <message>
        <source>Open Settings &gt; Device Manager to review this device.</source>
        <translation>请打开“设置 → 设备管理”核对该设备。</translation>
    </message>
    <message>
        <source>Waiting for a managed H.265 frame for host inference</source>
        <translation>等待受管 H.265 视频帧进行上位机推理</translation>
    </message>
    <message>
        <source>Host inference from H.265; no synchronized dToF</source>
        <translation>H.265 上位机推理；无同步 dToF</translation>
    </message>
    <message>
        <source>H.265 video and host inference</source>
        <translation>H.265 视频与上位机推理</translation>
    </message>
    <message>
        <source>RV1103 | %1 | %2×%3 | compute %4 ms</source>
        <translation>RV1103 | %1 | %2×%3 | 计算 %4 ms</translation>
    </message>
    <message><source>RGB paused for high-resolution depth</source><translation>高分辨率深度模式，RGB已暂停</translation></message>
<message><source>Depth 256 | valid coverage %1% | inference %2 ms | age %3 ms</source><translation>深度 256 | 有效覆盖 %1% | 推理 %2 ms | 帧龄 %3 ms</translation></message>
    <message>
        <source>DEPTHOR V3 %1 | inference %2 ms | age %3 ms | experimental</source>
        <translation>DEPTHOR V3 %1 | 推理 %2 ms | 帧龄 %3 ms | 实验</translation>
    </message>
<message><source>This host depth model requires 48×32 dToF. Stop capture and save compatible settings before switching from DEPTHOR V3 with 8×8.</source><translation>此主机深度模型要求 48×32 dToF。请先停止采集并保存兼容参数，再从使用 8×8 的 DEPTHOR V3 切换。</translation></message><message><source>Selected model does not support the current %1×%2 native ToF grid. Stop capture and save compatible settings first.</source><translation>所选模型不支持当前%1×%2原生ToF网格。请先停止采集并保存兼容参数。</translation></message>
    <message><source>On-board inference model</source><translation>板端推理模型</translation></message>

    <message><source>Required board files are missing.</source><translation>板端缺少成像所需文件。</translation></message>
    <message><source>The board capture script is unavailable or unsupported.</source><translation>板端采集脚本不可用或不受支持。</translation></message>
    <message><source>The on-board inference model checksum does not match.</source><translation>板端推理模型的校验值不符。</translation></message>
    <message><source>The on-board inference runtime is incomplete.</source><translation>板端推理运行环境不完整。</translation></message>
</context>
<context>
    <name>TactileView</name>
    <message>
        <source>Point color and lift: Z pressure</source>
        <translation>测点颜色与抬升：Z 向下压力</translation>
    </message>
    <message>
        <source>Data unavailable</source>
        <translation>数据不可用</translation>
    </message>
    <message>
        <source>Raw counts</source>
        <translation>原始计数</translation>
    </message>
    <message>
        <source>Interpolated counts</source>
        <translation>插值 counts</translation>
    </message>
    <message>
        <source>Delta counts</source>
        <translation>相对零点计数</translation>
    </message>
</context>
<context>
    <name>TactileVisualizationWidget</name>
    <message><source>Display gain</source><translation>显示倍率</translation></message>
    <message><source>Arrow thickness gain</source><translation>箭头粗细倍率</translation></message>
    <message><source>Resultant arrow size gain</source><translation>合力箭头大小倍率</translation></message>
    <message><source>The connected serial port was removed.</source><translation>已连接的串口设备已拔出。</translation></message>
    <message><source>Scale the entire resultant arrow from 0.1x to 10x around its base, including the shaft and head. 1x uses the current display and thickness gains.</source><translation>以箭尾为基准，将合力箭头的箭杆和箭头整体缩放至 0.1～10 倍。1 倍使用当前显示倍率和箭头粗细倍率。</translation></message>
    <message><source>Display jump limit</source><translation>显示突变阈值</translation></message>
    <message><source>Noise threshold</source><translation>噪声阈值</translation></message>
    <message><source>Display interpolation</source><translation>显示插值</translation></message>
    <message><source>Display method</source><translation>显示方式</translation></message>
    <message><source>Display skips: %1</source><translation>显示跳过：%1</translation></message>
    <message><source>Zero-point delta per output · counts</source><translation>各输出点零点差值 · counts</translation></message>
    <message><source>Zero-point delta per output · counts (held)</source><translation>各输出点零点差值 · counts（保持）</translation></message>
    <message><source>Hardware display: X/Y components at or below this absolute threshold become zero in the model, chart and list. Z always passes through, whether suppression is on or off. Tare is subtracted first. Default: 150 counts.</source><translation>硬件显示中，X、Y 分量的绝对值不超过阈值时，在模型、曲线和列表中归零。无论抑制开关状态如何，Z 分量始终正常显示。先减去零点基线，再进行抑制。默认阈值为 150 counts。</translation></message>
    <message><source>Only the difference from the zero baseline is shown. X/Y follow the noise threshold; Z is never noise-suppressed. -- means no valid zero baseline or data. Added points use surface interpolation.</source><translation>仅显示相对零点基线的差值。X、Y 使用轻微读数阈值，Z 始终不受抑制。“--”表示没有有效零点基线或数据。新增点采用表面插值。</translation></message>
    <message><source>Hide chart and values list</source><translation>隐藏曲线和数据列表</translation></message>
    <message><source>Hardware only: compare every axis with the last displayed raw frame. Reject the whole display frame above this limit; CRC/range errors are always skipped. Current readings remain in the list, following the display noise threshold. Increase the limit if a real force step is intentionally large.</source><translation>仅用于硬件：逐轴与上一份已显示的原始帧比较，超出阈值时跳过整帧显示；CRC 或量程错误始终跳过。列表仍显示当前读数，并应用轻微读数抑制阈值。若确有较大的力变化，可调高此阈值。</translation></message>
    <message><source>%1 tactile output points</source><translation>%1 触觉输出点</translation></message>
    <message><source>Measured point</source><translation>原始测点</translation></message>
    <message><source>Measured points</source><translation>原始测点</translation></message>
    <message><source>Interpolated output point</source><translation>插值输出点</translation></message>
    <message><source>Measured points plus surface interpolation</source><translation>原始测点与表面插值点</translation></message>
    <message><source>Choose the output point count shared by the model, chart and list.</source><translation>选择模型、曲线和列表共同使用的输出点数。</translation></message>
    <message><source>X displayed / delta</source><translation>X 显示值 / delta</translation></message>
    <message><source>Y displayed / delta</source><translation>Y 显示值 / delta</translation></message>
    <message><source>Z displayed / delta</source><translation>Z 显示值 / delta</translation></message>
    <message><source>Display / delta per point · counts</source><translation>各输出点显示值 / delta · counts</translation></message>
    <message><source>Display / delta per point · counts (held)</source><translation>各输出点显示值 / delta · counts（保持）</translation></message>
    <message><source>Hardware display: each axis at or below this absolute threshold becomes zero in the model, chart and list. Delta uses the original tare baseline before suppression. Raw diagnostics keep all received values. Default: 150 counts.</source><translation>硬件显示中，各轴分量的绝对值不超过阈值时，模型、曲线和列表中的显示值归零。delta 先根据原始零点基线计算，再进行抑制。原始数据诊断保留全部接收值。默认阈值为 150 counts。</translation></message>
    <message><source>Select a row to plot that output point. Added points use surface interpolation. Display and delta columns follow the noise threshold; delta is calculated from the original baseline. -- means not tared. Full sensor values remain in Raw data / diagnostics.</source><translation>选择一行查看对应输出点的曲线。新增点采用表面插值；显示值与 delta 列均使用轻微读数阈值，delta 从原始零点基线计算。“--”表示尚未设置零点。完整传感器数据可在“原始数据 / 诊断”中查看。</translation></message>
    <message><source>1 point per interval</source><translation>一档 · 相邻点间插入 1 点</translation></message>
    <message><source>2 points per interval</source><translation>二档 · 相邻点间插入 2 点</translation></message>
    <message><source>Response timeout</source><translation>响应超时</translation></message>
    <message><source>Request gap</source><translation>请求后间隔</translation></message>
    <message><source>x</source><translation>倍</translation></message>
    <message><source>counts</source><translation>counts</translation></message>
    <message><source>counts/frame</source><translation>counts/帧</translation></message>
    <message><source>ms</source><translation>ms</translation></message>
    <message><source>Suppress small readings (hardware only)</source><translation>抑制轻微读数（仅硬件）</translation></message>
    <message><source>Noise threshold: </source><translation>噪声阈值：</translation></message>
    <message><source> counts</source><translation> counts</translation></message>
    <message><source>3D visualization only: after CRC, range and jump checks, each axis at or below this absolute threshold becomes zero. Tare is subtracted first. Tables, curves and raw diagnostics keep full readings and deltas. The default 150 counts covers the observed stationary noise; adjust it for your sensor.</source><translation>仅影响三维模型：通过 CRC、量程与大跳变检查后，各轴数值的绝对值不超过阈值时归零；设置零点后先计算差值。表格、曲线与原始诊断保留完整读数及差值。默认 150 counts 覆盖本次实测静置噪声，请按传感器情况调整。</translation></message>
    <message><source> · noise suppression ≤ %1 counts</source><translation> · 噪声抑制 ≤ %1 counts</translation></message>
    <message><source>P%1 · displayed counts · time (s)</source><translation>P%1 · 显示值 / counts · 时间（秒）</translation></message>
    <message><source>X display / delta</source><translation>X 显示值 / 差值</translation></message>
    <message><source>Y display / delta</source><translation>Y 显示值 / 差值</translation></message>
    <message><source>Z display / delta</source><translation>Z 显示值 / 差值</translation></message>
    <message><source>Unfiltered raw: %1. Displayed values within the noise threshold are zeroed.</source><translation>未处理的原始值：%1。阈值范围内的显示读数已归零。</translation></message>
    <message><source>Displayed readings and deltas are noise suppressed. Original unfiltered readings are available in Raw data / diagnostics. Tare is subtracted before suppressing deltas.</source><translation>显示读数和差值已进行噪声抑制。未处理的原始读数可在“原始数据 / 诊断”中查看；差值先减去原始零点，再进行噪声抑制。</translation></message>
    <message>
        <source>Disconnecting...</source>
        <translation>正在断开…</translation>
    </message>
    <message>
        <source>Waiting for the serial port to close. If this persists, unplug/replug the USB device.</source>
        <translation>正在等待串口关闭。如持续未完成，请拔下再插回 USB 设备。</translation>
    </message>
    <message>
        <source>Display mode</source>
        <translation>显示方式</translation>
    </message>
    <message>
        <source>XYZ axes</source>
        <translation>xyz轴体</translation>
    </message>
    <message>
        <source>Dot matrix</source>
        <translation>点阵</translation>
    </message>
    <message>
        <source>Point colors show Z pressure from 0 to 2500 counts; higher values keep the maximum color. Dot matrix spheres rise outward with positive Z only. X/Y do not move the dots; zero or negative Z leaves them on the surface.</source>
        <translation>测点颜色表示 0～2500 counts 的 Z 向下压力，超过上限时保持最大颜色。点阵小球仅随正向 Z 压力向表面外侧抬升；X/Y 不影响位移，Z 为零或负值时保持在表面。</translation>
    </message>
    <message>
        <source>Display gain: </source>
        <translation>显示倍率：</translation>
    </message>
    <message>
        <source>Scale arrow lengths or dot lift from 0.1x to 100x. Dot lift scales linearly with positive Z and gain; arrow lengths remain capped. Raw data, delta and curves stay unchanged.</source>
        <translation>调节箭头长度或点阵抬升距离为 0.1～100 倍。点阵位移与正向 Z 压力及倍率成正比；箭头长度仍保留上限。原始值、相对零点值和曲线不变。</translation>
    </message>
    <message>
        <source>Copy all raw data</source>
        <translation>复制全部原始数据</translation>
    </message>
    <message>
        <source>Off accepts CRC-mismatched data for diagnosis. Length, ID and function checks remain active; range warnings do not alter raw values.</source>
        <translation>关闭后允许接收 CRC 不匹配的数据用于诊断，仍检查长度、从站地址和功能码；越界提示不会更改原始值。</translation>
    </message>
    <message>
        <source>Connection settings</source>
        <translation>连接设置</translation>
    </message>
    <message>
        <source>No valid sensor response. Check the serial port, baud rate and device ID.</source>
        <translation>未收到有效传感器回包，请检查串口、波特率和设备地址。</translation>
    </message>
    <message>
        <source>Delay after each completed serial request. Changes apply during acquisition; response time is additional. The display refreshes separately.</source>
        <translation>每次串口请求完成后的等待时间，采集中修改会直接生效；单次请求的响应耗时需另计。界面刷新独立进行。</translation>
    </message>
    <message>
        <source>Minimize connection settings</source>
        <translation>最小化连接设置</translation>
    </message>
    <message>
        <source>Minimize display settings</source>
        <translation>最小化显示设置</translation>
    </message>
    <message>
        <source>Connect selected module</source>
        <translation>连接当前模块</translation>
    </message>
    <message>
        <source>Disconnect</source>
        <translation>断开连接</translation>
    </message>
    <message>
        <source>Use simulated data</source>
        <translation>恢复模拟数据</translation>
    </message>
    <message>
        <source>Raw data / diagnostics</source>
        <translation>原始数据 / 诊断</translation>
    </message>
    <message>
        <source>Refresh ports</source>
        <translation>刷新串口</translation>
    </message>
    <message>
        <source>Verify CRC</source>
        <translation>校验 CRC</translation>
    </message>
    <message>
        <source>All 64 registers and raw RTU frames</source>
        <translation>完整 64 个寄存器与原始 RTU 报文</translation>
    </message>
    <message>
        <source>Serial port</source>
        <translation>串口</translation>
    </message>
    <message>
        <source>0 is the vendor public address: single-device bus only. New device IDs must be 1-127; 0 cannot be written.</source>
        <translation>0 为厂家公共地址，仅适用于单设备总线。新设备地址必须为 1-127，不能写入 0。</translation>
    </message>
    <message>
        <source>Timeout </source>
        <translation>响应超时 </translation>
    </message>
    <message>
        <source>Gap </source>
        <translation>请求后间隔 </translation>
    </message>
    <message>
        <source> ms</source>
        <translation> ms</translation>
    </message>
    <message>
        <source>Pause acquisition</source>
        <translation>暂停采集</translation>
    </message>
    <message>
        <source>Invalid connection settings</source>
        <translation>连接参数无效</translation>
    </message>
    <message>
        <source>%1: %2</source>
        <translation>%1：%2</translation>
    </message>
    <message>
        <source>Hardware data</source>
        <translation>真实硬件数据</translation>
    </message>
    <message>
        <source>No current hardware data</source>
        <translation>当前无有效硬件数据</translation>
    </message>
    <message><source>Showing last valid hardware frame</source><translation>显示上一有效硬件帧</translation></message>
    <message><source>Reverse Z arrow direction</source><translation>翻转 Z 轴箭头方向</translation></message>
    <message><source>Reverse only the Z direction of force arrows, including the Z component of resultants. Raw readings, tables, curves, point colors and dot displacement are unchanged.</source><translation>仅翻转受力箭头的 Z 方向，包括合力的 Z 分量；原始读数、表格、曲线、测点颜色和点阵位移保持不变。</translation></message>
    <message>
        <source>%1: SIMULATED</source>
        <translation>%1：模拟数据 SIMULATED</translation>
    </message>
    <message>
        <source> | %1 ID %2 | RX accepted %3/%4 | CRC errors %5 | Timeouts %6 | Protocol errors %7</source>
        <translation> | %1 ID %2 | 接受 %3/%4 | CRC 错误 %5 | 超时 %6 | 协议错误 %7</translation>
    </message>
    <message>
        <source> | CRC OFF: data unverified</source>
        <translation> | CRC 已关闭：数据未经校验</translation>
    </message>
    <message>
        <source>Latest received frame (diagnostic snapshot, not necessarily valid or current).
</source>
        <translation>最近收到的报文（诊断快照，不代表有效或当前数据）。
</translation>
    </message>
    <message>
        <source>Point</source>
        <translation>测点</translation>
    </message>
    <message>
        <source>Model visibility</source>
        <translation>模型可见度</translation>
    </message>
    <message>
        <source>0% hidden; 100% opaque. Lower visibility reveals pressure inside the model.</source>
        <translation>0% 隐藏，100% 不透明。降低可见度可查看模型内部的压力箭头。</translation>
    </message>
    <message>
        <source>XYZ components</source>
        <translation>XYZ 三轴</translation>
    </message>
    <message>
        <source>X red / Y green / Z blue; no Z display threshold. Z points into the surface.</source>
        <translation>X 红 / Y 绿 / Z 蓝；Z 无显示阈值，正压力朝向模型内部。</translation>
    </message>
    <message>
        <source>Fingertip tactile</source>
        <translation>指尖触觉</translation>
    </message>
    <message>
        <source>Terminal tactile</source>
        <translation>末端触觉</translation>
    </message>
    <message>
        <source>Ultrathin tactile</source>
        <translation>超薄触觉</translation>
    </message>
    <message>
        <source>Collapse sensor choices</source>
        <translation>收起触觉模型切换按钮</translation>
    </message>
    <message>
        <source>Expand sensor choices</source>
        <translation>展开触觉模型切换按钮</translation>
    </message>
    <message>
        <source>Press / release</source>
        <translation>按压 / 释放</translation>
    </message>
    <message>
        <source>Tactile visualization</source>
        <translation>磁触觉可视化</translation>
    </message>
    <message>
        <source>Pause simulation</source>
        <translation>暂停模拟</translation>
    </message>
    <message>
        <source>Moving across modules</source>
        <translation>跨模块移动</translation>
    </message>
    <message>
        <source>No contact</source>
        <translation>无接触</translation>
    </message>
    <message>
        <source>Tangential change</source>
        <translation>切向变化</translation>
    </message>
    <message>
        <source>3D view unavailable: %1</source>
        <translation>三维视图不可用：%1</translation>
    </message>
    <message>
        <source>Point forces</source>
        <translation>点力箭头</translation>
    </message>
    <message>
        <source>Resultant</source>
        <translation>合力箭头</translation>
    </message>
    <message>
        <source>Reset view</source>
        <translation>重置视角</translation>
    </message>
    <message>
        <source>Front view</source>
        <translation>正面视角</translation>
    </message>
    <message>
        <source>P%1 · raw counts · time (s)</source>
        <translation>P%1 · 原始值 / counts · 时间（秒）</translation>
    </message>
    <message>
        <source>Set delta zero point</source>
        <translation>设置 delta 零点</translation>
    </message>
    <message>
        <source>Raw / delta per point · counts</source>
        <translation>各测点原始值 / delta · counts</translation>
    </message>
    <message>
        <source>X raw / delta</source>
        <translation>X 原始 / delta</translation>
    </message>
    <message>
        <source>Y raw / delta</source>
        <translation>Y 原始 / delta</translation>
    </message>
    <message>
        <source>Z raw / delta</source>
        <translation>Z 原始 / delta</translation>
    </message>
    <message>
        <source>Select a row to plot that point. Delta = raw - tare baseline; -- means not tared. X/Y raw: -32767 to 32767; Z raw: 0 to 32767.</source>
        <translation>选择表格行查看该测点曲线。delta = 原始值 − 去皮基准；-- 表示尚未去皮。X/Y 原始范围：-32767～32767；Z：0～32767。</translation>
    </message>
    <message>
        <source>Original points</source>
        <translation>原始测点</translation>
    </message>
    <message>
        <source>Interpolation 1: insert one point per interval</source>
        <translation>一档插值：相邻点之间插入 1 点</translation>
    </message>
    <message>
        <source>Interpolation 2: insert two points per interval</source>
        <translation>二档插值：相邻点之间插入 2 点</translation>
    </message>
    <message>
        <source>Densify both directions on the model surface. Display only; raw values and resultants stay unchanged.</source>
        <translation>沿模型表面横纵加密，仅用于显示；不改变原始值与合力。</translation>
    </message>
    <message>
        <source>P%1 · delta counts · time (s)</source>
        <translation>P%1 · delta / counts · 时间（秒）</translation>
    </message>
    <message>
        <source>Display jump limit: </source>
        <translation>显示突变阈值：</translation>
    </message>
    <message>
        <source> counts/frame</source>
        <translation> counts/帧</translation>
    </message>
    <message>
        <source>Hardware only: compare every axis with the last displayed raw frame. Reject the whole display frame above this limit; CRC/range errors are always skipped. Raw table values remain visible. Increase the limit if a real force step is intentionally large.</source>
        <translation>仅用于硬件：各测点各轴与上一帧可信原始值比较，超限则整帧不更新曲线和箭头；CRC 错误或越界始终跳过，表格仍保留原始值。真实受力突变较大时可提高阈值。</translation>
    </message>
    <message>
        <source>Display skips: %1 | %2</source>
        <translation>显示跳过 %1 帧 | %2</translation>
    </message>
    <message>
        <source>Dashed line: interpolation across missing samples (up to 500 ms). Arrows interpolate trusted values over 100 ms.</source>
        <translation>虚线表示跨缺失样本的插值（最多 500 毫秒）；箭头在可信数值之间以 100 毫秒平滑过渡。</translation>
    </message>
    <message>
        <source>Display settings</source>
        <translation>显示设置</translation>
    </message>
    <message>
        <source>Hide chart and raw values list</source>
        <translation>隐藏折线图和原始值列表</translation>
    </message>
    <message>
        <source>Show communication status</source>
        <translation>显示通信状态</translation>
    </message>
    <message>
        <source>Raw / delta per point · counts (held)</source>
        <translation>各测点原始值 / delta · counts（保持）</translation>
    </message>
    <message>
        <source>Use the latest CRC-valid values, or the previous values held in the table after a CRC error. Without valid data, the zero point stays unchanged.</source>
        <translation>使用最新 CRC 校验通过的数据；CRC 失败时使用列表保持的上一帧。尚无有效数据时，零点保持不变。</translation>
    </message>
    <message>
        <source>Held: %1</source>
        <translation>已保持上一可信帧：%1</translation>
    </message>
    <message>
        <source>Current frame</source>
        <translation>当前帧</translation>
    </message>
    <message>
        <source>SIMULATED: display filter inactive</source>
        <translation>模拟数据：未启用突变过滤</translation>
    </message>
    <message>
        <source>Arrow length gain: </source>
        <translation>箭头长度倍率：</translation>
    </message>
    <message>
        <source> x</source>
        <translation> 倍</translation>
    </message>
    <message>
        <source>Scale the lengths of all force arrows from 0.1x to 100x. Raw data, delta, curves and rejection limit stay unchanged. Arrow lengths remain capped; 1x restores the original length.</source>
        <translation>调节全部力箭头的长度为 0.1～100 倍。原始值、delta、曲线和过滤阈值不变。箭头保留最大长度限制；1 倍恢复原始长度。</translation>
    </message>
    <message>
        <source>Arrow thickness gain: </source>
        <translation>箭头粗细倍率：</translation>
    </message>
    <message>
        <source>Scale the shaft and head widths of all force arrows from 0.1x to 10x. Length uses its own gain; 1x restores the original thickness.</source>
        <translation>调节全部力箭头的箭杆及头部宽度为 0.1～10 倍。长度使用独立倍率；1 倍恢复原始粗细。</translation>
    </message>
    <message>
        <source>Device parameter ID %1; baud %2; responding ID %3. Parameter readback alone does not prove persistence.</source>
        <translation>设备参数地址 %1；波特率 %2；本次应答地址 %3。参数读回本身不代表已断电保存。</translation>
    </message>
    <message>
        <source>Device configuration failed: %1</source>
        <translation>设备配置失败：%1</translation>
    </message>
    <message>
        <source>Configuration interrupted; device state may have changed. Reconnect and read parameters before another write.</source>
        <translation>配置操作已中断，设备参数可能已改变。请重新连接并读取参数后再决定是否写入。</translation>
    </message>
    <message>
        <source>Power cycle required</source>
        <translation>必须重新上电</translation>
    </message>
    <message>
        <source>Device configuration uncertain</source>
        <translation>设备配置结果不确定</translation>
    </message>
    <message>
        <source>Connect only ONE sensor on %1.
Current communication ID: %2
%3
Device baud: %4 -&gt; %5
Write the selected changes and save together? Acquisition will stop. After saving, disconnect sensor power and unplug/replug ONCE, then connect using the new settings.</source>
        <translation>%1 请只连接一个传感器。
当前通信地址：%2
%3
设备波特率：%4 → %5
是否写入选中的修改并统一保存？采集将停止。保存完成后将传感器断电插拔一次，再按新参数连接。</translation>
    </message>
    <message>
        <source>Change sensor settings</source>
        <translation>修改传感器参数</translation>
    </message>
    <message>
        <source>New device IDs must be 1-127. Address 0 is only for connecting; it cannot be written back to the sensor.</source>
        <translation>新设备地址必须为 1-127。地址 0 仅用于连接，不能写回传感器。</translation>
    </message>
    <message>
        <source>Requested device ID: %1</source>
        <translation>目标设备地址：%1</translation>
    </message>
    <message>
        <source>Keep the device ID read from the sensor; only the baud rate will be changed.</source>
        <translation>保留从传感器读回的设备地址，仅修改波特率。</translation>
    </message>
    <message>
        <source>Writing selected parameters and checking readback; do not unplug yet...</source>
        <translation>正在写入所选参数并读回核对，请暂勿拔插…</translation>
    </message>
    <message>
        <source>Device state uncertain; one or both parameters may have changed. Persistence NOT verified. Do not repeat blindly. Power-cycle, then check ID %1 / baud %2; if unavailable, read using the previous settings. Details: %3</source>
        <translation>设备状态不确定，可能已修改一项或两项，尚未验证断电保存。请勿盲目重复写入。断电插拔后检查地址 %1 / 波特率 %2；若无响应，请使用原参数读取。详情：%3</translation>
    </message>
    <message>
        <source>Baud rate</source>
        <translation>波特率</translation>
    </message>
    <message>
        <source>Slave ID</source>
        <translation>从站 ID</translation>
    </message>
    <message>
        <source>Connect with the current settings first. While connected, edit these values and click Write and save to change the sensor.</source>
        <translation>请先按设备当前参数连接。连接后修改数值，点击“写入并保存”才能修改传感器。</translation>
    </message>
    <message>
        <source>Connect using the current settings; edit ID / baud while connected, then write and save. Power-cycle to apply.</source>
        <translation>先按当前参数连接；连接后修改地址 / 波特率，点击“写入并保存”，再断电插拔生效。</translation>
    </message>
    <message>
        <source>Write and save</source>
        <translation>写入并保存</translation>
    </message>
    <message>
        <source>Save changed ID and baud together, then power-cycle the sensor once.</source>
        <translation>统一保存修改后的地址和波特率，然后将传感器断电插拔一次。</translation>
    </message>
    <message>
        <source>Only one sensor may be connected when saving device settings.</source>
        <translation>保存设备参数时，总线只连接一个传感器。</translation>
    </message>
    <message>
        <source>Save and power cycle required</source>
        <translation>请保存并重新上电</translation>
    </message>
    <message>
        <source>Click 'Write and save'. Once saving succeeds, power-cycle the sensor once by unplugging and reconnecting its power, then connect using the new settings.</source>
        <translation>请点击“写入并保存”。保存成功后，将传感器断电插拔一次，再用新参数点击“连接当前模块”。</translation>
    </message>
    <message>
        <source>Selected settings read back and save acknowledged: ID %1, baud %2. Power-cycle once, then click 'Connect selected module' using these new values.</source>
        <translation>参数已读回核对，保存已收到应答：地址 %1，波特率 %2。请断电插拔一次，再按这些新参数点击“连接当前模块”。</translation>
    </message>
    <message>
        <source>Device settings have been written and the save command acknowledged.
Target ID: %1; target baud: %2.
You MUST disconnect sensor power and unplug/replug ONCE for all changes to take effect. Disconnecting only the software connection is not enough.
After reconnecting power, click 'Connect selected module' using the new values.</source>
        <translation>设备参数已写入，保存命令已收到应答。
目标地址：%1；目标波特率：%2。
必须将传感器断电并重新插拔一次，全部修改才会生效。仅在软件中断开连接不能替代断电。
重新上电后，请使用新参数点击“连接当前模块”。</translation>
    </message>
    <message>
        <source>ID %1 at baud %2 responds with CRC-valid sensor data. Communication with the new settings verified.</source>
        <translation>地址 %1 / 波特率 %2 已返回 CRC 正确的传感器数据，新参数通信验证通过。</translation>
    </message>
</context>
<context>
<name>DepthMapWidget</name>
<message><source>Model input orientation</source><translation>模型输入朝向</translation></message>
<message><source>Turn RGB and projected dToF upright for the selected model. Output returns to the calibrated sensor grid; display rotation is independent.</source><translation>将 RGB 与投影后的 dToF 转正后输入所选模型。输出还原至标定网格；显示旋转独立。</translation></message>
</context>
<context>
    <name>RvLiteSrDepth</name>
    <message>
        <source>LiteSR preview timed out. Reselect the option to retry.</source>
        <translation>LiteSR 预览超时；重新选择该方案可重试。</translation>
    </message>
    <message>
        <source>LiteSR preview expired; waiting for a matched D/I frame.</source>
        <translation>LiteSR 预览已过期，等待同帧 D/I。</translation>
    </message>
    <message>
        <source>LiteSR native process unavailable. Check its optional runtime.</source>
        <translation>LiteSR 原生进程不可用，请检查可选运行包。</translation>
    </message>
    <message>
        <source>LiteSR preview process stopped. Reselect the option to retry.</source>
        <translation>LiteSR 预览进程已停止；重新选择该方案可重试。</translation>
    </message>
    <message>
        <source>LiteSR selection changed</source>
        <translation>LiteSR 方案已切换</translation>
    </message>
    <message>
        <source>Experimental LiteSR: waiting for native 32 x 48 D/I. Counts mapping is uncalibrated.</source>
        <translation>实验性 LiteSR：等待原生 32×48 同帧 D/I。计数映射尚未标定。</translation>
    </message>
    <message>
        <source>LiteSR counts mapping changed; waiting for a new frame.</source>
        <translation>LiteSR 计数映射已更改，等待新帧。</translation>
    </message>
    <message>
        <source>LiteSR native input orientation changed.</source>
        <translation>LiteSR 原生输入方向已更改。</translation>
    </message>
    <message>
        <source>LiteSR requires a fresh complete 32 x 48 first-peak D/I frame with actual FOV.</source>
        <translation>LiteSR 需要新鲜、完整的 32×48 首峰 D/I 及实际 FOV。</translation>
    </message>
    <message>
        <source>LiteSR input session changed.</source>
        <translation>LiteSR 输入会话已更换。</translation>
    </message>
    <message>
        <source>Loading optional LiteSR host preview...</source>
        <translation>正在加载可选 LiteSR 上位机预览…</translation>
    </message>
    <message>
        <source>LiteSR unavailable: %1</source>
        <translation>LiteSR 不可用：%1</translation>
    </message>
    <message>
        <source>LiteSR request failed</source>
        <translation>LiteSR 推理请求失败</translation>
    </message>
    <message>
        <source>LiteSR ready; waiting for a fresh matched D/I frame.</source>
        <translation>LiteSR 已就绪，等待新的同帧 D/I。</translation>
    </message>
    <message>
        <source>LiteSR input rejected: %1</source>
        <translation>LiteSR 输入被拒绝：%1</translation>
    </message>
    <message>
        <source>Experimental LiteSR %1 | radial 256 x 256 | %2 ms | counts %3-%4 / white %5 | uncalibrated signal mapping</source>
        <translation>实验性 LiteSR %1 | 径向 256×256 | %2 ms | 计数 %3–%4 / 白点 %5 | 信号映射未标定</translation>
    </message>
    <message>
        <source>LiteSR result too old; waiting for a new matched D/I frame.</source>
        <translation>LiteSR 结果已过期，等待新的同帧 D/I。</translation>
    </message>
    <message>
        <source>LiteSR protocol rejected: %1</source>
        <translation>LiteSR 协议被拒绝：%1</translation>
    </message>
    <message>
        <source>LiteSR contour refinement failed: %1</source>
        <translation>LiteSR 轮廓优化失败：%1</translation>
    </message>
    <message>
        <source>LiteSR area full + contour | model %1 ms + refinement %2 ms | filled %3 / outliers %4 cells | radial 256 x 256</source>
        <translation>LiteSR 区域完整＋轮廓优化 | 模型 %1 ms＋优化 %2 ms | 填洞 %3 格／异常 %4 格 | 径向 256×256</translation>
    </message>
    <message><source>LiteSR repair settings changed; waiting for a new frame.</source><translation>LiteSR 修复设置已改变，等待新帧。</translation></message>
</context>
<context>
    <name>DepthMapWidget</name>
    <message><source>RV1103 LiteSR area full (INT8, experimental)</source><translation>板端 LiteSR 区域完整模型（INT8，实验）</translation></message>
    <message><source>RV1103 LiteSR area full + contour (INT8, experimental)</source><translation>板端 LiteSR 区域完整＋轮廓优化（INT8，实验）</translation></message>
    <message><source>On-board INT8 inference and all depth processing run on RV1103. Same-frame native D/I, 256 x 256 output displayed at the native ToF aspect ratio. Positive near returns are interpolated separately below the trained 10 cm minimum.</source><translation>板端 INT8 推理及全部深度处理均在 RV1103 上进行。同帧原生 D/I 输入，256×256 输出按原生 ToF 比例显示。低于训练下限 10 厘米的有效正距离单独插值。</translation></message>
    <message><source>RV1103 computation | %1 | %2×256</source><translation>RV1103 板端计算 | %1 | %2×256</translation></message>
</context>
<context>
    <name>RvSessionController</name>
    <message><source>RV1103 | %1 | %2×256 | compute %3 ms</source><translation>RV1103 | %1 | %2×256 | 计算 %3 ms</translation></message>
    <message><source> | dToF-supported output %1%; outside support is unknown</source><translation> | dToF 支持区域 %1%；区域外为未知深度</translation></message>
    <message><source>No connected current dToF correction support; depth is unknown</source><translation>本帧没有连续的 dToF 校正支持区域；深度为未知</translation></message>
</context>
<context>
    <name>RvDeviceManagerDialog</name>
    <message><source>Read automatically when connecting</source><translation>连接后自动读取</translation></message>
    <message><source>Connect and read</source><translation>连接并读取</translation></message>
    <message><source>Confirm device identity</source><translation>确认设备身份</translation></message>
    <message><source>Connect to %1:%2 as %3 to read its serial?
Check this public-key fingerprint against a trusted device record:
%4
Enter the usual device password when asked. No capture will start.</source><translation>以 %3 登录 %1:%2 并读取序列号？
请与可信设备记录核对以下公钥指纹：
%4
随后输入平时登录设备的密码。此操作不会启动采集。</translation></message>
    <message><source>Reading cancelled. Click Connect and read to try again.</source><translation>已取消读取，可点击“连接并读取”重试。</translation></message>
    <message><source>Log in with the usual device password to read its serial...</source><translation>请输入平时登录设备的密码，软件将自动读取序列号…</translation></message>
    <message><source>Serial read automatically. Confirm the calibration and actual inference orientation, then save the device. No capture was started.</source><translation>序列号已自动读入。请确认标定和实际推理输入朝向，再保存设备。未启动采集。</translation></message>
    <message><source>Automatic serial reading failed. Retry or follow the manual steps below.</source><translation>序列号自动读取失败，可重试或按下方步骤手动获取。</translation></message>
    <message><source>Automatic reading failed: %1

Check the address, SSH port, username and password, then click Connect and read to retry. If it still fails:
1. Open Windows Terminal and run:
%2
2. After login, run:
cat /proc/cpuinfo
3. Copy the 16 hexadecimal characters after Serial into the Serial field. Save, then verify the connection.
If Serial is missing or not 16 hexadecimal characters, contact the device provider; do not invent a serial.</source><translation>自动读取失败：%1

请检查地址、SSH 端口、用户名和密码，然后点击“连接并读取”重试。仍失败时：
1. 打开 Windows 终端，执行：
%2
2. 登录后执行：
cat /proc/cpuinfo
3. 复制 Serial 后的 16 位十六进制字符，填入“序列号”，保存后验证连接。
如果没有 Serial，或不是 16 位十六进制字符，请联系设备提供方，不要自行编造序列号。</translation></message>
    <message><source>Device details changed during reading; connect and read again</source><translation>读取期间设备信息已变更，请重新连接并读取。</translation></message>
    <message><source>Serial response does not match the confirmed device</source><translation>序列号回执与已确认的设备不一致。</translation></message>
    <message><source>Read serial differs from the saved device; add the new device separately</source><translation>读到的序列号与已保存设备不一致，请单独添加新设备。</translation></message>
    <message><source>Cannot read a 16-character CPU Serial from /proc/cpuinfo</source><translation>无法从 /proc/cpuinfo 读取 16 位 CPU 序列号。</translation></message>
    <message><source>Device returned more than one CPU Serial</source><translation>设备返回了多个 CPU 序列号，无法确认身份。</translation></message>
    <message><source>Device response is too large.</source><translation>设备回执过大，已停止读取。</translation></message>
    <message><source>RV SSH authentication failed</source><translation>设备登录失败，请检查用户名和密码。</translation></message>
    <message><source>RV authentication cancelled or credentials unavailable</source><translation>设备登录已取消或无法获取登录凭据。</translation></message>
    <message>
        <source>0 degrees</source>
        <translation>0°</translation>
    </message>
    <message>
        <source>180 degrees</source>
        <translation>180°</translation>
    </message>
    <message>
        <source>16-character device serial</source>
        <translation>16 位设备序列号</translation>
    </message>
    <message>
        <source>2026-10-08 calibration / F=1</source>
        <translation>2026-10-08 标定 / F=1</translation>
    </message>
    <message>
        <source>2026-10-08 calibration / F=2</source>
        <translation>2026-10-08 标定 / F=2</translation>
    </message>
    <message>
        <source>Active device: %1</source>
        <translation>当前设备：%1</translation>
    </message>
    <message>
        <source>Add device</source>
        <translation>添加设备</translation>
    </message>
    <message>
        <source>Address</source>
        <translation>地址</translation>
    </message>
    <message>
        <source>Calibration</source>
        <translation>标定配置</translation>
    </message>
    <message>
        <source>Clear saved login</source>
        <translation>清除登录凭据</translation>
    </message>
    <message>
        <source>Device Manager</source>
        <translation>设备管理</translation>
    </message>
    <message>
        <source>Device details</source>
        <translation>设备信息</translation>
    </message>
    <message>
        <source>Device files (*.json)</source>
        <translation>设备配置文件 (*.json)</translation>
    </message>
    <message>
        <source>Device operation failed.</source>
        <translation>设备操作失败。</translation>
    </message>
    <message>
        <source>Device operation timed out.</source>
        <translation>设备操作超时。</translation>
    </message>
    <message>
        <source>Device profiles exported without passwords.</source>
        <translation>设备配置已导出，不包含密码。</translation>
    </message>
    <message>
        <source>Device saved. Click Use selected device to select it for imaging.</source>
        <translation>设备已保存。点击「使用所选设备」将其选为成像设备。</translation>
    </message>
    <message>
        <source>Device selected. Click Start in either imaging panel when ready.</source>
        <translation>设备已选定。就绪后可在任一成像模块点击「开始」。</translation>
    </message>
    <message>
        <source>Device trust removed.</source>
        <translation>已移除对此设备的信任。</translation>
    </message>
    <message>
        <source>Devices imported. Passwords were not imported.</source>
        <translation>设备已导入，不包含密码。</translation>
    </message>
    <message>
        <source>Enter a complete Ed25519 public key</source>
        <translation>请输入完整的 Ed25519 公钥</translation>
    </message>
    <message>
        <source>Export devices</source>
        <translation>导出设备</translation>
    </message>
    <message>
        <source>Export...</source>
        <translation>导出…</translation>
    </message>
    <message>
        <source>Fingerprint</source>
        <translation>公钥指纹</translation>
    </message>
    <message>
        <source>I verified the public key, serial, calibration and actual inference orientation.</source>
        <translation>我已核对公钥、序列号、标定配置及实际推理输入朝向。</translation>
    </message>
    <message>
        <source>Import %1 device(s) from a trusted source? Existing devices will not be overwritten.</source>
        <translation>确认从可信来源导入 %1 台设备？已有设备不会被覆盖。</translation>
    </message>
    <message>
        <source>Import devices</source>
        <translation>导入设备</translation>
    </message>
    <message>
        <source>Import...</source>
        <translation>导入…</translation>
    </message>
    <message>
        <source>Key read. Verify the fingerprint using a trusted record before saving.</source>
        <translation>公钥已读取，请与可信记录核对指纹后再保存。</translation>
    </message>
    <message>
        <source>Last connected</source>
        <translation>最近连接</translation>
    </message>
    <message>
        <source>Legacy coordinator (no automatic fault recovery)</source>
        <translation>旧版协调器（不支持自动故障恢复）</translation>
    </message>
    <message>
        <source>Login username</source>
        <translation>登录用户名</translation>
    </message>
    <message>
        <source>Name</source>
        <translation>名称</translation>
    </message>
    <message>
        <source>Never</source>
        <translation>尚未连接</translation>
    </message>
    <message>
        <source>No active device</source>
        <translation>尚未选择设备</translation>
    </message>
    <message>
        <source>Not configured</source>
        <translation>未配置</translation>
    </message>
    <message>
        <source>Not verified</source>
        <translation>未核对</translation>
    </message>
    <message>
        <source>Public key</source>
        <translation>设备公钥</translation>
    </message>
    <message>
        <source>Read device key</source>
        <translation>读取设备公钥</translation>
    </message>
    <message>
        <source>Reading a key does not establish trust. Check it against a trusted device record. Saving does not start capture; passwords stay in Windows Credential Manager.</source>
        <translation>读取公钥不会自动信任设备，请与可信设备记录核对。保存配置不会启动采集；密码保存在本机 Windows 凭据管理器中。</translation>
    </message>
    <message>
        <source>Reload</source>
        <translation>重新加载</translation>
    </message>
    <message>
        <source>Remove</source>
        <translation>移除</translation>
    </message>
    <message>
        <source>Remove device</source>
        <translation>移除设备</translation>
    </message>
    <message>
        <source>Remove the saved Windows login for this device? The next connection will ask for its password.</source>
        <translation>清除此设备在 Windows 中保存的登录凭据？下次连接时需重新输入密码。</translation>
    </message>
    <message>
        <source>Remove trust for %1? This does not change the device.</source>
        <translation>移除对“%1”的信任？设备本身不会被修改。</translation>
    </message>
    <message>
        <source>SSH port</source>
        <translation>SSH 端口</translation>
    </message>
    <message>
        <source>Save device</source>
        <translation>保存设备</translation>
    </message>
    <message>
        <source>Saved login cleared.</source>
        <translation>登录凭据已清除。</translation>
    </message>
    <message>
        <source>Select a device or add a new one.</source>
        <translation>请选择已有设备，或添加新设备。</translation>
    </message>
    <message>
        <source>Serial</source>
        <translation>序列号</translation>
    </message>
    <message>
        <source>Trust imported devices</source>
        <translation>信任导入的设备</translation>
    </message>
    <message>
        <source>Host inference input orientation</source>
        <translation>上位机推理输入朝向</translation>
    </message>
    <message>
        <source>Use selected device</source>
        <translation>使用所选设备</translation>
    </message>
    <message>
        <source>Verified %1. No capture was started.</source>
        <translation>“%1”验证通过，未启动采集。</translation>
    </message>
    <message>
        <source>Verify connection</source>
        <translation>验证连接</translation>
    </message>
    <message>
        <source>Working...</source>
        <translation>正在处理…</translation>
    </message>
    <message>
        <source>Verify the fingerprint, serial, calibration and orientation, then check the confirmation box</source>
        <translation>请先核对指纹、序列号、标定和输入朝向，再勾选确认框。</translation>
    </message>
    <message>
        <source>Confirm calibration and inference orientation before selecting this device</source>
        <translation>请先确认标定配置及推理输入朝向，再选择此设备。</translation>
    </message>
    <message>
        <source>Save the edited device before verifying or clearing its login</source>
        <translation>请先保存修改，再验证连接或清除登录凭据。</translation>
    </message>
    <message>
        <source>Device registry changed in another window; reload before saving</source>
        <translation>设备库已在其他窗口中更改，请重新加载后再保存。</translation>
    </message>
    <message>
        <source>Duplicate device ID, public key or serial; edit the existing device explicitly</source>
        <translation>设备 ID、公钥或序列号重复，请直接编辑已有设备。</translation>
    </message>
    <message>
        <source>Invalid device address</source>
        <translation>设备地址无效。</translation>
    </message>
    <message>
        <source>Device name must contain 1 to 80 characters</source>
        <translation>设备名称应为 1–80 个字符。</translation>
    </message>
    <message>
        <source>RV serial must be 16 lowercase hexadecimal characters</source>
        <translation>RV 序列号必须为 16 位十六进制字符。</translation>
    </message>
    <message>
        <source>A complete ssh-ed25519 public key is required</source>
        <translation>需要完整的 ssh-ed25519 公钥。</translation>
    </message>
    <message>
        <source>Invalid login username</source>
        <translation>登录用户名无效。</translation>
    </message>
    <message>
        <source>Device registry is busy; reload and try again</source>
        <translation>设备库正在使用中，请重新加载后重试。</translation>
    </message>
    <message>
        <source>Close</source>
        <translation>关闭</translation>
    </message>
    <message>
        <source>Import conflicts with a saved device identity; rebind it explicitly</source>
        <translation>导入配置与已保存的设备身份冲突，请明确核对并重新绑定。</translation>
    </message>
    <message>
        <source>Saved: %1
New: %2</source>
        <translation>原指纹：%1
新指纹：%2</translation>
    </message>
    <message>
        <source>Camera mounting angle</source>
        <translation>摄像头安装角度</translation>
    </message>
    <message>
        <source>Correct the camera mounting angle before display and model preprocessing. Model input orientation is a separate setting.</source>
        <translation>在显示和模型预处理之前校正摄像头安装角度，模型推理输入方向是独立设置。</translation>
    </message>
<message><source>Install protected on-board inference...</source><translation>安装板端推理与自动切换…</translation></message>
<message><source>Verified %1. Protected LiteSR is missing. Use Install protected on-board inference.</source><translation>已验证 %1。缺少加密 LiteSR，请点击「安装板端推理与自动切换」。</translation></message>
<message><source>Verified %1. The existing on-board inference model checksum differs.</source><translation>已验证 %1。现有板端推理模型的校验值不符。</translation></message>
<message><source>Verified %1. The on-board inference runtime is incomplete.</source><translation>已验证 %1。板端推理运行环境不完整。</translation></message>
<message><source>Verified %1. Protected LiteSR and automatic dToF are ready.</source><translation>已验证 %1。加密 LiteSR 和 dToF 自动切换均已就绪。</translation></message>
<message><source>Verified %1. On-board inference model status is unavailable.</source><translation>已验证 %1。暂时无法获取板端推理模型状态。</translation></message>
<message><source>LiteSR model verified on %1. Click Start in the camera panel when ready.</source><translation>已校验 %1 上的 LiteSR 模型。就绪后可在原视频面板点击“开始”。</translation></message>
<message><source>On-board model import is unavailable in offline mode.</source><translation>离线模式下无法导入板端推理模型。</translation></message>
<message><source>Stop imaging and dToF tuning before importing a LiteSR model.</source><translation>导入 LiteSR 模型前，请先停止成像及 dToF 调参。</translation></message>
<message><source>Select near-range on-board INT8 model</source><translation>选择板端近距 INT8 模型</translation></message>
<message><source>RKNN model (*.rknn)</source><translation>RKNN 模型 (*.rknn)</translation></message>
<message><source>Import on-board inference model</source><translation>导入板端推理模型</translation></message>
<message><source>Copy the verified LiteSR model to %1 (%2) if it is missing? Existing model files will be preserved.</source><translation>如果 %1（%2）缺少 LiteSR 模型，是否将校验通过的文件复制到板端？已有模型文件将保留。</translation></message>
</context>

<context><name>RvTunerWidget</name><message><source>ID %1 | row %2, column %3 | target %4 | measurement %5
Range %6 mm (radial) | SNR %7 | slot %8 | Signal %9 | Noise encoded %10</source><translation>原生 ID %1 | 行 %2，列 %3 | 目标 %4 | 测量 %5
距离 %6 mm（径向）| SNR %7 | 有效槽 %8 | Signal %9 | Noise 编码 %10</translation></message><message><source>%1 counts, encoded 0x%2</source><translation>%1 counts，编码值 0x%2</translation></message><message><source>not enabled</source><translation>未启用</translation></message><message><source>Ordinary</source><translation>普通</translation></message><message><source>Long range</source><translation>远距离</translation></message><message><source>High accuracy</source><translation>高精度</translation></message><message><source>Dual</source><translation>双模式</translation></message><message><source>Dual long range</source><translation>双模式远距离</translation></message><message><source>not read / not applied</source><translation>尚未读取／尚未应用</translation></message><message><source>%1×%2 / mode %3 / SNR %4 / %5k / %6ms / near %7k / %8 target(s) / Signal %9 / Noise %10</source><translation>%1×%2／模式 %3／SNR %4／%5k／%6ms／近距 %7k／%8 目标／Signal %9／Noise %10</translation></message><message><source>on</source><translation>开</translation></message><message><source>off</source><translation>关</translation></message><message><source>Pending: %1</source><translation>待应用：%1</translation></message><message><source>Read the current device configuration first.</source><translation>请先读取设备当前配置。</translation></message><message><source>Experiment: %1</source><translation>当前实验：%1</translation></message><message><source>Long-term saved: %1</source><translation>已长期保存：%1</translation></message><message><source>Read the current device configuration first. Opening or restoring this dock does not start acquisition.</source><translation>请先读取设备当前配置。打开模块或恢复布局不会自动采集。</translation></message><message><source>All supported resolutions can be saved permanently. Apply pending edits first. Both Native and LiteSR sensor command lines are backed up and verified. DEPTHOR V3 also accepts 8×8 ordinary, long range or high accuracy with one target and Noise off; other shared imaging requires compatible 48×32 settings.</source><translation>所有受支持分辨率均可长期保存。请先应用待修改参数。Native 和 LiteSR 两份传感器命令均备份并核验；DEPTHOR V3 还支持 8×8 普通、远距离或高精度模式，要求单目标、Noise 关；其他共享成像仍要求兼容的 48×32 参数。</translation></message><message><source>Target %1</source><translation>目标 %1</translation></message><message><source>0 – %1 mm (radial)</source><translation>0 – %1 mm（径向）</translation></message><message><source>0 – %1 counts (%2)</source><translation>0 – %1 counts（%2）</translation></message><message><source>log</source><translation>对数</translation></message><message><source>linear</source><translation>线性</translation></message><message><source>Signal not enabled / no data. Enable Signal preview to compare both plots. Preview does not save permanent settings.</source><translation>Signal 未启用／无数据。请启用 Signal 预览以比较双图。预览不会长期保存参数。</translation></message><message><source>Valid %1/%2 (%3%) | SNR min/mean/max %4/%5/%6 | received %7 FPS | %8 °C | warnings 0x%9 | dropped %10</source><translation>有效 %1/%2（%3%）| SNR 最小／均值／最大 %4/%5/%6 | 实收 %7 FPS | %8 °C | 告警 0x%9 | 丢弃 %10</translation></message><message><source>Save experiment preset</source><translation>保存实验预设</translation></message><message><source>JSON files (*.json)</source><translation>JSON 文件 (*.json)</translation></message><message><source>Load experiment preset</source><translation>加载实验预设</translation></message><message><source>Export comparison results</source><translation>导出比较结果</translation></message><message><source>Read current</source><translation>读取当前配置</translation></message><message><source>Start</source><translation>开始</translation></message><message><source>Stop</source><translation>停止</translation></message><message><source>Apply experiment</source><translation>应用实验参数</translation></message><message><source>Long-term save</source><translation>长期保存</translation></message><message><source>Save preset</source><translation>保存预设</translation></message><message><source>Load preset</source><translation>加载预设</translation></message><message><source>Enable Signal preview</source><translation>启用 Signal 预览</translation></message><message><source>Record comparison</source><translation>记录比较结果</translation></message><message><source>Export results</source><translation>导出结果</translation></message><message><source>Resolution</source><translation>分辨率</translation></message><message><source>Mode</source><translation>测量模式</translation></message><message><source>Iterations (k)</source><translation>累计次数（k）</translation></message><message><source>SNR threshold</source><translation>SNR 阈值</translation></message><message><source>Period (ms)</source><translation>测量周期（ms）</translation></message><message><source>Dual near iterations (k)</source><translation>双模式近距累计（k）</translation></message><message><source>Targets per zone</source><translation>每区域目标数</translation></message><message><source>Signal output</source><translation>输出 Signal</translation></message><message><source>Noise output</source><translation>输出 Noise</translation></message><message><source>Log</source><translation>对数</translation></message><message><source>Native grid</source><translation>原生网格</translation></message><message><source>Aligned display</source><translation>对齐显示</translation></message><message><source>Both plots use the same complete measurement and target.</source><translation>两图来自同一个完整测量和同一目标。</translation></message><message><source>Distance display maximum; does not change measurements.</source><translation>距离色标上限，不改变测量值。</translation></message><message><source>Signal display maximum in decoded counts. Encoded values are shown on hover.</source><translation>Signal 色标上限，单位为解码 counts。悬停可查看原始编码值。</translation></message><message><source>Distance (radial)</source><translation>距离图（径向）</translation></message><message><source>Photon intensity (Signal counts)</source><translation>光子强度图（Signal counts）</translation></message><message><source>No measurement. Read configuration, then click Start.</source><translation>暂无测量。请读取配置后点击“开始”。</translation></message><message><source>Parameters</source><translation>参数</translation></message><message><source>Target</source><translation>目标</translation></message><message><source>Valid %</source><translation>有效占比 %</translation></message><message><source>Mean SNR</source><translation>平均 SNR</translation></message><message><source>Received FPS</source><translation>实收 FPS</translation></message><message><source>Measurement</source><translation>测量序号</translation></message></context>
<context><name>RvTunerController</name><message><source>Device operation timed out; ownership and fault evidence are retained.</source><translation>设备操作超时，保留归属和故障证据。</translation></message><message><source>No fresh complete measurement; stopping the experiment.</source><translation>没有新鲜完整测量，正在停止实验。</translation></message><message><source>dToF Parameter Tuning</source><translation>dToF 参数调试</translation></message><message><source>Malformed device operation receipt.</source><translation>设备操作回执格式错误。</translation></message><message><source>Device identity changed; operation receipt rejected.</source><translation>设备身份已变化，拒绝操作回执。</translation></message><message><source>Device operation failed or timed out; check device logs.</source><translation>设备操作失败或超时，请检查设备日志。</translation></message><message><source>Reading current Native and LiteSR settings...</source><translation>正在读取 Native 和 LiteSR 当前参数…</translation></message><message><source>Checking parameters, installed runtime, device identity and actual capture owner...</source><translation>正在核验参数、已安装运行库、设备身份及实际采集所有者…</translation></message><message><source>Cancelled before handover; shared acquisition was left running.</source><translation>接管前已取消，原共享采集保持运行。</translation></message><message><source>Current settings read. Opening this module does not start capture.</source><translation>当前参数已读取，打开模块不会自动采集。</translation></message><message><source>Shared session changed during preflight; retry Start.</source><translation>预检查期间共享会话已变化，请重新点击“开始”。</translation></message><message><source>Stopping shared receivers and inference; waiting for the verified board owner and STOP acknowledgement...</source><translation>正在停止共享接收与推理，等待已核验的板端所有者退出及 STOP 应答…</translation></message><message><source>Experiment stopped. Shared imaging remains stopped; use Camera raw video &gt; Start.</source><translation>实验已停止。普通成像保持停止，请从“摄像头原视频”手动开始。</translation></message><message><source>Stopped</source><translation>已停止</translation></message><message><source>dToF tuning ended; start shared imaging manually.</source><translation>dToF 调试已结束，请手动启动共享成像。</translation></message><message><source>dToF settings saved. Shared imaging requires 48×32; use dToF tuning for this resolution. Saved values will not be overridden.</source><translation>dToF 参数已长期保存。共享成像要求 48×32；当前分辨率请在 dToF 调试模块中使用。已保存值不会被自动覆盖。</translation></message><message><source>dToF settings saved and read back for Native and LiteSR.</source><translation>Native 和 LiteSR 的 dToF 参数已长期保存并回读核验。</translation></message><message><source>dToF settings saved. LiteSR cannot start with this combination; saved values will not be overridden.</source><translation>dToF 参数已保存。此组合不兼容 LiteSR，启动时将明确拒绝，不会覆盖已保存值。</translation></message><message><source>Experiment receipt differs from requested settings.</source><translation>实验回执与请求参数不一致。</translation></message><message><source>Experiment running. Changes are temporary until Long-term save is clicked.</source><translation>实验正在运行。点击“长期保存”前的修改仅用于当前实验。</translation></message><message><source>Applying experiment parameters...</source><translation>正在应用实验参数…</translation></message><message><source>Stopping the current experiment and verifying a new complete measurement...</source><translation>正在停止当前实验并核验新参数的完整测量…</translation></message><message><source>Backing up and atomically saving only dToF settings; verifying both scripts...</source><translation>正在备份并原子保存 dToF 参数，核验两份采集脚本…</translation></message><message><source>Stopping dToF tuning...</source><translation>正在停止 dToF 调试…</translation></message><message><source>Waiting for normal sensor STOP and resource release...</source><translation>正在等待传感器正常 STOP 及资源释放…</translation></message><message><source>Waiting for a complete DTN1 measurement...</source><translation>正在等待完整 DTN1 测量…</translation></message><message><source>dToF settings saved. DEPTHOR V3 supports these 8×8 settings; other shared imaging requires compatible 48×32 settings. Saved values will not be overridden.</source><translation>dToF 参数已保存。DEPTHOR V3 支持此 8×8 配置；其他共享成像要求兼容的 48×32 参数，不会自动覆盖已保存值。</translation></message><message><source>dToF settings saved. The separate RGB576 FP16 + 16×16 ToF option supports these settings. Other model requirements are unchanged.</source><translation>dToF参数已保存。独立的RGB576 FP16＋16×16 ToF选项支持此配置，其他模型的要求保持。</translation></message></context>
<context><name>RvSessionController</name><message><source>dToF Parameter Tuning</source><translation>dToF 参数调试</translation></message><message><source>Stop dToF tuning before selecting another device.</source><translation>请先停止 dToF 调试，再选择其他设备。</translation></message><message><source>RV1103 is owned by dToF tuning. Stop it before starting shared imaging.</source><translation>RV1103 正由 dToF 参数调试占用，请先停止调试再启动共享成像。</translation></message></context>
<context><name>RvDeviceManagerDialog</name><message><source>Stop dToF tuning before changing trusted devices.</source><translation>请先停止 dToF 调试，再修改可信设备配置。</translation></message></context>
<context><name>RvSessionController</name></context>
<context><name>RvAssistDepth</name><message><source>Host model ready; waiting for input</source><translation>上位机模型已就绪，等待输入</translation></message></context>
<context><name>RvAssistDepth</name><message><source>Invalid dToF output support contract</source><translation>dToF 输出支持范围契约无效</translation></message></context>
<context><name>RvAssistFusion</name><message><source>Invalid dToF output support contract</source><translation>dToF 输出支持范围契约无效</translation></message></context>
<context><name>RvSessionController</name><message><source> | dToF geometric field %1%; continuous predicted depth inside</source><translation> | dToF 几何视野 %1%；范围内保留连续预测深度</translation></message></context>
<context><name>DepthMapWidget</name>
<message><source>UniDepth correction mode</source><translation>UniDepth 校正方案</translation></message>
<message><source>A: Current fusion / RGB</source><translation>A：原融合 / RGB 触发</translation></message>
<message><source>B: Current fusion / ToF</source><translation>B：原融合 / ToF 触发</translation></message>
<message><source>UniDepth only. A uses the original RGB inference cadence; B uses current fusion on each new ToF measurement; C adds edge-preserving residual correction. RGB preview stays independent. Switching clears old depth and waits for fresh input. C has not shown better held-out real-scene accuracy.</source><translation>仅用于 UniDepth。A 保持原 RGB 推理节奏；B 在每次新 ToF 测量时使用原融合；C 增加保边残差校正。RGB 预览保持独立。切换后清除旧深度并等待新输入。C 尚未证明实景留出点精度更好。</translation></message>
</context>
<context><name>RvAssistDepth</name>
<message><source>Host depth correction mode changed; waiting for a new measurement</source><translation>上位机深度校正方案已切换，等待新测量</translation></message>
</context>
<context><name>DepthMapWidget</name>
</context>
<context><name>RvDepthView</name>
<message><source>Waiting for matching correction points</source><translation>等待与深度图匹配的校正点</translation></message>
<message><source>Correction point data unavailable for this depth frame</source><translation>本幅深度图的校正点信息不可用</translation></message>
<message><source>No current dToF correction points (monocular or held scale)</source><translation>本帧无当前 dToF 校正点（单目预测或沿用历史尺度）</translation></message>
<message><source>ToF %1 | adopted %2 | global %3 | local %4</source><translation>ToF %1 | 实际采用 %2 点 | 全局 %3 | 局部 %4</translation></message>
<message><source>Yellow rings: global scale | magenta dots: local correction</source><translation>黄圈：全局尺度 | 洋红点：局部校正</translation></message>
<message><source>Excluded: invalid/quality %1 | boundary %2 | conflict %3 | duplicate %4 | outside %5</source><translation>筛除：无效/质量 %1 | 边界 %2 | 冲突 %3 | 重复 %4 | 范围外 %5</translation></message>
<message><source>Global scale + local correction</source><translation>全局尺度 + 局部校正</translation></message>
<message><source>Global scale only</source><translation>仅参与全局尺度</translation></message>
<message><source>Local correction only</source><translation>仅参与局部校正</translation></message>
<message><source>ToF %1 | ID %2 | row %3, column %4
%5
Radial %6 m | SNR %7
Projected camera-Z %8 m</source><translation>ToF %1 | ID %2 | 行 %3，列 %4
%5
径向距离 %6 m | SNR %7
投影 camera-Z %8 m</translation></message>
<message><source>Final camera-Z %1 m | final - ToF %2 m</source><translation>最终 camera-Z %1 m | 最终 − ToF %2 m</translation></message>
<message><source>Depth is valid but outside the color range (black)</source><translation>深度有效，但超出色标范围（显示为黑色）</translation></message>
<message><source>No valid final depth at this projection; check the geometric field boundary</source><translation>此投影位置没有有效输出深度，请检查几何视野边界</translation></message>
<message><source>Pair delta %1 ms | clock budget %2 ms</source><translation>配对时差 %1 ms | 时钟预算 %2 ms</translation></message>
<message><source>Agreement with an adopted point is not independent accuracy</source><translation>与采用点的一致性不代表独立真值精度</translation></message>
</context>
<context><name>DepthMapWidget</name>
<message><source>B: Fusion + local recovery / ToF</source><translation>B：原融合 + 局部恢复 / ToF 触发</translation></message>
<message><source>G: Global scale only / RGB (fast trial)</source><translation>G：仅全局尺度 / RGB 触发（快速试验）</translation></message>
<message><source>Host assisted depth inference (wide RGB 576, fast trial)</source><translation>上位机辅助深度推理 宽视野 RGB 576（快速试验）</translation></message>
<message><source>Host assisted depth inference (wide RGB 576 FP16, temporary candidate)</source><translation>上位机辅助深度推理 宽视野 RGB 576 FP16（临时候选）</translation></message>
<message><source>UniDepth only. A uses original fusion at RGB cadence. B recovers coherent unsupported local groups on new ToF measurements. G uses global scale only at RGB cadence; it omits local corrections and can lose local metric accuracy. A fresh RGB prediction may use a clearly identified historical scale between ToF measurements. Diagnostic snapshots save one matched input only when clicked.</source><translation>仅用于 UniDepth。A 按 RGB 节奏运行原融合；B 在新 ToF 测量到达时恢复可信局部测点组；G 按 RGB 节奏只校正全局尺度，省略局部校正，局部米制精度可能降低。两次 ToF 测量之间的新 RGB 预测可能使用明确标记的历史尺度。诊断快照仅在点击时保存一份配对输入。</translation></message>
<message><source>UniDepth only. A uses original fusion at RGB cadence. B recovers coherent unsupported local groups on new ToF measurements. RGB preview stays independent. Diagnostic snapshots save one matched input only when clicked.</source><translation>仅用于 UniDepth。A 按 RGB 节奏运行原融合；B 在新 ToF 测量到达时恢复可信的局部测点组。RGB 预览独立。诊断快照仅在点击时保存一份配对输入。</translation></message>
<message><source>UniDepth only. A keeps the original fusion and RGB cadence. B keeps supported original results and recovers coherent unsupported local groups on new ToF measurements. C uses the full edge-residual experiment. RGB preview stays independent. Diagnostic snapshots save one matched input only when clicked.</source><translation>仅用于 UniDepth。A 保留旧融合与 RGB 节奏；B 保留已有支持的旧结果，在新 ToF 测量中恢复一致但缺少局部支持的区域；C 保留全分辨率残差试验。RGB 预览独立。只有点击诊断快照才保存一次配对输入。</translation></message>
<message><source>Diagnostic snapshot saved to:
%1</source><translation>诊断快照已保存到：
%1</translation></message>
<message><source>Cannot save a complete current diagnostic snapshot:
%1</source><translation>无法保存完整的当前诊断快照：
%1</translation></message>
</context>
<context><name>RvAssistDepth</name><message><source>Invalid host depth diagnostic payload</source><translation>主机深度诊断负载无效</translation></message></context>
<context><name>RvSessionController</name>
<message><source> | Local recovery: %1 anchors</source><translation> | 局部恢复：%1 个锚点</translation></message>
<message><source> | Global fit unavailable; current local correction only</source><translation> | 全局拟合未更新，仅当前局部校正</translation></message>
</context>
<context><name>DepthMapWidget</name>
<message><source>H: Global scale / RGB + latest dToF reuse</source><translation>H：全局尺度 / RGB 配最新 dToF（可复用）</translation></message>
<message><source>H uses global scale at RGB cadence and may reuse the newest dToF measurement within 100 ms. Reuse is labeled; it is not a new measurement. Without a valid pair, only the historical scale is retained.</source><translation>H 按 RGB 节奏做全局尺度校正，允许复用配对时间差 100 ms 内最新的 dToF 测量。界面标注复用，不视为新测量；没有有效配对时仅沿用历史尺度。</translation></message>
</context>
<context><name>RvDepthView</name>
<message><source>dToF %1 | pair delta %2 ms</source><translation>dToF %1 | 配对差 %2 ms</translation></message>
<message><source>reused</source><translation>复用测量</translation></message>
<message><source>new measurement</source><translation>新测量</translation></message>
</context>
<context><name>RvSessionController</name>
<message><source> | ToF %1: %2 | pair delta %3 ms</source><translation> | ToF %1：%2 | 配对差 %3 ms</translation></message>
<message><source>reused</source><translation>复用测量</translation></message>
<message><source>new measurement</source><translation>新测量</translation></message>
</context>
<context><name>RvDepthView</name>
</context>
<context><name>SensorMainWindow</name>
    <message><source>Host-assisted inference unavailable</source><translation>上位机辅助推理不可用</translation></message>
    <message><source>For now, choose On-board inference under Imaging scheme, then click Start if needed.</source><translation>建议先在「成像方案」中选择「板端推理」，必要时点击「开始」。</translation></message><message><source>About</source><translation>关于</translation></message><message><source>Magnetic tactile sensors</source><translation>磁触觉传感器</translation></message><message><source>Depth Map</source><translation>深度图</translation></message><message><source>Camera raw video</source><translation>摄像头原视频</translation></message><message><source>dToF Parameter Tuning</source><translation>dToF参数设置</translation></message><message><source>File</source><translation>文件</translation></message><message><source>Exit</source><translation>退出</translation></message><message><source>View</source><translation>视图</translation></message><message><source>Reset layout</source><translation>恢复默认布局</translation></message><message><source>Layout Presets</source><translation>布局预设</translation></message><message><source>Save layout...</source><translation>保存布局…</translation></message><message><source>Delete layout...</source><translation>删除布局…</translation></message><message><source>Settings</source><translation>设置</translation></message><message><source>Device Manager...</source><translation>设备管理…</translation></message><message><source>Rendering Mode</source><translation>渲染设置</translation></message><message><source>Standard Mode</source><translation>标准模式</translation></message><message><source>Smooth Mode</source><translation>流畅模式</translation></message><message><source>Language</source><translation>语言</translation></message><message><source>English</source><translation>English</translation></message><message><source>Simplified Chinese</source><translation>简体中文</translation></message><message><source>Delete layout</source><translation>删除布局</translation></message><message><source>Layout name:</source><translation>布局名称：</translation></message><message><source>Save layout</source><translation>保存布局</translation></message><message><source>No saved layouts</source><translation>尚无已保存布局</translation></message>    <message><source>Install required board files</source><translation>安装板端所需文件</translation></message>
    <message><source>%1 is missing required board files. Open the on-board inference model import to install them?</source><translation>%1 缺少成像所需的板端文件。是否通过「安装板端推理与自动切换」进行安装？</translation></message>
</context><context>
    <name>RvUsbEnrollment</name>
    <message>
        <source>RV1103 setup unavailable: %1</source>
        <translation>无法自动配置 RV1103：%1</translation>
    </message>
    <message>
        <source>New RV1103 detected</source>
        <translation>检测到新的 RV1103</translation>
    </message>
    <message>
        <source>Add USB device %1 and start imaging?

SSH fingerprint: %2
This uses the factory login and F=%3 calibration with 0° mounting and 0° model input. Confirm these settings match the device. Any current imaging session will stop. Windows may ask for permission to configure the USB network adapter.</source>
        <translation>是否添加 USB 设备 %1 并开始成像？

SSH 指纹：%2
将使用出厂账号、F=%3 标定、0° 摄像头安装朝向和 0° 模型输入朝向。请确认这些设置与设备相符。当前成像会话将停止。Windows 可能会请求配置 USB 网卡的权限。</translation>
    </message>
    <message>
        <source>Stop imaging before setting up the new RV1103.</source>
        <translation>请先停止成像，再配置新的 RV1103。</translation>
    </message>
    <message>
        <source>The RV1103 connection changed. Reconnect the board and try again.</source>
        <translation>RV1103 的连接已变化，请重新插入设备后重试。</translation>
    </message>
    <message>
        <source>Could not configure the RV1103 USB network adapter.</source>
        <translation>无法配置 RV1103 的 Windows USB 网卡。</translation>
    </message>
    <message>
        <source>Could not assign an IPv4 address to the RV1103.</source>
        <translation>无法为 RV1103 的 USB 网口设置 IPv4 地址。</translation>
    </message>
    <message>
        <source>RV1103 verification failed. Check its factory login, calibration and USB connection.</source>
        <translation>RV1103 验证失败，请检查出厂账号、标定和 USB 连接。</translation>
    </message>
    <message>
        <source>RV1103 setup failed</source>
        <translation>RV1103 配置失败</translation>
    </message>
    <message>
        <source>Reconnect the USB device to retry, or open Device Manager.</source>
        <translation>请重新插入 USB 设备重试，或打开设备管理。</translation>
    </message>
<message><source>The device was added. To use on-board inference, open Settings &gt; Device Manager and choose Install protected on-board inference.</source><translation>设备已添加。缺少板端推理模型，可在“设置 → 设备管理”中选择“安装板端推理与自动切换”。</translation></message>
<message><source>The device was added. The existing on-board inference model checksum differs. Review the board model before starting on-board inference.</source><translation>设备已添加。现有板端推理模型的校验值不符，请核对板端模型后再启动板端推理。</translation></message>
<message><source>The device was added. On-board inference requires its runtime to be installed on the device.</source><translation>设备已添加。使用板端 板端推理 前，需要先在设备上部署配套运行环境。</translation></message>
<message><source>RV1103 on-board inference model</source><translation>RV1103 板端推理模型</translation></message>
</context>
<context>
    <name>RvDeviceManagerDialog</name>
    <message>
        <source>Verified %1. Automatic dToF support is missing. Use Install protected on-board inference to install the required files.</source>
        <translation>已验证 %1。缺少 dToF 自动切换支持，请点击「安装板端推理与自动切换」。</translation>
    </message>
    <message>
        <source>Verified %1. The board capture script is unavailable or unsupported.</source>
        <translation>已验证 %1。板端采集脚本缺失或版本不受支持。</translation>
    </message>
    <message>
        <source>Automatic dToF is ready. Select the on-board inference model file to enable board inference.</source>
        <translation>dToF 自动切换已就绪。选择板端推理模型文件以启用板端推理。</translation>
    </message>
    <message>
        <source>Protected LiteSR and automatic dToF are ready on %1. Click Start in either imaging panel.</source>
        <translation>%1 的加密 LiteSR 和 dToF 自动切换均已就绪。可在任一成像模块点击「开始」。</translation>
    </message>
    <message>
        <source>Automatic dToF is ready. The existing on-board inference model checksum differs and the file was preserved.</source>
        <translation>dToF 自动切换已就绪。现有板端推理模型校验不符，已保留原文件。</translation>
    </message>
    <message>
        <source>Automatic dToF is ready. The on-board inference runtime is incomplete.</source>
        <translation>dToF 自动切换已就绪。板端推理运行环境不完整。</translation>
    </message>
    <message>
        <source>Stop imaging before importing board files.</source>
        <translation>请先停止成像，再导入板端文件。</translation>
    </message>
    <message>
        <source>Install automatic dToF support on %1 (%2) and check the on-board inference model? If the model is missing, you can select a file to copy.</source>
        <translation>是否在 %1（%2）上安装 dToF 自动切换支持并检查板端推理模型？如模型缺失，可选择文件复制。</translation>
    </message>
</context>
<context>
    <name>RvUsbEnrollment</name>
    <message>
        <source>The device was added. Open Settings &gt; Device Manager &gt; Install protected on-board inference to install the required files.</source>
        <translation>设备已添加。请打开「设置 → 设备管理 → 安装板端推理与自动切换」安装所需文件。</translation>
    </message>
    <message>
        <source>The device was added. The board capture script is unavailable or unsupported.</source>
        <translation>设备已添加。板端采集脚本缺失或版本不受支持。</translation>
    </message>
</context>
<context>
    <name>UiStyle</name>
    <message><source>host-assisted inference model</source><translation>上位机辅助推理模型</translation></message>
    <message><source>on-board inference model</source><translation>板端推理模型</translation></message>
</context>
</TS>
