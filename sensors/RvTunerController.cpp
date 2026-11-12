#include "RvTunerController.h"
#include "RvSessionController.h"
#include "../RuntimeDiscovery.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkProxy>

RvTunerController::RvTunerController(RvSessionController *session):QObject(session),m_session(session){
    setObjectName("rvTunerController");m_deadline.setSingleShot(true);m_deadline.setInterval(95000);
    connect(&m_deadline,&QTimer::timeout,this,[this]{if(m_process)m_process->kill();else if(m_testPending)completed(m_operation,property("operation").toString(),{},tr("Device operation timed out; ownership and fault evidence are retained."));});
    m_heartbeat.setInterval(10000);connect(&m_heartbeat,&QTimer::timeout,this,[this]{if(m_state==Running&&!pending())launch("heartbeat",request());});
    m_expiry.setSingleShot(true);connect(&m_expiry,&QTimer::timeout,this,[this]{fail(tr("No fresh complete measurement; stopping the experiment."));});
}
RvTunerController::~RvTunerController(){m_destroying=true;++m_operation;clearReceiver({});m_heartbeat.stop();m_deadline.stop();if(m_process){m_process->disconnect(this);m_process->kill();}/* Board lease expires on an abnormal process exit; it never resumes Native. */}
bool RvTunerController::exclusive() const{return m_session->m_tunerExclusive;}
void RvTunerController::setState(State state,const QString &detail){m_state=state;m_detail=detail;emit changed();if(exclusive())m_session->setState(tr("dToF Parameter Tuning"),detail);}
QJsonObject RvTunerController::request() const{
    auto r=m_session->m_captureOwner;r["shared_receiving"]=!m_session->userStopped();r["token"]=m_token;r["old_token"]=m_oldToken;r["snapshot"]=m_info["snapshot"];r["boot_id"]=m_info["boot_id"];r["settings"]=m_applied;return r;
}
void RvTunerController::launch(const QString &action,QJsonObject req){
    if(pending())return;
    const auto operation=++m_operation;setProperty("operation",action);
    if(m_testTransport){m_testPending=true;m_testTransport(action,req,operation);emit changed();return;}
    try {
        RvDeviceStore store(m_session->deviceStorePath());
        const auto selected=store.byId(m_deviceId);
        // A read/edit in Device Manager cannot redirect an owned STOP to another board.
        if(store.revision()!=m_revision||selected.host!=m_host)throw std::runtime_error("Trusted device registry changed; restore its identity before stopping. The experiment lease will expire safely.");
        auto *p=new QProcess(this);m_process=p;p->setObjectName("rvTunerControlProcess");
        p->setProgram(m_worker);p->setArguments({"--tuner",action,"--device-store",m_session->deviceStorePath(),"--device-revision",m_revision,"--device-id",m_deviceId});
        p->setProcessEnvironment(RuntimeDiscovery::nativeWorkerEnvironment(m_worker));p->setWorkingDirectory(QFileInfo(m_worker).absolutePath());
        connect(p,&QProcess::started,this,[p,req]{p->write(QJsonDocument(req).toJson(QJsonDocument::Compact));p->closeWriteChannel();});
        connect(p,&QProcess::errorOccurred,this,[this,p,operation,action](QProcess::ProcessError error){if(error==QProcess::FailedToStart&&m_process==p){m_process=nullptr;p->deleteLater();completed(operation,action,{},p->errorString());}});
        connect(p,&QProcess::finished,this,[this,p,operation,action](int code,QProcess::ExitStatus exit){
            if(m_process!=p)return;m_process=nullptr;
            const auto bytes=p->readAllStandardOutput(),errors=p->readAllStandardError();p->deleteLater();
            QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);
            QString problem;
            if(code||exit!=QProcess::NormalExit)problem=QString::fromUtf8(errors).trimmed();
            else if(parse.error!=QJsonParseError::NoError||!doc.isObject())problem=tr("Malformed device operation receipt.");
            else if(doc.object()["device_registry_revision"]!=m_revision||doc.object()["trusted_device"].toObject()["id"]!=m_deviceId)problem=tr("Device identity changed; operation receipt rejected.");
            if((code||exit!=QProcess::NormalExit)&&problem.isEmpty())problem=tr("Device operation failed or timed out; check device logs.");
            completed(operation,action,doc.object(),problem);
        });
        p->start();m_deadline.start();emit changed();
    }catch(const std::exception &e){completed(operation,action,{},QString::fromUtf8(e.what()));}
}
void RvTunerController::read(){
    if(exclusive()||pending())return;
    try{
        RvDeviceStore store(m_session->deviceStorePath());const auto device=store.byId(m_session->deviceProfile().id);
        if(!device.ready())throw std::runtime_error("Select and verify a trusted RV device in Device Manager");
        const auto runtime=RuntimeDiscovery::rvControlForLaunch();if(runtime.executable.isEmpty())throw std::runtime_error(runtime.error.toStdString());
        m_worker=runtime.executable;m_deviceId=device.id;m_revision=store.revision();m_host=device.host;
        m_cancel=false;m_readOnly=true;m_sessionEpoch=m_session->presentationEpoch();setState(Reading,tr("Reading current native capture settings..."));launch("inspect",request());
    }catch(const std::exception &e){setState(Fault,QString::fromUtf8(e.what()));}
}
void RvTunerController::start(const RvTuner::Settings &s){
    if(exclusive()||pending()||m_state==Preflight||m_state==Handover)return;
    try{
        s.validate(); // Capacity errors must precede all handover effects.
        if(m_session->busy()||m_session->m_previewControl->state()!=QProcess::NotRunning)throw std::runtime_error("A shared imaging transition is still active; wait for it to finish");
        if(!m_session->controlledSource())throw std::runtime_error("Select the trusted RV device source before tuning");
        RvDeviceStore store(m_session->deviceStorePath());const auto device=store.byId(m_session->deviceProfile().id);
        if(!device.ready())throw std::runtime_error("Select and verify a trusted RV device in Device Manager");
        const auto runtime=RuntimeDiscovery::rvControlForLaunch();if(runtime.executable.isEmpty())throw std::runtime_error(runtime.error.toStdString());
        m_worker=runtime.executable;m_deviceId=device.id;m_revision=store.revision();m_host=device.host;
        m_requested=s;m_cancel=false;m_readOnly=false;m_sessionEpoch=m_session->presentationEpoch();
        setState(Preflight,tr("Checking parameters, installed runtime, device identity and actual capture owner..."));launch("inspect",request());
    }catch(const std::exception &e){setState(Fault,QString::fromUtf8(e.what()));}
}
void RvTunerController::completed(quint64 operation,const QString &action,const QJsonObject &r,const QString &error){
    if(m_destroying||operation!=m_operation)return;m_deadline.stop();m_testPending=false;
    if(!error.isEmpty()){
        if(action=="inspect"){setState(Fault,error);if(m_cancel)emit shutdownFinished();return;}
        if(action=="persist"){
            // A failed transaction is never reported as saved; refresh requires a new read.
            m_info.remove("snapshot");m_saved={};
        }
        clearReceiver(error);m_heartbeat.stop();setState(Fault,error);
        // Even a failed start may have acquired the new token. Stop only our identity.
        if(action!="stop"&&exclusive()){m_cancel=true;launch("stop",request());}
        else emit shutdownFinished();return;
    }
    if(action=="inspect"){
        m_info=r;m_saved=r["settings"].toObject();emit configurationRead(m_saved);
        if(m_readOnly||m_cancel){setState(Idle,m_cancel?tr("Cancelled before handover; shared acquisition was left running."):tr("Current settings read. Opening this module does not start capture."));if(m_cancel)emit shutdownFinished();return;}
        bool deviceUnchanged=false;
        try{RvDeviceStore store(m_session->deviceStorePath());deviceUnchanged=store.revision()==m_revision&&m_session->deviceProfile().id==m_deviceId;}catch(const std::exception &){}
        if(!deviceUnchanged||m_session->presentationEpoch()!=m_sessionEpoch||m_session->busy()||m_session->m_previewControl->state()!=QProcess::NotRunning){setState(Fault,tr("Shared session changed during preflight; retry Start."));return;}
        // The session owns the mutex, all ordinary start/reconnect/switch entrypoints
        // consult it. stop() cancels consumers and worker epochs before board STOP.
        m_session->m_tunerExclusive=true;m_session->stop();
        m_token=QUuid::createUuid().toString(QUuid::Id128);m_applied={};
        setState(Handover,tr("Stopping shared receivers and inference; waiting for the verified board owner and STOP acknowledgement..."));
        auto req=request();req["settings"]=m_requested.json();launch("start",req);return;
    }
    if(action=="stop"){
        clearReceiver(tr("Experiment stopped. Shared imaging remains stopped; use Camera raw video > Start."));m_heartbeat.stop();
        m_token.clear();m_oldToken.clear();m_applied={};m_session->m_tunerExclusive=false;m_session->m_captureOwner={};
        m_session->setState(tr("Stopped"),tr("dToF tuning ended; start shared imaging manually."));m_session->updateState();
        // Keep the original failure visible even if cleanup succeeded.
        if(m_state!=Fault)setState(Idle,tr("Experiment stopped. Shared imaging remains stopped; use Camera raw video > Start."));else emit changed();
        emit shutdownFinished();return;
    }
    if(action=="persist"){
        m_saved=r["settings"].toObject();m_info["snapshot"]=r["snapshot"];m_info["backup"]=r["backup"];
        setState(Running,tr("dToF settings saved and read back. Imaging uses a 48×32 session preset without changing the saved settings."));
    }else if(action=="start"||action=="switch"){
        if(r["token"]!=m_token||r["settings"].toObject()!=m_requested.json()){fail(tr("Experiment receipt differs from requested settings."));return;}
        m_applied=m_requested.json();m_info["snapshot"]=r["snapshot"];m_oldToken.clear();
        m_boardNow=r["remote_now_ns"].toString().toULongLong();m_boardClock.start();
        setState(Running,tr("Experiment running. Changes are temporary until Long-term save is clicked."));
        if(!m_cancel)receive();m_heartbeat.start();
    }
    if(m_cancel)stop();
}
void RvTunerController::apply(const RvTuner::Settings &s){
    if(m_state!=Running||pending())return;
    try{s.validate();m_requested=s;}catch(const std::exception &e){m_detail=QString::fromUtf8(e.what());emit changed();return;}
    if(s.json()==m_applied)return;
    clearReceiver(tr("Applying experiment parameters..."));m_heartbeat.stop();m_oldToken=m_token;m_token=QUuid::createUuid().toString(QUuid::Id128);
    auto req=request();req["old_token"]=m_oldToken;req["settings"]=s.json();setState(Applying,tr("Stopping the current experiment and verifying a new complete measurement..."));launch("switch",req);
}
void RvTunerController::persist(){if(m_state!=Running||pending()||m_applied.isEmpty())return;try{RvTuner::Settings::fromJson(m_applied);setState(Saving,tr("Backing up and atomically saving only dToF settings; verifying both scripts..."));launch("persist",request());}catch(const std::exception &e){m_detail=QString::fromUtf8(e.what());emit changed();}}
void RvTunerController::stop(){
    m_cancel=true;clearReceiver(tr("Stopping dToF tuning..."));m_heartbeat.stop();
    if(pending())return; // Never kill an in-flight ownership transaction and race STOP.
    if(!exclusive()){setState(Idle,tr("Stopped"));emit shutdownFinished();return;}
    if(m_state!=Fault)setState(Stopping,tr("Waiting for normal sensor STOP and resource release..."));launch("stop",request());
}
void RvTunerController::fail(const QString &why){if(m_destroying)return;clearReceiver(why);setState(Fault,why);stop();}
void RvTunerController::clearReceiver(const QString &reason){++m_epoch;m_expiry.stop();if(m_socket){m_socket->disconnect(this);m_socket->abort();m_socket->deleteLater();m_socket=nullptr;}m_decoder.reset();m_fps=0;m_received=0;emit cleared(reason);}
void RvTunerController::receive(){
    clearReceiver(tr("Waiting for a complete DTN1 measurement..."));if(m_testTransport)return;
    m_decoder=std::make_unique<RvTuner::Decoder>(m_token,m_requested);
    auto *socket=new QTcpSocket(this);m_socket=socket;socket->setProxy(QNetworkProxy::NoProxy);socket->setReadBufferSize(512*1024);
    const auto epoch=m_epoch;m_rateClock.start();
    connect(socket,&QTcpSocket::readyRead,this,[this,socket,epoch]{
        if(m_socket!=socket||m_epoch!=epoch||m_cancel||m_state==Stopping||m_state==Fault)return;
        try{
            const auto frames=m_decoder->feed(socket->readAll());if(frames.isEmpty())return;
            const qint64 age=qint64(m_boardNow/1000000)+m_boardClock.elapsed()-qint64(frames.last().timestamp/1000000);
            if(!m_boardNow||age < -1000||age>1000)throw std::runtime_error("DTN1 measurement is stale or outside the confirmed board clock");
            m_received+=frames.size();const auto elapsed=m_rateClock.elapsed();if(elapsed>=1000){m_fps=m_received*1000.0/elapsed;m_received=0;m_rateClock.restart();}
            m_expiry.start(std::max(3000,3*m_requested.periodMs));
            // A single latest complete measurement is published to both plots.
            emit frameReady(frames.last());
        }catch(const std::exception &e){const auto why=QString::fromUtf8(e.what());QTimer::singleShot(0,this,[this,epoch,why]{if(epoch==m_epoch)fail(why);});}
    });
    connect(socket,&QTcpSocket::errorOccurred,this,[this,socket,epoch](QAbstractSocket::SocketError){const auto reason=socket->errorString();QTimer::singleShot(0,this,[this,epoch,reason]{if(epoch==m_epoch)fail(reason);});});
    socket->connectToHost(m_host,8833);m_expiry.start(10000);
}
