#pragma once
#include "RvTofFrame.h"
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtCore/QPoint>
#include <QtGui/QImage>
#include <memory>
class QElapsedTimer;
struct RvAssistWorkerPaths;

// Optional native computation process. Receives paired snapshots from the one
// RV session; it never opens a camera, connects to the board or controls motion.
class RvAssistDepth final : public QObject {
    Q_OBJECT
public:
    explicit RvAssistDepth(QObject *parent=nullptr);
    ~RvAssistDepth() override;
    void setEnabled(bool enabled);
    void cancel(const QString &reason);
    void submitHd(const QImage &rectifiedPreview,const QJsonObject &source,quint64 epoch,
                  const QByteArray &nativeSamples={},const QByteArray &planarInput={});
    // Converts the rectified image to CHW RGB without changing its orientation.
    static QByteArray preparePlanarInput(const QImage &image);
    void setInputRotationDegrees(int degrees);
    QString hostModel() const { return m_hostModel; }
    static bool supportsHostModel(const QString &model);
    static bool isUniDepth(const QString &model);
    // File preflight only; runtime identity and GPU checks stay in the worker.
    static bool hostModelAvailable(const QString &model);
    static QString preflightError(const QString &model);
    static std::shared_ptr<void> prepareProbeWorker(QString &program,QString &runtime);
    int inputRotationDegrees() const {return m_inputRotation;}
    bool enabled() const {return m_enabled;}
    bool failed() const {return m_failed;}
signals:
    void inferenceUnavailable(const QString &explanation,const QString &detail);
    void loadingStateChanged(int state); // 0 stopped/failed, 1 loading, 2 ready
    void resultReady(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,quint64 epoch,const RvTofFrame &tof=RvTofFrame(),const QByteArray &diagnostic=QByteArray(),const QImage &inferenceInput=QImage());
    void cleared(const QString &reason);
    void statusChanged(const QString &status);
    void frameRejected(const QJsonObject &metadata,const QString &reason);
private:
    friend int runRvAssistDepthTests(const QString &reportPath);
    struct Input {
        QImage camera;
        RvTofFrame tof;
        quint64 epoch=0,request=0;
        qint64 receivedMs=0;
        double serializeMs=0;
        QByteArray planarInput;
    };
    void startWorker(bool forceStaging = false);
    bool takePending();
    void sendLatest();
    void readOutput();
    bool acceptResponse(const QJsonObject &response,const QByteArray &depth,const QByteArray &coverage,QElapsedTimer &responseClock);
    void handleTimeout();
    void fail(const QString &reason, bool tryNextBackend = true);
    static QString failureExplanation(const QString &reason);
    QString m_root,m_token;
    QString m_backend;
    int m_inferenceTimeoutMs=1500,m_resultAgeLimitMs=1000;
    const QString m_hostModel;
    std::unique_ptr<RvAssistWorkerPaths> m_workerPaths;
    QProcess m_process;
    QTimer m_timeout;
    QByteArray m_buffer,m_stderr;
    Input m_pending,m_current;
    quint64 m_request=0,m_dropped=0;
    quint64 m_lastHdTofSequence=0;
    double m_lastGuiPublishMs=0;
    bool m_enabled=false,m_ready=false,m_busy=false,m_stopping=false,m_failed=false,m_destroying=false,m_cancelling=false;
    int m_inputRotation=180; // Current RV camera mounting; independent of display rotation.
};

int runRvAssistWorker(const QString &runtimeRoot,const QString &token,const QString &hostModel=QString());
int runRvInferenceProbe(const QString &backend,const QString &reportPath);
