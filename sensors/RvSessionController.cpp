#include "RvSessionController.h"
#include "RvTunerController.h"
#include "../RuntimeDiscovery.h"
#include "../UiStyle.h"
#include "RvCameraStream.h"
#include "RvHdVideoStream.h"
#include "RvAssistDepth.h"
#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include "RvDtofBoardStream.h"
#include "RvCaptureCalibration.h"
#include <QtCore/QUuid>
#include <QtCore/QDateTime>

#include <QtCore/QSettings>
#include <QtCore/QProcess>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QDebug>
#include <QtCore/QUrl>
#include <QtCore/QLibrary>
#include <QtCore/QDir>
#include <QtCore/qscopeguard.h>
#include <QtCore/QScopedValueRollback>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QHostAddress>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QPlaybackOptions>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>
#include <mutex>
#include <memory>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <limits>

namespace {
constexpr auto kDefaultRtspUrl = "nv12://172.32.0.93:8831";
constexpr auto kManagedRtspUrl = "rtsp://172.32.0.93/live/0";

bool isManagedRtsp(const QUrl &url,const QString &host)
{
    return !host.isEmpty() && url.scheme()=="rtsp" && url.host().compare(host,Qt::CaseInsensitive)==0 && url.port(554)==554
        && url.path()=="/live/0" && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}

bool isControlledSource(const QUrl &url,const QString &host)
{
    if (host.isEmpty()) return false;
    return isManagedRtsp(url,host) || (url.scheme()=="nv12" && url.host().compare(host,Qt::CaseInsensitive)==0
        && url.port(8831)==8831 && (url.path().isEmpty()||url.path()=="/")
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment());
}

QStringList vlcDirectories()
{
    return RuntimeDiscovery::vlcDirectories();
}

qint64 sessionGeneration(const QJsonValue &value)
{
    if (value.isString()) {
        bool valid = false;
        const auto generation = value.toString().toLongLong(&valid);
        return valid && generation > 0 ? generation : -1;
    }
    const auto generation = value.toInteger(-1);
    return value.isDouble() && generation > 0 && value.toDouble() == double(generation)
        ? generation : -1;
}

bool validRtspStatus(const QJsonObject &status)
{
    const auto service=status["service"].toObject(),telemetry=status["telemetry"].toObject();
    const qint64 generation=sessionGeneration(service["generation"]);
    const qint64 now=status["now_monotonic_ns"].toInteger(),updated=telemetry["updated_monotonic_ns"].toInteger();
    const qint64 encoded=telemetry["frame_id"].toInteger();
    for(const auto *field:{"tof_sequence","tof_timestamp_ns","pair_delta_ms","depth_unit","tof_reused"})
        if(telemetry.contains(QString::fromLatin1(field)))return false;
    return !status["boot_id"].toString().isEmpty() && service["state"]=="running"
        && service["preview_format"]=="rtsp_h265" && service["rgb_width"].toInt()==2304 && generation>0
        && service["model"]=="Native"
        && sessionGeneration(telemetry["generation"])==generation
        && telemetry["model"]==service["model"] && telemetry["transport"]=="rtsp"
        && telemetry["pixel_format"]=="H265" && telemetry["work_mode"]=="camera_only"
        && telemetry["depth_present"].isBool() && !telemetry["depth_present"].toBool()
        && telemetry["rgb_width"].toInt()==2304 && telemetry["rgb_height"].toInt()==1296
        && encoded>0 && telemetry["encoded_frame_id"].toInteger()==encoded
        && telemetry["progress_source"]=="venc_info:chn0"
        && telemetry["encoder_retrieved_frame_id"].toInteger()>0
        && updated>0 && now>=updated && now-updated<3000000000LL;
}

bool selectedFrameMatches(const QJsonObject &frame, const QJsonObject &expected)
{
    return frame["model"] == expected["model"]
        && sessionGeneration(frame["generation"]) == sessionGeneration(expected["generation"])
        && frame["frame_id"].toDouble() > expected["frame_id"].toDouble()
        && frame["rgb_width"] == expected["rgb_width"]
        && frame["work_mode"] == expected["work_mode"]
        && frame["depth_present"] == expected["depth_present"]
        && frame["target_width"].toInt(144) == expected["target_width"].toInt(144)
        && frame["requested_location"].toString("none") == expected["requested_location"].toString("none")
        && frame["requested_method"].toString("none") == expected["requested_method"].toString("none");
}
}

// Optional libVLC 3 ABI, loaded at runtime: no additional project/link dependency.
// Decoder callbacks only replace a single image; the GUI never queues video frames.
struct RtspVlcBackend
{
    QLibrary core, library;
    void *instance = nullptr, *player = nullptr;
    void *(*create)(int, const char *const *) = nullptr;
    void (*release)(void *) = nullptr;
    void *(*mediaNew)(void *, const char *) = nullptr;
    void (*mediaOption)(void *, const char *) = nullptr;
    void (*mediaRelease)(void *) = nullptr;
    void *(*playerNew)(void *) = nullptr;
    void (*playerRelease)(void *) = nullptr;
    int (*play)(void *) = nullptr;
    void (*stop)(void *) = nullptr;
    int (*videoSize)(void *, unsigned, unsigned *, unsigned *) = nullptr;
    using Lock = void *(*)(void *, void **);
    using Unlock = void (*)(void *, void *, void *const *);
    using Display = void (*)(void *, void *);
    using Format = unsigned (*)(void **, char *, unsigned *, unsigned *, unsigned *, unsigned *);
    void (*callbacks)(void *, Lock, Unlock, Display, void *) = nullptr;
    void (*formatCallbacks)(void *, Format, void (*)(void *)) = nullptr;
    std::mutex mutex;
    QImage latest;
    unsigned width = 0, height = 0, pitch = 0, lines = 0;
    quint64 sequence = 0, presented = 0;
    // Completion of the owned QImage at the memory output, not decode-only time.
    qint64 completedNs = 0;
    double outputMs = 0, copyMs = 0;
    int cacheMs = 100, threads = 1, probeBytes = 32;
    qint64 openNs = 0, firstOutputNs = 0, firstPtsUs = -1, latestPtsUs = -1;
    QString transport = "tcp", output = "vmem";
    static qint64 nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    // libVLC 3 public ABI (libvlc_media.h); counters are independent of Qt paints.
    struct Stats { int readBytes; float inputBitrate; int demuxBytes; float demuxBitrate;
        int corrupted, discontinuity, decodedVideo, decodedAudio, displayed, lostPictures,
            playedAudio, lostAudio, sentPackets, sentBytes; float sendBitrate; };
    void *(*playerMedia)(void *) = nullptr;
    int (*mediaStats)(void *, Stats *) = nullptr;
    bool managedCamera = false;
    struct Picture { QByteArray storage; unsigned char *data; unsigned width, height, pitch; qint64 lockedNs = 0; };

    template<class T> bool resolve(T &target, const char *name) {
        target = reinterpret_cast<T>(library.resolve(name));
        return target != nullptr;
    }
    bool load(const QString &directory) {
        core.setFileName(QDir(directory).filePath("libvlccore.dll"));
        library.setFileName(QDir(directory).filePath("libvlc.dll"));
        if (!core.load() || !library.load()) return false;
        resolve(playerMedia,"libvlc_media_player_get_media");
        resolve(mediaStats,"libvlc_media_get_stats");
        auto version = reinterpret_cast<const char *(*)()>(library.resolve("libvlc_get_version"));
        if (!version || !QByteArray(version()).startsWith("3.")) return false;
        return resolve(create, "libvlc_new") && resolve(release, "libvlc_release")
            && resolve(mediaNew, "libvlc_media_new_location") && resolve(mediaOption, "libvlc_media_add_option")
            && resolve(mediaRelease, "libvlc_media_release") && resolve(playerNew, "libvlc_media_player_new_from_media")
            && resolve(playerRelease, "libvlc_media_player_release") && resolve(play, "libvlc_media_player_play")
            && resolve(stop, "libvlc_media_player_stop") && resolve(callbacks, "libvlc_video_set_callbacks")
            && resolve(videoSize, "libvlc_video_get_size")
            && resolve(formatCallbacks, "libvlc_video_set_format_callbacks");
    }
    static unsigned format(void **opaque, char *chroma, unsigned *w, unsigned *h,
                           unsigned *pitches, unsigned *rows) {
        if (!*w || !*h || *w > 8192 || *h > 8192) return 0;
        auto *self = static_cast<RtspVlcBackend *>(*opaque);
        const unsigned codedWidth=*w,codedHeight=*h;
        // Codec allocation dimensions can include padding (e.g. 1312 vs 1296).
        unsigned visibleWidth = 0, visibleHeight = 0;
        if (self->videoSize(self->player, 0, &visibleWidth, &visibleHeight) == 0
            && visibleWidth > 0 && visibleHeight > 0 && visibleWidth <= *w && visibleHeight <= *h) {
            *w = visibleWidth; *h = visibleHeight;
        }
        if(self->managedCamera) {
            // The independently checked receipt/status fixes this source at
            // 2304x1296. H.265 may negotiate 1312 coded rows before videoSize is
            // ready. Accept only that known alignment; never crop another mode
            // to impersonate the managed source. Keep rows padded for decoding.
            if(codedWidth!=2304 || (codedHeight!=1296 && codedHeight!=1312)
                    || *w!=2304 || (*h!=1296 && *h!=1312))return 0;
            *w=2304;*h=1296;
        }
        std::memcpy(chroma, "RV32", 4);
        self->width = *w; self->height = *h;
        self->pitch = pitches[0] = (*w * 4 + 31) & ~31u;
        self->lines = rows[0] = (*h + 31) & ~31u;
        return 1;
    }
    static void *lock(void *opaque, void **planes) {
        auto *self = static_cast<RtspVlcBackend *>(opaque);
        auto *picture = new Picture;
        picture->width = self->width; picture->height = self->height; picture->pitch = self->pitch;
        picture->storage.resize(qsizetype(self->pitch) * self->lines + 31);
        picture->data = reinterpret_cast<unsigned char *>((quintptr(picture->storage.data()) + 31) & ~quintptr(31));
        picture->lockedNs = nowNs();
        planes[0] = picture->data;
        return picture;
    }
    static void unlock(void *opaque, void *id, void *const *) {
        auto *self = static_cast<RtspVlcBackend *>(opaque);
        auto *picture = static_cast<Picture *>(id);
        QImage frame(picture->data, int(picture->width), int(picture->height),
                     int(picture->pitch), QImage::Format_RGB32);
        const qint64 outputDone = nowNs();
        QImage copied = frame.copy();
        const qint64 copyDone = nowNs();
        std::lock_guard<std::mutex> guard(self->mutex);
        self->latest = std::move(copied);
        self->completedNs = copyDone;
        self->outputMs = (outputDone-picture->lockedNs)/1e6;
        self->copyMs = (copyDone-outputDone)/1e6;
        ++self->sequence;
        delete picture;
    }
    // Publish at memory output completion. VLC may discard a picture without displaying it.
    static void display(void *, void *) {}
    ~RtspVlcBackend() {
        // Synchronous VLC 3 stop joins callbacks before their storage is destroyed.
        if (player) { stop(player); playerRelease(player); }
        if (instance) release(instance);
    }
};

QJsonObject RvSessionController::videoDiagnostics(bool counters) const
{
    if(m_hdVideo){const auto s=m_hdVideo->snapshot();return {{"output","ffmpeg_prft"},{"transport","tcp"},{"avcodec_threads",1},
        {"output_sequence",QString::number(s.sequence)},{"frame_pts_us",QString::number(s.ptsUs)},
        {"producer_realtime_us",QString::number(s.producerRealtimeUs)},{"color_conversion_ms",s.decodeMs},
        {"clock_samples",m_hdClockSamples},{"clock_offset_ns",QString::number(m_hdRealtimeOffsetNs)},
        {"error",s.error},{"frame_delivery","timer_poll"},
        {"frame_watchdog_ms",m_frameTimer.interval()},{"frame_event_wakeups",0},
        {"frame_watchdog_checks",0}};}
    if(!m_vlc)return {};
    QJsonObject result;
    {std::lock_guard<std::mutex> lock(m_vlc->mutex);
     result={{"output_sequence",QString::number(m_vlc->sequence)},
         {"memory_output_completed_ns",QString::number(m_vlc->completedNs)},
         {"output_callback_ms",m_vlc->outputMs},{"image_copy_ms",m_vlc->copyMs},
         {"network_caching_ms",m_vlc->cacheMs},{"avcodec_threads",m_vlc->threads},{"transport",m_vlc->transport},{"output",m_vlc->output}};}
    if(m_vlc->output=="qt"){
        result["qt_probe_bytes"]=m_vlc->probeBytes;
        result["qt_open_ns"]=QString::number(m_vlc->openNs);
        result["qt_first_output_ns"]=QString::number(m_vlc->firstOutputNs);
        result["qt_first_pts_us"]=QString::number(m_vlc->firstPtsUs);
        result["qt_frame_pts_us"]=QString::number(m_vlc->latestPtsUs);
    }
    if(counters&&m_vlc->playerMedia&&m_vlc->mediaStats){
        RtspVlcBackend::Stats stats{};auto* media=m_vlc->playerMedia(m_vlc->player);
        if(media&&m_vlc->mediaStats(media,&stats))result["vlc_counters"]=QJsonObject{
            {"decoded_video",stats.decodedVideo},{"lost_pictures",stats.lostPictures},
            {"displayed_pictures",stats.displayed},{"demux_corrupted",stats.corrupted},
            {"demux_discontinuity",stats.discontinuity},{"demux_bytes",stats.demuxBytes}};
        // libvlc_media_player_get_media returns a retained reference.
        if(media)m_vlc->mediaRelease(media);
    }
    return result;
}

RvSessionController::RvSessionController(QObject *parent,const QString &deviceStorePath)
    : QObject(parent)
    , m_previewControl(new QProcess(this))
    , m_statusNetwork(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("rvSessionController"));
    m_tuner=new RvTunerController(this);
    m_previewControl->setObjectName(QStringLiteral("rvPreviewControl"));
    m_deviceStorePath=deviceStorePath;
    try { RvDeviceStore store(m_deviceStorePath);m_device=store.active(); }
    catch(const std::exception &e){m_deviceError=QString::fromUtf8(e.what());}
    QSettings settings;
    m_streamUrl = m_device.streamUrl();
    // Retired persisted model names never re-enable old inference.
    m_config.model = QStringLiteral("Native");
    auto savedScheme=settings.value("depth/imagingScheme").toString();
    if(savedScheme!="board_native_tof48"&&savedScheme!="board_native_tof48_high_accuracy")savedScheme="host_unidepth_rgb672_vga_fp16_tof16";
    settings.setValue("depth/imagingScheme",savedScheme);
    for(const auto *key:{"depth/boardLiteSrHostContourEnabled","depth/boardLiteSrHostHoleMode","depth/boardLiteSrHostOutliers"})settings.remove(key);
    m_config.cameraWidth=2304;
    m_dtofMode=savedScheme=="board_native_tof48"?RvDtofCandidates::Native48:savedScheme=="board_native_tof48_high_accuracy"?RvDtofCandidates::Native48HighAccuracy:RvDtofCandidates::NativeHd;
    m_dtofOptions=nativeCaptureOptions(m_dtofSettings);

    m_controlTimeout.setSingleShot(true);
    m_controlTimeout.setInterval(60000);
    m_switchFrameTimeout.setSingleShot(true);
    m_switchFrameTimeout.setInterval(15000);
    connect(&m_switchFrameTimeout, &QTimer::timeout, this, [this] {
        failPreviewControl(tr("Selected model frames did not arrive; check board status"), true);
    });
    connect(&m_controlTimeout, &QTimer::timeout, this, [this] {
        if (!m_controlPending) return;
        m_controlTimedOut = true;
        if (m_previewControl->state() != QProcess::NotRunning) m_previewControl->kill();
        else failPreviewControl(tr("Model or preview switch timed out; check board status"), true);
    });
    connect(m_previewControl, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_controlPending) failPreviewControl(m_previewControl->errorString());
    });
    connect(m_previewControl, &QProcess::finished, this, [this](int code, QProcess::ExitStatus result) {
        m_controlTimeout.stop();
        if (!m_controlPending || m_destroying) return;
        const QString error = QString::fromUtf8(m_previewControl->readAllStandardError()).trimmed();
        if (code != 0 || result != QProcess::NormalExit) {
            QString detail = error.isEmpty() ? tr("Preview switch failed; check board status") : error;
            if (code == 2 && m_previewControl->arguments().contains("--recover-stream-fault"))
                detail = tr("Automatic recovery stopped because the board could not safely restart.") + " " + m_lastReconnectReason + " " + detail;
            if(code==2 && (error.contains("key",Qt::CaseInsensitive)||error.contains("registry",Qt::CaseInsensitive)||error.contains("calibration",Qt::CaseInsensitive)||error.contains("serial",Qt::CaseInsensitive)))
                detail+=" "+tr("Open Settings > Device Manager to review this device.");
            else if (code == 2) detail += " " + tr("Click Start or Reconnect to attempt a controlled sensor reset. If recovery fails, the fault and ownership remain protected.");
            failPreviewControl(m_controlTimedOut ? tr("Model or preview switch timed out; check board status")
                : detail, code != 2 && !m_previewControl->arguments().contains("--automatic-start"));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(m_previewControl->readAllStandardOutput(), &parseError);
        const auto status = document.object();
        try {
            RvDeviceStore store(m_deviceStorePath);
            if(store.revision()!=m_deviceRevision || status["device_registry_revision"]!=m_deviceRevision)
                throw std::runtime_error("Device registry changed during connection; start a new session");
            const auto identified=RvDeviceProfile::fromJson(status["trusted_device"].toObject());
            if(store.byId(identified.id).json()!=identified.json() || identified.host.compare(m_device.host,Qt::CaseInsensitive)!=0 || identified.sshPort!=m_device.sshPort || !identified.ready())
                throw std::runtime_error("Control receipt does not match a trusted device at this address");
            m_device=identified;m_assist->setInputRotationDegrees(effectiveHostInputRotation(m_device));
            emit deviceChanged();
            if(!m_controlPending)return;
        } catch(const std::exception &e){failPreviewControl(QString::fromUtf8(e.what()));return;}
        if (m_dtofMode >= 0 && status["state"] == "board_files_unavailable" && status["generation"] == m_dtofToken) {
            const auto capture = status["capture_runtime"].toString();
            const auto automaticTof = status["automatic_tof"].toString();
            const bool installable = capture=="missing"||capture=="mismatch"||automaticTof!="ready";
            const bool manual = m_previewControl->arguments().contains("--recover-fault");
            const auto device = m_device;
            const auto detail = installable ? tr("Required board files are missing.")
                : automaticTof != "ready" ? tr("The board capture script is unavailable or unsupported.")
                : tr("The board capture runtime is incomplete.");
            failPreviewControl(detail);
            if (manual && installable && m_userStopped && !busy() && m_device.json() == device.json())
                emit boardFilesRequired(device);
            return;
        }
        if(m_dtofMode>=0){
            if(parseError.error!=QJsonParseError::NoError||!dtofBoardReceipt(status,m_dtofMode,m_dtofToken,m_dtofOptions)){failPreviewControl(tr("Candidate receipt mismatch"));return;}
            if(m_device.temporaryFovCalibration&&!RvCaptureCalibration::valid(status["tof_calibration"].toObject(),m_dtofToken,m_device.fov(),true)){
                failPreviewControl(tr("Candidate receipt mismatch"));return;
            }
            QSettings().setValue("rvDevices/lastConnected/"+m_device.fingerprint(),QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            m_dtofReceipt=status;m_controlPending=false;
            m_captureOwner={{"owner_boot",status["boot_id"]},{"owner_generation",m_dtofToken}};
            if(!m_userStopped){m_waitingForSelectedFrame=true;m_switchFrameTimeout.start();updateState();if(!m_userStopped)beginConnection(false);}
            return;
        }
        const auto service = status["service"].toObject(), telemetry = status["telemetry"].toObject();
        const auto model = service["model"].toString();
        const auto generation = sessionGeneration(service["generation"]);
        const int width = telemetry["rgb_width"].toInt();
        const bool cameraOnly = width == 2304;
        const bool transportValid = cameraOnly ? validRtspStatus(status) : telemetry["pixel_format"] == "NV12";
        const bool valid = parseError.error == QJsonParseError::NoError && document.isObject()
            && service["state"] == "running"
            && model == "Native"
            && (m_requestedModel.isEmpty() || model == m_requestedModel)
            && generation > 0 && sessionGeneration(telemetry["generation"]) == generation
            && telemetry["model"] == model && telemetry["frame_id"].toDouble() > 0
            && (width == 144 || width == 576 || width == 2304)
            && (m_requestedPreviewWidth == 0 || width == m_requestedPreviewWidth)
            && transportValid && telemetry["work_mode"] == (cameraOnly ? "camera_only" : "rgb_tof")
            && telemetry["depth_present"].isBool() && !telemetry["depth_present"].toBool()
            && (cameraOnly || (telemetry["capture_only"] == true && telemetry["inference_location"] == "none"))
            && telemetry["target_width"].toInt(144) == m_requestedDepthWidth
            && telemetry["requested_location"].toString("none") == m_requestedLocation
            && telemetry["requested_method"].toString("none") == m_requestedMethod;
        Configuration receipt;
        receipt.cameraWidth = width; receipt.model = model;
        receipt.depthWidth = telemetry["target_width"].toInt(144);
        receipt.location = telemetry["requested_location"].toString("none");
        receipt.method = telemetry["requested_method"].toString("none");
        if (!valid || !validConfiguration(receipt)) {
            failPreviewControl(tr("Model or preview confirmation mismatch; check board status"));
            return;
        }
        // keep-preview may discover a camera-only selection made by another client.
        if (cameraOnly && m_managedVlcDirectory.isEmpty() && !prepareManagedVlc()) {
            failPreviewControl(tr("VLC 3 with H.265 support is required for the managed camera"));
            return;
        }
        m_expectedDepthMetadata = cameraOnly ? QJsonObject() : telemetry;
        m_confirmedBoot=status["boot_id"].toString();
        m_captureOwner={{"owner_boot",m_confirmedBoot},{"owner_generation",QString::number(generation)}};
        m_expectedCameraStatus = cameraOnly ? status : QJsonObject();
        QSettings().setValue("rvDevices/lastConnected/"+m_device.fingerprint(),QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        m_config = receipt;
        m_confirmedConfig = receipt;
        m_hasConfirmedConfig = true;
        m_streamUrl = m_device.streamUrl(cameraOnly);
        QSettings settings;
        settings.setValue("camera/managedWidth", width);
        settings.setValue("camera/managedModel", model);
        if (!m_userStopped) {
            m_waitingForSelectedFrame = true;
            m_switchFrameTimeout.start();
        }
        // Publish the complete receipt while competing requests remain locked.
        // Stop remains available and can revoke reception from this callback.
        emit configurationChanged();
        m_controlPending = false;
        if (!m_userStopped) {
            updateState();
            if (!m_userStopped) beginConnection(false);
        } else {
            setProperty("rvMode", "stopped");
            m_stateText = tr("Stopped");
            m_detailText = tr("Model and preview selected; click Start");
            updateState();
        }
    });

    m_assist=new RvAssistDepth(this);
    connect(m_assist,&RvAssistDepth::inferenceUnavailable,this,&RvSessionController::hostInferenceUnavailable);
    const auto vgaModel=QString::fromLatin1(RvCameraView::UniDepthVgaNative16);
    const bool vgaSelected=savedScheme=="host_"+vgaModel&&RvAssistDepth::supportsHostModel(vgaModel);
    m_assist->setHostModel(vgaSelected?vgaModel:QString::fromLatin1(RvCameraView::UniDepthVgaNative16));
    m_assist->setExperiment(QStringLiteral("H"));
    m_assist->setInputRotationDegrees(effectiveHostInputRotation(m_device));
    m_assist->setEnabled(hostHdAssist());
    connect(m_assist,&RvAssistDepth::loadingStateChanged,this,[this](int state){
        if(m_destroying)return;m_assistLoadingState=state;emit assistStateChanged();
    });
    m_nativeDepthExpiry.setSingleShot(true);
    connect(&m_nativeDepthExpiry,&QTimer::timeout,this,[this]{if(!m_destroying)emit depthStreamCleared(tr("Native ToF depth expired"));});
    m_assistExpiry.setSingleShot(true);
    connect(&m_assistExpiry,&QTimer::timeout,this,[this]{
        if(m_destroying)return;
        m_assistInfo=tr("Host assisted depth expired; waiting for a new reliable frame");
        setProperty("rvAssistMetadata",QVariant());emit assistDepthStreamCleared(m_assistInfo);emit assistStateChanged();
    });
    connect(m_assist,&RvAssistDepth::statusChanged,this,[this](const QString &status){
        if(m_destroying)return;m_assistInfo=status;emit assistStateChanged();
    });
    connect(m_assist,&RvAssistDepth::cleared,this,[this](const QString &reason){
        m_nativeDepthExpiry.stop();m_assistExpiry.stop();
        m_lastAssistFrame=0;m_assistInfo=UiStyle::depthErrorText(reason);setProperty("rvAssistMetadata",QVariant());
        if(m_assist->failed()){
            if(m_imagingSwitchActive)cancelImagingSwitch(reason);
            else if(!m_destroying)emit imagingSwitchAborted(UiStyle::depthErrorText(reason));
        }
        if(!m_destroying){emit assistDepthStreamCleared(reason);emit assistStateChanged();}
    });
    connect(m_assist,&RvAssistDepth::resultReady,this,[this](const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,quint64 epoch,const RvTofFrame &tof,const QByteArray &diagnostic){
        if(m_destroying||!assistDepthEnabled()||m_userStopped||busy()||epoch!=m_presentationEpoch
            ||(m_config.cameraWidth!=576&&!hostHdAssist())||metadata["generation"]!=m_currentMetadata["generation"]
            ||(hostHdAssist()&&metadata["boot_id"]!=m_currentMetadata["boot_id"])
            ||metadata["assist_input_rotation_degrees"].toInt(-1)!=assistInputRotationDegrees()
            ||(RvAssistDepth::isUniDepth(assistHostModel())&&metadata.value("assist_experiment").toString("A")!=assistExperiment())
            ||metadata["frame_id"].toDouble()>m_currentMetadata["frame_id"].toDouble()
            ||metadata["frame_id"].toDouble()<=m_lastAssistFrame)return;
        if(m_imagingSwitchActive&&metadata["assist_host_model"]!=assistHostModel())return;
        m_lastAssistFrame=metadata["frame_id"].toDouble();
        m_recoveryDisplayTimer.stop();
        const auto fit=metadata["assist_fusion"].toObject();
        const auto &outputView=RvCameraView::profile(assistHostModel());
        const double outputPixels=outputView.outputWidth*outputView.outputHeight;
        m_assistInfo=tr("ToF anchors: %1 | scale %2 | inference %3 ms | age %4 ms")
            .arg(fit["participating_unique_native_points"].toInt(fit["anchors_inlier"].toInt())).arg(fit["scale"].toDouble(),0,'f',3)
            .arg(metadata["assist_inference_ms"].toDouble(),0,'f',1).arg(metadata["assist_frame_age_ms"].toDouble(),0,'f',0);
        if(fit["fusion_mode"]=="held_scale")
            m_assistInfo=tr("Using last dToF scale %1 | reference %2 s ago | inference %3 ms")
                .arg(fit["scale"].toDouble(),0,'f',3).arg(fit["scale_reference_age_ms"].toDouble()/1000,0,'f',1)
                .arg(metadata["assist_inference_ms"].toDouble(),0,'f',1);
        else if(fit["fusion_mode"]=="monocular")
            m_assistInfo=tr("Monocular estimate, not dToF corrected | inference %1 ms | age %2 ms")
                .arg(metadata["assist_inference_ms"].toDouble(),0,'f',1).arg(metadata["assist_frame_age_ms"].toDouble(),0,'f',0);
        else m_assistInfo+=QStringLiteral(" | ")+(fit["fusion_mode"]=="global_scale"?tr("Global scale"):tr("Scale + local correction"));
        if(assistExperiment()=="H"&&metadata["dtof_present"]==true)m_assistInfo+=tr(" | ToF %1: %2 | pair delta %3 ms")
            .arg(metadata["tof_sequence"].toInteger()).arg(tr(metadata["tof_reused"]==true?"reused":"new measurement"))
            .arg(metadata["pair_delta_ms"].toDouble(),0,'f',1);
        if(fit["recovery_applied"]==true)m_assistInfo+=tr(" | Local recovery: %1 anchors").arg(fit["recovery_anchors"].toInt());
        if(fit["recovery_applied"]==true&&!fit["global_fit_accepted"].toBool())m_assistInfo+=tr(" | Global fit unavailable; current local correction only");
        if((fit["fusion_mode"]=="held_scale"||fit["fusion_mode"]=="monocular")&&!fit["fallback_reason"].toString().isEmpty()){
            const auto reason=fit["fallback_reason"].toString().toUtf8();m_assistInfo+='\n'+QCoreApplication::translate("RvAssistFusion",reason.constData());
        }
        if(metadata["assist_output_policy"]==QLatin1String(RvAssistFusion::SupportPolicy)){
            if(fit["support_pixels"].toInt()>0)m_assistInfo+=tr(" | dToF-supported output %1%; outside support is unknown")
                .arg(100.*fit["support_pixels"].toDouble()/outputPixels,0,'f',1);
            else m_assistInfo=tr("No connected current dToF correction support; depth is unknown");
        }
        if(metadata["assist_output_policy"]==QLatin1String(RvAssistFusion::FieldOfViewPolicy))
            m_assistInfo+=tr(" | dToF geometric field %1%; continuous predicted depth inside")
                .arg(100.*fit["fov_pixels"].toDouble()/outputPixels,0,'f',1);
        if(fit["fusion_mode"]=="rgbd_completion")
            m_assistInfo=tr("%1 | ToF %2 x %3 | points %4 | inference %5 ms | age %6 ms")
                .arg(metadata["model"].toString()).arg(fit["native_columns"].toInt()).arg(fit["native_rows"].toInt())
                .arg(fit["projected_points"].toInt()).arg(metadata["assist_inference_ms"].toDouble(),0,'f',1).arg(metadata["assist_frame_age_ms"].toDouble(),0,'f',0);
        setProperty("rvAssistMetadata",metadata.toVariantMap());
        const double sourceAge=std::max(metadata["assist_frame_age_ms"].toDouble(),metadata["assist_measurement_age_ms"].toDouble());
        const int processingBudget=std::clamp(metadata["assist_processing_budget_ms"].toInt(1000),1000,120000);
        m_assistExpiry.start(std::max(1,processingBudget-int(std::ceil(sourceAge))));
        finishImagingSwitch();
        emit assistDepthSnapshotReady(depth,metadata,camera,tof,diagnostic);
        if(!m_destroying&&epoch==m_presentationEpoch)emit assistStateChanged();
    });
    resetQtPlayer();
    m_frameTimer.setInterval(15);
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_frameTimer,&QTimer::timeout,this,[this]{presentVlcFrame();});
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setObjectName(QStringLiteral("rvReconnectTimer"));
    m_frameTimer.setObjectName(QStringLiteral("rvFrameTimer"));
    m_switchFrameTimeout.setObjectName(QStringLiteral("rvSelectedFrameTimeout"));
    m_statusNetwork->setProxy(QNetworkProxy::NoProxy);
    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, &RvSessionController::pollManagedStatus);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &RvSessionController::retryConnection);
    m_recoveryDisplayTimer.setSingleShot(true);
    m_recoveryDisplayTimer.setInterval(15000);
    m_recoveryDisplayTimer.setObjectName(QStringLiteral("rvRecoveryDisplayTimer"));
    connect(&m_recoveryDisplayTimer, &QTimer::timeout, this, [this] {
        m_recoveryDisplayTimer.stop();
        if (!m_destroying) { emit recoveryDisplayExpired(); updateState(); }
    });
    m_stateText = tr("Stopped");
    m_detailText = tr("Camera is not started");
    setProperty("rvMode", "stopped");
}

void RvSessionController::resetQtPlayer()
{
    // Each connection owns its sink/player and a fixed presentation epoch.
    // Disconnect before teardown: delayed events from a retired decoder must
    // never publish a frame or reset the new connection's watchdog.
    if (m_videoSink) m_videoSink->disconnect(this);
    if (m_player) {
        m_player->disconnect(this);
        m_player->stop(); m_player->setSource(QUrl()); m_player->setVideoSink(nullptr);
        delete m_player;
    }
    delete m_videoSink;
    m_player = new QMediaPlayer(this);
    m_videoSink = new QVideoSink(this);
    m_player->setObjectName(QStringLiteral("rvQtPlayer"));
    m_videoSink->setObjectName(QStringLiteral("rvQtVideoSink"));
    m_player->setVideoSink(m_videoSink);
    const quint64 epoch = m_presentationEpoch;
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this, epoch](QMediaPlayer::MediaStatus status) {
        if (m_destroying || epoch != m_presentationEpoch || m_userStopped || m_controlPending
            || m_transitioning || m_reconnectTimer.isActive() || m_vlc || m_nv12) return;
        if (status == QMediaPlayer::LoadingMedia) setState(tr("Connecting"), tr("Opening RTSP stream..."));
        else if (status == QMediaPlayer::BufferingMedia || status == QMediaPlayer::StalledMedia)
            setState(tr("Buffering"), tr("Waiting for H.265 video data..."));
        else if (status == QMediaPlayer::InvalidMedia || status == QMediaPlayer::EndOfMedia)
            scheduleReconnect(tr("The stream is temporarily unavailable"));
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this, epoch](QMediaPlayer::PlaybackState state) {
        if (!m_destroying && epoch == m_presentationEpoch && !m_userStopped && !m_controlPending
            && !m_transitioning && !m_reconnectTimer.isActive()
            && !m_vlc && !m_nv12 && state == QMediaPlayer::PlayingState) {
            setState(tr("Connecting"), tr("Waiting for H.265 video data..."));
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this, epoch](QMediaPlayer::Error error, const QString &description) {
        if (m_destroying || epoch != m_presentationEpoch || m_userStopped || m_controlPending
            || m_transitioning || m_reconnectTimer.isActive()
            || m_vlc || m_nv12 || error == QMediaPlayer::NoError) return;
        const QString reason = description.isEmpty() ? tr("Media playback error") : description;
        if (error == QMediaPlayer::NetworkError || error == QMediaPlayer::ResourceError) scheduleReconnect(reason);
        else setState(tr("Error"), reason);
    });
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, [this, epoch](const QVideoFrame &frame) {
        if (!m_destroying && epoch == m_presentationEpoch) handleFrame(frame);
    });
}

RvSessionController::~RvSessionController()
{
    // QObject destroys children after member timers. Tear down every callback
    // source first so process/media/network completion cannot enter dead timers.
    m_destroying = true;
    delete m_tuner;m_tuner=nullptr;
    m_recoveryDisplayTimer.stop(); m_recoveryDisplayTimer.disconnect(this);
    m_nativeDepthExpiry.stop();m_assistExpiry.stop();m_assistExpiry.disconnect(this);
    if(m_assist){m_assist->disconnect(this);delete m_assist;m_assist=nullptr;}
    clearCurrentFrame(tr("Disconnected / No data"));
    m_reconnectTimer.stop(); m_frameTimer.stop(); m_controlTimeout.stop();
    m_switchFrameTimeout.stop(); m_statusTimer.stop();
    m_reconnectTimer.disconnect(this); m_frameTimer.disconnect(this);
    m_controlTimeout.disconnect(this); m_switchFrameTimeout.disconnect(this); m_statusTimer.disconnect(this);
    m_userStopped = true; m_controlPending = false; m_waitingForSelectedFrame = false;
    m_previewControl->disconnect(this);
    if (m_previewControl->state() != QProcess::NotRunning) {
        m_previewControl->kill(); m_previewControl->waitForFinished(2000);
    }
    m_player->disconnect(this); m_videoSink->disconnect(this);
    if (m_statusReply) m_statusReply->disconnect(this);
    // A session may die before its display widgets. Clear their snapshots now,
    // and retain QObject::destroyed observers so they can disable their controls.
    emit streamCleared();
    emit depthStreamCleared(tr("Camera is not started"));
    stopVlc();
    m_player->stop(); m_player->setSource(QUrl()); m_player->setVideoSink(nullptr);
    // Explicitly delete these children while all session members are alive.
    delete m_previewControl; m_previewControl = nullptr;
    delete m_videoSink; m_videoSink = nullptr;
    delete m_player; m_player = nullptr;
    delete m_statusNetwork; m_statusNetwork = nullptr;
}

bool RvSessionController::controlledSource() const
{
    return isControlledSource(QUrl(m_streamUrl.trimmed()),m_device.host);
}

bool RvSessionController::validConfiguration(const Configuration &candidate)
{
    return candidate.model == "Native"
        && (candidate.cameraWidth == 144 || candidate.cameraWidth == 576 || candidate.cameraWidth == 2304)
        && candidate.depthWidth == 144 && candidate.location == "none" && candidate.method == "none";
}

RvSessionController::Configuration RvSessionController::withCameraWidth(int width) const
{
    Configuration candidate = m_config;
    candidate.cameraWidth = width;
    if (width == 2304) { candidate.depthWidth = 144; candidate.location = "none"; candidate.method = "none"; }
    return candidate;
}

RvSessionController::Configuration RvSessionController::withDepthWidth(int width) const
{
    Configuration candidate = m_config;
    candidate.depthWidth = width;
    if (width == 144) { candidate.location = "none"; candidate.method = "none"; }
    else if (m_config.depthWidth == 144) { candidate.location = "host"; candidate.method = "bilinear_valid"; }
    return candidate;
}

bool RvSessionController::canSelectDepthWidth(int width) const
{
    if(m_dtofMode>=0)return false;
    return controlledSource() && !busy() && m_config.cameraWidth != 2304 && validConfiguration(withDepthWidth(width));
}

bool RvSessionController::canSelectDepthLocation(const QString &location) const
{
    Configuration candidate = m_config; candidate.location = location;
    return controlledSource() && !busy() && m_config.cameraWidth != 2304
        && m_config.depthWidth != 144 && validConfiguration(candidate);
}

bool RvSessionController::canSelectDepthMethod(const QString &method) const
{
    Configuration candidate = m_config; candidate.method = method;
    return controlledSource() && !busy() && m_config.cameraWidth != 2304
        && m_config.depthWidth != 144 && validConfiguration(candidate);
}

void RvSessionController::reloadDeviceProfiles()
{
    if(m_tunerExclusive){setState(tr("dToF Parameter Tuning"),tr("Stop dToF tuning before selecting another device."));return;}
    stop();
    try {
        RvDeviceStore store(m_deviceStorePath);m_device=store.active();m_deviceError.clear();
        m_streamUrl=m_device.streamUrl(m_config.cameraWidth==2304&&m_dtofMode<0);
        if(m_assist){m_assist->cancel(tr("Device changed"));m_assist->setInputRotationDegrees(effectiveHostInputRotation(m_device));}
        m_hasConfirmedConfig=false;m_confirmedBoot.clear();m_deviceRevision.clear();
        m_captureOwner={};
        setState(tr("Stopped"), m_device.id.isEmpty()
            ? tr("Connect a device using USB one-click connection in Device Manager.")
            : tr("Device selected; click Start when ready."));
    }catch(const std::exception &e){m_deviceError=QString::fromUtf8(e.what());setState(tr("Error"),m_deviceError);}
    emit deviceChanged();emit configurationChanged();
}

bool RvSessionController::assistDepthEnabled() const {return m_assist&&m_assist->enabled();}
QString RvSessionController::legacyHostInputRotationKey(const RvDeviceProfile &device) {
    return "depth/device/"+device.id+(device.cameraMountRotation==180?"/cameraMount180":"")+"/inputRotation";
}
int RvSessionController::effectiveHostInputRotation(const RvDeviceProfile &device) {
    // Keep the former depth-panel choice until this device is saved in Device Manager.
    if(device.inputRotation<0)return device.inputRotation;
    bool valid=false;
    const int legacy=QSettings().value(legacyHostInputRotationKey(device)).toInt(&valid);
    return valid&&(legacy==0||legacy==180)?legacy:device.inputRotation;
}
QImage RvSessionController::cameraInCalibrationFrame(const QImage &image) const {
    return m_device.calibration==QLatin1String(RvCalibration::Id)?image:RvCameraStream::correctCameraMounting(image,m_device.cameraMountRotation);
}
int RvSessionController::assistInputRotationDegrees() const {return m_assist?m_assist->inputRotationDegrees():180;}
QString RvSessionController::assistHostModel() const {return m_assist?m_assist->hostModel():QStringLiteral("unidepth");}
QString RvSessionController::hostInferencePreflightError() const
{
    return RvAssistDepth::preflightError(assistHostModel());
}
QString RvSessionController::assistExperiment() const {return m_assist?m_assist->experiment():QStringLiteral("A");}
void RvSessionController::setAssistExperiment(const QString &mode) {
    if(m_destroying||busy()||m_imagingSwitchActive||!hostHdAssist()||!m_assist||!RvAssistDepth::isUniDepth(assistHostModel())
        ||!RvAssistDepth::validExperiment(mode)||mode==assistExperiment())return;
    // Retire all pre-switch input, without restarting the camera/board or
    // changing its epoch, clock mapping, calibration or sensor parameters.
    for(const auto &raw:m_hdNativeFrames)
        m_hdScheduledTof=std::max(m_hdScheduledTof,quint64(raw.metadata["tof_sequence"].toInteger()));
    m_hdNativeFrames.clear();m_hdRgbFrames.clear();m_hdPairDrops={};
    if(!m_assist->setExperiment(mode))return;
    QSettings().setValue("depth/uniDepthExperiment",mode);
    emit assistStateChanged();
}
void RvSessionController::setAssistDepthEnabled(bool enabled)
{
    if(m_destroying||!m_assist)return;
    if(enabled&&((m_config.cameraWidth!=576&&m_config.cameraWidth!=2304)||busy()))return;
    m_assist->setEnabled(enabled);
    if(enabled)m_assistInfo=m_config.cameraWidth==2304?tr("Waiting for a managed H.265 frame for host inference"):
        tr("Waiting for a paired 576×324 camera and dToF frame");
    emit assistStateChanged();
}

bool RvSessionController::setDtofCandidate(int mode,const RvDtofCandidates::Settings& settings)
{
    if(m_imagingSwitchActive)return false;
    return applyDtofCandidate(mode,settings);
}

bool RvSessionController::applyDtofCandidate(int mode,const RvDtofCandidates::Settings& settings)
{
    if(m_destroying||!RvDtofCandidates::supports(mode))return false;
    QString options;
    try{if(mode>=0)options=nativeCaptureOptions(settings);}catch(...){return false;}
    if(mode==m_dtofMode&&(mode<0||options==m_dtofOptions))return true;
    if(busy()||!controlledSource())return false;
    if(RvDtofCandidates::isCaptureOnly(mode)&&!m_userStopped&&!prepareManagedVlc()){m_depthConfigError=tr("VLC 3 with H.265 support is required for the managed camera");emit configurationChanged();return false;}
    m_dtofMode=mode;m_dtofOptions=options;m_dtofSettings=settings;m_dtofReceipt={};
    if(mode>=0){m_config.cameraWidth=RvDtofCandidates::isCaptureOnly(mode)?2304:576;m_config.depthWidth=144;m_config.location="none";m_config.method="none";m_streamUrl=m_device.streamUrl();}
    emit dtofCandidateSelectionChanged();
    if(!m_userStopped)submitConfiguration(m_config,true,true);
    else{emit configurationChanged();}
    return true;
}

void RvSessionController::setDepthModel(const QString &model)
{
    if (m_destroying || busy() || m_imagingSwitchActive) return;
    Configuration candidate = m_config;
    if (candidate.model != model) {
        candidate.depthWidth = 144; candidate.location = "none"; candidate.method = "none";
    }
    candidate.model = model;
    if (!controlledSource() || !validConfiguration(candidate)) { rejectConfiguration(); return; }
    if(m_dtofMode>=0){m_dtofMode=-1;m_dtofReceipt={};emit dtofCandidateSelectionChanged();}
    const bool freshSameModel = model == m_config.model && !m_userStopped && (m_nv12 || m_managedRtsp)
        && m_lastFrameClock.isValid() && m_lastFrameClock.elapsed() <= 3000 && property("rvModel").toString() == model;
    if (freshSameModel) { emit configurationChanged(); return; }
    submitConfiguration(candidate, false, true);
}

void RvSessionController::selectImagingScheme(const QString& scheme,const RvDtofCandidates::Settings& settings)
{
    const int nativeMode=scheme=="board_native_tof48"?RvDtofCandidates::Native48:scheme=="board_native_tof48_high_accuracy"?RvDtofCandidates::Native48HighAccuracy:-1;
    if(nativeMode<0&&scheme!="host_unidepth_rgb672_vga_fp16_tof16")return;
    if(m_destroying||busy()||m_imagingSwitchActive||!controlledSource())return;
    if(nativeMode>=0){
        if(m_dtofMode==nativeMode&&!assistDepthEnabled())return;
        if(!m_userStopped)beginImagingSwitch(scheme,true);
        m_assist->setEnabled(false);
        m_dtofMode=nativeMode;m_dtofOptions=nativeCaptureOptions(settings);m_dtofSettings=settings;m_dtofReceipt={};
        m_config=withCameraWidth(2304);m_streamUrl=m_device.streamUrl();
        QSettings().setValue("depth/imagingScheme",scheme);emit dtofCandidateSelectionChanged();
        if(!m_userStopped)submitConfiguration(m_config,true,true);
        else{emit configurationChanged();updateState();}
        return;
    }
    if(scheme.startsWith("host_")) {
        const auto selected=scheme=="host_assisted"?QStringLiteral("unidepth"):scheme.mid(5);
        if(!RvAssistDepth::supportsHostModel(selected))return;
        if(!m_userStopped){
            const auto problem=RvAssistDepth::preflightError(selected);
            if(!problem.isEmpty()){emit hostInferenceUnavailable(problem,{});emit configurationChanged();return;}
        }
        if(!RvAssistDepth::hostModelAvailable(selected)){
            emit hostInferenceUnavailable(tr("The required host inference files are missing or invalid."),{});
            emit configurationChanged();return;
        }
        const auto currentGrid=nativeCaptureGrid(m_dtofReceipt);
        if(!m_userStopped&&hostHdAssist()&&!RvCameraView::supportsNativeGrid(selected,currentGrid.width(),currentGrid.height())){
            m_depthConfigError=tr("Selected model does not support the current %1×%2 native ToF grid. Stop capture and save compatible settings first.").arg(currentGrid.width()).arg(currentGrid.height());
            m_depthInfoText=m_depthConfigError;setState(tr("Error"),m_depthConfigError);emit configurationChanged();return;
        }
        const auto options=nativeCaptureOptions(settings);
        if(m_dtofMode==RvDtofCandidates::NativeHd&&m_config.cameraWidth==2304&&m_dtofOptions==options
            &&assistHostModel()==selected&&assistDepthEnabled()&&!m_assist->failed())return;
        const bool keepCapture=!m_userStopped&&hostHdAssist()&&assistDepthEnabled()&&m_dtofStream
            &&m_dtofOptions==options&&m_rtspStatusReady&&m_hdFrameClock.isValid()&&m_hdFrameClock.elapsed()<3000
            &&dtofBoardReceipt(m_dtofReceipt,m_dtofMode,m_dtofToken,m_dtofOptions);
        if(!m_userStopped){beginImagingSwitch(scheme,!keepCapture);if(!m_imagingSwitchActive||m_userStopped)return;}
        QSettings().setValue("depth/imagingScheme",scheme);
        if(keepCapture){
            // Keep the one board owner, LSG2 polling, clock mapping and RTSP
            // decoder. Only host inputs and the disposable inference process change.
            m_hdNativeFrames.clear();m_hdRgbFrames.clear();m_hdPairDrops={};
            m_currentMetadata={};
            m_assist->setHostModel(selected);
            // An explicit re-selection may retry a failed host process without
            // resetting the sensor. Ordinary incoming frames never clear a failure.
            if(m_assist->failed()){m_assist->setEnabled(false);m_assist->setEnabled(true);}
            QSettings().setValue("depth/hostModel",selected);
            emit dtofCandidateSelectionChanged();emit configurationChanged();updateState();
            return;
        }
        m_assist->setHostModel(selected);QSettings().setValue("depth/hostModel",selected);
        if(m_assist->failed()){m_assist->setEnabled(false);m_assist->setEnabled(true);}
        m_dtofMode=RvDtofCandidates::NativeHd;m_dtofOptions=options;m_dtofSettings=settings;m_dtofReceipt={};
        m_config=withCameraWidth(2304);m_streamUrl=m_device.streamUrl();
        setAssistDepthEnabled(true);emit dtofCandidateSelectionChanged();
        // Selecting while stopped is inert; an active selection uses the shared
        // coordinator to renew native capture and the camera stream.
        if(!m_userStopped)submitConfiguration(m_config,true,true);
        else{emit configurationChanged();updateState();}
        return;
    }

}

QString RvSessionController::currentImagingScheme() const
{
    if(hostHdAssist()&&assistDepthEnabled())return assistHostModel()=="unidepth"?QStringLiteral("host_assisted"):"host_"+assistHostModel();
    if(m_dtofMode>=0)return RvDtofCandidates::name(m_dtofMode);
    return {};
}

void RvSessionController::beginImagingSwitch(const QString &scheme,bool boardRestart)
{
    if(m_destroying||m_userStopped)return;
    m_imagingSwitchScheme=scheme;m_imagingSwitchBoardRestart=boardRestart;
    // Retained Small timings include cold file caches, plus capture handover.
    const int hostMs=5500;
    const int fallback=scheme.startsWith("board_")?12000:hostMs+(boardRestart?9500:0);
    const QString key="depth/imagingSwitchMs/"+m_device.id+"/"+(boardRestart?"capture/":"host/")+scheme;
    m_imagingSwitchEstimateMs=std::clamp(QSettings().value(key,fallback).toInt(),1000,60000);
    m_imagingSwitchActive=true;m_imagingSwitchClock.restart();
    emit imagingSwitchStarted(scheme,m_imagingSwitchEstimateMs);
}

void RvSessionController::finishImagingSwitch()
{
    if(!m_imagingSwitchActive||m_userStopped||currentImagingScheme()!=m_imagingSwitchScheme)return;
    const auto elapsed=m_imagingSwitchClock.elapsed();const auto scheme=m_imagingSwitchScheme;
    m_imagingSwitchActive=false;
    m_lastImagingSwitch={{"scheme",scheme},{"completed",true},{"elapsed_ms",double(elapsed)},
        {"board_restart",m_imagingSwitchBoardRestart},{"capture_generation",m_dtofToken},{"boot_id",m_dtofReceipt["boot_id"]}};
    if(elapsed>=100&&elapsed<=60000){
        const QString key="depth/imagingSwitchMs/"+m_device.id+"/"+(m_imagingSwitchBoardRestart?"capture/":"host/")+scheme;
        QSettings settings;const int prior=settings.value(key,int(elapsed)).toInt();
        settings.setValue(key,std::clamp(int(.65*prior+.35*elapsed),1000,60000));
    }
    emit imagingSwitchFinished(scheme,elapsed);
}

void RvSessionController::cancelImagingSwitch(const QString &reason)
{
    if(!m_imagingSwitchActive)return;
    m_imagingSwitchActive=false;
    m_lastImagingSwitch={{"scheme",m_imagingSwitchScheme},{"completed",false},
        {"elapsed_ms",double(m_imagingSwitchClock.elapsed())},{"reason",reason}};
    if(!m_destroying)emit imagingSwitchAborted(UiStyle::depthErrorText(reason));
}

QJsonObject RvSessionController::imagingSwitchDiagnostics() const
{
    return {{"active",m_imagingSwitchActive},{"scheme",m_imagingSwitchScheme},
        {"estimated_ms",m_imagingSwitchEstimateMs},{"elapsed_ms",m_imagingSwitchClock.isValid()?double(m_imagingSwitchClock.elapsed()):0.},
        {"board_restart",m_imagingSwitchBoardRestart},{"last",m_lastImagingSwitch}};
}

void RvSessionController::setDepthWidth(int width)
{
    if (m_destroying || busy() || m_imagingSwitchActive) return;
    if (!canSelectDepthWidth(width)) { rejectConfiguration(); return; }
    submitConfiguration(withDepthWidth(width), false, false);
}

void RvSessionController::setDepthLocation(const QString &location)
{
    if (m_destroying || busy() || m_imagingSwitchActive) return;
    if (!canSelectDepthLocation(location)) { rejectConfiguration(); return; }
    Configuration candidate = m_config; candidate.location = location;
    submitConfiguration(candidate, false, false);
}

void RvSessionController::setDepthMethod(const QString &method)
{
    if (m_destroying || busy() || m_imagingSwitchActive) return;
    if (!canSelectDepthMethod(method)) { rejectConfiguration(); return; }
    Configuration candidate = m_config; candidate.method = method;
    submitConfiguration(candidate, false, false);
}

void RvSessionController::rejectConfiguration()
{
    // A rejected request does not stop or relabel the running stream.
    m_depthConfigError = tr("Select host-assisted inference for depth inference.");
    m_depthInfoText = m_depthConfigError;
    setState(tr("Error"), m_depthConfigError);
    emit configurationChanged();
}

void RvSessionController::setDepthConfiguration(int width, const QString &location, const QString &method)
{
    if (m_destroying || busy() || m_imagingSwitchActive) return;
    Configuration candidate = m_config;
    candidate.depthWidth = width;
    candidate.location = location;
    candidate.method = method;
    if (!controlledSource() || m_config.cameraWidth == 2304 || !validConfiguration(candidate)) {
        rejectConfiguration();
        return;
    }
    submitConfiguration(candidate, false, false);
}

void RvSessionController::submitConfiguration(const Configuration &candidate, bool previewChange, bool modelChange,
                                            bool startAfter, bool reconnecting, bool automatic)
{
    if (busy() || m_previewControl->state() != QProcess::NotRunning) return;
    if (!validConfiguration(candidate)) { rejectConfiguration(); return; }
    const QUrl address(m_streamUrl.trimmed());
    if (!isControlledSource(address,m_device.host)) { rejectConfiguration(); return; }
    const auto worker = RuntimeDiscovery::rvControlForLaunch();
    if (!worker.ok()) { failPreviewControl(worker.error); return; }
    const QString root = QFileInfo(worker.executable).absolutePath();
    if (((m_dtofMode<0&&candidate.cameraWidth==2304)||RvDtofCandidates::isCaptureOnly(m_dtofMode))&&!prepareManagedVlc()) {
        failPreviewControl(tr("VLC 3 with H.265 support is required for the managed camera")); return;
    }
    m_transitioning = true;
    const auto transition = qScopeGuard([this] { m_transitioning = false; updateState(); });
    m_config = candidate;
    if(candidate.cameraWidth!=576&&candidate.cameraWidth!=2304)setAssistDepthEnabled(false);
    m_depthConfigError.clear();
    if (startAfter) m_userStopped = false;
    if (!reconnecting) { m_reconnectAttempt = 0; m_nextReconnectDelayMs = 1000; m_lastReconnectReason.clear(); m_recoveryDisplayTimer.stop(); }
    m_controlPending = true; m_waitingForSelectedFrame = false; m_controlTimedOut = false;
    const bool expectsReception=startAfter||reconnecting||!m_userStopped;
    const auto receptionAuthorized = [this, expectsReception] {
        if(m_controlPending&&(!expectsReception||!m_userStopped))return true;
        // Stop arrived in a synchronous notification before a retry command
        // was launched. Revoke this local transaction without touching board.
        m_controlPending = false;
        m_controlTimeout.stop(); m_switchFrameTimeout.stop();
        m_waitingForSelectedFrame = false;
        m_expectedDepthMetadata = {}; m_expectedCameraStatus = {};
        setProperty("rvMode", "stopped");
        setState(tr("Stopped"), tr("Camera is not started"));
        return false;
    };
    if(startAfter&&!reconnecting&&assistDepthEnabled()&&m_assist->failed()){
        m_assist->setEnabled(false);m_assist->setEnabled(true);
    }
    if(startAfter&&!m_imagingSwitchActive&&hostHdAssist()&&assistDepthEnabled())
        beginImagingSwitch(currentImagingScheme(),true);
    if(!receptionAuthorized())return;
    // Revoke old callbacks immediately, but keep the old LSG2 receiver polling
    // until the coordinator has stopped its owner. Disconnecting first races
    // the board's normal session cleanup against the next candidate command.
    m_reconnectTimer.stop(); stopVlc(true); m_player->stop(); m_player->setSource(QUrl());
    if (!receptionAuthorized()) return;
    m_expectedDepthMetadata = QJsonObject(); m_expectedCameraStatus = QJsonObject(); m_switchFrameTimeout.stop();
    m_requestedModel = modelChange ? candidate.model : QString();
    m_requestedPreviewWidth = previewChange ? candidate.cameraWidth : 0;
    m_requestedDepthWidth = candidate.depthWidth; m_requestedLocation = candidate.location; m_requestedMethod = candidate.method;
    emit configurationChanged();
    if (!receptionAuthorized()) return;
    updateState();
    if (!receptionAuthorized()) return;
    emit streamCleared();
    if (!receptionAuthorized()) return;
    setProperty("rvMode", "switching"); emit depthStreamCleared(tr("Switching work mode..."));
    if (!receptionAuthorized()) return;
    setState(tr("Connecting"), reconnecting ? tr("Automatically restoring camera and depth...") : tr("Switching work mode..."));
    if (!receptionAuthorized()) return;
    QStringList args = {QString::number(candidate.cameraWidth), "--host", address.host(),
        "--depth-width", QString::number(candidate.depthWidth), "--location", candidate.location, "--method", candidate.method};
    if (!previewChange) args.append("--keep-preview");
    if (modelChange) { args.append("--model"); args.append(candidate.model); }
    if(m_dtofMode>=0){
        m_dtofToken=QUuid::createUuid().toString(QUuid::Id128);m_dtofReceipt={};
        args={"--dtof",QString::number(m_dtofMode),m_dtofToken,m_dtofOptions};
        if(hostHdAssist())args<<"--host-model"<<assistHostModel();
    }
    // Explicit Start may recover any fault after ownership checks. Background
    // recovery must pass the board's structural-fault classifier and rate cap.
    if (automatic) args.append("--automatic-start");
    else if (startAfter && !reconnecting) args.append("--recover-fault");
    else if (reconnecting) args.append("--recover-stream-fault");
    try {
        RvDeviceStore store(m_deviceStorePath);const auto selected=store.byId(m_device.id);
        if(!m_deviceError.isEmpty() || !selected.ready())throw std::runtime_error("Select a configured trusted device in Settings > Device Manager");
        m_device=selected;m_deviceRevision=store.revision();
        args<<"--device-store"<<m_deviceStorePath<<"--device-revision"<<m_deviceRevision<<"--device-id"<<m_device.id;
    }catch(const std::exception &e){failPreviewControl(QString::fromUtf8(e.what()));return;}
    m_previewControl->setProcessEnvironment(RuntimeDiscovery::nativeWorkerEnvironment(worker.executable));
    m_previewControl->setWorkingDirectory(root);
    m_previewControl->setProgram(worker.executable); m_previewControl->setArguments(args);
    if (!receptionAuthorized()) return;
    m_previewControl->start(); m_controlTimeout.start();
}

void RvSessionController::setConfiguration(int cameraWidth, const QString &model, int depthWidth,
                                         const QString &location, const QString &method)
{
    if (m_destroying || busy() || m_imagingSwitchActive) return;
    Configuration candidate;
    candidate.cameraWidth = cameraWidth;
    candidate.model = model;
    candidate.depthWidth = depthWidth;
    candidate.location = location;
    candidate.method = method;
    if (!controlledSource() || !validConfiguration(candidate)) { rejectConfiguration(); return; }
    submitConfiguration(candidate, true, true);
}

void RvSessionController::syncConfiguration(const QJsonObject &metadata)
{
    Configuration config;
    config.model = metadata["model"].toString();
    config.cameraWidth = metadata["rgb_width"].toInt();
    config.depthWidth = metadata["target_width"].toInt(144);
    config.location = metadata["requested_location"].toString("none");
    config.method = metadata["requested_method"].toString("none");
    if (!validConfiguration(config)) return;
    const bool changed = config.model != m_config.model || config.cameraWidth != m_config.cameraWidth
        || config.depthWidth != m_config.depthWidth || config.location != m_config.location || config.method != m_config.method;
    m_config = config; m_confirmedConfig = config; m_hasConfirmedConfig = true;
    if(config.cameraWidth!=576&&config.cameraWidth!=2304)setAssistDepthEnabled(false);
    if (changed) emit configurationChanged();
    updateState();
}

void RvSessionController::updateState()
{
    setProperty("rvControlPending", busy());
    setProperty("rvReconnectPending", m_reconnectTimer.isActive());
    setProperty("rvReconnectAttempt", m_reconnectAttempt);
    setProperty("rvReconnectDelayMs", m_reconnectTimer.isActive() ? m_reconnectTimer.interval() : 0);
    if (!m_destroying) emit stateChanged();
}

void RvSessionController::failPreviewControl(const QString &reason, bool retry)
{
    if (m_destroying) return;
    cancelImagingSwitch(reason);
    if (retry && !m_userStopped) {
        m_controlPending = false;
        scheduleReconnect(reason);
        return;
    }
    const bool previousTransition = m_transitioning;
    m_transitioning = true;
    const auto transition = qScopeGuard([this, previousTransition] {
        m_transitioning = previousTransition; updateState();
    });
    m_controlTimeout.stop(); m_switchFrameTimeout.stop();
    m_controlPending = false; m_waitingForSelectedFrame = false;
    m_expectedDepthMetadata = QJsonObject(); m_expectedCameraStatus = QJsonObject(); m_userStopped = true;
    m_recoveryDisplayTimer.stop();
    m_reconnectTimer.stop(); stopVlc(); m_player->stop(); m_player->setSource(QUrl());
    if (m_hasConfirmedConfig) m_config = m_confirmedConfig;
    emit configurationChanged(); updateState(); setProperty("rvMode", "error");
    setState(tr("Error"), reason); emit depthStreamCleared(reason);
}

void RvSessionController::setState(const QString &state, const QString &detail)
{
    const auto displayDetail = UiStyle::depthErrorText(detail);
    if (m_stateText == state && m_detailText == displayDetail) return;
    m_stateText = state; m_detailText = displayDetail;
    if (!m_destroying) emit stateChanged();
}

void RvSessionController::retranslateState()
{
    if (m_destroying) return;
    if (m_userStopped && !busy()) {
        setState(tr("Stopped"), tr("Camera is not started"));
    }
}

void RvSessionController::start()
{
    startImaging(false);
}

bool RvSessionController::automaticStartAllowed(const QString &deviceId) const
{
    return qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1" && !m_destroying
        && m_userStopped && !busy() && !m_tunerExclusive && !deviceId.isEmpty()
        && m_autoStartAttemptedDevice!=deviceId;
}

void RvSessionController::startAutomatically(bool verifiedUsbAttachment)
{
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1" || !m_userStopped || busy()
        || m_dtofMode < 0 || !controlledSource() || !m_device.ready()
        || (!verifiedUsbAttachment && m_autoStartAttemptedDevice == m_device.id)) return;
    startImaging(true);
}

void RvSessionController::deviceUnreachable(const QString &deviceId)
{
    if (m_autoStartAttemptedDevice == deviceId) m_autoStartAttemptedDevice.clear();
}

void RvSessionController::startImaging(bool automatic)
{
    if (m_destroying) return;
    if (m_device.id.isEmpty() && m_deviceError.isEmpty()) {
        if (!automatic) setState(tr("Stopped"),tr("Connect a device using USB one-click connection in Device Manager."));
        return;
    }
    if(m_tunerExclusive){setState(tr("dToF Parameter Tuning"),tr("RV1103 is owned by dToF tuning. Stop it before starting shared imaging."));return;}
    if(!busy()&&!m_imagingSwitchActive&&hostHdAssist()&&assistDepthEnabled()){
        const auto problem=hostInferencePreflightError();
        if(!problem.isEmpty()){
            if(automatic)m_autoStartAttemptedDevice=m_device.id;
            emit hostInferenceUnavailable(problem,{});
            return;
        }
    }
    // A saved video URL cannot bypass a missing or invalid device identity.
    if (!m_deviceError.isEmpty()) {
        failPreviewControl(m_deviceError + " " + tr("Open Settings > Device Manager to review this device."));
        return;
    }
    if(busy()||m_imagingSwitchActive)return;
    const QUrl source(m_streamUrl.trimmed());
    if (!source.isValid() || source.host().isEmpty() || (source.scheme()!=QStringLiteral("rtsp") && source.scheme()!=QStringLiteral("nv12"))) {
        setState(tr("Error"), tr("Enter a valid nv12:// or rtsp:// address"));
        return;
    }
    // Start re-confirms the selected board mode, including an app restart whose
    // saved address is RTSP while the board has rebooted into default fusion.
    if(isControlledSource(source,m_device.host)){
        m_autoStartAttemptedDevice = m_device.id;
        submitConfiguration(m_config,true,true,true,false,automatic);
        return;
    }
    m_expectedDepthMetadata=QJsonObject();m_expectedCameraStatus=QJsonObject();
    m_waitingForSelectedFrame=false;m_switchFrameTimeout.stop();
    m_userStopped = false;
    if(!m_expectedDepthMetadata.isEmpty()) {m_waitingForSelectedFrame=true;m_switchFrameTimeout.start();}
    beginConnection(true);
}

void RvSessionController::stop()
{
    ++m_stopRevision;
    if (m_destroying) return;
    if (m_userStopped && m_transitioning) return;
    const bool previousTransition = m_transitioning;
    m_transitioning = true;
    const auto transition = qScopeGuard([this, previousTransition] {
        m_transitioning = previousTransition; updateState();
    });
    m_autoStartAttemptedDevice = m_device.id;
    m_userStopped = true;
    cancelImagingSwitch(tr("Camera is not started"));
    m_recoveryDisplayTimer.stop();
    // Revoke before kill: a queued completion cannot accept a late receipt or
    // revive reception after Stop. Already submitted board configure is not undone.
    m_controlPending = false; m_controlTimeout.stop();
    if (m_previewControl->state() != QProcess::NotRunning) m_previewControl->kill();
    m_switchFrameTimeout.stop();m_waitingForSelectedFrame=false;m_expectedDepthMetadata=QJsonObject();
    m_expectedCameraStatus=QJsonObject();
    emit depthStreamCleared(tr("Camera is not started"));
    m_reconnectTimer.stop();
    stopVlc();
    m_player->stop();
    m_player->setSource(QUrl());
    updateState();
    setProperty("rvMode",m_controlPending?"switching":"stopped");
    setState(tr("Stopped"), tr("Camera is not started"));
    emit stateChanged();
}

void RvSessionController::reconnectNow()
{
    start();
}

void RvSessionController::retryConnection()
{
    if (m_destroying || m_userStopped || busy()) return;
    // Re-confirm every managed reconnect. The coordinator permits only a
    // classified stream fault to reset, after clean exit and within its budget;
    // a new boot/generation cannot reuse an old receipt or frame counter.
    if (controlledSource()) submitConfiguration(m_config, true, true, false, true);
    else beginConnection(false);
}

void RvSessionController::beginConnection(bool resetAttempts)
{
    if (m_destroying || m_tunerExclusive || m_userStopped || m_controlPending || m_transitioning) return;
    const auto sourceHost=m_dtofMode>=0?m_device.host:QUrl(m_streamUrl).host();
    if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"&&sourceHost!="localhost"&&!QHostAddress(sourceHost).isLoopback()){
        failPreviewControl(QStringLiteral("Offline test forbids external camera connections"));return;
    }
    m_transitioning = true;
    const auto transition = qScopeGuard([this] { m_transitioning = false; updateState(); });
    m_reconnectTimer.stop();
    stopVlc();
    const quint64 epoch = m_presentationEpoch;
    const auto stillConnecting = [this, epoch] {
        return !m_userStopped && !m_controlPending && epoch == m_presentationEpoch;
    };
    if (!stillConnecting()) return;
    if(m_dtofMode>=0){
        if(!dtofBoardReceipt(m_dtofReceipt,m_dtofMode,m_dtofToken,m_dtofOptions)){failPreviewControl(tr("Candidate confirmation is missing"));return;}
        auto* stream=new RvDtofBoardStream(m_dtofReceipt,m_dtofSettings,this,m_device.host);m_dtofStream=stream;
        if(RvDtofCandidates::isCaptureOnly(m_dtofMode)){
            m_rtspStatusReady=false;m_lastEncodedFrame=m_lastRetrievedFrame=0;m_hdVideoSeen=false;m_hdVideoReconnects=0;setProperty("rvVideoReconnectCount",0);
            m_encoderProgressClock.restart();m_hdFrameClock.restart();
            stream->videoProgress=[this,stream](const QJsonObject& meta){
                if(m_dtofStream!=stream||m_userStopped||m_controlPending||(!m_vlc&&!m_hdVideo))return;
                if(hostHdAssist())observeHostHdClock(meta);
                const auto encoded=meta["video_encoded_sequence"].toString().toULongLong(),retrieved=meta["video_retrieved_sequence"].toString().toULongLong();
                if(encoded>quint64(m_lastEncodedFrame)&&retrieved>quint64(m_lastRetrievedFrame)){
                    m_lastEncodedFrame=qint64(encoded);m_lastRetrievedFrame=qint64(retrieved);m_encoderProgressClock.restart();
                    if(!m_rtspStatusReady){if(m_hdVideo)m_rtspDecodeFloor=m_hdVideo->snapshot().sequence;else{std::lock_guard<std::mutex> lock(m_vlc->mutex);m_rtspDecodeFloor=m_vlc->sequence;}m_rtspStatusReady=true;}
                }
            };
        }
        stream->rawFrame=[this,stream](const RvTofFrame& received){
            if(m_dtofStream!=stream||m_userStopped||m_controlPending)return;
            auto raw=received;RvCaptureCalibration::annotate(raw.metadata,m_dtofReceipt);
            if(hostHdAssist()){
                if(m_assist->tofDriven()&&!m_hdNativeFrames.empty()&&quint64(m_hdNativeFrames.back().metadata["tof_sequence"].toInteger())>m_hdScheduledTof)
                    m_hdPairDrops["unpaired_superseded"]=m_hdPairDrops["unpaired_superseded"].toInt()+1;
                if(m_hdNativeFrames.size()==16)m_hdNativeFrames.pop_front();m_hdNativeFrames.push_back(raw);
                if(m_assist->tofDriven())dispatchHostHdMeasurement();
            }
            setProperty("rvRawTofSequence",raw.metadata["tof_sequence"].toVariant());emit tofFrameReady(raw);
            if(RvDtofCandidates::isNativeDepth(m_dtofMode)){
                const auto bytes=raw.radialMetres();if(bytes.isEmpty())return;
                auto metadata=raw.metadata;
                metadata["source_generation"]=metadata["generation"];metadata["generation"]=double(m_presentationEpoch);
                metadata["model"]=RvDtofCandidates::name(m_dtofMode);metadata["width"]=48;metadata["height"]=32;
                metadata["depth_unit"]="sensor_radial_m_float32";metadata["depth_location"]="native_sensor";
                metadata["sensor_mode"]=RvCaptureContract::sensorMode(m_dtofMode);
                metadata["native_order"]=true;metadata["interpolation"]=false;metadata["calibration_release"]=RvCalibration::Release;
                m_nativeDepthExpiry.start(std::max(1,1000-int(std::ceil(metadata["raw_age_ms"].toDouble()))));
                if(m_waitingForSelectedFrame){m_waitingForSelectedFrame=false;m_switchFrameTimeout.stop();updateState();}
                finishImagingSwitch();emit depthFrameReady(bytes,metadata);emit depthSnapshotReady(bytes,metadata,QImage());
            }
        };
        stream->error=[this,stream,epoch](const QString& reason){
            if(m_dtofStream!=stream||m_userStopped)return;
            const QPointer<RvDtofBoardStream> alive(stream);
            QTimer::singleShot(0,this,[this,alive,epoch,reason]{
                if(alive&&m_dtofStream==alive&&epoch==m_presentationEpoch&&!m_userStopped)failPreviewControl(reason,true);
            });
        };
        m_connectionClock.restart();m_framesInWindow=0;m_fpsClock.restart();
        if(RvDtofCandidates::isCaptureOnly(m_dtofMode)){
            m_hdVideo=new RvHdVideoStream(m_device.streamUrl(true));m_hdVideoSequence=0;m_hdOutputSequence=0;
            m_hdVideo->start();m_frameTimer.start();
        }
        setState(tr("Connecting"),tr("Waiting for RV1103 candidate output"));return;
    }
    if (resetAttempts) { m_reconnectAttempt = 0; m_nextReconnectDelayMs = 1000; }
    m_framesInWindow = 0;
    m_fpsClock.restart();
    m_connectionClock.restart();
    m_lastFrameClock.restart();
    m_player->stop();
    m_player->setSource(QUrl());

    setProperty("rvMode", m_waitingForSelectedFrame ? "switching" : "connecting");
    updateState();
    if (!stillConnecting()) return;
    const QUrl streamUrl(m_streamUrl.trimmed());
    if(isManagedRtsp(streamUrl,m_device.host)) {
        if(m_expectedCameraStatus.isEmpty()){failPreviewControl(tr("Managed camera confirmation is missing; click Start"));return;}
        m_managedRtsp=true;m_rtspStatusReady=false;
        m_lastEncodedFrame=m_expectedCameraStatus["telemetry"].toObject()["frame_id"].toInteger();
        m_lastRetrievedFrame=m_expectedCameraStatus["telemetry"].toObject()["encoder_retrieved_frame_id"].toInteger();
        m_encoderProgressClock.restart();
        m_waitingForSelectedFrame=true;m_switchFrameTimeout.start();updateState();
        if (!stillConnecting()) return;
    }
    if (streamUrl.scheme()==QStringLiteral("nv12")) {
        m_nv12 = new RvCameraStream;
        m_nv12->host = streamUrl.host();
        m_nv12->port = quint16(streamUrl.port(8831));
        m_nv12Sequence = 0;
        m_nv12->start(); m_frameTimer.start();
        setState(tr("Connecting"), tr("NV12: host color conversion and rectification"));
        return;
    }
    if (startVlc(QUrl(m_streamUrl.trimmed()))) {
        setState(tr("Connecting"), QStringLiteral("VLC / TCP / 100 ms"));
        if(m_managedRtsp){m_statusTimer.start();pollManagedStatus();}
        return;
    }
    if(m_managedRtsp){failPreviewControl(tr("VLC 3 with H.265 support is required for the managed camera"));return;}

    QPlaybackOptions options;
    options.setNetworkTimeout(std::chrono::milliseconds(5000));
    options.setProbeSize(128 * 1024);
    options.setPlaybackIntent(QPlaybackOptions::PlaybackIntent::LowLatencyStreaming);
    resetQtPlayer();
    m_player->setPlaybackOptions(options);
    // Qt can remain stalled without emitting a network error. The same timer
    // that polls VLC also bounds Qt's wait for an initial/subsequent real frame.
    m_frameTimer.start();
    m_player->setSource(QUrl(m_streamUrl.trimmed()));
    m_player->play();
    setState(tr("Connecting"), tr("Opening RTSP stream..."));
}

void RvSessionController::scheduleReconnect(const QString &reason)
{
    if (m_destroying || m_tunerExclusive || m_userStopped || m_controlPending || m_reconnectTimer.isActive())
        return;
    cancelImagingSwitch(reason);
    if(m_userStopped)return;
    m_lastReconnectReason=reason;
    const bool previousTransition = m_transitioning;
    m_transitioning = true;
    const auto transition = qScopeGuard([this, previousTransition] {
        m_transitioning = previousTransition; updateState();
    });
    if (m_reconnectAttempt < std::numeric_limits<int>::max()) ++m_reconnectAttempt;
    const int delayMs = m_nextReconnectDelayMs;
    m_nextReconnectDelayMs = std::min(delayMs * 2, 5000);
    m_controlTimeout.stop(); m_switchFrameTimeout.stop();
    m_waitingForSelectedFrame = false;
    m_expectedDepthMetadata = {}; m_expectedCameraStatus = {};
    // Keep only the last displayed pixels during this bounded recovery window.
    // All source metadata, metric arrays and worker epochs are still revoked.
    if (!m_recoveryDisplayTimer.isActive() && (!m_currentMetadata.isEmpty() || property("rvSourceWidth").isValid()))
        m_recoveryDisplayTimer.start();
    stopVlc();
    const quint64 epoch = m_presentationEpoch;
    if (m_userStopped || m_controlPending) return;
    m_player->stop();
    m_player->setSource(QUrl());
    setProperty("rvMode", "reconnecting");
    setState(tr("Reconnecting"),
             tr("%1; reconnect %2 in %3 ms").arg(reason).arg(m_reconnectAttempt).arg(delayMs));
    if (m_userStopped || m_controlPending || epoch != m_presentationEpoch) return;
    m_reconnectTimer.start(delayMs);
}

void RvSessionController::handleFrame(const QVideoFrame &frame)
{
    if(m_vlc&&m_vlc->output=="qt"){
        if(m_userStopped||m_controlPending||m_transitioning||m_reconnectTimer.isActive()
                ||!frame.isValid())return;
        const qint64 started=RtspVlcBackend::nowNs();
        const QImage image=frame.toImage();
        if(image.isNull())return;
        const qint64 completed=RtspVlcBackend::nowNs();
        std::lock_guard<std::mutex> lock(m_vlc->mutex);
        m_vlc->latest=image;m_vlc->completedNs=completed;m_vlc->outputMs=(completed-started)/1e6;
        m_vlc->copyMs=0;m_vlc->latestPtsUs=frame.startTime();
        if(!m_vlc->sequence){
            m_vlc->firstOutputNs=completed;m_vlc->firstPtsUs=frame.startTime();
            if(qEnvironmentVariable("RV_VIDEO_LATENCY_TEST")=="1")
                qInfo().noquote()<<"RV_QT_FIRST_OUTPUT qpc_ns="<<completed
                    <<" pts_us="<<frame.startTime()<<" open_to_output_ms="<<(completed-m_vlc->openNs)/1e6
                    <<" probe_bytes="<<m_vlc->probeBytes;
        }
        ++m_vlc->sequence;
        return; // the managed presenter applies all source checks.
    }
    if (m_userStopped || m_controlPending || m_transitioning || m_reconnectTimer.isActive() || m_vlc || m_nv12)
        return;
    const quint64 epoch = m_presentationEpoch;
    // Qt emits an empty frame when its video output is cleared.
    if (!frame.isValid() || !frame.size().isValid() || frame.size().isEmpty()) {
        clearCurrentFrame(tr("Disconnected / No data"));
        return;
    }
    const QImage image = cameraInCalibrationFrame(frame.toImage());
    if (image.isNull()) {
        clearCurrentFrame(tr("Disconnected / No data"));
        return;
    }
    m_recoveryDisplayTimer.stop();
    emit cameraFrameReady(image);
    if (m_userStopped || m_controlPending || epoch != m_presentationEpoch) return;
    m_lastFrameClock.restart();
    m_nextReconnectDelayMs = 1000;
    setState(tr("Playing"), tr("Low-latency RTSP video"));
    if (m_userStopped || m_controlPending || epoch != m_presentationEpoch) return;
    setProperty("rvSourceWidth", image.width());setProperty("rvSourceHeight", image.height());
    setProperty("rvPresentedFrames", property("rvPresentedFrames").toULongLong() + 1);
    ++m_framesInWindow;
    const qint64 elapsed = m_fpsClock.elapsed();
    if (elapsed >= 1000) {
        m_videoFps = m_framesInWindow * 1000.0 / elapsed;
        m_framesInWindow = 0;
        m_fpsClock.restart();
        emit stateChanged();
    }
}

bool RvSessionController::startVlc(const QUrl &source)
{
    // HD uses Qt/FFmpeg low-latency playback. The prior VLC path remains
    // available via the process-local RV_RTSP_OUTPUT=vmem override.
    // Both receivers feed the same bounded mailbox; only the existing generation,
    // VENC progress gate and freshness watchdog authorize presentation.
    if((RvDtofCandidates::isCaptureOnly(m_dtofMode)||hostHdAssist())&&qEnvironmentVariable("RV_RTSP_OUTPUT")!="vmem"){
        auto backend=std::make_unique<RtspVlcBackend>();
        backend->managedCamera=true;backend->output="qt";backend->cacheMs=-1;backend->threads=-1;
        backend->transport="qt_ffmpeg_default";
        resetQtPlayer();
        QPlaybackOptions options;
        options.setNetworkTimeout(std::chrono::milliseconds(5000));
        bool probeOk=false;
        const int probe=qEnvironmentVariableIntValue("RV_RTSP_QT_PROBE_BYTES",&probeOk);
        if(probeOk&&probe>=32&&probe<=1048576)backend->probeBytes=probe;
        // This fixed managed stream already advertises H.265 parameters in SDP.
        // A larger probe consumes initial packets; Qt anchors playback at zero
        // after probing, retaining that PTS gap as steady live latency.
        // Keep receipt/dimension/freshness validation before any presentation.
        options.setProbeSize(backend->probeBytes);
        options.setPlaybackIntent(QPlaybackOptions::PlaybackIntent::LowLatencyStreaming);
        m_player->setPlaybackOptions(options);
        m_vlc=backend.release();
        m_lastFrameClock.restart();m_frameTimer.start();
        m_vlc->openNs=RtspVlcBackend::nowNs();
        m_player->setSource(source);m_player->play();
        return true;
    }
    // Optional A/B comparison without changing the project or persistent settings.
    if (qEnvironmentVariable("RTSP_CAMERA_BACKEND").compare(QStringLiteral("qt"), Qt::CaseInsensitive) == 0)
        return false;
    const QStringList directories=(m_managedRtsp||RvDtofCandidates::isCaptureOnly(m_dtofMode))?QStringList{m_managedVlcDirectory}:vlcDirectories();
    for (const auto &directory : directories) {
        if (directory.isEmpty()) continue;
        auto backend = std::make_unique<RtspVlcBackend>();
        if (!backend->load(directory)) continue;
        backend->managedCamera=m_managedRtsp||RvDtofCandidates::isCaptureOnly(m_dtofMode);
        const char *args[] = {"--no-audio", "--no-video-title-show", "--avcodec-hw=none"};
        backend->instance = backend->create(3, args);
        if (!backend->instance) continue;
        void *media = backend->mediaNew(backend->instance, source.toEncoded().constData());
        if (!media) continue;
        // Session-local A/B overrides; invalid values fail back to tested defaults.
        bool cacheOk=false, threadsOk=false;
        const int cache=qEnvironmentVariableIntValue("RV_RTSP_NETWORK_CACHING_MS",&cacheOk);
        const int threads=qEnvironmentVariableIntValue("RV_RTSP_AVCODEC_THREADS",&threadsOk);
        if(cacheOk&&cache>=0&&cache<=1000)backend->cacheMs=cache;
        if(threadsOk&&threads>=1&&threads<=8)backend->threads=threads;
        if(qEnvironmentVariable("RV_RTSP_TRANSPORT")=="udp")backend->transport="udp";
        backend->mediaOption(media,backend->transport=="tcp"?":rtsp-tcp":":no-rtsp-tcp");
        const QByteArray cacheOption=":network-caching="+QByteArray::number(backend->cacheMs);
        const QByteArray threadOption=":avcodec-threads="+QByteArray::number(backend->threads);
        backend->mediaOption(media,cacheOption.constData());backend->mediaOption(media,threadOption.constData());
        for(const char* option:{":clock-jitter=0",":clock-synchro=0",":drop-late-frames",":skip-frames"})
            backend->mediaOption(media,option);
        backend->player = backend->playerNew(media);
        backend->mediaRelease(media);
        if (!backend->player) continue;
        backend->callbacks(backend->player, RtspVlcBackend::lock, RtspVlcBackend::unlock,
                           RtspVlcBackend::display, backend.get());
        backend->formatCallbacks(backend->player, RtspVlcBackend::format, nullptr);
        if (backend->play(backend->player) != 0) continue;
        m_vlc = backend.release();
        m_lastFrameClock.restart();
        m_frameTimer.start();
        return true;
    }
    return false; // Qt Multimedia remains usable when VLC 3 is unavailable.
}

bool RvSessionController::prepareManagedVlc()
{
    m_managedVlcDirectory.clear();
    if(qEnvironmentVariable("RTSP_CAMERA_BACKEND").compare("qt",Qt::CaseInsensitive)==0)return false;
    for(const auto &directory:vlcDirectories()) {
        const QDir root(directory);
        if(!QFileInfo::exists(root.filePath("plugins/codec/libavcodec_plugin.dll"))
                ||!QFileInfo::exists(root.filePath("plugins/access/liblive555_plugin.dll")))continue;
        auto backend=std::make_unique<RtspVlcBackend>();
        if(!backend->load(directory))continue;
        qputenv("VLC_PLUGIN_PATH",root.filePath("plugins").toLocal8Bit());
        const char *args[]={"--no-audio","--no-video-title-show","--avcodec-hw=none"};
        backend->instance=backend->create(3,args);
        if(!backend->instance)continue;
        m_managedVlcDirectory=directory;
        return true;
    }
    return false;
}

void RvSessionController::stopManagedStatus()
{
    ++m_statusEpoch;
    m_statusTimer.stop();
    m_managedRtsp=false;m_rtspStatusReady=false;m_rtspDecodeFloor=0;
    if(m_statusReply) {
        auto *reply=m_statusReply.data();m_statusReply=nullptr;
        reply->abort();reply->deleteLater();
    }
}

void RvSessionController::pollManagedStatus()
{
    if(!m_managedRtsp||m_userStopped||m_statusReply||!m_vlc)return;
    QUrl endpoint;endpoint.setScheme("http");endpoint.setHost(m_device.host);endpoint.setPort(8837);endpoint.setPath("/status");
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control","no-cache");
    request.setTransferTimeout(2000);
    auto *reply=m_statusNetwork->get(request);
    reply->setReadBufferSize(65537);
    m_statusReply=reply;
    const quint64 epoch=m_statusEpoch;
    connect(reply,&QNetworkReply::readyRead,reply,[reply]{if(reply->bytesAvailable()>65536)reply->abort();});
    connect(reply,&QNetworkReply::finished,this,[this,reply,epoch]{
        reply->deleteLater();
        if(epoch!=m_statusEpoch)return;
        m_statusReply=nullptr;
        if(!m_managedRtsp||m_userStopped||!m_vlc)return;
        const auto body=reply->readAll();
        QJsonParseError parse;
        const auto document=QJsonDocument::fromJson(body,&parse);
        const auto status=document.object();
        const auto service=status["service"].toObject();
        const auto expected=m_expectedCameraStatus["service"].toObject();
        const bool valid=reply->error()==QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200
            && body.size()<=65536 && parse.error==QJsonParseError::NoError && document.isObject()
            && validRtspStatus(status) && status["boot_id"]==m_expectedCameraStatus["boot_id"]
            && service["model"]==expected["model"]
            && sessionGeneration(service["generation"])==sessionGeneration(expected["generation"]);
        if(!valid){failPreviewControl(tr("Managed camera status changed or became unavailable"), true);return;}
        const auto frame=status["telemetry"].toObject()["encoded_frame_id"].toInteger();
        const auto retrieved=status["telemetry"].toObject()["encoder_retrieved_frame_id"].toInteger();
        const bool progressed=frame>m_lastEncodedFrame && retrieved>m_lastRetrievedFrame;
        if(frame<m_lastEncodedFrame || retrieved<m_lastRetrievedFrame
                || (!progressed&&m_encoderProgressClock.elapsed()>=3000)) {
            failPreviewControl(tr("Managed camera encoder progress stopped; check board status"), true);return;
        }
        if(progressed) {
            m_lastEncodedFrame=frame;m_lastRetrievedFrame=retrieved;m_encoderProgressClock.restart();
            if(!m_rtspStatusReady) {
                std::lock_guard<std::mutex> guard(m_vlc->mutex);
                m_rtspDecodeFloor=m_vlc->sequence;
                m_rtspStatusReady=true;
            }
        }
    });
}

void RvSessionController::stopVlc(bool retainBoardReceiver)
{
    ++m_presentationEpoch;
    m_hdVideoSeen=false;
    stopManagedStatus();
    m_frameTimer.stop();
    // Detach ownership before deleting or notifying: direct observers can Stop
    // again without touching either receiver while its destructor is running.
    auto *nv12 = m_nv12; m_nv12 = nullptr;
    auto *hdVideo=m_hdVideo;m_hdVideo=nullptr;delete hdVideo;
    auto *vlc = m_vlc; m_vlc = nullptr;
    if(vlc&&vlc->output=="qt"){m_player->stop();m_player->setSource(QUrl());}
    auto* dtof=m_dtofStream;m_dtofStream=nullptr;
    if(retainBoardReceiver&&dtof){
        auto *previous=m_retiringDtofStream.data();m_retiringDtofStream=dtof;
        if(previous!=dtof)delete previous;
    }else{
        auto *previous=m_retiringDtofStream.data();m_retiringDtofStream=nullptr;
        delete dtof;if(previous!=dtof)delete previous;
    }
    delete nv12;
    delete vlc;
    clearCurrentFrame(tr("Disconnected / No data"));
}

void RvSessionController::clearCurrentFrame(const QString &reason)
{
    m_videoFps = 0;
    m_nativeDepthExpiry.stop();m_assistExpiry.stop();
    if(m_assist)m_assist->cancel(reason);
    m_lastAssistFrame=0;
    m_currentMetadata = {};
    m_hdNativeFrames.clear();m_hdRgbFrames.clear();m_hdScheduledTof=0;m_hdPairDrops={};
    m_hdRealtimeOffsetNs=m_hdBoardSampleNs=m_hdSampleReceivedNs=0;m_hdClockSamples=0;
    m_depthInfoText.clear();
    for(const auto* name:{"rvDepthMetadata","rvModel","rvFrameId","rvGeneration","rvWidth","rvRawTofSequence","rvRgbSequence",
            "rvTransport","rvIdentitySource","rvBootId","rvSourceWidth","rvSourceHeight",
            "rvDecodedSequence","rvEncodedSequence","rvFirstFrameMs","rvReconnectCount"})setProperty(name,QVariant());
    if (!m_destroying) {
        emit depthStreamCleared(reason);
        emit streamCleared();
    }
}

bool RvSessionController::reconnectHostHdVideo()
{
    // An RTSP decoder can join before parameter sets/IDR arrive. Renew only
    // that receiver once; retain native capture and the board generation.
    if(!RvDtofCandidates::isCaptureOnly(m_dtofMode)||!m_dtofStream||m_hdVideoReconnects>=1||m_encoderProgressClock.elapsed()>=3000)return false;
    ++m_hdVideoReconnects;setProperty("rvVideoReconnectCount",m_hdVideoReconnects);
    if(m_hdVideo){auto *old=m_hdVideo;m_hdVideo=nullptr;delete old;
        m_hdVideo=new RvHdVideoStream(m_device.streamUrl(true));m_hdVideoSequence=0;
        m_hdVideoSeen=false;m_rtspStatusReady=false;m_hdFrameClock.restart();m_hdVideo->start();return true;}
    auto* old=m_vlc;m_vlc=nullptr;delete old;
    m_hdVideoSeen=false;m_rtspStatusReady=false;m_hdFrameClock.restart();
    return startVlc(QUrl(m_device.streamUrl(true)));
}

void RvSessionController::observeHostHdClock(const QJsonObject &m)
{
    bool validReal=false,validMono=false;
    const auto real=m["server_realtime_ns"].toString().toLongLong(&validReal);
    const auto mono=m["server_monotonic_ns"].toString().toLongLong(&validMono);
    if(!validReal||!validMono||real<=0||mono<=0||m["boot_id"]!=m_dtofReceipt["boot_id"]||m["generation"]!=m_dtofToken){
        m_hdClockSamples=0;m_hdRgbFrames.clear();return;
    }
    if(mono<=m_hdBoardSampleNs)return;
    const auto offset=real-mono;
    // RTCP and the native ToF timestamps originate on this board. A wall-clock
    // step invalidates the mapping; require two fresh, stable samples again.
    if(m_hdClockSamples&&std::abs(offset-m_hdRealtimeOffsetNs)>1000000LL){m_hdClockSamples=0;m_hdNativeFrames.clear();m_hdRgbFrames.clear();}
    m_hdRealtimeOffsetNs=offset;m_hdBoardSampleNs=mono;m_hdSampleReceivedNs=RvHdVideoStream::nowNs();
    m_hdClockSamples=std::min(m_hdClockSamples+1,1000000);
}

int RvSessionController::nearestHostHdRgb(const RvTofFrame &raw,qint64 now) const
{
    const auto &m=raw.metadata;const auto tofNs=m["tof_timestamp_ns"].toInteger();
    int nearest=-1; qint64 distance=std::numeric_limits<qint64>::max();
    for(int i=0;i<int(m_hdRgbFrames.size());++i){const auto &rgb=m_hdRgbFrames[i];
        if(rgb.source["boot_id"]!=m["boot_id"]||rgb.source["source_generation"]!=m["generation"]
           ||rgb.source["generation"]!=double(m_presentationEpoch)||now-rgb.source["host_decoded_monotonic_ns"].toInteger()>1000000000LL
           ||rgb.source["rectify_map_sha256"]!=QLatin1String(RvCameraView::profile(assistHostModel()).mapSha))continue;
        const auto delta=std::abs(rgb.boardNs-tofNs);if(delta<distance){distance=delta;nearest=i;}
    }
    return nearest;
}

const RvTofFrame *RvSessionController::latestReusableNative(qint64 rgbNs,qint64 now) const
{
    if(!(m_assist&&m_assist->reusesNativeMeasurements())||m_hdNativeFrames.empty()||rgbNs<=0
        ||m_hdClockSamples<2||now-m_hdSampleReceivedNs>=1500000000LL)return nullptr;
    // Raw capture is monotonic. An invalid/expired newest frame never falls
    // back to an older, apparently closer frame and hides the new condition.
    const auto &raw=m_hdNativeFrames.back();const auto &m=raw.metadata;
    const auto tofNs=m["tof_timestamp_ns"].toInteger();
    const auto boardNow=m_hdBoardSampleNs+now-m_hdSampleReceivedNs;
    if(!raw.isValid()||m["boot_id"]!=m_dtofReceipt["boot_id"]||m["generation"]!=m_dtofToken
        ||m["dtof_fov_correction"].toInt(-1)!=m_device.fov()||(m["tof_warning_flags"]!=0&&m["tof_warning_flags"]!=16)
        ||tofNs<=0||boardNow-tofNs>1000000000LL||tofNs-boardNow>200000000LL
        ||std::abs(rgbNs-tofNs)>qint64(RvTofFrame::GlobalScaleMaxPairDeltaMs*1000000.))return nullptr;
    return &raw;
}

void RvSessionController::dispatchHostHdMeasurement()
{
    if(!m_assist||!m_assist->tofDriven()||m_userStopped||m_controlPending||m_transitioning||m_hdNativeFrames.empty())return;
    const auto now=RvHdVideoStream::nowNs();
    const RvTofFrame *candidate=&m_hdNativeFrames.back();

    const auto &raw=*candidate;const auto &m=raw.metadata;
    const auto seq=quint64(m["tof_sequence"].toInteger());if(seq<=m_hdScheduledTof)return;
    const auto drop=[&](const char *reason){m_hdPairDrops[reason]=m_hdPairDrops[reason].toInt()+1;m_hdScheduledTof=seq;};
    if(!raw.isValid()||m["boot_id"]!=m_dtofReceipt["boot_id"]||m["generation"]!=m_dtofToken
       ||m["dtof_fov_correction"].toInt(-1)!=m_device.fov()||(m["tof_warning_flags"]!=0&&m["tof_warning_flags"]!=16)){drop("native_identity");return;}
    if(m_hdClockSamples<2||now-m_hdSampleReceivedNs>=1500000000LL){drop("clock_unavailable");return;}
    const auto tofNs=m["tof_timestamp_ns"].toInteger();
    const auto estimatedBoardNow=m_hdBoardSampleNs+now-m_hdSampleReceivedNs;
    if(tofNs<=0||estimatedBoardNow-tofNs>1000000000LL||tofNs-estimatedBoardNow>200000000LL){drop("measurement_expired");return;}
    const auto selected=nearestHostHdRgb(raw,now);
    const HdRgb *nearest=selected<0?nullptr:&m_hdRgbFrames[selected];
    // When the video is temporarily behind, keep just the newest measurement
    // until a suitable RGB arrives. Never manufacture a timestamp or catch up.
    if(!nearest)return;
    const auto distance=std::abs(nearest->boardNs-tofNs);
    if(distance>165000000LL){if(nearest->boardNs>tofNs)drop("pair_delta_exceeded");return;}

    auto source=nearest->source;
    for(const auto *key:{"tof_sequence","tof_timestamp_ns","tof_reused","dtof_width","dtof_height","dtof_bytes","dtof_format",
                        "dtof_range_unit","dtof_source","dtof_coordinate","dtof_fov_correction","tof_warning_flags"})source[key]=m[key];
    source["work_mode"]="rgb_tof";source["dtof_present"]=true;source["rgb_timestamp_ns"]=double(nearest->boardNs);
    source["pair_delta_ms"]=(tofNs-nearest->boardNs)/1e6;source["pair_clock_uncertainty_ms"]=35;
    source["pair_clock_source"]="rtcp_prft_board_realtime_monotonic";source["pairing_policy"]="nearest_cached_rgb_new_tof_v1";

    source["measurement_age_at_pair_ms"]=std::max(0.,(estimatedBoardNow-tofNs)/1e6);
    source["assist_pair_drop_reasons"]=m_hdPairDrops;
    m_hdScheduledTof=seq;
    m_assist->submitHd(nearest->image,source,m_presentationEpoch,raw.samples);
}

void RvSessionController::presentHostHdVideo()
{
    if(!m_hdVideo||!m_dtofStream||m_userStopped||m_controlPending||m_transitioning)return;
    if(m_encoderProgressClock.elapsed()>=3000){failPreviewControl(tr("Managed camera encoder progress stopped; check board status"),true);return;}
    auto s=m_hdVideo->snapshot();
    if(!s.error.isEmpty()){if(!reconnectHostHdVideo())failPreviewControl(s.error,true);return;}
    if(s.sequence==m_hdVideoSequence||s.image.isNull()){
        if(m_hdFrameClock.elapsed()>(m_hdVideoSeen?3000:15000)&&!reconnectHostHdVideo())failPreviewControl(tr("The stream is temporarily unavailable"),true);
        return;
    }
    m_hdVideoSequence=s.sequence;
    if(!m_rtspStatusReady||s.sequence<=m_rtspDecodeFloor)return;
    s.image=cameraInCalibrationFrame(s.image);
    const auto sourceSequence=++m_hdOutputSequence;
    m_hdVideoSeen=true;m_hdFrameClock.restart();m_lastFrameClock.restart();
    if(m_waitingForSelectedFrame){m_waitingForSelectedFrame=false;m_switchFrameTimeout.stop();updateState();}
    setProperty("rvMode","host_hd");setProperty("rvTransport","rtsp_h265+lsg2");
    setProperty("rvIdentitySource","lsg2_receipt_and_venc_progress");setProperty("rvBootId",m_dtofReceipt["boot_id"].toString());
    setProperty("rvGeneration",m_dtofToken);setProperty("rvModel","Native");setProperty("rvWidth",2304);
    setProperty("rvSourceWidth",2304);setProperty("rvSourceHeight",1296);setProperty("rvDecodedSequence",s.sequence);
    setProperty("rvEncodedSequence",m_lastEncodedFrame);setProperty("rvPresentedFrames",property("rvPresentedFrames").toULongLong()+1);
    const auto epoch=m_presentationEpoch;emit cameraFrameReady(s.image);
    if(m_userStopped||epoch!=m_presentationEpoch)return;
    if(!hostHdAssist()||!assistDepthEnabled()){
        setState(tr("Playing"),tr("H.265 video and native 48x32 ToF"));
        ++m_framesInWindow;
        if(m_fpsClock.elapsed()>=1000){m_videoFps=m_framesInWindow*1000.0/m_fpsClock.elapsed();
            m_framesInWindow=0;m_fpsClock.restart();emit stateChanged();}
        return;
    }
    try{
        const auto model=assistHostModel();
        const auto &view=RvCameraView::profile(model);
        const bool wide=RvCameraView::isWide(model);
        const auto &calibration=RvCameraView::isUniDepthVga(model)?RvCameraStream::hdVgaCalibration():RvCameraStream::hdCalibration(wide);
        if(RvCameraView::isUniDepthVga(model))
            RvCameraStream::require(calibration.sha256()==view.mapSha,"HD model calibration fingerprint mismatch");
        QElapsedTimer prepare;prepare.start();const auto preview=RvCameraStream::prepareHd(s.image,calibration);
        QJsonObject source{{"source_protocol","HDA1"},{"source_transport","rtsp_h265"},
            {"source_rgb_width",2304},{"source_rgb_height",1296},{"rgb_width",view.inputWidth},{"rgb_height",view.inputHeight},
            {"camera_mount_rotation_degrees",m_device.cameraMountRotation},{"camera_coordinate_frame","calibrated_reference"},
            {"model","Native"},{"work_mode","host_rgb"},{"dtof_present",false},
            {"generation",double(epoch)},{"generation_source","host_presentation_epoch"},{"source_generation",m_dtofToken},
            {"source_candidate_mode",14},{"source_engine","native-hd-capture-v1"},{"boot_id",m_dtofReceipt["boot_id"]},
            {"frame_id",double(sourceSequence)},{"video_decoder_sequence",double(s.sequence)},{"host_decoded_monotonic_ns",double(s.receivedNs)},
            {"rectify_map_sha256",QString::fromLatin1(calibration.sha256())},{"width",0},{"height",0},
            {"host_hd_prepare_ms",prepare.nsecsElapsed()/1e6},{"video_pts_us",double(s.ptsUs)},
            {"video_producer_realtime_us",QString::number(s.producerRealtimeUs)}};
        if(wide)source["assist_camera_profile"]="wide";
        RvCaptureCalibration::annotate(source,m_dtofReceipt);
        if(RvCameraView::isUniDepth(assistHostModel())&&RvCameraView::isWide(assistHostModel())){
            source["assist_output_policy"]=RvAssistFusion::FieldOfViewPolicy;
            source["assist_tof_fov_correction"]=m_device.fov();
        }
        QByteArray samples;
        if(m_assist->tofDriven()){
            const auto now=RvHdVideoStream::nowNs();
            if(s.producerRealtimeUs>0&&m_hdClockSamples>=2&&now-m_hdSampleReceivedNs<1500000000LL){
                const auto boardNs=s.producerRealtimeUs*1000-m_hdRealtimeOffsetNs;
                if(boardNs>0&&std::abs(boardNs-m_hdBoardSampleNs)<2000000000LL){
                    if(m_hdRgbFrames.size()==16)m_hdRgbFrames.pop_front();
                    m_hdRgbFrames.push_back({preview,source,boardNs});
                }
            }
            m_currentMetadata=source;setProperty("rvFrameId",sourceSequence);dispatchHostHdMeasurement();
        }else if(m_assist->reusesNativeMeasurements()){
            const auto now=RvHdVideoStream::nowNs();
            const qint64 rgbNs=s.producerRealtimeUs>0?s.producerRealtimeUs*1000-m_hdRealtimeOffsetNs:0;
            const auto *latest=std::abs(rgbNs-m_hdBoardSampleNs)<2000000000LL?latestReusableNative(rgbNs,now):nullptr;
            if(qEnvironmentVariable("RV_DEPTHOR_PERF_PROFILE")=="1"){
                if(!m_hdNativeFrames.empty())source["profile_latest_native_delta_ms"]=(m_hdNativeFrames.back().metadata["tof_timestamp_ns"].toInteger()-rgbNs)/1e6;
                const char *reason=latest?"depthor_paired":m_hdClockSamples<2?"depthor_clock_unavailable":m_hdNativeFrames.empty()?"depthor_native_empty":
                    std::abs(source["profile_latest_native_delta_ms"].toDouble(1000))>100?"depthor_pair_over_100ms":"depthor_native_identity_fov_warning_age";
                m_hdPairDrops[reason]=m_hdPairDrops[reason].toInt()+1;
            }
            if(latest){
                const auto &m=latest->metadata;
                for(const auto *key:{"tof_sequence","tof_timestamp_ns","tof_reused","dtof_width","dtof_height","dtof_bytes","dtof_format",
                    "dtof_range_unit","dtof_source","dtof_coordinate","dtof_fov_correction","tof_warning_flags"})source[key]=m[key];
                source["work_mode"]="rgb_tof";source["dtof_present"]=true;source["rgb_timestamp_ns"]=double(rgbNs);
                source["pair_delta_ms"]=(m["tof_timestamp_ns"].toInteger()-rgbNs)/1e6;
                source["pair_clock_uncertainty_ms"]=35;source["pair_clock_source"]="rtcp_prft_board_realtime_monotonic";
                source["pairing_policy"]=RvTofFrame::GlobalScaleReusePolicy;
                source["measurement_age_at_pair_ms"]=std::max(0.,(m_hdBoardSampleNs+now-m_hdSampleReceivedNs-m["tof_timestamp_ns"].toInteger())/1e6);
                samples=latest->samples;
            }
            m_currentMetadata=source;setProperty("rvFrameId",sourceSequence);
            m_assist->submitHd(preview,source,epoch,samples);
        }else{
        // RTCP supplies the reference for the decoded packet, rather than PC
        // arrival. One frame interval (35 ms) is reserved for timestamp/exposure
        // uncertainty; this is software pairing, not hardware synchronization.
        const auto now=RvHdVideoStream::nowNs();
        if(s.producerRealtimeUs>0&&m_hdClockSamples>=2&&now-m_hdSampleReceivedNs<1500000000LL){
            const qint64 rgbNs=s.producerRealtimeUs*1000-m_hdRealtimeOffsetNs;
            const RvTofFrame *nearest=nullptr;qint64 distance=std::numeric_limits<qint64>::max();
            for(const auto &raw:m_hdNativeFrames){
                const auto &m=raw.metadata;
                if(!raw.isValid()||m["boot_id"]!=source["boot_id"]||m["generation"]!=m_dtofToken
                    ||m["dtof_fov_correction"].toInt(-1)!=m_device.fov()||(m["tof_warning_flags"]!=0&&m["tof_warning_flags"]!=16))continue;
                const auto delta=std::abs(rgbNs-m["tof_timestamp_ns"].toInteger());
                if(delta<distance){distance=delta;nearest=&raw;}
            }
            if(rgbNs>0&&std::abs(rgbNs-m_hdBoardSampleNs)<2000000000LL&&nearest&&distance<=165000000LL){
                const auto &m=nearest->metadata;
                for(const auto *key:{"tof_sequence","tof_timestamp_ns","tof_reused","dtof_width","dtof_height","dtof_bytes","dtof_format",
                        "dtof_range_unit","dtof_source","dtof_coordinate","dtof_fov_correction","tof_warning_flags"})source[key]=m[key];
                source["work_mode"]="rgb_tof";source["dtof_present"]=true;source["rgb_timestamp_ns"]=double(rgbNs);
                source["pair_delta_ms"]=(m["tof_timestamp_ns"].toInteger()-rgbNs)/1e6;
                source["pair_clock_uncertainty_ms"]=35;source["pair_clock_source"]="rtcp_prft_board_realtime_monotonic";
                source["pairing_policy"]="bounded_software_time_pair_not_hardware_sync";samples=nearest->samples;
            }
        }
        m_currentMetadata=source;setProperty("rvFrameId",sourceSequence);
        m_assist->submitHd(preview,source,epoch,samples);
        }
    }catch(const std::exception &error){failPreviewControl(QString::fromUtf8(error.what()));return;}
    if(m_userStopped||epoch!=m_presentationEpoch)return;
    setState(tr("Playing"),tr("H.265 video and host inference"));
    ++m_framesInWindow;
    if(m_fpsClock.elapsed()>=1000){m_videoFps = m_framesInWindow * 1000.0 / m_fpsClock.elapsed();
        m_framesInWindow=0;m_fpsClock.restart();emit stateChanged();}
}

void RvSessionController::presentVlcFrame()
{
    // Poll the opt-in RGB576 latest-frame mailbox promptly. A sequence is
    // still consumed once; no duplicated input or catch-up queue is created.
    const int pollMs=(hostHdAssist()&&assistDepthEnabled()&&RvCameraView::profile(assistHostModel()).networkProfile?5:15);
    if(m_frameTimer.interval()!=pollMs)m_frameTimer.setInterval(pollMs);
    if (m_userStopped || m_controlPending || m_transitioning) return;
    if(RvDtofCandidates::isCaptureOnly(m_dtofMode)){presentHostHdVideo();return;}
    const quint64 epoch = m_presentationEpoch;
    const auto stillCurrent = [this, epoch] {
        return !m_userStopped && !m_controlPending && epoch == m_presentationEpoch;
    };
    if (m_nv12) {
        auto snap=m_nv12->snapshot();
        snap.image=cameraInCalibrationFrame(snap.image);
        snap.metadata["camera_mount_rotation_degrees"]=m_device.cameraMountRotation;
        if (snap.sequence==m_nv12Sequence) return;
        m_nv12Sequence=snap.sequence;
        // The bounded mailbox may already contain a recovered frame when the
        // GUI polls, hiding the intermediate empty snapshot. Still renew the
        // managed receipt after any transport/processing failure.
        if (controlledSource() && snap.reconnects > 0) {
            scheduleReconnect(snap.image.isNull() ? snap.status : tr("The stream is temporarily unavailable"));
            return;
        }
        if(controlledSource()&&snap.tof.isValid()) {
            const auto fov=snap.tof.metadata.contains("dtof_fov_correction")?snap.tof.metadata["dtof_fov_correction"]:snap.tof.metadata["tof_fov_correction"];
            if(fov.toInt(-1)!=m_device.fov()){failPreviewControl(tr("Device FOV does not match the selected calibration."));return;}
        }
        if(snap.image.width()==2304){failPreviewControl(tr("Uncompressed 2304 camera frames are no longer supported; select H.265 camera mode"));return;}
        if (snap.image.isNull()) {
            if (controlledSource()) {
                scheduleReconnect(snap.status);
                return;
            }
            clearCurrentFrame(snap.status);
            if (!stillCurrent()) return;
            setState(tr("Reconnecting"), snap.status);
            return;
        }
        if(!m_expectedDepthMetadata.isEmpty()&&!selectedFrameMatches(snap.metadata,m_expectedDepthMetadata)){
            if (!m_waitingForSelectedFrame && controlledSource()) {
                scheduleReconnect(tr("Waiting for selected model frames..."));
                return;
            }
            if(!m_waitingForSelectedFrame){m_waitingForSelectedFrame=true;m_switchFrameTimeout.start();updateState();}
            if (!stillCurrent()) return;
            clearCurrentFrame(tr("Waiting for selected model frames..."));
            if (!stillCurrent()) return;
            setProperty("rvMode","switching");setState(tr("Connecting"),tr("Waiting for selected model frames..."));return;
        }
        if(m_waitingForSelectedFrame){m_waitingForSelectedFrame=false;m_switchFrameTimeout.stop();}
        m_lastFrameClock.restart();
        m_nextReconnectDelayMs = 1000;
        syncConfiguration(snap.metadata);
        if (!stillCurrent()) return;
        m_currentMetadata = snap.metadata;
        m_depthInfoText = (QString("%1 | %2 x %3 | %4").arg(snap.metadata["depth_location"].toString("none")).arg(snap.metadata["width"].toInt()).arg(snap.metadata["height"].toInt()).arg(snap.metadata["depth_method"].toString("none"))+m_depthConfigError);
        setProperty("rvDepthMetadata",snap.metadata.toVariantMap());setProperty("rvReceived",snap.received);setProperty("rvPostDropped",snap.dropped);
        const bool cameraOnly=snap.metadata["work_mode"]=="camera_only";
        if (!assistDepthEnabled()) m_recoveryDisplayTimer.stop();
        setProperty("rvMode",cameraOnly?"camera_only":"rgb_tof");
        setProperty("rvFrameId",snap.metadata["frame_id"].toDouble());setProperty("rvGeneration",snap.metadata["generation"].toDouble());
        setProperty("rvModel",snap.metadata["model"].toString());setProperty("rvWidth",snap.image.width());
        setProperty("rvTransport","nv12");setProperty("rvIdentitySource","can_packet");
        setProperty("rvSourceWidth",snap.image.width());setProperty("rvSourceHeight",snap.image.height());
        setProperty("rvReconnectCount",qulonglong(m_reconnectAttempt) + qulonglong(snap.reconnects));
        setProperty("rvPresentedFrames",property("rvPresentedFrames").toULongLong()+1);
        emit cameraFrameReady(snap.image);
        if (!stillCurrent()) return;
        setState(tr("Playing"), tr("NV12: host color conversion and rectification"));
        if (!stillCurrent()) return;
        ++m_framesInWindow;
        if(m_fpsClock.elapsed()>=1000) {
            m_videoFps = m_framesInWindow * 1000.0 / m_fpsClock.elapsed();
            m_framesInWindow=0; m_fpsClock.restart();
            emit stateChanged();
        }
        if (!stillCurrent()) return;
        if(cameraOnly)emit depthStreamCleared(tr("Camera-only mode; depth inference paused"));
        else {
            // Native capture publishes no inferred-depth snapshot.
            if (!snap.depth.isEmpty()) emit depthSnapshotReady(snap.depth,snap.metadata,snap.image);
            if(stillCurrent()&&!m_waitingForSelectedFrame)emit tofFrameReady(snap.tof);
            if(stillCurrent()&&!m_waitingForSelectedFrame&&!snap.depth.isEmpty())emit depthFrameReady(snap.depth,snap.metadata);
            if(stillCurrent()&&!m_waitingForSelectedFrame&&assistDepthEnabled()&&snap.image.width()==576){
                auto pairedTof=snap.tof;pairedTof.metadata=snap.metadata;
                m_assist->submit(snap.image,pairedTof,epoch);
            }

        }
        return;
    }
    if (!m_vlc) {
        if (m_lastFrameClock.isValid() && m_lastFrameClock.elapsed() >= 10000)
            scheduleReconnect(tr("The stream is temporarily unavailable"));
        return;
    }
    QImage frame;
    quint64 decodedSequence=0;
    {
        std::lock_guard<std::mutex> guard(m_vlc->mutex);
        if (m_vlc->presented != m_vlc->sequence) {
            frame = m_vlc->latest;
            m_vlc->presented = m_vlc->sequence;
            decodedSequence=m_vlc->sequence;
        }
    }
    if(m_managedRtsp && !frame.isNull()) {
        if(frame.size()!=QSize(2304,1296)){failPreviewControl(tr("Managed H.265 source dimensions do not match 2304 x 1296"));return;}
        if(!m_rtspStatusReady || decodedSequence<=m_rtspDecodeFloor)return;
        if(m_waitingForSelectedFrame){m_waitingForSelectedFrame=false;m_switchFrameTimeout.stop();updateState();}
        if (!stillCurrent()) return;
        const auto service=m_expectedCameraStatus["service"].toObject();
        setProperty("rvMode","camera_only");setProperty("rvTransport","rtsp");
        setProperty("rvIdentitySource","control_receipt_and_http_status");
        setProperty("rvModel",service["model"].toString());setProperty("rvGeneration",sessionGeneration(service["generation"]));
        setProperty("rvBootId",m_expectedCameraStatus["boot_id"].toString());
        setProperty("rvEncodedSequence",m_lastEncodedFrame);
        // The decoded sequence identifies a host RGB frame, never a CAN/ToF pair.
        m_depthInfoText=tr("Camera-only mode; depth inference paused");
    }
    if (frame.isNull()) {
        // Keep the selected-first-frame deadline absolute for this connection;
        // reconnecting before it expires would indefinitely restart that timer.
        if(m_managedRtsp&&m_waitingForSelectedFrame)return;
        if (m_lastFrameClock.elapsed() > (m_managedRtsp?3000:10000))
            scheduleReconnect(tr("The stream is temporarily unavailable"));
        return;
    }
    m_lastFrameClock.restart();
    m_nextReconnectDelayMs = 1000;
    setProperty("rvSourceWidth",frame.width());setProperty("rvSourceHeight",frame.height());
    setProperty("rvWidth",frame.width());setProperty("rvDecodedSequence",decodedSequence);
    setProperty("rvReconnectCount",m_reconnectAttempt);
    if(!property("rvFirstFrameMs").isValid())setProperty("rvFirstFrameMs",m_connectionClock.elapsed());
    setProperty("rvPresentedFrames",property("rvPresentedFrames").toULongLong()+1);
    m_recoveryDisplayTimer.stop();
    frame=cameraInCalibrationFrame(frame);
    emit cameraFrameReady(frame);
    if (!stillCurrent()) return;
    setState(tr("Playing"),QStringLiteral("VLC / TCP / 100 ms"));
    if (!stillCurrent()) return;
    ++m_framesInWindow;
    if (m_fpsClock.elapsed() >= 1000) {
        m_videoFps = m_framesInWindow * 1000.0 / m_fpsClock.elapsed();
        m_framesInWindow = 0;
        m_fpsClock.restart();
        emit stateChanged();
    }
}
