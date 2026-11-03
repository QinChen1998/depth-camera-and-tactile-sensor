#pragma once
#include "RvPhotonFrame.h"
#include "RvLiteSrContour.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <thread>

// Explicit optional host preview, fed by the existing matched PHC1 channel.
// Does not acquire sensors, project RGB, configure the board or control motion.
class RvLiteSrDepth final : public QObject {
    Q_OBJECT
public:
    explicit RvLiteSrDepth(QObject *parent=nullptr);
    ~RvLiteSrDepth() override;
    static bool supportsModel(const QString &model);
    static bool isNearModel(const QString &model);
    static bool runtimeAvailable(const QString &model);
    static float minimumDepthM(const QString &model);
    static bool compatibleFrame(const RvPhotonFrame &frame);
    static QString inferenceModel(const QString &model);
    QString model() const {return m_model;}
    QString status() const {return m_status;}
    bool selected() const {return !m_model.isEmpty();}
    int loadingState() const {return m_loading;}
    int whiteCounts() const {return m_white;}
    int inputRotation() const {return m_rotation;}
    RvLiteSrContour::RepairSettings repairSettings() const {return m_repairSettings;}
    void setModel(const QString &model);
    void setWhiteCounts(int counts);
    void setInputRotation(int degrees);
    void setRepairSettings(const RvLiteSrContour::RepairSettings &settings);
    void submit(const RvPhotonFrame &frame,quint64 epoch);
    void cancel(const QString &reason);
    void pauseInput(const QString &reason);
signals:
    void changed();
    void cleared(const QString &reason);
    void resultReady(const QByteArray &depth,const QJsonObject &metadata,const QByteArray &coverage);
private:
    friend int runRvLiteSrTests(const QString &,bool);
    struct Input {RvPhotonFrame frame;QElapsedTimer clock;quint64 epoch=0,request=0,revision=0;};
    void startWorker();
    void sendLatest();
    void readOutput();
    void publishResult(const QByteArray &depth,const QJsonObject &diagnostic,
                       const QByteArray &coverage={},const QJsonObject &postprocess={});
    void refineResult(const QByteArray &depth,const QJsonObject &diagnostic);
    void fail(const QString &reason);
    void setStatus(const QString &reason);
    QString m_model,m_status,m_token,m_root,m_runtimeSha,m_checkpointSha,m_backend;
    QProcess m_process;
    std::thread m_refinement;
    QTimer m_timeout,m_expiry;
    QByteArray m_buffer,m_stderr;
    Input m_pending,m_current;
    RvLiteSrContour::RepairSettings m_repairSettings;
    quint64 m_request=0,m_lastSequence=0,m_lastStamp=0,m_inputRevision=0;
    int m_white=512,m_rotation=0,m_loading=0;
    bool m_ready=false,m_busy=false,m_failed=false,m_stopping=false,m_destroying=false;
};
int runRvLiteSrTests(const QString &path,bool actualRuntime=false);
