#include "../sensors/TactileSerialSource.h"
#include "../sensors/TactileHistory.h"
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <QSemaphore>
#include <QTimer>
#include <QEvent>
#include "../sensors/TactileVisualizationWidget.h"
#include "../sensors/TactileSceneAdapter.h"
#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QTableWidget>
#include <QFileInfo>
#include <QLabel>
#include <QSpinBox>
#include <QSignalBlocker>
#include <atomic>
#include <vector>
#include <algorithm>

int runTactileSerialTests(const QString &path)
{
    QFile out(path);if(!out.open(QIODevice::WriteOnly|QIODevice::Text))return 3;QTextStream report(&out);int failures=0;
    const auto check=[&](bool ok,const char *name){report<<(ok?"PASS ":"FAIL ")<<name<<'\n';if(!ok)++failures;};
    using namespace tactile;SerialSettings s;s.port="TEST";
    check(s.intervalMs==10&&s.valid(),"serial settings default to a valid 10 ms request gap");
    for(int interval:{0,1,4,5,9,60001}){auto invalid=s;invalid.intervalMs=interval;
        check(!invalid.valid(),"request gaps outside 10-60000 ms are rejected");}
    for(int interval:{10,11,20,100,60000}){auto valid=s;valid.intervalMs=interval;
        check(valid.valid(),"10 ms, intermediate and upper-bound request gaps are accepted");}
    const auto sealConfig=[](QByteArray b){const auto crc=tactileCrc(b);b.append(char(crc));b.append(char(crc>>8));return b;};
    for(int model=1;model<=3;++model){
        const auto identity=identifyTactileDevice(7,[&](const QByteArray &tx){
            check(tx==sealConfig(QByteArray::fromHex("070344c00001")),"model read uses register 17600 and one word");
            QByteArray reply=QByteArray::fromHex("0703020000");reply[4]=char(model);return sealConfig(reply);
        });
        check(identity.type==model-1&&identity.address==7,"F, G and S map to their actual layouts independently of UI selection");
    }
    const auto modelF=sealConfig(QByteArray::fromHex("0103020001"));
    auto corruptModel=modelF;corruptModel.back()^=1;
    for(const auto &reply:{QByteArray{},corruptModel,modelF.first(6),modelF+QByteArray(1,0),
            sealConfig(QByteArray::fromHex("0203020001")),sealConfig(QByteArray::fromHex("0104020001")),
            sealConfig(QByteArray::fromHex("018302")),sealConfig(QByteArray::fromHex("0103020000")),
            sealConfig(QByteArray::fromHex("0103020004"))}){
        check(identifyTactileDevice(1,[&](const QByteArray &){return reply;}).type<0,
            "untrusted, incomplete, exceptional and unknown model replies cannot select a layout");
    }
    for(bool mismatch:{false,true}){
        QVector<QByteArray> traffic;
        const auto identity=identifyTactileDevice(7,[&](const QByteArray &tx){
            traffic.append(tx);
            if(tx[0]==7)return QByteArray{};
            if(tx[0]==0&&tx.mid(2,2)==QByteArray::fromHex("466c"))return sealConfig(QByteArray::fromHex("0003020009"));
            QByteArray reply=QByteArray::fromHex("0003020003");reply[0]=tx[0];
            if(mismatch&&tx[0]==9)reply[4]=1;
            return sealConfig(reply);
        });
        check(traffic.size()==4&&std::all_of(traffic.begin(),traffic.end(),[](const QByteArray &tx){return tx[1]==3;}),
            "public-address recovery reads the stored ID and verifies its model using only read requests");
        check(mismatch?identity.type<0:(identity.type==2&&identity.address==9),
            "public recovery requires matching model at the discovered address");
    }
    // Wire emulator uses literal register addresses and reply formats; no port opens.
    int deviceAddress=1,configuredAddress=1,writeCount=0,saveRegister=0,readCount=0;
    bool immediate=false,badCrc=false,badEcho=false,dropWrite=false,badReadback=false;
    auto exchange=[&](const QByteArray &tx)->QByteArray{
        if(quint8(tx[0])!=deviceAddress)return {};
        const int reg=(quint8(tx[2])<<8)|quint8(tx[3]);QByteArray reply;
        if(quint8(tx[1])==3){
            ++readCount;reply=QByteArray::fromHex(reg==0x466c?"0103020001":"0103041000000e");
            reply[0]=char(deviceAddress);if(reg==0x466c)reply[4]=char(badReadback?1:configuredAddress);
        }else{
            ++writeCount;reply=tx.first(6);
            if(reg==0x466c){configuredAddress=quint8(tx[8]);if(immediate)deviceAddress=configuredAddress;}
            else {saveRegister=reg;check(tx.mid(7,4)==QByteArray::fromHex("00000002"),"save sets only Bit17, low word first, no zeroing bit");}
            if(dropWrite)return {};
            if(badEcho)reply[5]=char(9);
        }
        reply=sealConfig(reply);if(badCrc)reply[reply.size()-1]^=1;return reply;
    };
    auto config=configureTactileDevice(1,0,SaveCandidate::None,exchange);
    check(config.readOk&&config.configuredAddress==1&&config.configuredBaud==921600&&writeCount==0,"configuration read is read-only and decodes 32-bit low word first");
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,exchange);
    check(config.writeAcknowledged&&config.addressVerified&&config.saveAcknowledged&&saveRegister==0x465d&&writeCount==2,
        "deferred-address device: one address write, strict readback, one explicit decimal save");
    configuredAddress=1;deviceAddress=1;writeCount=0;immediate=true;
    config=configureTactileDevice(1,2,SaveCandidate::Hex466F,exchange);
    check(config.saveAcknowledged&&config.respondingAddress==2&&saveRegister==0x466f&&writeCount==2,
        "immediate-address device: read-only probe of new ID then selected save, no alternate save");
    deviceAddress=1;configuredAddress=1;immediate=false;badCrc=true;writeCount=0;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,exchange);
    check(!config.readOk&&!config.writeAttempted&&writeCount==0,"bad CRC preflight fails closed before any write");
    badCrc=false;badEcho=true;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,exchange);
    check(config.writeAttempted&&!config.writeAcknowledged&&!config.saveAttempted&&writeCount==1,"wrong write echo cannot trigger saving");
    badEcho=false;configuredAddress=1;dropWrite=true;writeCount=0;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,exchange);
    check(config.writeAttempted&&!config.saveAttempted&&writeCount==1,"lost write acknowledgement is uncertain and never retried");
    dropWrite=false;configuredAddress=1;badReadback=true;writeCount=0;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,exchange);
    check(config.writeAcknowledged&&!config.addressVerified&&!config.saveAttempted&&writeCount==1,"mismatched readback cannot trigger saving");
    badReadback=false;configuredAddress=1;writeCount=0;readCount=0;
    for(const auto pair: {QPair<int,int>{1,128},{1,1},{-1,2}})
        check(!configureTactileDevice(pair.first,pair.second,SaveCandidate::Decimal18013,exchange).writeAttempted,"invalid/unchanged write rejected before transport");
    check(!configureTactileDevice(1,2,SaveCandidate::None,exchange).writeAttempted&&readCount==0&&writeCount==0,"unselected save candidate cannot issue I/O");
    TactileSerialSource unopened;
    check(!unopened.requestDeviceConfig(2,SaveCandidate::Decimal18013)&&!unopened.configBusy(),"disconnected worker rejects programmatic configuration");
    check(!unopened.requestDeviceConfig(2,SaveCandidate::Decimal18013,115200),"disconnected worker rejects combined configuration");
    for(const auto change:{QPair<int,int>{1,0},QPair<int,int>{127,0},QPair<int,int>{0,115200},QPair<int,int>{1,38400}}){
        TactileSerialSource queued;SerialSettings publicSettings;publicSettings.port.clear();publicSettings.slave=0;
        QSemaphore started,release;
        QObject::connect(&queued,&TactileSerialSource::started,[&]{started.release();release.acquire();});
        // Empty port plus a startup gate keeps this a worker-entry test with no serial I/O.
        queued.begin(publicSettings);const bool running=started.tryAcquire(1,1000);
        check(running,"public configuration worker fixture reaches its startup gate");
        if(running){
            check(!queued.requestDeviceConfig(-1,SaveCandidate::Decimal18013)
                &&!queued.requestDeviceConfig(128,SaveCandidate::Decimal18013)
                &&!queued.requestDeviceConfig(1,SaveCandidate::None)
                &&!queued.requestDeviceConfig(0,SaveCandidate::Decimal18013)
                &&!queued.requestDeviceConfig(0,SaveCandidate::Decimal18013,921600),
                "public worker rejects invalid, unsaved and unchanged configuration requests");
            check(queued.requestDeviceConfig(change.first,SaveCandidate::Decimal18013,change.second)&&queued.configBusy(),
                "public worker accepts address-only, baud-only and combined changes");
            check(!queued.requestDeviceConfig(2,SaveCandidate::Decimal18013),"busy public worker rejects a second configuration");
        }
        queued.stop();release.release();
        check(queued.wait(2000)&&!queued.isRunning()&&!queued.configBusy(),"stopping a public configuration cancels pending work");
    }
    {
        // Hold a real worker indefinitely, as a blocked USB close would do.
        // No port opens: cancellation precedes entry to run().
        auto blocked=std::make_unique<TactileSerialSource>();
        SerialSettings empty;empty.port.clear();QSemaphore entered,release;
        QThread *retainedWorker=nullptr;
        QObject::connect(blocked.get(),&TactileSerialSource::started,[&]{
            retainedWorker=QThread::currentThread();entered.release();release.acquire();
        });
        check(blocked->begin(empty)&&entered.tryAcquire(1,1000),"blocked-driver fixture reaches worker gate");
        const auto blockedSession=blocked->sessionId();const auto blockedGeneration=blocked->generation();
        QElapsedTimer elapsed;elapsed.start();blocked->stop();
        check(elapsed.elapsed()<100&&blocked->stopping(),"stop returns promptly while the serial worker is blocked");
        SerialSample cancelled;DeviceConfigResult cancelledConfig;
        check(!blocked->takeLatest(cancelled)&&!blocked->takeConfigResult(cancelledConfig)
            &&!blocked->requestDeviceConfig(2,SaveCandidate::Decimal18013),"cancelled worker cannot deliver data or accept writes");
        check(!blocked->begin(empty)&&blocked->sessionId()==blockedSession&&blocked->generation()==blockedGeneration,
            "reconnect cannot replace a worker that still owns the port");
        check(!blocked->wait(5),"diagnostic join has a finite deadline while driver is blocked");
        int heartbeat=0;QTimer pulse;QObject::connect(&pulse,&QTimer::timeout,[&]{++heartbeat;});pulse.start(5);
        elapsed.restart();while(elapsed.elapsed()<60){QCoreApplication::processEvents();QThread::msleep(1);}pulse.stop();
        check(heartbeat>=2,"event loop remains responsive while disconnect is pending");
        elapsed.restart();blocked.reset();
        check(elapsed.elapsed()<100,"destroying the source does not join a blocked driver");
        release.release();
        check(retainedWorker&&retainedWorker->wait(2000),"detached worker completes safely after its source is destroyed");
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    // Simulate a sensor that holds its active baud until power-cycle. Use wire
    // constants independently of the production packet builder.
    int parameterBaud=921600,parameterId=1,activeId=1,baudWrites=0,idWrites=0,saves=0;
    bool changeIdImmediately=false,dropBaudAck=false,wrongBaudReadback=false,rejectSave=false,corruptBaudReadback=false;
    bool publicAlias=false,publicEndsAfterId=false,dropIdAck=false,wrongIdReadback=false,corruptIdReadback=false,corruptPreflight=false;
    auto baudExchange=[&](const QByteArray &tx)->QByteArray{
        if(quint8(tx[0])!=activeId && !(quint8(tx[0])==0 && publicAlias && !(publicEndsAfterId && idWrites)))return {};
        const int reg=(quint8(tx[2])<<8)|quint8(tx[3]);QByteArray reply;
        if(quint8(tx[1])==3){
            reply.append(tx[0]);reply.append(char(3));reply.append(char(reg==0x466c?2:4));
            if(reg==0x466c){reply.append(char(0));reply.append(char(wrongIdReadback&&idWrites?0:parameterId));}
            else {
                int value=wrongBaudReadback&&baudWrites?921600:parameterBaud;
                for(int byte:{(value>>8)&255,value&255,(value>>24)&255,(value>>16)&255})reply.append(char(byte));
            }
        }else if(reg==0x466c){
            ++idWrites;parameterId=quint8(tx[8]);
            check(parameterId>=1&&parameterId<=127,"address payload never writes the public address 0");
            reply=tx.first(6);if(changeIdImmediately)activeId=parameterId;if(dropIdAck)return {};
        }
        else if(reg==0x4670){
            ++baudWrites;check(tx.mid(4,3)==QByteArray::fromHex("000204"),"baud write uses exactly two registers/four payload bytes");
            const auto payload=tx.mid(7,4);
            check(payload==QByteArray::fromHex("c2000001")||payload==QByteArray::fromHex("96000000"),"baud payload uses manual low-word-first byte order");
            parameterBaud=(quint8(tx[7])<<8)|quint8(tx[8])|(quint8(tx[9])<<24)|(quint8(tx[10])<<16);
            if(dropBaudAck)return {};reply=tx.first(6);
        }else{
            ++saves;check(reg==0x465d&&tx.mid(7,4)==QByteArray::fromHex("00000002"),"combined transaction saves once using Bit17 only");
            reply=rejectSave?QByteArray::fromHex("019003"):tx.first(6);reply[0]=tx[0];
        }
        reply=sealConfig(reply);
        if(corruptBaudReadback&&baudWrites&&quint8(tx[1])==3&&reg==0x4670)reply[reply.size()-1]^=1;
        if((corruptPreflight&&!idWrites&&!baudWrites)
            ||(corruptIdReadback&&idWrites&&quint8(tx[1])==3&&reg==0x466c))reply[reply.size()-1]^=1;
        return reply;
    };
    const auto resetBaud=[&]{parameterBaud=921600;parameterId=activeId=1;baudWrites=idWrites=saves=0;
        changeIdImmediately=dropBaudAck=wrongBaudReadback=rejectSave=corruptBaudReadback=false;
        publicAlias=publicEndsAfterId=dropIdAck=wrongIdReadback=corruptIdReadback=corruptPreflight=false;};
    config=configureTactileDevice(1,0,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.baudVerified&&config.saveAcknowledged&&config.configuredBaud==115200&&config.originalBaud==921600
        &&baudWrites==1&&idWrites==0&&saves==1,"baud-only change preserves ID and saves after readback");
    resetBaud();config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,baudExchange,921600,38400);
    check(config.addressVerified&&config.baudVerified&&config.saveAcknowledged&&idWrites==1&&baudWrites==1&&saves==1,
        "combined deferred changes verify both values then issue exactly one save");
    resetBaud();changeIdImmediately=true;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.saveAcknowledged&&config.respondingAddress==2&&baudWrites==1&&idWrites==1&&saves==1,
        "combined change uses immediately active new ID for baud readback and save");
    resetBaud();dropBaudAck=true;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.addressVerified&&!config.writeAcknowledged&&!config.saveAttempted&&baudWrites==1&&saves==0,
        "partial combined write remains uncertain, never repeats or saves after lost baud acknowledgement");
    resetBaud();wrongBaudReadback=true;
    config=configureTactileDevice(1,0,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(!config.baudVerified&&!config.saveAttempted&&saves==0,"mismatched baud readback cannot save");
    resetBaud();corruptBaudReadback=true;
    config=configureTactileDevice(1,0,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(!config.baudVerified&&!config.saveAttempted,"bad CRC baud readback cannot save");
    resetBaud();rejectSave=true;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.baudVerified&&config.addressVerified&&!config.saveAcknowledged&&saves==1,"save exception is uncertain with no fallback command");
    resetBaud();parameterBaud=38400;
    config=configureTactileDevice(1,2,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.readOk&&!config.writeAttempted&&!idWrites&&!baudWrites,"pending baud change blocks further writes before either parameter is touched");
    for(int targetBaud:{-1,9600,921600,1000000}){
        config=configureTactileDevice(1,0,SaveCandidate::Decimal18013,baudExchange,921600,targetBaud);
        check(config.requests.isEmpty()&&!config.writeAttempted,"unsupported/unchanged baud rejected before transport");
    }
    for(int targetId:{1,127}){
        resetBaud();parameterId=activeId=0;
        config=configureTactileDevice(0,targetId,SaveCandidate::Decimal18013,baudExchange,921600);
        check(config.readOk&&config.addressVerified&&config.saveAcknowledged&&config.configuredAddress==targetId
            &&idWrites==1&&baudWrites==0&&saves==1,"initial ID 0 can be assigned either endpoint 1 or 127 with one verified save");
    }
    resetBaud();parameterId=activeId=7;publicAlias=true;
    config=configureTactileDevice(0,12,SaveCandidate::Decimal18013,baudExchange,921600);
    check(config.addressVerified&&config.saveAcknowledged&&config.configuredAddress==12&&idWrites==1&&saves==1,
        "public communication ID differs from stored ID and still permits an explicit address change");
    resetBaud();parameterId=activeId=0;
    config=configureTactileDevice(0,0,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.baudVerified&&config.saveAcknowledged&&config.configuredAddress==0&&idWrites==0&&baudWrites==1&&saves==1,
        "baud-only write through ID 0 preserves an initial stored ID 0 without an address write");
    resetBaud();parameterId=activeId=7;publicAlias=true;
    config=configureTactileDevice(0,0,SaveCandidate::Decimal18013,baudExchange,921600,38400);
    check(config.baudVerified&&config.saveAcknowledged&&config.configuredAddress==7&&idWrites==0&&baudWrites==1&&saves==1,
        "baud-only public access retains the actual nonzero device ID for manual reconnection");
    resetBaud();parameterId=activeId=0;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.addressVerified&&config.baudVerified&&config.saveAcknowledged&&config.respondingAddress==0
        &&idWrites==1&&baudWrites==1&&saves==1,"combined initial ID 0 changes verify both parameters and save exactly once");
    resetBaud();parameterId=activeId=0;changeIdImmediately=true;
    config=configureTactileDevice(0,127,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.addressVerified&&config.baudVerified&&config.saveAcknowledged&&config.respondingAddress==127
        &&idWrites==1&&baudWrites==1&&saves==1,"initial ID 0 switches immediately and subsequent readback and save use the new ID");
    resetBaud();parameterId=activeId=7;publicAlias=publicEndsAfterId=changeIdImmediately=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600,38400);
    check(config.saveAcknowledged&&config.respondingAddress==1&&idWrites==1&&baudWrites==1&&saves==1,
        "public alias disappearing after address change is handled by a read-only probe of the new ID");
    resetBaud();parameterId=activeId=0;corruptPreflight=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600);
    check(!config.readOk&&!config.writeAttempted&&!idWrites&&!baudWrites&&!saves,
        "public preflight CRC failure blocks all parameter and save writes");
    resetBaud();parameterId=activeId=0;dropIdAck=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600);
    check(config.writeAttempted&&!config.writeAcknowledged&&!config.saveAttempted&&idWrites==1&&!saves,
        "lost public address write acknowledgement remains uncertain without retry or save");
    resetBaud();parameterId=activeId=0;wrongIdReadback=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600);
    check(config.writeAcknowledged&&!config.addressVerified&&!config.saveAttempted&&!saves,
        "public address readback mismatch cannot trigger saving");
    resetBaud();parameterId=activeId=0;corruptIdReadback=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600);
    check(!config.addressVerified&&!config.saveAttempted&&!saves,
        "public address readback CRC failure cannot trigger saving");
    resetBaud();parameterId=activeId=0;dropBaudAck=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.addressVerified&&!config.writeAcknowledged&&!config.saveAttempted&&idWrites==1&&baudWrites==1&&!saves,
        "partial combined public write is uncertain and never repeated or saved");
    resetBaud();parameterId=activeId=0;wrongBaudReadback=true;
    config=configureTactileDevice(0,0,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(!config.baudVerified&&!config.saveAttempted&&!idWrites&&!saves,"public baud readback mismatch cannot save");
    resetBaud();parameterId=activeId=0;corruptBaudReadback=true;
    config=configureTactileDevice(0,0,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(!config.baudVerified&&!config.saveAttempted&&!idWrites&&!saves,"public baud CRC failure cannot save");
    resetBaud();parameterId=activeId=0;rejectSave=true;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.addressVerified&&config.baudVerified&&!config.saveAcknowledged&&saves==1,
        "public save exception never falls back to another command");
    resetBaud();parameterId=activeId=0;parameterBaud=38400;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600,115200);
    check(config.readOk&&!config.writeAttempted&&!idWrites&&!baudWrites,"public access still rejects an unresolved baud change");
    resetBaud();parameterId=2;
    config=configureTactileDevice(1,3,SaveCandidate::Decimal18013,baudExchange,921600);
    check(config.readOk&&!config.writeAttempted&&!idWrites&&!saves,"unicast configured ID mismatch still blocks writing");
    resetBaud();activeId=0;parameterId=128;
    config=configureTactileDevice(0,1,SaveCandidate::Decimal18013,baudExchange,921600);
    check(!config.readOk&&!config.writeAttempted&&!idWrites,"public preflight rejects stored IDs outside 0-127");
    for(int invalidTarget:{-1,128}){
        config=configureTactileDevice(0,invalidTarget,SaveCandidate::Decimal18013,baudExchange,921600);
        check(config.requests.isEmpty()&&!config.writeAttempted,"public access rejects invalid new IDs before transport");
    }
    config=configureTactileDevice(0,0,SaveCandidate::Decimal18013,baudExchange,921600);
    check(config.requests.isEmpty()&&!config.writeAttempted,"target 0 means retain ID and cannot issue an address-zero write or an empty save");
    check(tactileReadRequest(1).toHex()=="010348440040138f","known request CRC vector");
    check(tactileCrc(QByteArray::fromHex("000343c60002"))==0x6330
        &&tactileCrc(QByteArray::fromHex("0003048000000a"))==0x3443
        &&tactileCrc(QByteArray::fromHex("0003020002"))==0x4504,
        "manual pages 13-14 request and response CRC vectors");
    check(!tactileReadRequest(0).isEmpty()&&tactileReadRequest(128).isEmpty(),"vendor public address supported; invalid ID rejected");
    // COM3 / 921600 captures, 2026-09-29: literal good and corrupt replies, without resealing CRC.
    const auto capturedGood=QByteArray::fromHex(
        "010380ffff00a50000001700ab0000ffc700580000fffd00ae0000ffb3006d00"
        "00001d00ba0000ff8e008b0000ffdd00860000ffdd006e0000ffe0005a000000"
        "1300a20000ffc7002a0000000100320000005200650000ff9bfff60000ffea00"
        "3100000000000000000000000000000000000000000000000000000000000000"
        "000000d94b");
    const auto capturedBad=QByteArray::fromHex(
        "010380ffe300c80000001600c30000ffde00740000002c00db0000ffc1007b00"
        "00fff300e40000ff7f00710000ffe900990000ffeb00630040160020f1ffffff"
        "00010000000020000180000000580040030000001f570008745600081d000001"
        "24000000009c00fc708e0020010000000000000040160020a55c0300ff000000"
        "80000000e1");
    const auto zeroFrame=[&](int type,int ms,int offset=0) {
        auto settings=s;settings.type=type;
        auto frame=serialFrame(decodeTactileResponse(capturedGood,settings),settings,
            QUuid("{578a438a-a6c4-49a9-b341-3a968d224001}"),1,quint64(ms+1),qint64(ms+1)*1000000);
        auto &finger=frame.fingers[type];auto &module=finger.modules[0];module.resultant={};
        for(int p=0;p<module.points.size();++p) {
            module.points[p].force={double(200+p+offset),double(-300-p+offset),double(500+p+offset)};
            module.resultant+=module.points[p].force;
        }
        finger.resultant=module.resultant;return frame;
    };
    for(int type=0;type<3;++type) {
        TactileAutoZero zero;
        for(int ms=0;ms<500;ms+=10)zero.observe(zeroFrame(type,ms,ms<150?8000:(ms%20?10:-10)),ms);
        check(zero.collecting()&&!zero.result().baseline.valid,"auto zero never uses the settling transient or finishes before 500 ms");
        zero.observe(zeroFrame(type,500,-10),500);
        const auto result=zero.result();
        check(result.phase==AutoZeroPhase::Complete&&result.elapsedMs==500&&result.sampleCount==31
            &&validateFrame(result.baseline)&&result.baseline.hardwareType==type,
            "F/G/S auto zero averages a stable native window with valid identity and sums at 500 ms");
        const auto &points=result.baseline.fingers[type].modules[0].points;
        bool mean=true;
        for(int p=0;p<points.size();++p)mean &= sameForce(points[p].force,{double(200+p),double(-300-p),double(500+p)});
        check(mean,"every native point and axis uses its rounded multi-frame mean, independent of selected UI point");
        zero.observe(zeroFrame(type,600,2000),600);
        check(zero.result().baseline.sequence==result.baseline.sequence&&zero.result().elapsedMs==500,
            "auto zero is fixed after completion and cannot absorb later contact");
    }
    {
        TactileAutoZero noisy,drifting,sparse,duplicates,foreign;
        for(int ms=0;ms<=900;ms+=10) {
            noisy.observe(zeroFrame(0,ms,ms%20?200:-200),ms);
            drifting.observe(zeroFrame(0,ms,ms/3),ms);
            if(ms%200==0)sparse.observe(zeroFrame(0,ms),ms);
            duplicates.observe(zeroFrame(0,0),ms);
            auto f=zeroFrame(0,ms);if(ms>0)f.generation=2;foreign.observe(f,ms);
        }
        sparse.advance(900);
        for(const auto *zero:{&noisy,&drifting,&sparse,&duplicates,&foreign})
            check(zero->result().phase==AutoZeroPhase::Complete&&validateFrame(zero->result().baseline)
                &&zero->result().sampleCount==1&&zero->result().elapsedMs==900,
                "deadline automatically sets one zero from the last valid frame when a stable mean is unavailable");
        check(noisy.result().baseline.sequence==901&&drifting.result().baseline.sequence==901
            &&sparse.result().baseline.sequence==801&&duplicates.result().baseline.sequence==1
            &&foreign.result().baseline.generation==1&&foreign.result().baseline.sequence==1,
            "deadline zero uses the latest trusted native frame without accepting duplicates or a foreign session");
        const auto fallbackSequence=noisy.result().baseline.sequence;
        noisy.observe(zeroFrame(0,1000,2000),1000);
        check(noisy.result().baseline.sequence==fallbackSequence&&noisy.result().sampleCount==1,
            "deadline fallback runs only once and does not follow later measurements");
        for(bool badCrc:{false,true}) {
            TactileAutoZero tail;tail.observe(zeroFrame(0,0),0);
            auto bad=zeroFrame(0,901,4000);
            if(badCrc){bad.crcVerified=false;bad.crcOk=false;}else bad.rangeOk=false;
            tail.observe(bad,901);
            check(tail.result().phase==AutoZeroPhase::Complete&&tail.result().baseline.sequence==1
                &&tail.result().baseline.crcOk&&tail.result().baseline.rangeOk,
                "corrupt or out-of-range deadline data cannot replace the previous valid zero candidate");
        }
        TactileAutoZero cancelled;cancelled.observe(zeroFrame(0,0),0);cancelled.cancel();cancelled.advance(900);
        check(cancelled.result().phase==AutoZeroPhase::Failed&&!cancelled.result().baseline.valid,
            "explicit cancellation does not trigger deadline fallback");
        TactileAutoZero invalid;
        for(int ms=0;ms<=1000;ms+=10){auto f=zeroFrame(0,ms);f.crcVerified=false;f.crcOk=false;invalid.observe(f,ms);}
        check(!invalid.result().started&&!invalid.result().baseline.valid,"CRC-off diagnostic data cannot start or initialize auto zero");
        TactileAutoZero gaps;
        for(int ms=0;ms<=500;ms+=10){auto f=zeroFrame(0,ms);if(ms%70==0&&ms>0)f.crcOk=false;gaps.observe(f,ms);}
        check(gaps.result().phase==AutoZeroPhase::Complete&&gaps.result().sampleCount<31,
            "isolated corrupt replies are excluded while a sufficiently dense stable series still completes");
        TactileAutoZero slowWire;
        for(int ms=0;ms<=540;ms+=45)slowWire.observe(zeroFrame(0,ms),ms);
        check(slowWire.result().phase==AutoZeroPhase::Complete&&slowWire.result().elapsedMs<=900,
            "native transmission time at the slow supported baud can still supply a stable multi-frame zero");
    }
    for(int interval:{10,100,1000,60000}) {
        TactileSerialSource zeroing;SerialSettings settings=s;settings.intervalMs=interval;
        zeroing.m_exchangeForTests=[&](const QByteArray &tx){
            return tx==sealConfig(QByteArray::fromHex("010344c00001"))?modelF:capturedGood;
        };
        zeroing.begin(settings,false,true);QElapsedTimer elapsed;elapsed.start();SerialSample sample;
        while(elapsed.elapsed()<1300) {
            SerialSample next;if(zeroing.takeLatest(next))sample=next;
            if(sample.autoZero.phase==AutoZeroPhase::Complete)break;
            QThread::msleep(1);
        }
        check(sample.autoZero.phase==AutoZeroPhase::Complete&&sample.autoZero.elapsedMs>=500
            &&sample.autoZero.elapsedMs<=900&&sample.autoZero.sampleCount>=6&&elapsed.elapsed()<1000
            &&sample.settings.intervalMs==interval&&sample.effectiveIntervalMs==10,
            "10/100/1000/60000 ms preferences all complete the worker zero in 500-900 ms using temporary 10 ms polling");
        const auto sequence=sample.autoZero.baseline.sequence;
        // Delivery is persistent across the latest-value mailbox, and changing
        // the user's interval during/after zero never overwrites their choice.
        zeroing.setIntervalMs(20);elapsed.restart();bool restored=false;
        while(elapsed.elapsed()<300&&!restored){SerialSample next;if(zeroing.takeLatest(next))
            restored=next.settings.intervalMs==20&&next.effectiveIntervalMs==20
                &&next.autoZero.baseline.sequence==sequence;QThread::msleep(1);}
        check(restored,"normal polling resumes at the current requested interval and retains the completed zero result");
        zeroing.stop();check(zeroing.wait(500),"auto-zero worker stops promptly including the longest user interval");
    }
    {
        TactileSerialSource paced;SerialSettings settings;settings.port="OFFLINE_TACTILE_PACING";
        QElapsedTimer elapsed;elapsed.start();std::atomic_int requests{0};
        std::vector<qint64> sent,completed;
        paced.m_exchangeForTests=[&](const QByteArray &tx){
            if(tx==sealConfig(QByteArray::fromHex("010344c00001")))return modelF;
            sent.push_back(elapsed.nsecsElapsed());
            QThread::msleep(2); // Response time must be additional to the gap.
            completed.push_back(elapsed.nsecsElapsed());++requests;
            return tx==tactileReadRequest(1)?capturedGood:QByteArray{};
        };
        check(paced.begin(settings),"offline wire emulator enters the normal serial worker");
        while(requests.load()<48&&elapsed.elapsed()<3000)QThread::msleep(1);
        SerialSample latest;const bool delivered=paced.takeLatest(latest);
        paced.stop();const bool joined=paced.wait(2000);
        check(joined&&requests.load()>=48&&delivered&&latest.accepted&&latest.frame.valid
            &&latest.settings.intervalMs==10&&latest.frame.sequence>1,
            "real worker decodes successive emulator replies at the configured 10 ms gap");
        if(joined&&sent.size()>2){
            double gapSum=0,periodSum=0,minGap=60000,maxGap=0;
            for(size_t i=1;i<sent.size();++i){
                const double gap=(sent[i]-completed[i-1])/1000000.0;
                minGap=std::min(minGap,gap);maxGap=std::max(maxGap,gap);gapSum+=gap;
                periodSum+=(sent[i]-sent[i-1])/1000000.0;
            }
            const double meanGap=gapSum/(sent.size()-1),meanPeriod=periodSum/(sent.size()-1);
            report<<"PACING gap_ms min="<<minGap<<" mean="<<meanGap<<" max="<<maxGap
                <<" mean_request_period_ms="<<meanPeriod<<" requests="<<sent.size()<<'\n';
            check(minGap>=9.9&&meanGap<18&&meanPeriod>meanGap+1,
                "measured post-response gap is at least 10 ms and is not clamped to 20/100 ms");
        }else check(false,"enough completed requests are available for pacing measurement");
    }
    {
        TactileSerialSource edited;SerialSettings settings;settings.port="OFFLINE_TACTILE_INTERVAL_EDIT";settings.intervalMs=60000;
        std::atomic_int requests{0};edited.m_exchangeForTests=[&](const QByteArray &tx){
            if(tx==sealConfig(QByteArray::fromHex("010344c00001")))return modelF;
            ++requests;return capturedGood;
        };
        edited.begin(settings);QElapsedTimer deadline;deadline.start();
        while(requests.load()<1&&deadline.elapsed()<1000)QThread::msleep(1);
        check(requests.load()==1&&!edited.setIntervalMs(5)&&!edited.setIntervalMs(9)&&!edited.setIntervalMs(60001),
            "long request gap remains interruptible and invalid live edits are rejected");
        QThread::msleep(20);deadline.restart();check(edited.setIntervalMs(10),"10 ms live edit is accepted");
        while(requests.load()<5&&deadline.elapsed()<1000)QThread::msleep(1);
        SerialSample sample;const bool delivered=edited.takeLatest(sample);
        check(requests.load()>=5&&deadline.elapsed()<200&&delivered&&sample.settings.intervalMs==10,
            "editing a 60-second wait to 10 ms immediately wakes the worker and updates diagnostics");
        edited.setIntervalMs(60000);QThread::msleep(20);deadline.restart();edited.stop();
        check(edited.wait(500)&&deadline.elapsed()<100&&!edited.takeLatest(sample),
            "stop wakes a long timer promptly and discards late samples");
    }
    const auto capturedSession=QUuid::createUuid();
    const auto trustedSample=decodeTactileResponse(capturedGood,s);
    const auto trustedFrame=serialFrame(trustedSample,s,capturedSession,1,1,1);
    check(capturedGood.size()==133&&tactileCrc(capturedGood.first(131))==0x4bd9
        &&trustedSample.accepted&&trustedSample.crcOk&&trustedSample.rangeOk&&validateFrame(trustedFrame)
        &&trustedFrame.fingers[0].modules[0].points[0].force.x==-1
        &&trustedFrame.fingers[0].modules[0].points[0].force.y==165
        &&trustedFrame.fingers[0].modules[0].points[15].force.x==-22,
        "captured valid reply verifies literal wire CRC and signed values");
    const auto rejectedSample=decodeTactileResponse(capturedBad,s);
    check(capturedBad.size()==133&&!rejectedSample.accepted&&!rejectedSample.crcOk&&!rejectedSample.rangeOk
        &&rejectedSample.rx==capturedBad&&rejectedSample.registers.size()==64
        &&rejectedSample.registers[27]==5632&&rejectedSample.registers[29]==65535,
        "captured corrupt reply rejected with raw bytes and spike retained for diagnosis");
    auto uncheckedSettings=s;uncheckedSettings.verifyCrc=false;
    const auto uncheckedSample=decodeTactileResponse(capturedBad,uncheckedSettings);
    const auto uncheckedFrame=serialFrame(uncheckedSample,uncheckedSettings,capturedSession,1,2,2);
    check(uncheckedSample.accepted&&!uncheckedSample.verifyCrc&&!uncheckedSample.crcOk&&!uncheckedSample.rangeOk
        &&uncheckedSample.rx==rejectedSample.rx&&uncheckedSample.registers==rejectedSample.registers
        &&validateFrame(uncheckedFrame)&&uncheckedFrame.fingers[0].modules[0].points[9].force.z==65535,
        "CRC bypass exposes identical captured raw values with failed integrity flags");
    TactileDisplayFilter capturedDisplay;
    check(capturedDisplay.accept(trustedFrame,5000)&&!capturedDisplay.accept(uncheckedFrame,5000)
        &&capturedDisplay.last.sequence==1&&capturedDisplay.last.fingers[0].modules[0].points[0].force.x==-1,
        "captured CRC-bad spike cannot replace trusted display even with CRC bypass");
    QByteArray response(133,0);response[0]=1;response[1]=3;response[2]=char(128);
    auto word=[&](int i,int v){response[3+2*i]=char(v>>8);response[4+2*i]=char(v);};
    auto seal=[&]{const auto c=tactileCrc(response.first(131));response[131]=char(c);response[132]=char(c>>8);};
    word(0,65513);word(1,225);word(2,199);word(57,123);word(59,32767);word(63,4321);seal();
    auto sample=decodeTactileResponse(response,s);check(sample.accepted&&sample.crcOk&&sample.registers.size()==64&&sample.registers[63]==4321,"64 raw words including reserved");
    for(int type=0;type<3;++type){s.type=type;sample=decodeTactileResponse(response,s);auto f=serialFrame(sample,s,QUuid::createUuid(),1,1,1);
        check(validateFrame(f)&&f.fingers[type].modules[0].points.size()==(type==2?16:20),"F/G 20 and S 16 layouts");
        check(f.fingers[type].modules[0].points[0].force.x==-23&&f.fingers[type].modules[0].points[0].force.z==199,"signed XY, unsigned Z, no raw deadband");
        TactileHistory h;h.begin(f.sessionId,1);check(h.append(f)&&h.samples().size()==1&&!h.append(f),"hardware history and duplicate rejection");
        auto bad=f;bad.fingers[(type+1)%3].valid=true;check(!validateFrame(bad),"hardware frame cannot invent other sensors");
    }
    s.type=0;response[131]^=1;sample=decodeTactileResponse(response,s);check(!sample.accepted&&!sample.crcOk&&sample.registers.size()==64,"CRC reject retains diagnostic raw words");
    s.verifyCrc=false;sample=decodeTactileResponse(response,s);check(sample.accepted&&!sample.crcOk,"CRC bypass explicit and observable");
    auto f=serialFrame(sample,s,QUuid::createUuid(),1,1,1);check(validateFrame(f)&&!f.crcVerified&&!f.crcOk,"unverified hardware provenance");f.crcVerified=true;check(!validateFrame(f),"bad CRC cannot claim verification");
    response[0]=2;check(!decodeTactileResponse(response,s).accepted,"ID remains checked without CRC");response[0]=1;
    response[1]=4;check(!decodeTactileResponse(response,s).accepted,"function remains checked without CRC");response[1]=3;
    check(!decodeTactileResponse(response.first(100),s).accepted&&!decodeTactileResponse(response+QByteArray(1,0),s).accepted,"short and excess frames rejected without CRC");
    word(2,64624);word(0,32768);seal();sample=decodeTactileResponse(response,s);f=serialFrame(sample,s,QUuid::createUuid(),1,1,1);
    check(sample.accepted&&!sample.rangeOk&&validateFrame(f)&&f.fingers[0].modules[0].points[0].force.z==64624,"out of manual range preserved and flagged, never clipped");
    TactileDisplayFilter display;
    check(!display.accept(f,5000)&&!display.last.valid,"range error cannot initialize display");
    word(0,10);word(2,100);seal();sample=decodeTactileResponse(response,s);
    const auto session=QUuid::createUuid();
    auto good=serialFrame(sample,s,session,1,1,1);
    check(display.accept(good,5000),"first trusted raw frame initializes display");
    auto corrupt=good;corrupt.sequence=2;corrupt.monotonicNs=2;corrupt.crcOk=false;
    check(!display.accept(corrupt,5000)&&display.last.sequence==1,"CRC off raw acceptance cannot reach display on CRC failure");
    word(0,5011);seal();sample=decodeTactileResponse(response,s);
    auto spike=serialFrame(sample,s,session,1,3,3);
    check(!display.accept(spike,5000)&&display.last.sequence==1&&spike.fingers[0].modules[0].points[0].force.x==5011,"5001 count spike stays raw but does not advance display reference");
    word(0,5010);seal();sample=decodeTactileResponse(response,s);
    auto boundary=serialFrame(sample,s,session,1,4,4);
    check(display.accept(boundary,5000),"exact 5000 count change allowed against last trusted frame");
    word(0,10);word(2,50);seal();sample=decodeTactileResponse(response,s);
    auto release=serialFrame(sample,s,session,1,5,5);
    check(display.accept(release,5000)&&display.last.sequence==5,"normal frame recovers after rejected spike");
    display.reset();check(!display.last.valid&&display.rejected==0,"reconnect resets display reference and rejection count");
    QByteArray exception=QByteArray::fromHex("0183030000");check(!decodeTactileResponse(exception,s).accepted,"exception never becomes data");
    TactileSerialSource source;s.port="NONEXISTENT_TACTILE_TEST_PORT";source.begin(s);source.wait(2000);SerialSample failed;
    check(!source.isRunning()&&source.takeLatest(failed)&&!failed.portOpen&&!failed.accepted&&!failed.error.isEmpty(),"open failure publishes unavailable state");
    source.stop();check(!source.takeLatest(failed),"stop clears pending samples");
    return failures?1:0;
}

// Explicit bounded hardware diagnostic. Never part of the unattended Release suite.
int runTactileConfigHardware(const QString &path,const QString &port,int baud,int slave,int target,int save,int newBaud)
{
    tactile::SerialSettings settings;settings.port=port;settings.baud=baud;settings.slave=slave;
    if(!settings.valid()||target<0||target>127||save<0||save>2||(!target&&!newBaud&&save)
        ||(newBaud&&(newBaud==baud||(newBaud!=38400&&newBaud!=115200&&newBaud!=921600)))
        ||((target||newBaud)&&((target&&target==slave)||save==0)))return 3;
    QFile file(path);if(!file.open(QIODevice::WriteOnly))return 3;
    tactile::TactileSerialSource source;source.begin(settings);
    QElapsedTimer clock;clock.start();bool queued=false,done=false;tactile::DeviceConfigResult result;
    while(clock.elapsed()<10000){
        tactile::SerialSample sample;
        if(source.takeLatest(sample)&&sample.portOpen&&!queued){queued=source.requestDeviceConfig(target,tactile::SaveCandidate(save),newBaud);}
        if(source.takeConfigResult(result)){done=true;break;}
        if(!source.isRunning())break;
        QThread::msleep(5);
    }
    source.stop();const bool closed=source.wait(2000);QJsonArray traffic;
    for(int i=0;i<result.requests.size();++i)traffic.append(QJsonObject{{"tx",QString::fromLatin1(result.requests[i].toHex(' '))},{"rx",QString::fromLatin1(result.responses.value(i).toHex(' '))}});
    QJsonObject report{{"completed",done},{"closed",closed},{"port",port},{"baud",baud},{"current_id",slave},{"target_id",target},{"save_candidate",save},
        {"target_baud",newBaud},{"original_baud",result.originalBaud},{"baud_readback",result.baudVerified},
        {"configured_id",result.configuredAddress},{"configured_baud",result.configuredBaud},{"responding_id",result.respondingAddress},
        {"read_ok",result.readOk},{"write_attempted",result.writeAttempted},{"write_ack",result.writeAcknowledged},
        {"address_readback",result.addressVerified},{"save_attempted",result.saveAttempted},{"save_ack",result.saveAcknowledged},
        {"persistence_verified",false},{"error",result.error},{"traffic",traffic}};
    file.write(QJsonDocument(report).toJson());return done&&closed&&result.error.isEmpty()?0:1;
}
int runTactileSerialHardware(const QString &path,const QString &port,int baud,int slave,bool crc,int count)
{
    QFile out(path);if(!out.open(QIODevice::WriteOnly|QIODevice::Text))return 3;QTextStream report(&out);
    tactile::SerialSettings s;s.port=port;s.baud=baud;s.slave=slave;s.verifyCrc=crc;
    if(count<1||count>1000||!s.valid())return 3;
    tactile::TactileSerialSource source;source.begin(s);QElapsedTimer deadline;deadline.start();quint64 last=0,accepted=0;
    while(deadline.elapsed()<count*(s.timeoutMs+s.intervalMs+100)+2000) {
        tactile::SerialSample sample;
        if(source.takeLatest(sample)) {
            QJsonArray words;for(auto w:sample.registers)words.append(int(w));
            QJsonObject o{{"requests",double(sample.requests)},{"accepted",sample.accepted},{"crc_ok",sample.crcOk},{"verify_crc",sample.verifyCrc},
                {"range_ok",sample.rangeOk},{"port_open",sample.portOpen},{"error",sample.error},{"crc_errors",double(sample.crcErrors)},
                {"accepted_count",double(sample.acceptedCount)},{"timeouts",double(sample.timeouts)},{"registers",words},
                {"tx",QString::fromLatin1(sample.tx.toHex(' '))},{"rx",QString::fromLatin1(sample.rx.toHex(' '))}};
            report<<QJsonDocument(o).toJson(QJsonDocument::Compact)<<'\n';report.flush();last=sample.requests;accepted=sample.acceptedCount;
            if(!sample.portOpen||last>=quint64(count))break;
        }
        QThread::msleep(5);
    }
    source.stop();const bool closed=source.wait(2000);
    report<<QJsonDocument(QJsonObject{{"closed",closed},{"requests",double(last)},{"accepted_count",double(accepted)}}).toJson(QJsonDocument::Compact)<<'\n';
    return closed&&last>=quint64(count)&&accepted>0?0:1;
}

int runTactileSerialHardwareUi(const QString &path,const QString &port,bool lifecycleOnly)
{
    QFile out(path);if(!out.open(QIODevice::WriteOnly|QIODevice::Text))return 3;QTextStream report(&out);int failures=0;
    const auto check=[&](bool ok,const char *name){report<<(ok?"PASS ":"FAIL ")<<name<<'\n';report.flush();if(!ok)++failures;};
    const auto wait=[](int ms){QElapsedTimer t;t.start();while(t.elapsed()<ms){QApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);}};
    TactileVisualizationWidget w;
    w.findChild<QComboBox*>("tactileDensity")->setCurrentIndex(0);
    w.resize(1060,800);w.show();wait(500);w.start();
    auto *crc=w.findChild<QCheckBox*>("tactileVerifyCrc");auto *connectButton=w.findChild<QPushButton*>("tactileConnect");
    auto *table=w.findChild<QTableWidget*>("tactilePointValues");auto *adapter=w.findChild<TactileSceneAdapter*>();
    check(crc->isChecked()&&!w.frame().valid,"default CRC on; startup waits for device data");
    w.findChild<QPushButton*>("tactileConnectionSettings")->click();w.findChild<QComboBox*>("tactilePort")->setCurrentText(port);connectButton->click();
    if(lifecycleOnly){
        auto *disconnectButton=w.findChild<QPushButton*>("tactileDisconnect");
        int heartbeats=0;qint64 maxHeartbeatGap=0,lastHeartbeat=0;QElapsedTimer clock;clock.start();QTimer heartbeat;
        QObject::connect(&heartbeat,&QTimer::timeout,[&]{const auto now=clock.elapsed();
            maxHeartbeatGap=qMax(maxHeartbeatGap,now-lastHeartbeat);lastHeartbeat=now;++heartbeats;});heartbeat.start(10);
        QUuid previousSession;
        for(int cycle=0;cycle<3;++cycle){
            if(cycle)connectButton->click();
            int frames=0;quint64 sequence=0;QElapsedTimer deadline;deadline.start();
            while(deadline.elapsed()<5000&&frames<12){
                wait(5);const auto &frame=w.frame();
                if(frame.valid&&frame.sourceMode=="Serial"&&frame.crcVerified&&frame.crcOk&&frame.rangeOk&&frame.sequence!=sequence){
                    sequence=frame.sequence;++frames;
                }
            }
            const auto session=w.frame().sessionId;
            bool rows=table->rowCount()==20;
            for(int row=0;row<table->rowCount();++row)for(int col=1;col<4;++col)
                rows=rows&&table->item(row,col)&&table->item(row,col)->text()!="--";
            check(frames>=12&&adapter->valid()&&rows,
                "CRC-verified hardware reaches the model and all 20 table rows");
            check(!session.isNull()&&session!=previousSession,"manual reconnect creates a new hardware session");previousSession=session;
            QElapsedTimer elapsed;elapsed.start();disconnectButton->click();
            const auto disconnectMs=elapsed.elapsed();
            check(disconnectMs<250&&!w.frame().valid&&table->item(0,1)->text()=="--",
                "disconnect button returns promptly and immediately clears hardware data");
            deadline.restart();while(!connectButton->isEnabled()&&deadline.elapsed()<2000)wait(5);
            check(connectButton->isEnabled(),"driver releases the port before manual reconnect is enabled");
            report<<"CYCLE "<<cycle+1<<" frames="<<frames<<" disconnect_ms="<<disconnectMs<<" close_ms="<<deadline.elapsed()<<'\n';report.flush();
            if(!connectButton->isEnabled())break;
        }
        // A silent slave leaves a receive outstanding. This remains a read-only
        // transaction and must not delay closing the window or restore old data.
        auto *slave=w.findChild<QSpinBox*>("tactileSlave");
        {const QSignalBlocker blocker(slave);slave->setValue(127);}
        connectButton->click();wait(40);QElapsedTimer elapsed;elapsed.start();w.close();
        check(elapsed.elapsed()<250&&!w.frame().valid,"closing during an unanswered request returns promptly with data revoked");
        QElapsedTimer deadline;deadline.start();while(!connectButton->isEnabled()&&deadline.elapsed()<2000)wait(5);
        check(connectButton->isEnabled(),"pending receive is cancelled and the port closes within the diagnostic deadline");
        heartbeat.stop();check(heartbeats>5&&maxHeartbeatGap<500,"GUI heartbeat remains responsive throughout hardware connect and disconnect");
        report<<"HEARTBEAT count="<<heartbeats<<" max_gap_ms="<<maxHeartbeatGap<<'\n';
        return failures?1:0;
    }
    for(int phase=0;phase<2;++phase) {
        crc->setChecked(phase==0);bool hardware=false,unverified=false;quint64 sequence=0;QElapsedTimer t;t.start();
        qint64 trustedObservedMs=-1;int heldObservations=0,heldFailures=0;
        while(t.elapsed()<15000) {
            wait(25);const auto &f=w.frame();
            if(f.valid&&f.sourceMode=="Serial"&&f.sequence!=sequence){hardware=true;sequence=f.sequence;
                if(!f.crcOk&&!f.crcVerified)unverified=true;
                if(f.crcOk&&f.rangeOk&&adapter->valid()&&!adapter->sourceLabel().contains("DISPLAY HELD"))trustedObservedMs=t.elapsed();
                report<<"FRAME crc="<<phase<<" seq="<<sequence<<" checked="<<f.crcVerified<<" crc_ok="<<f.crcOk<<" range="<<f.rangeOk<<" XYZ=";
                for(const auto &p:f.fingers[0].modules[0].points)report<<p.force.x<<','<<p.force.y<<','<<p.force.z<<';';report<<'\n';
            }
            // Leave margin for UI polling delay before the 1000 ms trusted-data expiry.
            if(phase==0&&!f.valid&&trustedObservedMs>=0&&t.elapsed()-trustedObservedMs<400) {
                ++heldObservations;
                const bool held=adapter->valid()&&adapter->sourceLabel().contains("DISPLAY HELD")
                    &&table->item(0,1)&&table->item(0,1)->text()!="--"
                    &&w.findChild<QPushButton*>("tactileTare")->isEnabled();
                if(!held)++heldFailures;
                report<<"CRC_ON_HOLD age_observed_ms="<<t.elapsed()-trustedObservedMs
                    <<" visual_valid="<<adapter->valid()<<" labelled="<<adapter->sourceLabel().contains("DISPLAY HELD")<<'\n';
            }
            if(hardware&&t.elapsed()>1000&&t.elapsed()<1100)w.grab().save(QFileInfo(path).absolutePath()+QString("/hardware-%1.png").arg(phase));
        }
        check(hardware&&table->rowCount()==20,"F hardware reaches all 20 UI rows");
        if(phase==0) {
            if(heldObservations)check(heldFailures==0,"CRC-on rejected samples retain labelled trusted display and previous table with tare enabled");
            else report<<"INFO No short rejected-frame interval observed for hardware hold check\n";
            report<<"HOLD observations="<<heldObservations<<" failures="<<heldFailures<<'\n';
        }
        if(phase)check(unverified,"CRC off displays explicitly unverified frames");
    }
    for(int type:{1,2}) {w.findChild<QPushButton*>(QString("finger%1").arg(type))->click();wait(100);
        check(!w.frame().valid&&!adapter->valid()&&table->rowCount()==(type==2?16:20)
            &&table->item(0,1)->text()=="--","unconnected G/S models remain unavailable during F acquisition");}
    w.findChild<QPushButton*>("finger0")->click();w.setSuspended(true);wait(300);const auto n=w.refreshCount();wait(150);check(n==w.refreshCount()&&!w.frame().valid,"hardware visibility suspension clears current sample and suspends updates");w.setSuspended(false);wait(500);
    w.findChild<QPushButton*>("tactileDisconnect")->click();check(!w.frame().valid&&table->item(0,1)->text()=="--","disconnect clears hardware table without mock fallback");
    wait(100);check(!w.findChild<QWidget*>("tactileUseMock")&&!w.frame().valid&&!adapter->valid(),"disconnect cannot return to a simulated source");
    w.close();return failures?1:0;
}
