#include "RvSessionController.h"
#include "../AppEdition.h"
#include "DepthSdkClient.h"
#include "RvAssistDepth.h"
#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include "RvHdVideoStream.h"
#include <QRegularExpression>
#include <QSettings>
#include <cmath>
#include <limits>

namespace {
bool sdkGeneration(const QString &value)
{
    static const QRegularExpression token("^[0-9a-f]{32}$");
    return token.match(value).hasMatch();
}

QString orientationKey(const RvDeviceProfile &device)
{
    return "depth/sdkUsbInputRotation/" + device.serial;
}

struct SdkPair {
    const RvTofFrame *tof = nullptr;
    qint64 deltaNs = 0;
    double uncertaintyMs = 0;
    double ageMs = 0;
};

SdkPair selectSdkPair(const std::deque<RvTofFrame> &frames, qint64 rgbNs, qint64 nowNs)
{
    SdkPair selected;
    for (const auto &frame : frames) {
        const auto &m = frame.metadata;
        const double boardAge = m["raw_board_age_ms"].toDouble(-1);
        const double roundtrip = m["raw_request_roundtrip_ms"].toDouble(-1);
        const qint64 dispatch = m["raw_host_dispatch_ns"].toString().toLongLong();
        const double uncertainty = roundtrip * .5;
        if (dispatch <= 0 || dispatch > nowNs || !std::isfinite(boardAge) || boardAge < 0 || boardAge > 220
            || !std::isfinite(roundtrip) || roundtrip < 0 || uncertainty > RvTofFrame::PairClockUncertaintyMs) continue;
        const qint64 sampleNs = dispatch - std::llround((boardAge + uncertainty) * 1e6);
        const qint64 delta = sampleNs - rgbNs;
        const double age = (nowNs - sampleNs) / 1e6 + uncertainty;
        const double distance = std::abs(delta) / 1e6 + uncertainty;
        if (age > 1000 || distance > RvTofFrame::FreshRgbPairBudgetMs) continue;
        if (selected.tof && m["tof_sequence"].toInteger() <= selected.tof->metadata["tof_sequence"].toInteger()) continue;
        selected = {&frame, delta, uncertainty, age};
    }
    return selected;
}
}

bool RvSessionController::usesDepthSdk() const
{
    return m_sdk && qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") != "1";
}

bool RvSessionController::busy() const
{
    return m_tunerExclusive || m_controlPending || m_waitingForSelectedFrame || m_transitioning
        || (usesDepthSdk() && m_sdk->busy());
}

bool RvSessionController::retainingRecoveryDisplay() const
{
    return !m_destroying && !m_userStopped && (usesDepthSdk() ? m_sdk->retainingRecoveryDisplay()
                                                               : m_recoveryDisplayTimer.isActive());
}

void RvSessionController::initializeDepthSdk()
{
    if (m_sdk) return;
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") != "1") {
        try {
            const RvDeviceStore store(m_deviceStorePath);
            QSettings settings;
            for (const auto &profile : store.profiles()) {
                const auto key = orientationKey(profile);
                if (!settings.contains(key)) {
                    const auto previous = "depth/sdkInputRotation/" + profile.fingerprint();
                    settings.setValue(key, settings.value(previous, effectiveHostInputRotation(profile)));
                }
            }
            settings.sync();
        } catch (const std::exception &ex) {
            m_deviceError = QString::fromUtf8(ex.what());
        }
    }
    m_sdk = new DepthSdkClient(m_deviceStorePath, this);
    if (const auto mode = RvDtofCandidates::sdkMode(m_dtofMode)) m_sdk->setMode(mode);
    connect(m_sdk, &DepthSdkClient::deviceChanged, this, &RvSessionController::loadSdkDevice);
    connect(m_sdk, &DepthSdkClient::stateChanged, this, &RvSessionController::updateSdkState);
    connect(m_sdk, &DepthSdkClient::depthFrameReady, this, &RvSessionController::acceptSdkDepth);
    connect(m_sdk, &DepthSdkClient::cameraFrameReady, this, &RvSessionController::acceptSdkVideo);
    connect(m_sdk, &DepthSdkClient::streamCleared, this, [this] {
        if (usesDepthSdk()) resetSdkPresentation(tr("Waiting for SDK camera frames..."));
    });
    connect(m_sdk, &DepthSdkClient::depthStreamCleared, this, [this](const QString &reason) {
        if (usesDepthSdk()) resetSdkPresentation(reason);
    });
    connect(m_sdk, &DepthSdkClient::recoveryDisplayExpired, this, &RvSessionController::recoveryDisplayExpired);
    connect(m_sdk, &DepthSdkClient::imagingSwitchAborted, this, [this] {
        cancelImagingSwitch(tr("The SDK imaging switch did not complete."));
    });
    loadSdkDevice();
}

void RvSessionController::loadSdkDevice()
{
    if (!usesDepthSdk() || m_destroying) return;
    try {
        auto active = RvDeviceStore(m_deviceStorePath).active();
        if (!active.id.isEmpty()) {
            const int fallback = effectiveHostInputRotation(active);
            const int rotation = QSettings().value(orientationKey(active), fallback).toInt();
            active.inputRotation = rotation == 180 ? 180 : 0;
        }
        m_deviceError.clear();
        if (m_device.json() == active.json() && m_assist->inputRotationDegrees() == active.inputRotation) return;
        resetSdkPresentation(tr("Device changed"));
        m_device = std::move(active);
        QUrl direct;
        if (!m_device.serviceInstance.isEmpty()) {
            direct.setScheme(QStringLiteral("cx2"));
            direct.setHost(m_device.host);
            direct.setPort(8943);
        }
        m_streamUrl = direct.toString();
        if (!m_device.id.isEmpty()) m_assist->setInputRotationDegrees(m_device.inputRotation);
        emit deviceChanged();
        emit configurationChanged();
    } catch (const std::exception &ex) {
        m_deviceError = QString::fromUtf8(ex.what());
        setState(tr("Error"), m_deviceError);
    }
}

void RvSessionController::setSdkInputRotation(int degrees)
{
    if (AppEdition::Customer || !usesDepthSdk() || m_device.id.isEmpty() || !userStopped() || busy()
        || (degrees != 0 && degrees != 180)) return;
    QSettings().setValue(orientationKey(m_device), degrees);
    loadSdkDevice();
}

void RvSessionController::updateSdkState()
{
    if (!usesDepthSdk() || m_destroying) return;
    m_userStopped = m_sdk->userStopped() || m_sdk->stopping();
    m_videoFps = m_sdk->videoFps();
    switch (m_sdk->connectionStage()) {
    case CXWL_DEPTH_STAGE_CHECKING: setAutomaticConnectionStage(AutomaticConnectionStage::CheckingDevice); break;
    case CXWL_DEPTH_STAGE_STARTING: setAutomaticConnectionStage(AutomaticConnectionStage::StartingCapture); break;
    case CXWL_DEPTH_STAGE_WAITING_FOR_DEPTH: setAutomaticConnectionStage(AutomaticConnectionStage::WaitingForDepth); break;
    default: setAutomaticConnectionStage(AutomaticConnectionStage::Idle); break;
    }
    const auto status = m_sdk->captureStatus();
    if (status == DepthSdkClient::CaptureStatus::Failed || status == DepthSdkClient::CaptureStatus::StopFailed) {
        cancelImagingSwitch(m_sdk->captureStatusText());
        setState(tr("Error"), m_sdk->captureStatusText());
    } else if (m_sdk->stopping()) setState(tr("Stopping"), m_sdk->captureStatusText());
    else if (m_userStopped) setState(tr("Stopped"), tr("Camera is not started"));
    else if (m_sdk->recovering()) setState(tr("Connecting"), tr("Recovering the SDK capture session..."));
    else if (status == DepthSdkClient::CaptureStatus::Running) {
        const auto grid = RvDtofCandidates::nativeDepthGrid(m_dtofMode);
        setState(tr("Playing"), tr("H.265 video and native %1×%2 ToF").arg(grid.width()).arg(grid.height()));
    } else setState(tr("Connecting"), m_sdk->captureStatusText());
    updateState();
}

void RvSessionController::resetSdkPresentation(const QString &reason)
{
    ++m_presentationEpoch;
    m_sdkGeneration.clear();
    m_sdkBoot.clear();
    m_dtofToken.clear();
    m_dtofReceipt = {};
    clearCurrentFrame(reason);
}

bool RvSessionController::acceptSdkIdentity(const QString &generation, quint64 epoch)
{
    if (!usesDepthSdk() || m_userStopped || m_sdk->stopping()
        || !sdkGeneration(generation)) return false;
    if (m_sdkGeneration != generation || m_sdkEpoch != epoch) {
        resetSdkPresentation(tr("Waiting for the current SDK capture generation..."));
        m_sdkGeneration = generation;
        m_sdkEpoch = epoch;
        m_dtofToken = generation;
    }
    return !m_userStopped && !m_sdk->stopping();
}

void RvSessionController::acceptSdkDepth(const QByteArray &depth, QJsonObject metadata, const RvTofFrame &native)
{
    const auto generation = metadata["source_generation"].toString();
    const auto grid = RvDtofCandidates::nativeDepthGrid(m_dtofMode);
    if (grid.isEmpty() || metadata["candidate_mode"] != m_dtofMode
        || metadata["width"] != grid.width() || metadata["height"] != grid.height()
        || metadata["sensor_mode"] != RvDtofCandidates::nativeSensorMode(m_dtofMode)
        || metadata["dtof_fov_correction"] != RvCalibration::Fov || depth.size() != grid.width() * grid.height() * 4
        || !acceptSdkIdentity(generation, quint64(metadata["generation"].toInteger()))) return;
    const auto boot = metadata["boot_id"].toString();
    if (boot.isEmpty() || (!m_sdkBoot.isEmpty() && boot != m_sdkBoot)) return;
    m_sdkBoot = boot;
    m_dtofReceipt["boot_id"] = boot;
    const auto epoch = m_presentationEpoch;
    if (native.isValid()) {
        if (hostHdAssist() && native.metadata["tof_warning_flags"] == 0) {
            if (m_hdNativeFrames.size() == 16) m_hdNativeFrames.pop_front();
            m_hdNativeFrames.push_back(native);
        }
        setProperty("rvRawTofSequence", native.metadata["tof_sequence"].toVariant());
        emit tofFrameReady(native);
    }
    if (m_userStopped || epoch != m_presentationEpoch || hostHdAssist()) return;
    metadata["generation"] = qint64(epoch);
    metadata["imaging_scheme"] = currentImagingScheme();
    m_currentMetadata = metadata;
    finishImagingSwitch();
    if (m_userStopped || epoch != m_presentationEpoch) return;
    emit depthFrameReady(depth, metadata);
    if (!m_userStopped && epoch == m_presentationEpoch) emit depthSnapshotReady(depth, metadata, {});
}

void RvSessionController::acceptSdkVideo(const QImage &image, quint64 sequence, qint64 decodedNs,
                                        quint64 epoch, const QString &generation)
{
    if (!acceptSdkIdentity(generation, epoch)) return;
    const auto presentation = m_presentationEpoch;
    setProperty("rvTransport", "cxwl_depth_sdk");
    setProperty("rvGeneration", generation);
    setProperty("rvBootId", m_sdkBoot);
    setProperty("rvDecodedSequence", sequence);
    setProperty("rvPresentedFrames", property("rvPresentedFrames").toULongLong() + 1);
    emit cameraFrameReady(image);
    if (m_userStopped || presentation != m_presentationEpoch || !hostHdAssist()
        || !assistDepthEnabled() || m_sdkBoot.isEmpty() || decodedNs <= 0) return;

    const auto model = assistHostModel();
    const auto &view = RvCameraView::profile(model);
    QJsonObject source{{"source_protocol", "HDA1"}, {"source_transport", "cxwl_depth_sdk"},
        {"sdk_version", CXWL_DEPTH_VERSION}, {"device_transport", "cx2_usb"},
        {"source_rgb_width", image.width()}, {"source_rgb_height", image.height()},
        {"rgb_width", view.inputWidth}, {"rgb_height", view.inputHeight}, {"model", "Native"},
        {"camera_coordinate_frame", "calibrated_reference"}, {"camera_mount_rotation_degrees", m_device.cameraMountRotation},
        {"work_mode", "host_rgb"}, {"dtof_present", false}, {"assist_experiment", assistExperiment()},
        {"generation", qint64(presentation)}, {"generation_source", "host_presentation_epoch"},
        {"source_generation", generation}, {"source_candidate_mode", m_dtofMode},
        {"source_engine", RvCaptureContract::NativeEngine}, {"boot_id", m_sdkBoot},
        {"frame_id", qint64(sequence)}, {"video_decoder_sequence", qint64(sequence)},
        {"host_decoded_monotonic_ns", decodedNs}, {"rectify_map_sha256", QLatin1String(view.mapSha)},
        {"width", 0}, {"height", 0}, {"assist_camera_profile", "wide"},
        {"assist_tof_fov_correction", RvCalibration::Fov}};

    const auto pair = selectSdkPair(m_hdNativeFrames, decodedNs, RvHdVideoStream::nowNs());
    QByteArray samples;
    if (pair.tof && pair.tof->metadata["tof_timestamp_ns"].toInteger() > pair.deltaNs) {
        const auto &measurement = pair.tof->metadata;
        for (const auto *key : {"tof_sequence", "tof_timestamp_ns", "tof_reused", "dtof_width", "dtof_height",
             "dtof_bytes", "dtof_format", "dtof_range_unit", "dtof_source", "dtof_coordinate", "dtof_fov_correction",
             "tof_warning_flags", "processing_fov_correction", "native_range_source"}) source[key] = measurement[key];
        source["work_mode"] = "rgb_tof";
        source["dtof_present"] = true;
        source["rgb_timestamp_ns"] = measurement["tof_timestamp_ns"].toInteger() - pair.deltaNs;
        source["pair_delta_ms"] = pair.deltaNs / 1e6;
        source["pair_clock_uncertainty_ms"] = pair.uncertaintyMs;
        source["pair_clock_source"] = QLatin1String(RvTofFrame::SdkPairClock);
        source["pairing_policy"] = QLatin1String(RvTofFrame::FreshRgbReusePolicy);
        source["measurement_age_at_pair_ms"] = pair.ageMs;
        samples = pair.tof->samples;
    }
    m_currentMetadata = source;
    setProperty("rvFrameId", sequence);
    prepareHostHdFrame(image, source, presentation, samples);
}
