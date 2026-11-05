#include "TactileSerialSource.h"
#include <QSerialPort>
#include <QElapsedTimer>
#include <atomic>
#include <QtCore/qt_windows.h>

namespace tactile {
namespace {
bool validBaud(int baud) { return baud==38400 || baud==115200 || baud==921600; }
QByteArray configPacket(int slave, int function, int address, int words, const QByteArray &payload = {}) {
    QByteArray b;
    for (int v : {slave, function, address >> 8, address & 255, words >> 8, words & 255}) b.append(char(v));
    if (function == 0x10) { b.append(char(payload.size())); b += payload; }
    const auto crc=tactileCrc(b); b.append(char(crc)); b.append(char(crc >> 8)); return b;
}
bool validConfigReply(const QByteArray &tx, const QByteArray &rx) {
    if(rx.size()<5 || rx[0]!=tx[0] || rx[1]!=tx[1])return false;
    if(tactileCrc(rx.first(rx.size()-2))!=(quint8(rx[rx.size()-2])|(quint16(quint8(rx.back()))<<8)))return false;
    if(quint8(tx[1])==0x10)return rx.size()==8 && rx.first(6)==tx.first(6);
    const int bytes=2*((quint16(quint8(tx[4]))<<8)|quint8(tx[5]));
    return rx.size()==bytes+5 && quint8(rx[2])==bytes;
}
}
DeviceIdentity identifyTactileDevice(int address, const ConfigExchange &exchange) {
    if(address<0||address>127||!exchange)return {};
    const auto readWord=[&](int slave,int reg) {
        const auto tx=configPacket(slave,3,reg,1);
        const auto rx=exchange(tx);
        if(!validConfigReply(tx,rx))return -1;
        return (int(quint8(rx[3]))<<8)|int(quint8(rx[4]));
    };
    int model=readWord(address,0x44c0);
    if(model<0&&address!=0){address=0;model=readWord(0,0x44c0);}
    if(model<1||model>3)return {};
    if(address==0){
        address=readWord(0,0x466c);
        if(address<0||address>127||readWord(address,0x44c0)!=model)return {};
    }
    return {model-1,address};
}
DeviceConfigResult configureTactileDevice(int current, int target, SaveCandidate candidate, const ConfigExchange &exchange, int currentBaud, int newBaud) {
    DeviceConfigResult r; r.requestedAddress=target;r.requestedBaud=newBaud;r.candidate=candidate;
    const int choice=int(candidate);
    if(current<0 || current>127 || target<0 || target>127 || choice<0 || choice>2
        || (currentBaud && !validBaud(currentBaud))
        || (newBaud && (!validBaud(newBaud) || !validBaud(currentBaud) || newBaud==currentBaud))
        || ((target||newBaud) && ((target && target==current) || candidate==SaveCandidate::None))
        || (!target && !newBaud && candidate!=SaveCandidate::None) || !exchange) {
        r.error=QStringLiteral("Invalid configuration request; new IDs must be 1-127, baud must be supported and saving must be explicit");return r;
    }
    auto send=[&](const QByteArray &tx){r.requests.append(tx);auto rx=exchange(tx);r.responses.append(rx);return rx;};
    auto read=[&](int slave,int reg,int words,QByteArray &rx){auto tx=configPacket(slave,3,reg,words);rx=send(tx);return validConfigReply(tx,rx);};
    auto word=[](const QByteArray &b,int i){return (int(quint8(b[3+i*2]))<<8)|quint8(b[4+i*2]);};
    QByteArray address,baud;
    if(!read(current,0x466c,1,address)||!read(current,0x4670,2,baud)){
        r.error=QStringLiteral("Configuration read failed (CRC, address, exception, length or timeout)");return r;
    }
    r.configuredAddress=word(address,0);
    const quint32 baudValue=quint32(word(baud,0))|(quint32(word(baud,1))<<16);
    r.configuredBaud=baudValue<=921600?int(baudValue):0;r.originalBaud=r.configuredBaud;r.respondingAddress=current;
    if(r.configuredAddress<0||r.configuredAddress>127
        ||(r.configuredBaud!=38400&&r.configuredBaud!=115200&&r.configuredBaud!=921600)){
        r.error=QStringLiteral("Device configuration is outside the manual range");return r;
    }
    r.readOk=true;
    if(!target&&!newBaud)return r;
    // The vendor public address can access a device with a different stored ID.
    // A unicast mismatch still indicates an unresolved configuration change.
    if(current!=0 && r.configuredAddress!=current){r.error=QStringLiteral("Configured ID differs from current ID; resolve pending changes before writing");return r;}
    if(currentBaud && r.configuredBaud!=currentBaud){r.error=QStringLiteral("Configured baud differs from host baud; resolve pending changes before writing");return r;}
    int responder=current;
    const auto writeParameter=[&](int reg,int words,const QByteArray &payload){
        const auto tx=configPacket(responder,0x10,reg,words,payload);r.writeAttempted=true;
        r.writeAcknowledged=validConfigReply(tx,send(tx));
        if(!r.writeAcknowledged)r.error=QStringLiteral("Parameter write not acknowledged; device state is uncertain. Do not repeat blindly");
        return r.writeAcknowledged;
    };
    // Address first, including devices that switch their ID immediately. Keep
    // the established baud for all writes; both settings share one final save.
    if(target){
        QByteArray payload;payload.append(char(0));payload.append(char(target));
        if(!writeParameter(0x466c,1,payload))return r;
        if(!read(responder,0x466c,1,address)){responder=target;if(!read(responder,0x466c,1,address)){
            r.error=QStringLiteral("Address was acknowledged but readback failed; save was not attempted");return r;}}
        r.respondingAddress=responder;r.configuredAddress=word(address,0);r.addressVerified=r.configuredAddress==target;
        if(!r.addressVerified){r.error=QStringLiteral("Address readback differs from requested ID; save was not attempted");return r;}
    }
    if(newBaud){
        QByteArray payload;
        for(int byte:{(newBaud>>8)&255,newBaud&255,(newBaud>>24)&255,(newBaud>>16)&255})payload.append(char(byte));
        if(!writeParameter(0x4670,2,payload))return r;
        // Stay at the established host baud until the explicit power-cycle step.
        // No automatic baud scan or repeated write on a missing response.
        if(!read(responder,0x4670,2,baud)){
            r.error=QStringLiteral("Baud write acknowledged but readback failed; save was not attempted");return r;
        }
        const quint32 actual=quint32(word(baud,0))|(quint32(word(baud,1))<<16);
        r.baudVerified=actual==quint32(newBaud);r.configuredBaud=actual<=921600?int(actual):0;
        if(!r.baudVerified){r.error=QStringLiteral("Baud readback differs from requested value; save was not attempted");return r;}
    }
    // Bit17 only. Never echo other command bits, particularly the zeroing bit.
    const auto save=configPacket(responder,0x10,candidate==SaveCandidate::Decimal18013?0x465d:0x466f,2,QByteArray::fromHex("00000002"));
    r.saveAttempted=true;r.saveAcknowledged=validConfigReply(save,send(save));
    if(!r.saveAcknowledged)r.error=QStringLiteral("Save not acknowledged; persistence is unknown");
    return r;
}
bool SerialSettings::valid() const {
    return !port.trimmed().isEmpty() && (baud==38400||baud==115200||baud==921600)
        && slave>=0&&slave<=127&&type>=0&&type<3&&timeoutMs>=50&&timeoutMs<=5000
        && intervalMs>=10&&intervalMs<=60000;
}
quint16 tactileCrc(const QByteArray &bytes) {
    quint16 crc=0xffff;
    for(unsigned char b:bytes) { crc^=b;for(int i=0;i<8;++i)crc=(crc&1)?(crc>>1)^0xa001:crc>>1; }
    return crc;
}
QByteArray tactileReadRequest(int slave) {
    if(slave<0||slave>127)return {};
    QByteArray b; b.append(char(slave));b.append(char(3));b.append(char(0x48));b.append(char(0x44));b.append(char(0));b.append(char(64));
    const auto crc=tactileCrc(b);b.append(char(crc&255));b.append(char(crc>>8));return b;
}
SerialSample decodeTactileResponse(const QByteArray &rx,const SerialSettings &s) {
    SerialSample out;out.settings=s;out.rx=rx;out.verifyCrc=s.verifyCrc;
    const auto fail=[&](const QString &e){out.error=e;return out;};
    if(!s.valid())return fail(QStringLiteral("Invalid serial settings"));
    if(rx.size()<5)return fail(QStringLiteral("Incomplete response"));
    out.crcOk=tactileCrc(rx.first(rx.size()-2))==(quint8(rx[rx.size()-2])|(quint16(quint8(rx.back()))<<8));
    if(quint8(rx[0])!=s.slave)return fail(QStringLiteral("Slave ID mismatch"));
    if(quint8(rx[1])==0x83)return fail(QStringLiteral("Modbus exception %1").arg(quint8(rx[2])));
    if(quint8(rx[1])!=3||quint8(rx[2])!=128||rx.size()!=133)return fail(QStringLiteral("Function or length mismatch"));
    // Retain all 64 received words for diagnosis even when the frame is rejected.
    for(int i=0;i<64;++i)out.registers.append((quint16(quint8(rx[3+i*2]))<<8)|quint8(rx[4+i*2]));
    const int count=s.type==2?16:20;
    for(int i=0;i<count;++i)if(out.registers[3*i]==32768||out.registers[3*i+1]==32768||out.registers[3*i+2]>32767)out.rangeOk=false;
    if(s.verifyCrc&&!out.crcOk)return fail(QStringLiteral("CRC mismatch"));
    out.accepted=true;
    if(!out.rangeOk)out.error=QStringLiteral("Values outside manual range; raw values retained");
    return out;
}
Frame serialFrame(const SerialSample &sample,const SerialSettings &s,const QUuid &id,quint64 gen,quint64 seq,qint64 ns) {
    Frame f;f.sourceMode="Serial";f.layoutId="hardware-tactile-counts-v1";f.hardwareType=s.type;
    f.sessionId=id;f.generation=gen;f.sequence=seq;f.monotonicNs=ns;
    f.crcVerified=sample.verifyCrc;f.crcOk=sample.crcOk;f.rangeOk=sample.rangeOk;
    if(!sample.accepted||sample.registers.size()!=64||s.type<0||s.type>2)return f;
    for(int t=0;t<3;++t) {
        Finger finger;finger.id=sensorLayout().fingerIds[t];finger.valid=t==s.type;
        if(finger.valid) {
            Module module;module.id="M1";
            for(int p=0;p<sensorLayout(t).points.size();++p) {
                const auto signedWord=[](quint16 w){return w>=32768?int(w)-65536:int(w);};
                Force force{double(signedWord(sample.registers[p*3])),double(signedWord(sample.registers[p*3+1])),double(sample.registers[p*3+2])};
                module.points.append({QString("P%1").arg(p+1),force,true});module.resultant+=force;
            }
            finger.resultant=module.resultant;finger.modules.append(module);
        }
        f.fingers.append(finger);
    }
    f.valid=true;return f;
}
struct SerialWorkerState {
    SerialSettings settings;
    bool probeFirst=false, autoZero=false;
    QUuid session;
    quint64 generation = 0;
    std::atomic_bool stop{true}, closing{false}, verify{true}, paused{false}, configBusy{false};
    std::atomic<quint64> revision{0}, configCommand{0};
    std::atomic_int intervalMs{10};
    std::atomic_int respondingAddress{1};
    ConfigExchange exchangeForTests;
    LatestSensorFrameBuffer<SerialSample> latest;
    LatestSensorFrameBuffer<DeviceConfigResult> configResults;
    HANDLE wakeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE gapTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE|SYNCHRONIZE);
    ~SerialWorkerState() {
        if(gapTimer)CloseHandle(gapTimer);
        if(wakeEvent)CloseHandle(wakeEvent);
    }
    void wake() { if(wakeEvent)SetEvent(wakeEvent); }
    void waitForRequestGap(bool zeroing=false) {
        QElapsedTimer elapsed;elapsed.start();
        while(!stop&&!paused&&!configCommand) {
            const qint64 remaining=qint64(zeroing?TactileAutoZero::PollIntervalMs:intervalMs.load())*1000000-elapsed.nsecsElapsed();
            if(remaining<=0)return;
            // A high-resolution, one-shot Windows timer avoids rounding a 10 ms
            // request gap up to the system sleep tick. Stop/config/edits wake it.
            LARGE_INTEGER due;due.QuadPart=-((remaining+99)/100);
            if(gapTimer&&wakeEvent&&SetWaitableTimer(gapTimer,&due,0,nullptr,nullptr,FALSE)) {
                const HANDLE handles[]={wakeEvent,gapTimer};
                if(WaitForMultipleObjects(2,handles,FALSE,INFINITE)!=WAIT_FAILED)continue;
            }
            // Older Windows or unavailable timer resources: retain bounded stop
            // latency and the requested minimum gap without spinning the CPU.
            QThread::msleep(1);
        }
    }
};
TactileSerialSource::TactileSerialSource(QObject *parent)
    : QObject(parent), m_state(std::make_shared<SerialWorkerState>()) {}
bool TactileSerialSource::begin(const SerialSettings &s,bool probeFirst,bool autoZero) {
    if(isRunning())return false;
    stop();
    auto state=std::make_shared<SerialWorkerState>();
    state->settings=s;state->session=m_session=QUuid::createUuid();state->generation=++m_generation;
    state->intervalMs=s.intervalMs;state->respondingAddress=s.slave;state->exchangeForTests=m_exchangeForTests;
    state->probeFirst=probeFirst;state->autoZero=autoZero;
    state->stop=false;state->verify=s.verifyCrc;state->revision=1;m_state=state;
    // No parent and no capture of this: a blocked driver may outlive the panel.
    // The worker retains its state and deletes itself only after run() returns.
    auto *worker=QThread::create([state]{run(state);});m_worker=worker;
    connect(worker,&QThread::started,this,&TactileSerialSource::started,Qt::DirectConnection);
    connect(worker,&QThread::finished,this,[this,worker]{if(m_worker==worker)emit finished();});
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);
    worker->start();return true;
}
void TactileSerialSource::stop() {
    m_state->stop=true;++m_state->revision;m_state->latest.clear();
    m_state->configCommand=0;m_state->configBusy=false;m_state->configResults.clear();
    m_state->wake();
}
bool TactileSerialSource::isRunning() const { return m_worker&&m_worker->isRunning(); }
bool TactileSerialSource::stopping() const { return isRunning()&&(m_state->stop||m_state->closing); }
bool TactileSerialSource::wait(unsigned long timeoutMs) { return !m_worker||m_worker->wait(timeoutMs); }
void TactileSerialSource::setVerifyCrc(bool value) {
    if(m_state->verify.exchange(value)!=value){++m_state->revision;m_state->latest.clear();}
}
void TactileSerialSource::setPaused(bool value) {
    if(m_state->paused.exchange(value)!=value){++m_state->revision;m_state->latest.clear();m_state->wake();}
}
bool TactileSerialSource::setIntervalMs(int value) {
    if(value<10||value>60000)return false;
    m_state->intervalMs=value;m_state->wake();return true;
}
bool TactileSerialSource::takeLatest(SerialSample &sample) {
    return !m_state->stop&&m_state->latest.takeLatest(&sample)&&sample.revision==m_state->revision.load();
}
bool TactileSerialSource::configBusy() const { return !m_state->stop&&m_state->configBusy; }
bool TactileSerialSource::takeConfigResult(DeviceConfigResult &result) {
    return !m_state->stop&&m_state->configResults.takeLatest(&result);
}
bool TactileSerialSource::requestDeviceConfig(int target,SaveCandidate candidate,int newBaud) {
    if(!isRunning()||m_state->stop||m_state->closing||target<0||target>127||int(candidate)<0||int(candidate)>2
        ||(newBaud&&(!validBaud(newBaud)||newBaud==m_state->settings.baud))
        ||((target||newBaud)&&((target && target==m_state->respondingAddress.load())||candidate==SaveCandidate::None))
        ||(!target&&!newBaud&&candidate!=SaveCandidate::None))return false;
    bool idle=false;if(!m_state->configBusy.compare_exchange_strong(idle,true))return false;
    m_state->configResults.clear();++m_state->revision;m_state->latest.clear();
    m_state->configCommand=1+quint64(target)+(quint64(candidate)<<8)+(quint64(newBaud)<<16);m_state->wake();return true;
}
void TactileSerialSource::run(const std::shared_ptr<SerialWorkerState> &state) {
    if(state->stop)return;
    auto settings=state->settings;
    TactileAutoZero zero(state->autoZero);
    SerialSample totals;totals.settings=settings;totals.verifyCrc=settings.verifyCrc;totals.revision=state->revision.load();QElapsedTimer clock;clock.start();
    auto publish=[&](SerialSample sample){sample.requests=totals.requests;sample.acceptedCount=totals.acceptedCount;
        sample.crcErrors=totals.crcErrors;sample.timeouts=totals.timeouts;sample.protocolErrors=totals.protocolErrors;
        sample.receivedMs=clock.msecsSinceReference()+clock.elapsed();
        sample.autoZero=zero.result();
        if(!state->stop)state->latest.publish(sample);};
    if(!settings.valid()){totals.error="Invalid serial settings";publish(totals);return;}
    QSerialPort port;port.setPortName(settings.port);port.setBaudRate(settings.baud);
    port.setDataBits(QSerialPort::Data8);port.setParity(QSerialPort::NoParity);port.setStopBits(QSerialPort::OneStop);port.setFlowControl(QSerialPort::NoFlowControl);
    if(!state->exchangeForTests) {
        if(!port.open(QIODevice::ReadWrite)){totals.error=port.errorString();publish(totals);return;}
        port.setDataTerminalReady(false);port.setRequestToSend(false);port.setReadBufferSize(4096);
    }
    totals.portOpen=true;publish(totals);
    const auto exchange=[&](const QByteArray &tx){
        if(state->stop)return QByteArray{};
        if(state->exchangeForTests)return state->exchangeForTests(tx);
        QElapsedTimer silence;silence.start();while(!state->stop&&silence.elapsed()<20)QThread::msleep(2);
        if(state->stop)return QByteArray{};
        port.clear(QSerialPort::Input);QByteArray rx;QElapsedTimer deadline;deadline.start();
        if(port.write(tx)!=tx.size())return rx;
        while(port.bytesToWrite()>0&&!state->stop&&deadline.elapsed()<settings.timeoutMs)port.waitForBytesWritten(10);
        while(!state->stop&&deadline.elapsed()<settings.timeoutMs){
            if(port.bytesAvailable()||port.waitForReadyRead(10))rx+=port.readAll();
            if(port.error()==QSerialPort::ResourceError||port.error()==QSerialPort::DeviceNotFoundError)break;
            if(rx.size()>=3){int count=(quint8(rx[1])&0x80)?5:(quint8(rx[1])==0x10?8:quint8(rx[2])+5);if(rx.size()>=count)break;}
        }
        return state->stop?QByteArray{}:rx;
    };
    while(!state->stop) {
        if(!totals.modelIdentified){
            const auto identity=identifyTactileDevice(settings.slave,exchange);
            if(state->stop)break;
            if(identity.type<0){
                totals.error=QStringLiteral("Sensor model identification failed");publish(totals);
                if(state->probeFirst||port.error()==QSerialPort::ResourceError||port.error()==QSerialPort::DeviceNotFoundError)break;
                state->waitForRequestGap();continue;
            }
            settings.type=identity.type;settings.slave=identity.address;
            state->respondingAddress=identity.address;
            totals.settings=settings;totals.modelIdentified=true;totals.error.clear();
        }
        const quint64 command=state->configCommand.exchange(0);
        if(command){
            zero.cancel();
            const quint64 value=command-1;
            auto result=configureTactileDevice(settings.slave,int(value&255),SaveCandidate((value>>8)&255),exchange,settings.baud,int(value>>16));
            if(result.writeAttempted)state->closing=true;
            if(!state->stop)state->configResults.publish(result);
            state->configBusy=false;
            // Any attempted write invalidates the connection, even on timeout.
            if(result.writeAttempted)break;
            continue;
        }
        if(state->paused){zero.cancel();QThread::msleep(10);continue;}
        const auto revision=state->revision.load();
        const int effectiveInterval=zero.collecting()?TactileAutoZero::PollIntervalMs:state->intervalMs.load();
        const int remaining=zero.remainingMs(clock.elapsed());
        const int responseTimeout=remaining>=0?qMax(1,qMin(settings.timeoutMs,remaining)):settings.timeoutMs;
        const QByteArray tx=tactileReadRequest(settings.slave);
        if(!state->exchangeForTests)port.clear(QSerialPort::Input);
        ++totals.requests;
        QByteArray rx;QString ioError;QElapsedTimer deadline;deadline.start();
        if(state->exchangeForTests)rx=state->exchangeForTests(tx);
        else if(port.write(tx)!=tx.size())ioError=port.errorString();
        while(!state->exchangeForTests&&ioError.isEmpty()&&port.bytesToWrite()>0&&!state->stop&&deadline.elapsed()<responseTimeout)port.waitForBytesWritten(qMax(1,qMin(10,responseTimeout-int(deadline.elapsed()))));
        while(!state->exchangeForTests&&ioError.isEmpty()&&!state->stop&&deadline.elapsed()<responseTimeout) {
            if(port.bytesAvailable()||port.waitForReadyRead(qMax(1,qMin(10,responseTimeout-int(deadline.elapsed())))))rx+=port.readAll();
            if(port.error()==QSerialPort::ResourceError||port.error()==QSerialPort::DeviceNotFoundError){ioError=port.errorString();break;}
            if(rx.size()>=5) {
                const int expected=(quint8(rx[1])&0x80)?5:(rx.size()>=3?quint8(rx[2])+5:133);
                if(rx.size()>=expected)break;
            }
        }
        if(state->stop)break;
        auto currentSettings=settings;currentSettings.verifyCrc=state->verify.load();currentSettings.intervalMs=state->intervalMs.load();
        auto sample=decodeTactileResponse(rx,currentSettings);sample.tx=tx;sample.portOpen=ioError.isEmpty();sample.revision=revision;
        sample.effectiveIntervalMs=effectiveInterval;
        sample.modelIdentified=true;
        const int expected=rx.size()>=3?((quint8(rx[1])&0x80)?5:quint8(rx[2])+5):133;
        if(!ioError.isEmpty()){sample.accepted=false;sample.error=ioError;}
        else if(rx.size()<expected){sample.accepted=false;sample.error="Response timeout / incomplete frame";++totals.timeouts;}
        else {
            if(!sample.crcOk)++totals.crcErrors;
            if(!sample.accepted&&sample.error!="CRC mismatch")++totals.protocolErrors;
        }
        if(sample.accepted)++totals.acceptedCount;
        sample.frame=serialFrame(sample,currentSettings,state->session,state->generation,totals.requests,clock.nsecsElapsed());
        if(revision==state->revision.load())zero.observe(sample.frame,clock.elapsed());
        else zero.advance(clock.elapsed());
        publish(sample);
        if(!ioError.isEmpty())break;
        if(state->probeFirst){
            if(!sample.accepted||!sample.crcOk||!sample.rangeOk)break;
            state->probeFirst=false;
        }
        state->waitForRequestGap(zero.collecting());
    }
    state->closing=true;
    if(port.isOpen())port.close();
}
}
