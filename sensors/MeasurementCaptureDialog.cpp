#include "MeasurementCaptureDialog.h"
#include "RvCameraView.h"
#include "RvCalibration.h"
#include "RvSessionController.h"
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {
QJsonObject fields(const QJsonObject &metadata, std::initializer_list<const char *> keys)
{
    QJsonObject result;
    for (const auto *key : keys) {
        if (metadata.contains(QLatin1String(key)))
            result.insert(QLatin1String(key), metadata.value(QLatin1String(key)));
    }
    return result;
}
}

MeasurementCaptureDialog::MeasurementCaptureDialog(RvSessionController *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setObjectName("measurementCaptureDialog");
    setMinimumWidth(540);
    m_source = new QComboBox(this);
    m_source->setObjectName("measurementSource");
    m_source->addItem(QString(), QStringLiteral("rv1103"));
    m_source->addItem(QString(), QStringLiteral("gemini335"));
    m_duration = new QSpinBox(this);
    m_duration->setObjectName("measurementDuration");
    m_duration->setRange(30, 60);
    m_duration->setValue(30);
    m_distance = new QDoubleSpinBox(this);
    m_distance->setObjectName("measurementDistance");
    m_distance->setRange(0, 100000);
    m_distance->setDecimals(1);
    m_scene = new QLineEdit(this);
    m_scene->setObjectName("measurementScene");
    m_directory = new QLineEdit(this);
    m_directory->setObjectName("measurementDirectory");
    m_directory->setText(QSettings().value("measurement/directory",
        QSettings().value("depthMap/saveDirectory", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))).toString());
    m_browse = new QPushButton(this);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(m_directory, 1);
    directoryRow->addWidget(m_browse);
    auto *form = new QFormLayout;
    m_sourceLabel = new QLabel(this);
    m_durationLabel = new QLabel(this);
    m_distanceLabel = new QLabel(this);
    m_sceneLabel = new QLabel(this);
    m_directoryLabel = new QLabel(this);
    form->addRow(m_sourceLabel, m_source);
    form->addRow(m_durationLabel, m_duration);
    form->addRow(m_distanceLabel, m_distance);
    form->addRow(m_sceneLabel, m_scene);
    form->addRow(m_directoryLabel, directoryRow);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_status = new QLabel(this);
    m_status->setObjectName("measurementStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName("measurementProgress");

    m_start = new QPushButton(this);
    m_start->setObjectName("measurementStart");
    m_stop = new QPushButton(this);
    m_stop->setObjectName("measurementStop");
    m_open = new QPushButton(this);
    m_close = new QPushButton(this);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_start);
    actions->addWidget(m_stop);
    actions->addStretch();
    actions->addWidget(m_open);
    actions->addWidget(m_close);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_hint);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addLayout(actions);

    connect(m_browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("Select measurement folder"), m_directory->text());
        if (!path.isEmpty())
            m_directory->setText(path);
    });
    connect(m_start, &QPushButton::clicked, this, &MeasurementCaptureDialog::startRecording);
    connect(m_stop, &QPushButton::clicked, this, &MeasurementCaptureDialog::stopRecording);
    connect(m_close, &QPushButton::clicked, this, &QWidget::hide);
    connect(m_open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_last.directory)); });
    connect(m_source, &QComboBox::currentIndexChanged, this, &MeasurementCaptureDialog::refresh);
    connect(m_directory, &QLineEdit::textChanged, this, &MeasurementCaptureDialog::refresh);
    connect(m_session, &RvSessionController::tofFrameReady, this, &MeasurementCaptureDialog::acceptNative);
    connect(m_session, &RvSessionController::assistDepthSnapshotReady, this, &MeasurementCaptureDialog::acceptAssist);
    connect(m_session, &RvSessionController::dtofCandidateSelectionChanged, this, &MeasurementCaptureDialog::checkRvSession);
    connect(m_session, &RvSessionController::stateChanged, this, &MeasurementCaptureDialog::checkRvSession);
    connect(m_session, &RvSessionController::depthStreamCleared, this, [this] {
        if (m_job && m_rv && !m_assisted && m_bound)
            m_job->stop(tr("RV1103 depth stream ended during recording"));
    });
    connect(m_session, &RvSessionController::assistDepthStreamCleared, this, [this] {
        if (m_job && m_rv && m_assisted && m_bound)
            m_job->stop(tr("RV1103 depth stream ended during recording"));
    });
    connect(&m_watcher, &QFutureWatcher<DepthMeasurement::Result>::finished, this, &MeasurementCaptureDialog::finishRecording);
    m_timer = new QTimer(this);
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, &MeasurementCaptureDialog::refresh);
    retranslate();
}

MeasurementCaptureDialog::~MeasurementCaptureDialog()
{
    if (m_job) {
        m_job->stop();
        m_watcher.waitForFinished();
    }
}

bool MeasurementCaptureDialog::recording() const
{
    return m_job != nullptr;
}

void MeasurementCaptureDialog::stopRecording()
{
    if (m_job)
        m_job->stop();
    refresh();
}

void MeasurementCaptureDialog::startRecording()
{
    if (m_job)
        return;
    const DepthMeasurement::Options options{m_directory->text().trimmed(), m_source->currentData().toString(),
                                            m_scene->text().trimmed(), m_duration->value(), m_distance->value()};
    QSettings().setValue("measurement/directory", options.parentDirectory);
    m_last = {};
    m_rv = options.source == "rv1103";
    m_seconds = options.seconds;
    m_bound = false;
    m_scheme = m_session->currentImagingScheme();
    m_mode = m_session->dtofCandidateMode();
    m_assisted = m_session->hostHdAssist();
    m_startedRv = m_rv && m_session->userStopped() && qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") != "1";
    m_job = std::make_shared<DepthMeasurement::Job>();
    const auto work = m_job;
    const bool rv = m_rv;
    m_watcher.setFuture(QtConcurrent::run([options, work, rv] {
        if (rv)
            return DepthMeasurement::record(options, *work, [&] { return work->take(); });
        return DepthMeasurement::recordGemini(options, *work);
    }));
    m_timer->start();
    refresh();
    if (m_startedRv)
        m_session->start();
}

void MeasurementCaptureDialog::finishRecording()
{
    m_last = m_watcher.result();
    m_timer->stop();
    m_job.reset();
    if (m_startedRv && m_session->currentImagingScheme() == m_scheme
        && (!m_bound || m_session->presentationEpoch() == m_epoch))
        m_session->stop();
    m_startedRv = false;
    refresh();
    emit recordingFinished();
}

void MeasurementCaptureDialog::checkRvSession()
{
    if (m_job && m_rv && (m_session->currentImagingScheme() != m_scheme
        || (m_bound && m_session->presentationEpoch() != m_epoch)))
        m_job->stop(tr("RV1103 session or imaging mode changed during recording"));
}

void MeasurementCaptureDialog::submitRv(DepthMeasurement::Frame frame)
{
    checkRvSession();
    if (m_job->stopping.load() || m_session->userStopped())
        return;
    if (!m_bound) {
        m_epoch = m_session->presentationEpoch();
        m_bound = true;
    }
    m_job->submit(std::move(frame));
}

void MeasurementCaptureDialog::acceptNative(const RvTofFrame &frame)
{
    if (!m_job || !m_rv || m_assisted || frame.metadata["candidate_mode"] != m_mode
        || frame.columns() != 48 || frame.rows() != 32 || !frame.isValid()
        || frame.metadata["processing_fov_correction"] != RvCalibration::Fov)
        return;
    const auto &meta = frame.metadata;
    auto format = QJsonObject{{"encoding", "rv_uq2_snr_valid"}, {"coordinate", "tof_sensor_radial"},
        {"width", 48}, {"height", 32}, {"scale_mm_per_code", .25}, {"candidate_mode", m_mode},
        {"imaging_scheme", m_scheme}, {"source_clock", "rv_board_monotonic"},
        {"sensor_fov", meta["sensor_fov_correction"]}, {"processing_fov", RvCalibration::Fov},
        {"calibration_release", meta["calibration_release"].toString(QLatin1String(RvCalibration::Release))}};
    const bool sdk = meta["native_range_source"] == "sdk_radial_f32_and_quality";
    const auto sensor = meta["tof_sensor_readback"].toObject();
    if (m_session->native48ExtendedIntegration()
        && (!sdk || sensor.contains("main_iterations_k"))
        && sensor["main_iterations_k"] != RvCaptureContract::Native48ExtendedIntegrationK) {
        m_job->stop(RvSessionController::tr("The sensor did not confirm 1200K main integration."));
        return;
    }
    if (!sensor.isEmpty())
        format["sensor_readback"] = sensor;
    if (sdk) format["native_range_source"] = meta["native_range_source"];
    submitRv({frame.samples, format, fields(meta, {"generation", "tof_warning_flags", "raw_age_ms"}),
              meta["tof_sequence"].toInteger(), meta["tof_timestamp_ns"].toInteger(), DepthMeasurement::nowNs()});
}

void MeasurementCaptureDialog::acceptAssist(const QByteArray &depth, const QJsonObject &meta)
{
    if (!m_job || !m_rv || !m_assisted || meta["depth_unit"] != "camera-Z_m_float32" || !RvCameraView::validOutputGrid(meta))
        return;
    const auto &view = RvCameraView::outputProfile(meta["assist_host_model"].toString());
    if (depth.size() != view.depthBytes()) return;
    const auto format = QJsonObject{{"encoding", "f32_le_metres"}, {"coordinate", "rv_rgb_camera_z"},
        {"width", view.outputWidth}, {"height", view.outputHeight}, {"invalid_code", 0},
        {"depth_K", meta["depth_K"]}, {"imaging_scheme", m_scheme},
        {"source_clock", "host_rgb_decode_monotonic"}};
    submitRv({depth, format, fields(meta, {"source_generation", "video_pts_us", "rgb_timestamp_ns",
                                         "tof_sequence", "tof_timestamp_ns", "tof_reused", "pair_delta_ms", "pair_clock_source",
                                         "depth_source", "assist_fusion"}),
              meta["frame_id"].toInteger(), meta["host_decoded_monotonic_ns"].toInteger(), DepthMeasurement::nowNs()});
}

void MeasurementCaptureDialog::refresh()
{
    const bool active = recording();
    for (auto *field : {static_cast<QWidget *>(m_source), static_cast<QWidget *>(m_duration),
                        static_cast<QWidget *>(m_distance), static_cast<QWidget *>(m_scene),
                        static_cast<QWidget *>(m_directory), static_cast<QWidget *>(m_browse)})
        field->setEnabled(!active);
    m_start->setEnabled(!active && !m_directory->text().trimmed().isEmpty());
    m_stop->setEnabled(active && !m_job->stopping.load());
    m_open->setEnabled(!active && !m_last.directory.isEmpty());
    m_directory->setToolTip(m_directory->text());
    m_hint->setText(m_source->currentData() == "rv1103"
        ? tr("Uses the imaging mode selected in Depth Map.")
        : tr("Records Gemini 335 native depth at 1280 × 800 and 15 FPS."));
    m_progress->setRange(0, m_seconds * 1000);
    const auto elapsedMs = active ? m_job->elapsedMs.load() : m_last.spanNs / 1'000'000;
    m_progress->setValue(int(qMin(elapsedMs, qint64(m_seconds) * 1000)));
    if (active) {
        if (m_job->stopping.load())
            m_status->setText(tr("Finishing recording…"));
        else if (!m_job->frames.load())
            m_status->setText(tr("Waiting for the first depth frame…"));
        else
            m_status->setText(tr("Recording · %1 s · %2 frames")
                              .arg(m_job->elapsedMs.load() / 1000.0, 0, 'f', 1).arg(m_job->frames.load()));
    } else if (m_last.status == "complete") {
        m_status->setText(tr("Saved %1 frames · %2 s").arg(m_last.frames).arg(m_last.spanNs / 1e9, 0, 'f', 2));
    } else if (!m_last.status.isEmpty()) {
        m_status->setText(m_last.detail.isEmpty() ? tr("Stopped · %1 frames saved").arg(m_last.frames)
            : tr("Recording ended · %1 frames saved\n%2").arg(m_last.frames).arg(m_last.detail));
    } else {
        m_status->setText(tr("Ready"));
    }
}

void MeasurementCaptureDialog::retranslate()
{
    setWindowTitle(tr("Measurement Capture"));
    m_sourceLabel->setText(tr("Device"));
    m_durationLabel->setText(tr("Duration"));
    m_distanceLabel->setText(tr("Measured distance"));
    m_sceneLabel->setText(tr("Scene"));
    m_directoryLabel->setText(tr("Output folder"));
    m_source->setItemText(0, QStringLiteral("RV1103"));
    m_source->setItemText(1, QStringLiteral("Gemini 335"));
    m_duration->setSuffix(tr(" s"));
    m_distance->setSpecialValueText(tr("Unspecified"));
    m_distance->setSuffix(tr(" mm"));
    m_browse->setText(tr("Browse…"));
    m_start->setText(tr("Start recording"));
    m_stop->setText(tr("Stop recording"));
    m_open->setText(tr("Open folder"));
    m_close->setText(tr("Hide"));
    refresh();
}

void MeasurementCaptureDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslate();
}
