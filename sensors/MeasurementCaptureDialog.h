#pragma once

#include "DepthMeasurement.h"
#include <QDialog>
#include <QFutureWatcher>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class RvSessionController;
struct RvTofFrame;

class MeasurementCaptureDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MeasurementCaptureDialog(RvSessionController *session, QWidget *parent = nullptr);
    ~MeasurementCaptureDialog() override;
    bool recording() const;
    void stopRecording();

signals:
    void recordingFinished();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    void refresh();
    void startRecording();
    void finishRecording();
    void acceptNative(const RvTofFrame &frame);
    void acceptAssist(const QByteArray &depth, const QJsonObject &metadata);
    void submitRv(DepthMeasurement::Frame frame);
    void checkRvSession();

    RvSessionController *m_session;
    QComboBox *m_source;
    QSpinBox *m_duration;
    QDoubleSpinBox *m_distance;
    QLineEdit *m_scene, *m_directory;
    QLabel *m_sourceLabel, *m_durationLabel, *m_distanceLabel, *m_sceneLabel, *m_directoryLabel, *m_hint, *m_status;
    QPushButton *m_browse, *m_start, *m_stop, *m_open, *m_close;
    QProgressBar *m_progress;
    QTimer *m_timer;
    QFutureWatcher<DepthMeasurement::Result> m_watcher;
    std::shared_ptr<DepthMeasurement::Job> m_job;
    DepthMeasurement::Result m_last;
    QString m_scheme;
    quint64 m_epoch = 0;
    int m_mode = -1, m_seconds = 30;
    bool m_rv = false, m_assisted = false, m_bound = false, m_startedRv = false;
};
