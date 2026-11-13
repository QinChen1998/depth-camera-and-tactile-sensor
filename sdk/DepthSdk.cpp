#include "DepthSdk.h"
#include "DepthSdkQt.h"
#include "../RuntimeDiscovery.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvInferenceSettings.h"
#include "../sensors/RvSessionController.h"
#include "../sensors/RvUsbEnrollment.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QScopedValueRollback>
#include <QSettings>
#include <QThread>
#include <QtCore/qscopeguard.h>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto Board = "board_native_tof48";
constexpr auto BoardHighAccuracy = "board_native_tof48_high_accuracy";
QMutex sessionMutex;
DctsDepthSession *activeSession = nullptr;
bool changingSession = false;

struct StandaloneApplication {
    int argc = 1;
    char name[13] = "DctsDepthSdk";
    char *argv[2] = {name, nullptr};
    QCoreApplication application{argc, argv};
};

uint32_t modeOf(const RvSessionController &controller)
{
    if (controller.hostHdAssist()) return DCTS_DEPTH_HOST_ASSISTED;
    return controller.dtofCandidateMode() == RvDtofCandidates::Native48HighAccuracy
        ? DCTS_DEPTH_BOARD_HIGH_ACCURACY : DCTS_DEPTH_BOARD;
}

DctsDepthImage rgbView(const QImage &image)
{
    if (image.isNull()) return {};
    return {image.constBits(), uint64_t(image.sizeInBytes()), uint32_t(image.width()),
            uint32_t(image.height()), uint32_t(image.bytesPerLine()), DCTS_DEPTH_PIXEL_RGB8};
}
}

struct DctsDepthSession {
    const std::thread::id thread = std::this_thread::get_id();
    std::unique_ptr<StandaloneApplication> standalone;
    std::unique_ptr<RvInferenceSettings> inference;
    std::unique_ptr<RvSessionController> controller;
    RvUsbEnrollment *usb = nullptr;
    DctsDepthFrameCallback onFrame = nullptr;
    DctsDepthEventCallback onEvent = nullptr;
    void *user = nullptr;
    QString error;
    bool delivering = false;
    bool videoAvailable = false, depthAvailable = false, boardFilesRequired = false;

    ~DctsDepthSession()
    {
        onFrame = nullptr;
        onEvent = nullptr;
        controller.reset();
        inference.reset();
        RuntimeDiscovery::setSdkPaths({}, {});
    }

    void event(uint32_t type)
    {
        if (!onEvent || delivering) return;
        QScopedValueRollback<bool> dispatch(delivering, true);
        onEvent(user, type);
    }

    void video(const QImage &image, quint64 sequence, qint64 decodedNs, const QString &generation)
    {
        if (delivering || controller->userStopped()) return;
        if (!videoAvailable) {
            videoAvailable = true;
            event(DCTS_DEPTH_EVENT_STATE_CHANGED);
        }
        if (!onFrame) return;
        const auto rgb = image.convertToFormat(QImage::Format_RGB888);
        const auto device = controller->deviceProfile().id.toUtf8();
        const auto capture = generation.toUtf8();
        DctsDepthFrame frame{};
        frame.size = sizeof(frame);
        frame.stream = DCTS_DEPTH_STREAM_VIDEO;
        frame.mode = modeOf(*controller);
        frame.session_epoch = controller->presentationEpoch();
        frame.sequence = sequence;
        frame.timestamp_ns = uint64_t(decodedNs);
        frame.clock_domain = DCTS_DEPTH_CLOCK_HOST_MONOTONIC;
        frame.device_id_utf8 = device.constData();
        frame.capture_generation_utf8 = capture.constData();
        frame.image = rgbView(rgb);
        QScopedValueRollback<bool> dispatch(delivering, true);
        onFrame(user, &frame);
    }

    void depth(const QByteArray &bytes, const QJsonObject &metadata, const QImage &camera, const QByteArray &diagnostic)
    {
        if (delivering || controller->userStopped()) return;
        const bool radial = metadata["depth_unit"] == "sensor_radial_m_float32";
        const auto width = uint32_t(metadata["width"].toInt());
        const auto height = uint32_t(metadata["height"].toInt());
        if (radial ? width != 48 || height != 32 : !RvCameraView::validOutputGrid(metadata)) return;
        if (bytes.size() != qsizetype(width) * height * 4) return;
        if (!depthAvailable) {
            depthAvailable = true;
            event(DCTS_DEPTH_EVENT_STATE_CHANGED);
        }
        if (!onFrame) return;
        const auto device = controller->deviceProfile().id.toUtf8();
        const auto capture = metadata["source_generation"].toString().toUtf8();
        const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
        const auto pairedRgb = camera.convertToFormat(QImage::Format_RGB888);
        const auto indices = metadata["native_hole_fill_indices"].toArray();
        QByteArray estimated;
        if (!indices.isEmpty()) {
            estimated = QByteArray(qsizetype(width) * height, '\0');
            for (const auto &entry : indices) {
                const auto i = entry.toInt(-1);
                if (i >= 0 && i < estimated.size()) estimated[i] = 1;
            }
        }

        DctsDepthFrame frame{};
        frame.size = sizeof(frame);
        frame.stream = DCTS_DEPTH_STREAM_DEPTH;
        frame.mode = modeOf(*controller);
        frame.coordinate = radial ? DCTS_DEPTH_COORDINATE_SENSOR_RADIAL : DCTS_DEPTH_COORDINATE_CAMERA_Z;
        frame.session_epoch = controller->presentationEpoch();
        frame.sequence = uint64_t(metadata[radial ? "tof_sequence" : "frame_id"].toInteger());
        frame.timestamp_ns = uint64_t(metadata[radial ? "tof_timestamp_ns" : "host_decoded_monotonic_ns"].toInteger());
        frame.clock_domain = radial ? DCTS_DEPTH_CLOCK_BOARD_MONOTONIC : DCTS_DEPTH_CLOCK_HOST_MONOTONIC;
        frame.device_id_utf8 = device.constData();
        frame.capture_generation_utf8 = capture.constData();
        frame.image = {bytes.constData(), uint64_t(bytes.size()), width, height, width * 4, DCTS_DEPTH_PIXEL_F32_LE};
        frame.paired_rgb = rgbView(pairedRgb);
        if (!estimated.isEmpty()) {
            frame.estimated_mask = {estimated.constData(), uint64_t(estimated.size()), width, height, width, DCTS_DEPTH_PIXEL_U8};
        }
        if (!radial && metadata["assist_diagnostic_payload"] == "mono_reasons_v1"
            && RvDepthDiagnostic::validPayload(diagnostic, int(bytes.size()))) {
            const auto *reasons = diagnostic.constData() + bytes.size();
            frame.preview_prediction = frame.image;
            frame.preview_prediction.data = diagnostic.constData();
            frame.preview_global_reasons = {reasons, 48 * 32, 48, 32, 48, DCTS_DEPTH_PIXEL_U8};
            frame.preview_local_reasons = {reasons + RvDepthDiagnostic::ReasonBytes, 48 * 32, 48, 32, 48, DCTS_DEPTH_PIXEL_U8};
        }
        frame.metadata_json_utf8 = json.constData();
        QScopedValueRollback<bool> dispatch(delivering, true);
        onFrame(user, &frame);
    }

    void bindCallbacks()
    {
        auto *source = controller.get();
        QObject::connect(source, &RvSessionController::videoFrameStamped, source,
                         [this](const QImage &rgb, quint64 sequence, qint64 decodedNs, const QString &generation) {
            video(rgb, sequence, decodedNs, generation);
        });
        QObject::connect(source, &RvSessionController::depthSnapshotReady, source,
                         [this](const QByteArray &bytes, const QJsonObject &metadata, const QImage &rgb) { depth(bytes, metadata, rgb, {}); });
        QObject::connect(source, &RvSessionController::assistDepthSnapshotReady, source,
                         [this](const QByteArray &bytes, const QJsonObject &metadata, const QImage &rgb, const QByteArray &aux) {
            depth(bytes, metadata, rgb, aux);
        });
        QObject::connect(source, &RvSessionController::stateChanged, source, [this] { event(DCTS_DEPTH_EVENT_STATE_CHANGED); });
        QObject::connect(source, &RvSessionController::configurationChanged, source, [this] { event(DCTS_DEPTH_EVENT_STATE_CHANGED); });
        QObject::connect(source, &RvSessionController::streamCleared, source, [this] {
            videoAvailable = false;
            event(DCTS_DEPTH_EVENT_VIDEO_CLEARED);
        });
        const auto clearDepth = [this](const QString &) {
            depthAvailable = false;
            event(DCTS_DEPTH_EVENT_DEPTH_CLEARED);
        };
        QObject::connect(source, &RvSessionController::depthStreamCleared, source, clearDepth);
        QObject::connect(source, &RvSessionController::assistDepthStreamCleared, source, [this, clearDepth](const QString &reason) {
            if (controller->hostHdAssist()) clearDepth(reason);
        });
        QObject::connect(source, &RvSessionController::boardFilesRequired, source, [this](const RvDeviceProfile &) {
            boardFilesRequired = true;
            event(DCTS_DEPTH_EVENT_BOARD_FILES_REQUIRED);
        });
        QObject::connect(source->inferenceSettings(), &RvInferenceSettings::changed, source, [this] {
            event(DCTS_DEPTH_EVENT_INFERENCE_CHANGED);
        });
        QObject::connect(source, &RvSessionController::hostInferenceUnavailable, source,
                         [this](const QString &reason, const QString &detail) {
            error = detail.isEmpty() ? reason : reason + "\n" + detail;
            event(DCTS_DEPTH_EVENT_STATE_CHANGED);
        });
    }

    bool busy() const
    {
        return controller->busy() || controller->automaticConnecting() ||
            controller->imagingSwitchActive() || controller->controlProcessActive() || rvUsbOperationBusy(usb);
    }

    DctsDepthResult prepareInference()
    {
        auto *settings = controller->inferenceSettings();
        if (!settings->started()) settings->detectDevices();
        return settings->scanning() ? DCTS_DEPTH_BUSY : DCTS_DEPTH_OK;
    }

    void prepareUsb()
    {
        if (usb) return;
        RvUsbEnrollmentHooks hooks;
        hooks.changed = [this] { event(DCTS_DEPTH_EVENT_USB_CHANGED); };
        usb = createRvUsbEnrollment(controller.get(), controller.get(), std::move(hooks), false);
    }
};

namespace {
std::unique_ptr<DctsDepthSession> createSession(const QString &runtime, const QString &store)
{
    auto sdk = std::make_unique<DctsDepthSession>();
    if (!QCoreApplication::instance()) sdk->standalone = std::make_unique<StandaloneApplication>();
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) return {};
    if (!runtime.isEmpty()) {
        if (sdk->standalone) QCoreApplication::addLibraryPath(runtime);
        const auto preferences = QFileInfo(store).absoluteDir().filePath("depth-sdk.ini");
        RuntimeDiscovery::setSdkPaths(runtime, preferences);
        auto settings = RuntimeDiscovery::depthSettings();
        if (!settings.contains("depth/imagingScheme")) settings.setValue("depth/imagingScheme", QLatin1String(Board));
        sdk->inference = std::make_unique<RvInferenceSettings>();
    }
    sdk->controller = std::make_unique<RvSessionController>(nullptr, store, sdk->inference.get());
    if (!runtime.isEmpty()) sdk->bindCallbacks();
    return sdk;
}

DctsDepthResult reserveSession()
{
    std::lock_guard<QMutex> lock(sessionMutex);
    if (activeSession || changingSession) return DCTS_DEPTH_BUSY;
    changingSession = true;
    return DCTS_DEPTH_OK;
}

void publishSession(DctsDepthSession *session)
{
    std::lock_guard<QMutex> lock(sessionMutex);
    activeSession = session;
    changingSession = false;
}

void releaseSession(DctsDepthSession *session)
{
    {
        std::lock_guard<QMutex> lock(sessionMutex);
        activeSession = nullptr;
        changingSession = true;
    }
    delete session;
    publishSession(nullptr);
}

class QtSessionOwner final : public QObject {
public:
    QtSessionOwner(QObject *parent, DctsDepthSession *session) : QObject(parent), m_session(session) {}
    ~QtSessionOwner() override { releaseSession(m_session); }
private:
    DctsDepthSession *m_session;
};

template <typename Call>
DctsDepthResult withSession(DctsDepthSession *session, Call call, bool readOnly = false)
{
    {
        std::lock_guard<QMutex> lock(sessionMutex);
        if (!session || session != activeSession) return DCTS_DEPTH_INVALID_ARGUMENT;
        if (session->thread != std::this_thread::get_id()) return DCTS_DEPTH_WRONG_THREAD;
    }
    if (session->delivering && !readOnly) return DCTS_DEPTH_BUSY;
    try {
        return call(*session);
    } catch (const std::exception &error) {
        session->error = QString::fromUtf8(error.what());
        return DCTS_DEPTH_UNAVAILABLE;
    } catch (...) {
        return DCTS_DEPTH_UNAVAILABLE;
    }
}

DctsDepthResult copyJson(const QJsonDocument &document, char *buffer, uint32_t *size)
{
    if (!size) return DCTS_DEPTH_INVALID_ARGUMENT;
    const auto json = document.toJson(QJsonDocument::Compact);
    const uint32_t required = uint32_t(json.size()) + 1;
    const uint32_t capacity = buffer ? *size : 0;
    *size = required;
    if (!buffer) return DCTS_DEPTH_OK;
    if (capacity < required) return DCTS_DEPTH_BUFFER_TOO_SMALL;
    std::memcpy(buffer, json.constData(), required);
    return DCTS_DEPTH_OK;
}
}

RvSessionController *DepthSdkQt::attach(QObject *parent)
{
    if (!parent || reserveSession() != DCTS_DEPTH_OK) throw std::runtime_error("Depth SDK session already open");
    auto abandon = qScopeGuard([] { publishSession(nullptr); });
    auto sdk = createSession({}, RvDeviceStore::defaultPath());
    if (!sdk) throw std::runtime_error("Depth SDK must open on the Qt application thread");
    auto *controller = sdk->controller.get();
    new QtSessionOwner(parent, sdk.get());
    publishSession(sdk.release());
    abandon.dismiss();
    return controller;
}

uint32_t DCTS_DEPTH_CALL DctsDepth_AbiVersion(void)
{
    return DCTS_DEPTH_ABI_VERSION;
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Open(const DctsDepthOptions *options, DctsDepthSession **session)
{
    if (!session) return DCTS_DEPTH_INVALID_ARGUMENT;
    *session = nullptr;
    if (!options || options->size < sizeof(*options) || options->abi_version != DCTS_DEPTH_ABI_VERSION
        || !options->runtime_directory_utf8 || !options->device_store_utf8) return DCTS_DEPTH_INVALID_ARGUMENT;
    const auto reserved = reserveSession();
    if (reserved != DCTS_DEPTH_OK) return reserved;
    auto abandon = qScopeGuard([] { publishSession(nullptr); });
    try {
        const auto runtime = QString::fromUtf8(options->runtime_directory_utf8);
        const auto store = QString::fromUtf8(options->device_store_utf8);
        if (!QDir::isAbsolutePath(runtime) || !QFileInfo(runtime).isDir() ||
            !QDir::isAbsolutePath(store) || QFileInfo(store).isDir()) return DCTS_DEPTH_INVALID_ARGUMENT;
        if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread())
            return DCTS_DEPTH_WRONG_THREAD;
        auto sdk = createSession(QDir::cleanPath(runtime), QDir::cleanPath(store));
        if (!sdk) return DCTS_DEPTH_UNAVAILABLE;
        *session = sdk.release();
        publishSession(*session);
        abandon.dismiss();
        return DCTS_DEPTH_OK;
    } catch (...) {
        return DCTS_DEPTH_UNAVAILABLE;
    }
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Close(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        if (rvUsbOperationBusy(sdk.usb)) return DCTS_DEPTH_BUSY;
        releaseSession(&sdk);
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Subscribe(
    DctsDepthSession *session, DctsDepthFrameCallback frame, DctsDepthEventCallback event, void *user)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        sdk.onFrame = frame;
        sdk.onEvent = event;
        sdk.user = user;
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Poll(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        if (!sdk.standalone) return DCTS_DEPTH_INVALID_ARGUMENT;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_ListDevices(DctsDepthSession *session, char *buffer, uint32_t *size)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        const auto current = sdk.controller->deviceProfile().id;
        QJsonArray rows;
        for (const auto &device : RvDeviceStore(sdk.controller->deviceStorePath()).profiles()) {
            rows.append(QJsonObject{{"id", device.id}, {"name", device.name}, {"host", device.host},
                                    {"serial", device.serial}, {"ready", device.ready()}, {"selected", device.id == current},
                                    {"family", "rv1103"}, {"modes", QJsonArray{1, 2, 3}}});
        }
        return copyJson(QJsonDocument(rows), buffer, size);
    }, true);
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_GetState(DctsDepthSession *session, char *buffer, uint32_t *size)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        const auto &source = *sdk.controller;
        const QJsonObject state{
            {"device_id", source.deviceProfile().id}, {"mode", int(modeOf(source))},
            {"user_stopped", source.userStopped()}, {"busy", sdk.busy()},
            {"retry_pending", source.waitingForReconnect()}, {"retaining_display", source.retainingRecoveryDisplay()},
            {"video_available", sdk.videoAvailable}, {"depth_available", sdk.depthAvailable},
            {"board_files_required", sdk.boardFilesRequired}, {"connection_stage", int(source.automaticConnectionStage())},
            {"inference_loading_state", source.assistLoadingState()}, {"inference_detail", source.assistDepthInfoText()},
            {"inference_backend", source.inferenceSettings()->selected()}, {"inference_scanning", source.inferenceSettings()->scanning()},
            {"epoch", qint64(source.presentationEpoch())}, {"state", source.stateText()}, {"detail", source.detailText()},
            {"error", sdk.error}, {"video_fps", source.videoFps()}
        };
        return copyJson(QJsonDocument(state), buffer, size);
    }, true);
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_SelectDevice(DctsDepthSession *session, const char *id)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        if (!id || !*id) return DCTS_DEPTH_INVALID_ARGUMENT;
        if (!sdk.controller->userStopped() || sdk.busy()) return DCTS_DEPTH_BUSY;
        if (!sdk.controller->selectDevice(QString::fromUtf8(id))) return DCTS_DEPTH_DEVICE_NOT_FOUND;
        sdk.error.clear();
        sdk.boardFilesRequired = false;
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_SetMode(DctsDepthSession *session, uint32_t mode)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        const char *scheme = nullptr;
        switch (mode) {
        case DCTS_DEPTH_BOARD: scheme = Board; break;
        case DCTS_DEPTH_BOARD_HIGH_ACCURACY: scheme = BoardHighAccuracy; break;
        case DCTS_DEPTH_HOST_ASSISTED: scheme = RvSessionController::ReuseLatestTofImagingScheme; break;
        default: return DCTS_DEPTH_INVALID_ARGUMENT;
        }
        if (sdk.controller->deviceProfile().id.isEmpty()) return DCTS_DEPTH_DEVICE_NOT_FOUND;
        if (sdk.busy()) return DCTS_DEPTH_BUSY;
        if (mode == DCTS_DEPTH_HOST_ASSISTED && !sdk.controller->userStopped()
            && sdk.prepareInference() != DCTS_DEPTH_OK) return DCTS_DEPTH_BUSY;
        sdk.error.clear();
        sdk.controller->selectImagingScheme(QLatin1String(scheme), {});
        return modeOf(*sdk.controller) == mode ? DCTS_DEPTH_OK : DCTS_DEPTH_UNAVAILABLE;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Start(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        if (!sdk.controller->deviceProfile().ready()) return DCTS_DEPTH_DEVICE_NOT_FOUND;
        if (sdk.busy()) return DCTS_DEPTH_BUSY;
        if (sdk.controller->hostHdAssist() && sdk.prepareInference() != DCTS_DEPTH_OK) return DCTS_DEPTH_BUSY;
        sdk.error.clear();
        sdk.boardFilesRequired = false;
        sdk.controller->start();
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Stop(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        sdk.error.clear();
        sdk.controller->stop();
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Reconnect(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        if (!sdk.controller->deviceProfile().ready()) return DCTS_DEPTH_DEVICE_NOT_FOUND;
        if (sdk.busy()) return DCTS_DEPTH_BUSY;
        if (sdk.controller->hostHdAssist() && sdk.prepareInference() != DCTS_DEPTH_OK) return DCTS_DEPTH_BUSY;
        sdk.error.clear();
        sdk.boardFilesRequired = false;
        sdk.controller->reconnectNow();
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_DetectInferenceDevices(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        auto *settings = sdk.controller->inferenceSettings();
        if (settings->scanning()) return DCTS_DEPTH_BUSY;
        settings->detectDevices();
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_ListInferenceDevices(DctsDepthSession *session, char *buffer, uint32_t *size)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        const auto *settings = sdk.controller->inferenceSettings();
        auto status = settings->diagnostics();
        status["scanning"] = settings->scanning();
        status["benchmarking"] = settings->benchmarking();
        return copyJson(QJsonDocument(status), buffer, size);
    }, true);
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_SelectInferenceDevice(DctsDepthSession *session, const char *backendId, uint32_t remember)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        if (!backendId || !*backendId || remember > 1) return DCTS_DEPTH_INVALID_ARGUMENT;
        auto *settings = sdk.controller->inferenceSettings();
        if (settings->scanning()) return DCTS_DEPTH_BUSY;
        return settings->choose(QString::fromUtf8(backendId), remember != 0)
            ? DCTS_DEPTH_OK : DCTS_DEPTH_UNAVAILABLE;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_BenchmarkInferenceDevices(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        auto *settings = sdk.controller->inferenceSettings();
        if (settings->scanning()) return DCTS_DEPTH_BUSY;
        settings->benchmarkDevices();
        return DCTS_DEPTH_OK;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_CheckUsb(DctsDepthSession *session)
{
    return withSession(session, [](DctsDepthSession &sdk) {
        if (sdk.busy() || !sdk.controller->userStopped()) return DCTS_DEPTH_BUSY;
        sdk.prepareUsb();
        if (!sdk.usb) return DCTS_DEPTH_UNAVAILABLE;
        return checkRvUsbDevice(sdk.usb) ? DCTS_DEPTH_OK : DCTS_DEPTH_BUSY;
    });
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_GetUsbStatus(DctsDepthSession *session, char *buffer, uint32_t *size)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        return copyJson(QJsonDocument(rvUsbOperationStatus(sdk.usb)), buffer, size);
    }, true);
}

DctsDepthResult DCTS_DEPTH_CALL DctsDepth_EnrollUsb(DctsDepthSession *session, uint64_t checkId, uint32_t startAfter)
{
    return withSession(session, [&](DctsDepthSession &sdk) {
        if (!checkId || startAfter > 1) return DCTS_DEPTH_INVALID_ARGUMENT;
        if (sdk.busy() || !sdk.controller->userStopped()) return DCTS_DEPTH_BUSY;
        return enrollCheckedRvUsbDevice(sdk.usb, checkId, startAfter != 0) ? DCTS_DEPTH_OK : DCTS_DEPTH_UNAVAILABLE;
    });
}
