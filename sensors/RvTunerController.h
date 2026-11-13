#pragma once
#include "RvTunerProtocol.h"
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QTcpSocket>
#include <functional>
#include <memory>
class RvSessionController;
class DCTS_DEPTH_INTERNAL RvTunerController final:public QObject {
    Q_OBJECT
public:
    enum State {Idle,Reading,Preflight,Handover,Running,Applying,Saving,Stopping,Fault};Q_ENUM(State)
    explicit RvTunerController(RvSessionController *session);
    ~RvTunerController() override;
    State state() const{return m_state;}
    bool exclusive() const;
    bool pending() const{return m_process||m_testPending;}
    bool shutdownPending() const{return pending()||exclusive();}
    QString detail() const{return m_detail;}
    QJsonObject savedSettings() const{return m_saved;}
    QJsonObject appliedSettings() const{return m_applied;}
    QJsonObject inspection() const{return m_info;}
    double fps() const{return m_fps;}
    void read();
    void start(const RvTuner::Settings &);
    void apply(const RvTuner::Settings &);
    void persist();
    void stop();
signals:
    void changed();
    void configurationRead(const QJsonObject &);
    void frameReady(const RvTuner::Frame &);
    void cleared(const QString &);
    void shutdownFinished();
private:
    friend int runRvTunerTests(const QString &);
    void setState(State,const QString &);
    void launch(const QString &,QJsonObject);
    void completed(quint64,const QString &,const QJsonObject &,const QString &);
    void receive();
    void clearReceiver(const QString &);
    void fail(const QString &);
    QJsonObject request() const;
    RvSessionController *m_session;
    State m_state=Idle;QString m_detail,m_token,m_oldToken,m_revision,m_deviceId,m_worker,m_host;
    QJsonObject m_info,m_saved,m_applied;RvTuner::Settings m_requested;
    QPointer<QProcess> m_process;QPointer<QTcpSocket> m_socket;
    QTimer m_deadline,m_heartbeat,m_expiry;
    QElapsedTimer m_rateClock;int m_received=0;double m_fps=0;
    QElapsedTimer m_boardClock;quint64 m_boardNow=0;
    quint64 m_operation=0,m_epoch=0,m_sessionEpoch=0;
    bool m_cancel=false,m_readOnly=false,m_destroying=false;
    std::unique_ptr<RvTuner::Decoder> m_decoder;
    // Formal offline tests inject completions, never an alternate live endpoint.
    std::function<void(const QString &,const QJsonObject &,quint64)> m_testTransport;
    bool m_testPending=false;
};
