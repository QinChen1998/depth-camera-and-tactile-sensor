#include "RvAssistDepth.h"
#include "RvCameraView.h"
#include "RvDepthDiagnostic.h"
#include "../RuntimeDiscovery.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>
#include <windows.h>
#include <dxgi.h>
#include <qscopeguard.h>

namespace {
constexpr auto &View = RvCameraView::DepthPro2Portrait;
constexpr int HeaderLimit = 65536;
constexpr int PacketLimit = 12 + HeaderLimit + View.depthBytes();
constexpr int HostWorkerLoadTimeoutMs = 90000;
constexpr int FrameLifetimeMs = 1000;
constexpr int InferenceTimeoutMs = 1500;

bool hasNvidiaAdapter()
{
    HMODULE dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dxgi)return true; // Device enumeration is unavailable. Let the worker diagnose CUDA.
    const auto close=qScopeGuard([dxgi]{FreeLibrary(dxgi);});
    using CreateFactory=HRESULT (WINAPI *)(REFIID,void **);
    const auto create=reinterpret_cast<CreateFactory>(GetProcAddress(dxgi,"CreateDXGIFactory1"));
    IDXGIFactory1 *factory=nullptr;
    if(!create||FAILED(create(__uuidof(IDXGIFactory1),reinterpret_cast<void **>(&factory))))return true;
    const auto release=qScopeGuard([factory]{factory->Release();});
    for(UINT index=0;;++index){
        IDXGIAdapter1 *adapter=nullptr;
        const auto result=factory->EnumAdapters1(index,&adapter);
        if(result==DXGI_ERROR_NOT_FOUND)return false;
        if(FAILED(result))return true;
        DXGI_ADAPTER_DESC1 info{};
        const auto description=adapter->GetDesc1(&info);
        adapter->Release();
        if(SUCCEEDED(description)&&info.VendorId==0x10de&&!(info.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))return true;
    }
}

bool hdSource(const QJsonObject &m,const RvCameraView::Profile &view) {
    const auto integer=[&](const char *key){const double n=m[key].toDouble(-1);return m[key].isDouble()&&n>0&&n<=9007199254740991.0&&std::floor(n)==n;};
    if(m["source_protocol"]!="HDA1"||(m["source_transport"]!="rtsp_h265"&&m["source_transport"]!="cxwl_depth_sdk")
        ||m["source_candidate_mode"]!=RvCaptureContract::Native48
        ||m["model"]!="Native"||m["source_rgb_width"]!=2304||m["source_rgb_height"]!=1296
        ||m["rgb_width"]!=view.inputWidth||m["rgb_height"]!=view.inputHeight||!m["dtof_present"].isBool()
        ||m["rectify_map_sha256"]!=QLatin1String(view.mapSha)
        ||(QLatin1String(view.name)=="wide"&&m["assist_camera_profile"]!="wide")
        ||!integer("frame_id")||!integer("generation")||!integer("host_decoded_monotonic_ns")
        ||!QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(m["boot_id"].toString()).hasMatch())return false;
    if(m["dtof_present"].toBool()) {
        const double delta=m["pair_delta_ms"].toDouble(1000),bound=m["pair_clock_uncertainty_ms"].toDouble(-1);
        if(m["work_mode"]!="rgb_tof"||!integer("tof_sequence")||!integer("tof_timestamp_ns")||!integer("rgb_timestamp_ns")
            ||!m["tof_reused"].isBool()
            ||!std::isfinite(delta)||!std::isfinite(bound)||bound<0||bound>35||std::abs(delta)+bound>200
            ||std::abs((m["tof_timestamp_ns"].toDouble()-m["rgb_timestamp_ns"].toDouble())/1e6-delta)>.001
            ||(m["pair_clock_source"]!="rtcp_prft_board_realtime_monotonic"
                &&!(m["source_transport"]=="cxwl_depth_sdk"&&m["pair_clock_source"]==QLatin1String(RvTofFrame::SdkPairClock)))
            ||m["source_engine"]!="native-hd-capture-v1"||m["generation_source"]!="host_presentation_epoch"
            ||!QRegularExpression("^[0-9a-f]{32}$").match(m["source_generation"].toString()).hasMatch())return false;
    }else {
        if(m["work_mode"]!="host_rgb")return false;
        for(const auto *key:{"tof_sequence","tof_timestamp_ns","pair_delta_ms","tof_reused","rgb_timestamp_ns"})if(m.contains(key))return false;
    }
    return true;
}
QJsonObject nativeContext(const QJsonObject &m){
    QJsonObject result;
    for(const auto *key:{"boot_id","source_generation","dtof_fov_correction","tof_fov_correction","tof_warning_flags",
                        "dtof_width","dtof_height","dtof_bytes","dtof_format","dtof_range_unit","dtof_source","dtof_coordinate"})
        result[key]=m.value(key);
    return result;
}
constexpr const char *HdTofFields[]={"tof_sequence","tof_timestamp_ns","pair_delta_ms","tof_reused","rgb_timestamp_ns",
            "dtof_width","dtof_height","dtof_bytes","dtof_format","dtof_range_unit","dtof_source","dtof_coordinate",
            "dtof_fov_correction","tof_fov_correction","tof_warning_flags","pair_clock_uncertainty_ms","pair_clock_source",
            RvAssistFusion::NativeProjectionField};
void dropHdTof(QJsonObject &m) {
    m["dtof_present"]=false;m["work_mode"]="host_rgb";
    m["pairing_policy"]="no_current_native_measurement";m["dtof_absent_reason"]="native_measurement_stale";
    for(const auto *key:HdTofFields)m.remove(key);
}
QJsonObject depthPro2WorkerInput(QJsonObject source) {
    // The TensorRT worker only estimates monocular depth. The paired measurement stays in the host request.
    if(source["dtof_present"]==true){
        dropHdTof(source);
        source.remove("pairing_policy");
        source.remove("dtof_absent_reason");
    }
    source.remove("measurement_age_at_pair_ms");
    return source;
}
qint64 monotonicMs() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
QByteArray planarRgb(const QImage &source) {
    const auto image=source.convertToFormat(QImage::Format_RGB888);
    const int width=image.width(),height=image.height(),pixels=width*height;
    QByteArray out(3*pixels,Qt::Uninitialized);
    auto *destination=out.data();
    for(int y=0;y<height;++y){
        const auto *row=image.constScanLine(y);
        auto *red=destination+y*width,*green=red+pixels,*blue=green+pixels;
        for(int x=0;x<width;++x){red[x]=char(row[3*x]);green[x]=char(row[3*x+1]);blue[x]=char(row[3*x+2]);}
    }
    return out;
}

QByteArray packet(const QJsonObject &request, const QByteArray &rgb)
{
    const auto metadata = QJsonDocument(request).toJson(QJsonDocument::Compact);
    QByteArray result(12 + metadata.size() + rgb.size(), Qt::Uninitialized);
    qToLittleEndian<quint32>(quint32(metadata.size()), result.data());
    qToLittleEndian<quint32>(quint32(rgb.size()), result.data() + 4);
    qToLittleEndian<quint32>(0, result.data() + 8);
    std::memcpy(result.data() + 12, metadata.constData(), size_t(metadata.size()));
    std::memcpy(result.data() + 12 + metadata.size(), rgb.constData(), size_t(rgb.size()));
    return result;
}

bool matchesIdentity(const QJsonObject &value, const QString &prefix = {})
{
    const auto identity = RvCameraView::depthPro2Identity(QLatin1String(RvCameraView::DepthPro2Model), RvCameraView::DepthPro2SRuntime);
    for (auto field = identity.begin(); field != identity.end(); ++field)
        if (value[prefix + field.key()] != field.value()) return false;
    return true;
}
}

RvAssistDepth::RvAssistDepth(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &RvAssistDepth::handleTimeout);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &RvAssistDepth::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr = (m_stderr + m_process.readAllStandardError()).right(2048);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // A crash/exit can come with a fatal receipt. Drain it in finished().
        if (error != QProcess::Crashed && !m_stopping && !m_destroying) fail(m_process.errorString());
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        if (m_destroying) return;
        const bool stopped = m_stopping;
        if (!stopped && m_process.bytesAvailable()) {
            const QPointer<RvAssistDepth> alive(this);
            readOutput();
            if (!alive || m_failed || !m_enabled) return;
        }
        if (!stopped) {
            fail(tr("Host assisted depth process exited (%1): %2").arg(code).arg(QString::fromUtf8(m_stderr).right(600)));
            return;
        }
        m_stopping = false;
        m_ready = false;
        m_busy = false;
        m_timeout.stop();
        startWorker();
    });
}

RvAssistDepth::~RvAssistDepth()
{
    m_destroying = true;
    m_process.disconnect(this);
    m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
}

bool RvAssistDepth::hostModelAvailable(const QString &model)
{
    const auto *runtime = RvCameraView::depthPro2Runtime(model);
    if (!runtime) return false;
    const QDir root(RuntimeDiscovery::rvAssistModelRoot(model));
    QFile file(root.filePath("runtime.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return false;
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    if (manifest["schema"] != 1 || manifest["backend"] != QLatin1String(RvCameraView::DepthPro2Backend)
        || manifest["engine_sha256"] != QLatin1String(runtime->engineSha)
        || manifest["tensorrt_version"] != QLatin1String(RvCameraView::DepthPro2TensorRt)) return false;
    for (const auto *name : {RvCameraView::DepthPro2Worker, runtime->view.graphFile, "Qt6Core.dll",
                             "nvinfer_10.dll", "nvinfer_plugin_10.dll", "cudart64_12.dll"}) {
        const QFileInfo dependency(root.filePath(QLatin1String(name)));
        if (!dependency.isFile() || !dependency.isReadable() || !dependency.size()) return false;
    }
    return true;
}

QString RvAssistDepth::preflightError(const QString &model)
{
    if (!hostModelAvailable(model)) return tr("The %1 TensorRT runtime is missing or invalid.").arg(QLatin1String(View.modelName));
    if (!hasNvidiaAdapter()) return tr("%1 requires a compatible NVIDIA GPU.").arg(QLatin1String(View.modelName));
    return {};
}

void RvAssistDepth::setInputRotationDegrees(int degrees) {
    // Rotate the model input independently of the calibrated output and display.
    if((degrees!=0&&degrees!=180) || degrees==m_inputRotation)return;
    m_inputRotation=degrees;cancel(tr("Host inference input orientation changed; waiting for a new paired frame"));
}

void RvAssistDepth::setEnabled(bool enabled) {
    if(m_destroying||m_enabled==enabled)return;
    m_enabled=enabled;m_failed=false;
    if(!enabled)cancel(tr("Host assisted depth is disabled"));
    // Launch only when a selected, paired frame is received, never on startup.
}
void RvAssistDepth::cancel(const QString &reason) {
    if(m_cancelling)return;
    m_cancelling=true;
    emit loadingStateChanged(0);
    m_pending={};m_current={};m_busy=false;m_ready=false;m_buffer.clear();m_timeout.stop();m_lastHdTofSequence=0;m_lastGuiPublishMs=0;
    m_lastHdTofTimestampNs=0;m_lastHdTofSamples.clear();m_lastHdTofContext={};
    m_depthPro2Scale.reset();

    if(m_process.state()!=QProcess::NotRunning) {
        m_stopping=true;
        m_process.kill();
    }
    const QPointer<RvAssistDepth> alive(this);
    if(!m_destroying)emit cleared(reason);
    if(alive)m_cancelling=false;
}
void RvAssistDepth::fail(const QString &reason)
{
    if (m_failed) return;
    const bool initializationFailed = !m_ready;
    m_failed = true;
    const QPointer<RvAssistDepth> alive(this);
    cancel(reason);
    if (alive) emit inferenceUnavailable(tr("The host inference worker failed to initialize or run the model."),
                                         reason, initializationFailed);
}

QByteArray RvAssistDepth::preparePlanarInput(const QImage &image){return planarRgb(image);}
void RvAssistDepth::submitHd(const QImage &camera,const QJsonObject &source,quint64 epoch,const QByteArray &nativeSamples,const QByteArray &preparedInput) {
    if(m_destroying||!m_enabled||m_failed)return;
    const bool paired=source["dtof_present"].toBool();
    const auto &view=View;
    if(camera.size()!=view.inputSize()||(!preparedInput.isEmpty()&&preparedInput.size()!=view.rgbBytes())
        ||source["assist_experiment"]!="B"||!hdSource(source,view)
        ||(paired?!RvTofFrame{nativeSamples,source}.isValidForHostFusion():!nativeSamples.isEmpty())) {
        cancel(tr("Invalid managed H.265 model input"));return;
    }
    if(paired&&!RvCameraView::supportsNativeGrid(QLatin1String(RvCameraView::DepthPro2Model),source["dtof_width"].toInt(),source["dtof_height"].toInt())){
        cancel(tr("Invalid managed H.265 model input"));return;
    }
    if(!m_pending.camera.isNull()){++m_dropped;m_dropReasons["pending_superseded"]=m_dropReasons["pending_superseded"].toInt()+1;}
    // Empty auxiliary bytes mean RGB only. No all-zero synthetic ToF frame.
    m_pending={camera,{nativeSamples,source},epoch,++m_request,qint64(source["host_decoded_monotonic_ns"].toDouble()/1000000.)};
    m_pending.planarInput=preparedInput;
    m_pending.tof.metadata["assist_host_model"]=QLatin1String(RvCameraView::DepthPro2Model);
    m_pending.tof.metadata["assist_input_rotation_degrees"]=m_inputRotation;
    m_pending.tof.metadata["assist_diagnostic_capture"]=true;
    const double now=double(monotonicMs());const auto enqueued=source.value("host_pair_enqueued_ms");
    m_pending.tof.metadata["host_pair_enqueued_ms"]=enqueued.isDouble()&&enqueued.toDouble()>=0&&enqueued.toDouble()<=now?enqueued.toDouble():now;
    if(m_process.state()==QProcess::NotRunning)startWorker();
    else if(m_ready&&!m_busy)sendLatest();
}

void RvAssistDepth::startWorker()
{
    if (m_destroying || !m_enabled || m_failed || m_pending.camera.isNull() || m_process.state() != QProcess::NotRunning) return;
    const auto model = QLatin1String(RvCameraView::DepthPro2Model);
    if (!hostModelAvailable(model)) {
        fail(tr("Selected host model runtime is missing; prepare the optional runtime first"));
        return;
    }
    m_ready = false;
    m_busy = false;
    m_stopping = false;
    m_buffer.clear();
    m_stderr.clear();
    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto root = RuntimeDiscovery::rvAssistModelRoot(model);
    const auto program = QDir(root).filePath(QLatin1String(RvCameraView::DepthPro2Worker));
    m_process.setProcessEnvironment(RuntimeDiscovery::nativeWorkerEnvironment(program));
    m_process.setWorkingDirectory(root);
    m_process.setProgram(program);
    m_process.setArguments({"--rv-assist-worker", root, m_token, model});
    m_process.start();
    m_timeout.start(HostWorkerLoadTimeoutMs);
    emit statusChanged(tr("Loading the host-assisted inference model..."));
    emit loadingStateChanged(1);
}

bool RvAssistDepth::takePending() {
    if(m_pending.camera.isNull())return false;
    m_current=std::move(m_pending);m_pending={};
    if(monotonicMs()-m_current.receivedMs>1000) {m_dropReasons["pending_expired"]=m_dropReasons["pending_expired"].toInt()+1;m_current={};emit cleared(tr("Paired frame expired before host inference"));return false;}
    if(!m_current.tof.samples.isEmpty()) {
        auto &meta=m_current.tof.metadata;
        const auto sequence=quint64(meta["tof_sequence"].toInteger());
        const auto timestamp=meta["tof_timestamp_ns"].toInteger();
        const bool reused=sequence==m_lastHdTofSequence;
        const bool expired=meta["measurement_age_at_pair_ms"].toDouble()+monotonicMs()-meta["host_pair_enqueued_ms"].toDouble()>1000;
        if(expired||sequence<m_lastHdTofSequence){
            m_current.tof.samples.clear();
            dropHdTof(meta);
        }else{
            const auto context=nativeContext(meta);
            if((reused&&(timestamp!=m_lastHdTofTimestampNs||m_current.tof.samples!=m_lastHdTofSamples||context!=m_lastHdTofContext))
                ||(!reused&&m_lastHdTofSequence&&timestamp<=m_lastHdTofTimestampNs)){
                m_dropReasons["native_identity_regressed"]=m_dropReasons["native_identity_regressed"].toInt()+1;m_current={};return false;
            }
            meta["tof_reused"]=reused;
            m_lastHdTofSequence=sequence;m_lastHdTofTimestampNs=timestamp;m_lastHdTofSamples=m_current.tof.samples;m_lastHdTofContext=context;
        }
    }
    return true;
}
void RvAssistDepth::sendLatest()
{
    if (!m_enabled || m_failed || !m_ready || m_busy || !takePending()) return;
    const QJsonObject request{{"protocol", 1}, {"type", "infer"}, {"token", m_token},
        {"request", qint64(m_current.request)}, {"epoch", qint64(m_current.epoch)},
        {"metadata", depthPro2WorkerInput(m_current.tof.metadata)}, {"host_model", RvCameraView::DepthPro2Model}};
    const auto bytes = packet(request, m_current.planarInput.isEmpty() ? planarRgb(m_current.camera) : m_current.planarInput);
    m_busy = true;
    if (m_process.write(bytes) != bytes.size()) {
        fail(tr("Cannot send paired frame to host depth process"));
        return;
    }
    m_timeout.start(InferenceTimeoutMs);
}

void RvAssistDepth::handleTimeout() {

    // A busy GUI can deliver the expired timer before the already completed
    // worker's readyRead notification. Drain available IPC first; readOutput
    // still rejects old results and may start a NEW request/deadline.
    const QPointer<RvAssistDepth> alive(this);
    const auto token=m_token;
    if(m_process.state()==QProcess::Running&&m_process.bytesAvailable()==0)m_process.waitForReadyRead(0);
    if(!alive)return;
    readOutput();
    if(!alive||m_destroying||m_stopping||m_failed||!m_enabled||m_token!=token)return;
    if(m_timeout.isActive()||(m_ready&&!m_busy))return;
    fail(tr("Host assisted depth timed out; disable and enable it to retry"));
}
void RvAssistDepth::readOutput()
{
    const auto chunk = m_process.readAllStandardOutput();
    if (m_destroying || m_stopping || !m_enabled) return;
    m_buffer += chunk;
    if (m_buffer.size() > 2 * PacketLimit) {
        fail(tr("Host depth protocol exceeded its bound"));
        return;
    }
    while (m_buffer.size() >= 12) {
        QElapsedTimer responseClock;
        responseClock.start();
        const auto size = qFromLittleEndian<quint32>(m_buffer.constData());
        const auto body = qFromLittleEndian<quint32>(m_buffer.constData() + 4);
        const auto auxiliary = qFromLittleEndian<quint32>(m_buffer.constData() + 8);
        if (!size || size > HeaderLimit || (body != 0 && body != View.depthBytes()) || auxiliary != 0) {
            fail(tr("Invalid host depth packet size"));
            return;
        }
        const int total = 12 + int(size + body);
        if (m_buffer.size() < total) return;
        const auto document = QJsonDocument::fromJson(m_buffer.mid(12, size));
        const auto depth = m_buffer.mid(12 + size, body);
        m_buffer.remove(0, total);
        if (!document.isObject()) {
            fail(tr("Host depth protocol identity mismatch"));
            return;
        }
        if (!acceptResponse(document.object(), depth, responseClock)) return;
    }
}

RvAssistFusion::Result RvAssistDepth::correctDepthPro2(const QByteArray &depth,const Input &current,QJsonObject &metadata,QByteArray &diagnostic)
{
    QElapsedTimer clock;clock.start();
    const auto &view=View;
    const bool paired=!current.tof.samples.isEmpty();
    auto tof=current.tof;
    const auto projection=RvAssistFusion::defaultNativeProjection(tof);
    if(paired&&projection!=QLatin1String(RvAssistFusion::CloseNative48Projection)){
        RvAssistFusion::Result rejected;
        rejected.error=QStringLiteral("Invalid native ToF projection");
        return rejected;
    }
    tof.metadata.insert(QLatin1String(RvAssistFusion::NativeProjectionField),projection);
    std::vector<float> mono(depth.size()/int(sizeof(float)));
    qFromLittleEndian<quint32>(depth.constData(),qsizetype(mono.size()),mono.data());
    const auto &cal=RvAssistFusion::calibrationForFov(tof.metadata["dtof_fov_correction"].toInt());
    auto result=m_depthPro2Scale.apply(paired?RvAssistFusion::fuseGlobalScale(mono,tof,cal,view)
        :RvAssistFusion::monocular(mono,"H.265 input has no verified time-paired dToF",view.outputSize(),true),
        monotonicMs(),current.epoch,metadata["generation"].toInteger(),metadata["frame_id"].toInteger(),true);
    if(!result.ok())return result;
    if(paired){
        for(const auto *key:HdTofFields)metadata.insert(QLatin1String(key),tof.metadata.value(key));
        for(const auto *key:{"dtof_present","work_mode","pairing_policy","measurement_age_at_pair_ms","dtof_absent_reason"})
            metadata.insert(QLatin1String(key),tof.metadata.value(key));
    }
    const auto mode=result.diagnostics["fusion_mode"].toString();
    const bool unadjusted=mode=="monocular",held=mode=="held_scale";
    metadata["model"]=QString::fromLatin1(view.modelName)+(unadjusted?" (monocular)":held?" (held dToF scale)":" + dToF");
    metadata["depth_method"]=unadjusted?"monocular_unadjusted":held?"monocular_held_dtof_scale":"oriented_global_scale_latest_native_v2";
    metadata["depth_present"]=true;
    metadata["depth_bytes"]=view.depthBytes();
    metadata["assist_native_columns"]=RvCaptureContract::Native48Columns;
    metadata["assist_native_rows"]=RvCaptureContract::Native48Rows;
    metadata["assist_capture_mode"]=RvCaptureContract::Native48;
    metadata["assist_fusion"]=result.diagnostics;
    metadata["assist_fusion_revision"]=QLatin1String(RvAssistFusion::DepthPro2PortraitRevision);
    metadata["assist_diagnostic_payload"]="mono_reasons_v1";
    metadata["assist_diagnostic_native_count"]=RvDepthDiagnostic::ReasonBytes;
    diagnostic=RvDepthDiagnostic::encode(mono,result,RvDepthDiagnostic::ReasonBytes);
    metadata["assist_fusion_ms"]=clock.nsecsElapsed()/1e6;
    metadata["assist_total_ms"]=metadata["assist_inference_ms"].toDouble()+metadata["assist_fusion_ms"].toDouble();
    return result;
}

bool RvAssistDepth::acceptResponse(const QJsonObject &response, const QByteArray &depth, QElapsedTimer &responseClock)
{
    const QPointer<RvAssistDepth> alive(this);
    if (response["protocol"] != 1 || response["token"] != m_token) {
        fail(tr("Host depth protocol identity mismatch"));
        return false;
    }
    if (response["type"] == "fatal") {
        fail(response["error"].toString());
        return false;
    }
    const auto backend = QLatin1String(RvCameraView::DepthPro2Backend);
    if (response["type"] == "ready" && !m_ready && !m_busy && depth.isEmpty()) {
        const auto loading = response["model_loading"].toObject();
        if (!matchesIdentity(response) || loading["provider"] != "TensorRT" || loading["device"] != backend
            || loading["tensorrt_version"] != QLatin1String(RvCameraView::DepthPro2TensorRt)) {
            fail(tr("Host inference worker identity mismatch"));
            return false;
        }
        m_timeout.stop();
        m_ready = true;
        emit loadingStateChanged(2);
        if (!alive) return false;
        emit statusChanged(tr("Host model ready; waiting for input"));
        if (!alive || !m_enabled || m_stopping) return false;
        sendLatest();
        return bool(alive);
    }
    if (response["type"] != "result" || !m_busy || response["request"].toInteger() != qint64(m_current.request)
        || response["epoch"].toInteger() != qint64(m_current.epoch) || depth.size() != View.depthBytes()) {
        fail(tr("Unexpected host depth result"));
        return false;
    }
    const auto current = std::exchange(m_current, {});
    m_busy = false;
    m_timeout.stop();
    auto metadata = response["metadata"].toObject();
    if (!matchesIdentity(metadata, QStringLiteral("assist_")) || !RvCameraView::validOutputGrid(metadata)
        || metadata["depth_K"].toArray() != RvCameraView::intrinsics(View) || metadata["depth_unit"] != "camera-Z_m_float32"
        || metadata["assist_backend"] != backend || metadata["assist_execution_device"] != backend
        || metadata["assist_output_restored_to_calibrated_grid"] != true || metadata["assist_input_orientation_checked"] != true) {
        fail(tr("Host depth result does not match the selected model"));
        return false;
    }
    const auto source = depthPro2WorkerInput(current.tof.metadata);
    for (const auto *key : {"frame_id", "generation", "source_protocol", "boot_id", "source_generation", "host_decoded_monotonic_ns",
                           "dtof_present", "tof_sequence", "tof_timestamp_ns", "rgb_timestamp_ns", "pair_delta_ms", "tof_reused",
                           "pair_clock_uncertainty_ms", "pair_clock_source", "rectify_map_sha256", "rgb_width", "rgb_height",
                           "assist_experiment", "assist_input_rotation_degrees"}) {
        if (metadata.value(key) != source.value(key)) {
            fail(tr("Host depth result belongs to another RGB/dToF pair"));
            return false;
        }
    }
    const qint64 now = monotonicMs();
    const qint64 age = now - current.receivedMs;
    const double measurementAge = current.tof.metadata["measurement_age_at_pair_ms"].toDouble()
        + now - current.tof.metadata["host_pair_enqueued_ms"].toDouble(now);
    if (age > FrameLifetimeMs || (!current.tof.samples.isEmpty() && measurementAge > FrameLifetimeMs)) {
        m_dropReasons["result_expired"] = m_dropReasons["result_expired"].toInt() + 1;
        emit cleared(tr("Host assisted depth result expired"));
        if (!alive) return false;
        sendLatest();
        return alive && m_enabled && !m_stopping;
    }
    metadata["assist_frame_age_ms"] = double(age);
    metadata["assist_pending_dropped"] = qint64(m_dropped);
    metadata["assist_processing_budget_ms"] = FrameLifetimeMs;
    metadata["assist_parent_validation_ms"] = responseClock.nsecsElapsed() / 1e6;
    metadata["assist_previous_gui_publish_ms"] = m_lastGuiPublishMs;
    if (!current.tof.samples.isEmpty()) {
        metadata["assist_measurement_age_ms"] = measurementAge;
        metadata["assist_drop_reasons"] = m_dropReasons;
    }
    // Start the next GPU request before publishing this immutable RGB/ToF result.
    const auto token = m_token;
    sendLatest();
    if (!alive || !m_enabled || m_stopping || m_failed || token != m_token) return false;
    QByteArray diagnostic;
    auto fused = correctDepthPro2(depth, current, metadata, diagnostic);
    if (!fused.ok()) {
        const auto reason = QCoreApplication::translate("RvAssistFusion", fused.error.toUtf8().constData());
        emit frameRejected(metadata, reason);
        if (!alive) return false;
        emit cleared(reason);
        return alive && m_enabled && !m_stopping;
    }
    QElapsedTimer publishClock;
    publishClock.start();
    auto pairedTof = current.tof;
    pairedTof.metadata.insert(QLatin1String(RvAssistFusion::NativeProjectionField),
                              metadata.value(RvAssistFusion::NativeProjectionField));
    emit resultReady(fused.depth, metadata, current.camera, current.epoch, pairedTof, diagnostic, current.camera);
    if (!alive) return false;
    m_lastGuiPublishMs = publishClock.nsecsElapsed() / 1e6;
    return m_enabled && !m_stopping;
}

int runRvAssistDepthTests(const QString &reportPath) {
    // Guard exact model tensor bytes, including padded RGB rows and 180 degree
    // input orientation, independently of worker/backend floating point work.
    for(const auto size:{View.inputSize(),QSize(7,5)}){
        QImage rgb(size,QImage::Format_RGB888);
        for(int y=0;y<size.height();++y){auto *row=rgb.scanLine(y);
            for(int x=0;x<size.width();++x)for(int c=0;c<3;++c)row[3*x+c]=uchar((x*3+y*7+c*59)%256);}
        for(const auto &image:{rgb,rgb.flipped(Qt::Horizontal|Qt::Vertical),rgb.convertToFormat(QImage::Format_ARGB32)}){
            const auto converted=image.convertToFormat(QImage::Format_RGB888);
            const auto tensor=RvAssistDepth::preparePlanarInput(image);
            const int pixels=size.width()*size.height();
            if(tensor.size()!=3*pixels)return 3;
            for(int c=0;c<3;++c)for(int y=0;y<size.height();++y)for(int x=0;x<size.width();++x)
                if(quint8(tensor[c*pixels+y*size.width()+x])!=converted.constScanLine(y)[3*x+c])return 3;
        }
    }
    RvAssistDepth assist;
    const auto &profile=View;
    const bool testReuse=qEnvironmentVariable("DCTS_TEST_REUSE_TOF")=="1";
    const QString experiment=QStringLiteral("B");
    QEventLoop loop;
    QJsonObject report;
    QJsonArray statuses,cleared;
    QObject::connect(&assist,&RvAssistDepth::statusChanged,&loop,[&](const QString &message){
        if(statuses.size()<24)statuses.append(message);
    });
    QObject::connect(&assist,&RvAssistDepth::cleared,&loop,[&](const QString &message){
        if(cleared.size()<24)cleared.append(message);
    });
    QObject::connect(&assist,&RvAssistDepth::inferenceUnavailable,&loop,[&](const QString &,const QString &detail){
        report["error"]=detail;
        loop.quit();
    });
    QObject::connect(&assist,&RvAssistDepth::resultReady,&loop,[&](const QByteArray &depth,const QJsonObject &metadata,
        const QImage &,quint64,const RvTofFrame &tof,const QByteArray &diagnostic){
        if(testReuse&&metadata["tof_reused"]!=true)return;
        report["depth_bytes"]=depth.size();
        report["backend"]=metadata.value("assist_backend");
        report["width"]=metadata.value("width");report["height"]=metadata.value("height");
        report["experiment"]=metadata["assist_experiment"];report["tof_reused"]=metadata["tof_reused"];
        report["native_bytes"]=tof.samples.size();
        report["monocular_bytes"]=RvDepthDiagnostic::monocularDepth(diagnostic,metadata).size();
        loop.quit();
    });
    QTimer::singleShot(HostWorkerLoadTimeoutMs+10000,&loop,&QEventLoop::quit);
    assist.setEnabled(true);
    QImage image(profile.inputWidth,profile.inputHeight,QImage::Format_RGB32);
    image.fill(qRgb(88,130,170));
    const auto planar=RvAssistDepth::preparePlanarInput(image);
    QJsonObject source{{"source_protocol","HDA1"},{"source_transport","rtsp_h265"},
        {"model","Native"},{"source_rgb_width",2304},{"source_rgb_height",1296},
        {"assist_experiment",experiment},{"source_candidate_mode",RvCaptureContract::Native48},
        {"rgb_width",profile.inputWidth},{"rgb_height",profile.inputHeight},{"dtof_present",false},{"work_mode","host_rgb"},
        {"rectify_map_sha256",QLatin1String(View.mapSha)},{"assist_camera_profile","wide"},
        {"assist_tof_fov_correction",6},
        {"frame_id",1},{"generation",1},{"host_decoded_monotonic_ns",double(monotonicMs())*1000000.},
        {"boot_id","01234567-89ab-cdef-0123-456789abcdef"}};
    QByteArray samples;
    if(testReuse){
        samples.resize(48*32*4);
        for(int i=0;i<48*32;++i){
            qToLittleEndian(quint16(4000),samples.data()+i*4);
            samples[i*4+2]=9;
            samples[i*4+3]=1;
        }
        const QJsonObject paired{{"dtof_present",true},{"work_mode","rgb_tof"},
            {"source_generation",QString(32,'a')},{"source_engine","native-hd-capture-v1"},
            {"generation_source","host_presentation_epoch"},{"pairing_policy",RvTofFrame::FreshRgbReusePolicy},
            {"pair_clock_source","rtcp_prft_board_realtime_monotonic"},{"pair_clock_uncertainty_ms",35},
            {"tof_sequence",1},{"tof_timestamp_ns",1000000000.},{"tof_reused",false},{"tof_warning_flags",0},
            {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",samples.size()},{"dtof_fov_correction",6},
            {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
            {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"}};
        for(auto it=paired.constBegin();it!=paired.constEnd();++it)source[it.key()]=it.value();
    }
    // A real video stream supplies fresh frames while the first GPU request
    // warms up. Keep the production freshness gate, and exercise that stream
    // instead of relying on one cold-start frame surviving the GPU's 1 s gate.
    QTimer frames;
    frames.setInterval(100);
    qint64 frameId=0;
    const auto submit=[&]{
        auto frame=source;
        frame["frame_id"]=++frameId;
        frame["host_decoded_monotonic_ns"]=double(monotonicMs())*1000000.;
        if(testReuse){
            const double delta=-33.*std::min<qint64>(frameId,4);
            frame["pair_delta_ms"]=delta;
            frame["rgb_timestamp_ns"]=1000000000.-delta*1e6;
        }
        assist.submitHd(image,frame,1,samples,planar);
    };
    QObject::connect(&frames,&QTimer::timeout,&loop,submit);
    frames.start();
    submit();
    report["program"]=assist.m_process.program();
    report["working_directory"]=assist.m_process.workingDirectory();
    if(!report.contains("error"))loop.exec();
    frames.stop();
    report["statuses"]=statuses;report["cleared"]=cleared;report["submitted_frames"]=frameId;
    const bool passed=report["depth_bytes"].toInt()==profile.depthBytes()
        &&report["width"]==profile.outputWidth&&report["height"]==profile.outputHeight
        &&report["experiment"]==experiment&&report["native_bytes"].toInt(-1)==samples.size()
        &&report["monocular_bytes"].toInt()==profile.depthBytes()&&(!testReuse||report["tof_reused"]==true)
        &&report["backend"]==QLatin1String(RvCameraView::DepthPro2Backend);
    report["pass"]=passed;
    QFile file(reportPath);
    if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(report).toJson());
    return passed?0:2;
}

