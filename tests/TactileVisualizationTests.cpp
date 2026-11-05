#include "TactileTestFixtures.h"
#include <QQuickWidget>
#include <QQuickItem>
#include "../sensors/TactileVisualizationWidget.h"
#include "../RenderingSettings.h"
#include "../SensorMainWindow.h"
#include "../LanguageManager.h"
#include "../TactileDockWidget.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHelpEvent>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QLayout>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QSplitter>
#include <QSettings>
#include <QDir>
#include <QProcess>
#include "../sensors/TactileSceneAdapter.h"
#include "../sensors/TactileSurfaceModel.h"
#include <QTextStream>
#include <QThread>
#include <QSemaphore>
#include <QSignalBlocker>
#include <QTranslator>
#include <QPointer>
#include <limits>
#include <QMessageBox>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QAbstractSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QStyle>
#include <QMouseEvent>
#include <atomic>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSerialPortInfo>
#include <qt_windows.h>
#include <dbt.h>

namespace {
double processCpuMs()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto ticks=[](FILETIME time){return (quint64(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
    return (ticks(kernel)+ticks(user))/10000.0;
}
}

int runTactileVisualizationTests(const QString &path)
{
    QFile output(path); if(!output.open(QIODevice::WriteOnly|QIODevice::Text)) return 3;
    QTextStream report(&output); int failures=0;
    const auto folder=QFileInfo(path).absolutePath();
    const auto check=[&](bool ok,const char *name) { report<<(ok?"PASS ":"FAIL ")<<name<<'\n';report.flush();if(!ok) ++failures; };
    const auto wait=[](int ms) { QElapsedTimer t; t.start(); while(t.elapsed()<ms) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(2); } };
    const auto waitFor=[&](auto ready,int timeoutMs=1500) {
        QElapsedTimer deadline;deadline.start();
        while(!ready()&&deadline.elapsed()<timeoutMs)wait(5);
        return ready();
    };
    const auto splitRatio=[](const QList<int> &sizes){return sizes.size()==2&&sizes[0]+sizes[1]>0?double(sizes[0])/(sizes[0]+sizes[1]):-1.0;};
    const auto arguments=QCoreApplication::arguments();
    if(arguments.contains("--tactile-auto-zero-live")) {
        // Explicit, read-only hardware validation with isolated main.cpp settings.
        // This path cannot run in an offline suite or open an unverified adapter.
        const auto port=qEnvironmentVariable("DCTS_TACTILE_LIVE_PORT");
        const auto serial=qEnvironmentVariable("DCTS_TACTILE_LIVE_SERIAL");
        QSerialPortInfo info(port);
        if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"||port.isEmpty()||serial.isEmpty()
            ||info.isNull()||info.serialNumber()!=serial||info.vendorIdentifier()!=0x0403||info.productIdentifier()!=0x6001)return 3;
        QTranslator chinese;
        if(!chinese.load(":/i18n/hand_zh_CN.qm")||!QCoreApplication::installTranslator(&chinese))return 3;
        TactileVisualizationWidget live;live.resize(1280,900);live.show();live.start();wait(250);
        live.m_baud->setCurrentText("921600");live.m_slave->setValue(1);
        live.m_port->setCurrentText(port);live.m_noiseEnabled->setChecked(true);live.m_noiseThreshold->setValue(150);
        live.m_arrowGain->setValue(25);live.m_pointsToggle->setChecked(true);live.m_componentsToggle->setChecked(true);
        for(int interval:{10,100,1000,60000}) {
            live.m_fingers->button(2)->click();live.m_interval->setValue(interval);
            QElapsedTimer elapsed;elapsed.start();live.m_connect->click();
            const bool finished=waitFor([&]{return !live.m_waitingForFirstHardwareFrame
                &&live.m_autoZeroResult.phase!=tactile::AutoZeroPhase::Inactive;},2500);
            const auto result=live.m_autoZeroResult;
            const int wallMs=int(elapsed.elapsed());
            check(finished&&result.phase==tactile::AutoZeroPhase::Complete&&result.elapsedMs>=500
                &&result.elapsedMs<=900&&wallMs<=1000&&result.sampleCount>=6,
                "live connection establishes a stable multi-frame zero within one second");
            check(live.selectedFinger()==0&&live.hasDisplayBaseline()&&live.m_connectionSettings->isChecked()
                &&live.m_zeroStatus->isVisible()&&live.m_zeroStatus->toolTip().contains(QStringLiteral("自动置零完成")),
                "live model detection selects F and displays the Chinese completed status");
            const auto baselineSequence=result.baseline.sequence;
            QJsonArray baselineValues;
            if(result.baseline.valid)for(const auto &point:result.baseline.fingers[0].modules[0].points)
                baselineValues.append(QJsonArray{point.force.x,point.force.y,point.force.z});
            const auto first=live.m_serialSample;
            bool restored=interval==60000;
            if(interval!=60000)restored=waitFor([&]{return live.m_serialSample.frame.sequence>first.frame.sequence
                &&live.m_serialSample.effectiveIntervalMs==interval;},interval+700);
            else {wait(200);restored=live.m_serialSample.requests==first.requests;}
            check(restored&&live.m_interval->value()==interval&&live.m_serialSample.settings.intervalMs==interval
                &&live.m_tareFrame.sequence==baselineSequence,
                "live completion restores the requested polling interval and keeps the zero fixed");
            QJsonObject row{{"interval_ms",interval},{"elapsed_zero_ms",result.elapsedMs},{"elapsed_connection_ms",wallMs},
                {"baseline_frames",result.sampleCount},{"phase",int(result.phase)},{"model",live.m_hardwareType},
                {"requested_interval_after",live.m_serialSample.settings.intervalMs},
                {"effective_interval_after",live.m_serialSample.effectiveIntervalMs},
                {"next_response_gap_ms",double(live.m_serialSample.receivedMs-first.receivedMs)},
                {"crc_errors",double(live.m_serialSample.crcErrors)},{"timeouts",double(live.m_serialSample.timeouts)},
                {"status",live.m_zeroStatus->toolTip()},{"baseline_xyz",baselineValues}};
            report<<QJsonDocument(row).toJson(QJsonDocument::Compact)<<'\n';report.flush();
            if(interval==100)check(live.grab().save(QFileInfo(path).absolutePath()+"/live_zero_complete_100ms.png"),"capture live zero completion with force view");
            live.m_disconnect->click();check(live.m_serial.wait(1500),"live test releases the serial port after each interval");wait(30);
        }
        live.close();QCoreApplication::removeTranslator(&chinese);
        report<<(failures?"RESULT FAIL\n":"RESULT PASS\n");return failures?4:0;
    }
    if(arguments.contains("--tactile-perf-replay")) {
        constexpr int warmupFrames=320, measuredFrames=180, frameMs=33;
        QVector<QByteArray> replies;
        for(int frame=0;frame<warmupFrames+measuredFrames;++frame) {
            QByteArray reply(133,0);
            reply[0]=1;reply[1]=3;reply[2]=char(128);
            for(int point=0;point<20;++point)for(int axis=0;axis<3;++axis) {
                const int value=1200+int(850*std::sin(frame*.067+point*.2+axis));
                const int offset=3+2*(point*3+axis);
                reply[offset]=char(value>>8);reply[offset+1]=char(value);
            }
            const auto crc=tactile::tactileCrc(reply.first(131));
            reply[131]=char(crc);reply[132]=char(crc>>8);
            replies.append(reply);
        }
        QJsonArray results;
        bool valid=true;
        for(int grid:{3,4})for(bool smooth:{false,true}) {
            RenderingSettings::instance()->setLowCostRendering(smooth);
            TactileVisualizationWidget widget;
            widget.resize(1280,800);
            widget.m_density->setCurrentIndex(widget.m_density->findData(grid));
            widget.m_pointsToggle->setChecked(true);
            widget.m_componentsToggle->setChecked(true);
            widget.m_hideReadingsToggle->setChecked(false);
            widget.m_connectionSettings->setChecked(false);
            widget.show();widget.start();widget.m_timer.stop();wait(250);
            widget.m_hardwareType=0;
            const QUuid session("{3571ec6e-cd5f-4f1e-93ad-5da370771fb2}");
            widget.m_serialHistory.begin(session,1);
            const auto feed=[&](int index) {
                tactile::SerialSettings settings;
                settings.port=QStringLiteral("OFFLINE_REPLAY");
                auto sample=tactile::decodeTactileResponse(replies[index],settings);
                sample.portOpen=true;sample.requests=index+1;sample.acceptedCount=index+1;
                QElapsedTimer now;now.start();sample.receivedMs=now.msecsSinceReference();
                sample.frame=tactile::serialFrame(sample,settings,session,1,index+1,qint64(index+1)*frameMs*1000000);
                widget.receiveSerialSample(sample);
            };
            feed(0);widget.setDisplayZero();widget.selectPoint(widget.displayPointCount()-1);
            for(int index=1;index<warmupFrames;++index)feed(index);
            widget.updateValues();wait(200);
            widget.metrics().reset();
            const auto refreshStart=widget.refreshCount();
            QVector<double> submissionTimes;
            QElapsedTimer replay;replay.start();
            const auto cpuStart=processCpuMs();
            for(int index=0;index<measuredFrames;++index) {
                QElapsedTimer submission;submission.start();
                feed(warmupFrames+index);widget.refreshDisplay(true);
                submissionTimes.append(submission.nsecsElapsed()/1e6);
                do {
                    QApplication::processEvents(QEventLoop::AllEvents,5);
                    if(replay.elapsed()<(index+1)*frameMs)QThread::msleep(1);
                } while(replay.elapsed()<(index+1)*frameMs);
            }
            const auto cpuMs=processCpuMs()-cpuStart;
            const double seconds=replay.nsecsElapsed()/1e9;
            const auto &metrics=widget.metrics();
            const auto curve=metrics.curve.json(seconds), view=metrics.view.json(seconds);
            wait(150); // Allow the last coalesced readout to catch up after timing.
            bool finalReadingsMatch=tactile::validateFrame(widget.m_liveFrame);
            for(int row=0;finalReadingsMatch&&row<widget.displayPointCount();++row) {
                const auto &points=widget.m_liveFrame.fingers[0].modules[0].points;
                const auto force=sampleTactileDisplayPoint(points,0,grid,row);
                const auto delta=widget.curveForce(force,row);
                const double axes[]={delta.x,delta.y,delta.z};
                for(int axis=0;axis<3;++axis)
                    finalReadingsMatch &= widget.m_pointTable->item(row,axis+1)->text()==QString::number(axes[axis],'f',0);
            }
            const bool framebufferReady=!widget.m_canvas->grabFramebuffer().isNull();
            const auto published=widget.refreshCount()-refreshStart;
            valid &= widget.m_canvas->status()==QQuickWidget::Ready&&widget.m_adapter->valid()
                &&widget.m_tareFrame.valid&&widget.m_liveFrame.sequence==quint64(replies.size())
                &&widget.m_displayFilter.rejected==0&&published==measuredFrames
                &&framebufferReady&&finalReadingsMatch&&!widget.m_serial.isRunning();
            results.append(QJsonObject{
                {"points",widget.displayPointCount()},{"smooth",smooth},{"input_frames",measuredFrames},
                {"published_frames",double(published)},{"framebuffer_ready",framebufferReady},
                {"final_readings_match",finalReadingsMatch},
                {"seconds",seconds},{"process_cpu_ms",cpuMs},{"process_cpu_ms_per_input",cpuMs/measuredFrames},
                {"submit_median_ms",tactile::MetricStream::percentile(submissionTimes,.5)},
                {"submit_p95_ms",tactile::MetricStream::percentile(submissionTimes,.95)},
                {"view",view},{"curve",curve}});
            widget.close();
        }
        report<<QJsonDocument(QJsonObject{{"ok",valid},{"hardware_started",false},{"cases",results}}).toJson();
        return valid?0:4;
    }
    const int preferencesChild=arguments.indexOf("--tactile-preferences-child");
    if(preferencesChild>=0) {
        const auto directory=arguments.value(preferencesChild+2);
        if(directory.isEmpty())return 3;
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory);
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,directory);
        TactileVisualizationWidget child;
        child.setAttribute(Qt::WA_DontShowOnScreen);child.setAttribute(Qt::WA_ShowWithoutActivating);child.resize(1500,1000);child.show();wait(40);
        if(arguments.value(preferencesChild+1)=="write") {
            child.m_jumpThreshold->setValue(7135);child.m_arrowGain->setValue(17.5);child.m_arrowWidthGain->setValue(2.4);
            child.m_resultantSizeGain->setValue(1.8);
            child.m_noiseThreshold->setValue(125);child.m_noiseEnabled->setChecked(true);
            child.m_reverseZArrows->setChecked(true);
            child.m_density->setCurrentIndex(child.m_density->findData(4));child.m_opacitySlider->setValue(67);
            RenderingSettings::instance()->setLowCostRendering(false);
            child.m_displayMode->setCurrentIndex(child.m_displayMode->findData("dots"));
            child.m_pointsToggle->setChecked(true);child.m_resultantToggle->setChecked(false);child.m_componentsToggle->setChecked(false);
            child.m_statusToggle->setChecked(true);child.m_displaySettings->setChecked(true);
            wait(20);child.m_splitter->setSizes({880,560});wait(10);
            QSettings expected(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::organizationName(),QCoreApplication::applicationName());
            expected.setValue("tactile/display/gridSize",4);
            expected.setValue("tactile/display/statusVisible",true);
            expected.setValue("tactile/display/expanded",true); // Legacy preference must not reopen the panel.
            expected.setValue("rendering/lowCostRendering",false);
            expected.setValue("tests/tactileLayoutRatio",splitRatio(child.m_splitter->sizes()));expected.sync();
            child.close();
            report<<"RESULT PASS\n";return 0;
        }
        check(arguments.value(preferencesChild+1)=="read"
            &&child.m_jumpThreshold->value()==7135&&child.m_arrowGain->value()==17.5
            &&child.m_noiseEnabled->isChecked()&&child.m_noiseThreshold->value()==125&&child.m_noiseThreshold->isEnabled()
            &&child.m_reverseZArrows->isChecked()&&child.m_adapter->reverseZArrows()
            &&child.m_arrowWidthGain->value()==2.4&&child.m_adapter->arrowWidthGain()==2.4
            &&child.m_resultantSizeGain->value()==1.8&&child.m_adapter->resultantSizeGain()==1.8
            &&!child.m_resultantSizeGain->isEnabled()
            &&child.m_displayMode->currentData().toString()=="dots"&&child.m_adapter->dotMatrix()&&!child.m_arrowWidthGain->isEnabled()
            &&child.m_density->currentData().toInt()==4&&child.m_opacitySlider->value()==67
            &&!RenderingSettings::instance()->lowCostRendering()&&!child.m_adapter->lowCostRendering()
            &&child.m_pointsToggle->isChecked()&&!child.m_resultantToggle->isChecked()&&!child.m_componentsToggle->isChecked()
            &&!child.m_statusToggle->isChecked()&&!child.m_displaySettings->isChecked()&&child.m_displayControls->isHidden()
            &&child.m_connectionSettings->isChecked()&&child.m_serialControls->isVisible()
            &&child.m_filterStatus->isHidden()&&!child.m_adapter->communicationStatusVisible()
            &&child.m_adapter->arrowGain()==17.5&&child.m_adapter->gridSize()==4
            &&std::abs(child.m_adapter->modelOpacity()-0.67)<1e-6,
            "fresh process restores display preferences with connection settings open and display settings closed");
        QSettings expected(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::organizationName(),QCoreApplication::applicationName());
        check(child.m_splitterReady&&child.m_splitter->orientation()==Qt::Horizontal
            &&std::abs(splitRatio(child.m_splitter->sizes())-expected.value("tests/tactileLayoutRatio",-1).toDouble())<.02
            &&!child.running()&&!child.m_serial.isRunning(),"fresh process restores tactile split without starting acquisition");
        report<<(failures?"RESULT FAIL\n":"RESULT PASS\n");return failures?4:0;
    }
    const auto preferencesDirectory=QFileInfo(path).absolutePath()+"/tactile_preferences_restart";
    bool restarted=QDir().mkpath(preferencesDirectory);
    for(const auto &mode:{QStringLiteral("write"),QStringLiteral("read")}) {
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(),{"--tactile-ui-smoke-test",preferencesDirectory+"/"+mode+".txt",
            "--tactile-preferences-child",mode,preferencesDirectory});
        const bool finished=process.waitForFinished(20000);
        if(!finished){process.kill();process.waitForFinished(2000);}
        restarted &= finished&&process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0;
        if(!finished||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0)
            report<<mode<<" process: "<<process.errorString()<<' '<<process.readAllStandardError()<<'\n';
    }
    check(restarted,"display settings survive closing one application process and opening another");
    {
        QSettings settings(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::organizationName(),QCoreApplication::applicationName());
        const auto previousNoise=settings.value("tactile/display/noiseEnabled");
        settings.remove("tactile/display/noiseEnabled");settings.remove("tactile/serial");settings.sync();
        {
            TactileVisualizationWidget absent;absent.setAttribute(Qt::WA_DontShowOnScreen);
            absent.m_availableSerialPorts=[]{return QStringList{};};absent.refreshSerialPorts();absent.connectDefaultSerial();
            check(absent.m_defaultConnectRequested&&!absent.m_defaultConnectAttempted&&!absent.m_serial.isRunning(),
                "startup connection requested before visibility waits without acquiring hardware");
            absent.show();wait(40);absent.connectDefaultSerial();
            check(absent.m_connectionSettings->isChecked()&&absent.m_serialControls->isVisible()
                &&!absent.m_serial.isRunning()&&!absent.m_waitingForFirstHardwareFrame,
                "no default serial port keeps connection settings open without opening hardware");
            check(absent.m_interval->minimum()==10&&absent.m_interval->value()==10
                &&absent.m_noiseEnabled->isChecked()&&absent.m_noiseThreshold->isEnabled(),
                "new preferences default to 10 ms and enabled small-reading suppression");
            absent.close();
        }
        const auto reply=[](int x,int z,bool crcOk=true){
            QByteArray bytes(133,0);bytes[0]=char(1);bytes[1]=char(3);bytes[2]=char(128);
            bytes[3]=char(x>>8);bytes[4]=char(x);bytes[7]=char(z>>8);bytes[8]=char(z);
            const auto crc=tactile::tactileCrc(bytes.first(131));bytes[131]=char(crc);bytes[132]=char(crc>>8);
            if(!crcOk)bytes[132]^=1;return bytes;
        };
        const auto good=reply(300,600),changed=reply(700,800),badCrc=reply(300,600,false),badRange=reply(300,40000);
        const auto modelReply=[](const QByteArray &tx,int model=1){
            if(tx.mid(1,5)!=QByteArray::fromHex("0344c00001"))return QByteArray{};
            QByteArray bytes=QByteArray::fromHex("0103020001");bytes[0]=tx[0];bytes[4]=char(model);
            const auto crc=tactile::tactileCrc(bytes);bytes.append(char(crc));bytes.append(char(crc>>8));return bytes;
        };
        std::atomic_int phase{0},requests{0},unexpectedWrites{0};
        {
            TactileVisualizationWidget startup;startup.setAttribute(Qt::WA_DontShowOnScreen);
            QStringList available{"OFFLINE_TACTILE_DEFAULT"};
            startup.m_availableSerialPorts=[&]{return available;};
            startup.m_serial.m_exchangeForTests=[&](const QByteArray &tx){
                if(phase!=4&&!modelReply(tx).isEmpty())return modelReply(tx);
                ++requests;if(tx.size()<2||tx[1]!=3)++unexpectedWrites;
                switch(phase.load()){case 0:return badCrc;case 1:return badRange;case 2:return good;case 4:return QByteArray{};default:return changed;}
            };
            startup.show();wait(40);startup.connectDefaultSerial();
            // CRC/range faults are advanced explicitly below. The separate
            // silent-port case tests the real connection deadline; render cost
            // must not terminate this staged fault fixture before its next phase.
            startup.m_initialResponseTimer.stop();
            waitFor([&]{return startup.m_serialSample.requests>0;});
            check(startup.m_serial.isRunning()&&startup.m_serialSample.portOpen&&requests.load()>0
                &&startup.m_connectionSettings->isChecked()&&!startup.m_tareFrame.valid,
                "opening the port and receiving CRC-bad replies cannot collapse settings or initialize zero");
            check(!startup.findChild<QWidget *>("pauseSimulation")&&!startup.findChild<QWidget *>("scenario")
                &&!startup.findChild<QWidget *>("tactileUseMock"),"retired simulation controls do not exist during acquisition");
            phase=1;waitFor([&]{return startup.m_serialSample.crcOk&&!startup.m_serialSample.rangeOk;});
            check(startup.m_connectionSettings->isChecked()&&!startup.m_tareFrame.valid,
                "CRC-valid out-of-range replies cannot count as a successful initial connection");
            startup.m_fingers->button(1)->click();
            check(!startup.frame().valid&&!startup.m_adapter->valid()&&startup.m_serial.isRunning(),
                "selecting an unconnected model shows no data without pausing serial acquisition");
            phase=2;
            startup.m_fingers->button(0)->click(); // Return to the connected hardware view.
            startup.updateValues();
            check(!startup.hasDisplayBaseline()&&!startup.m_adapter->valid()&&!startup.m_tareButton->isEnabled()
                &&startup.m_zeroStatus->isVisible(),
                "stable zero waits briefly with raw arrows hidden and its status visible independently of communication diagnostics");
            waitFor([&]{return startup.m_tareFrame.valid&&!startup.m_waitingForFirstHardwareFrame;});
            check(startup.m_connectionSettings->isChecked()&&!startup.m_displaySettings->isChecked()&&!startup.m_waitingForFirstHardwareFrame
                &&startup.m_tareFrame.valid&&startup.m_tareFrame.hardwareType==0
                &&startup.m_tareFrame.fingers[0].modules[0].points[0].force.x==300
                &&startup.hasDisplayBaseline()&&startup.selectedFinger()==0,
                "stable native frames select and zero the identified module without changing either settings panel");
            startup.m_fingers->button(0)->click();
            waitFor([&]{return startup.m_pointTable->item(0,1)->text()=="0";});
            check(startup.hasDisplayBaseline()&&startup.m_pointTable->item(0,1)->text()=="0"
                &&startup.frame().fingers[0].modules[0].points[0].force.x==300,
                "automatic delta zero preserves raw readings and uses the manual zero display contract");
            const auto zeroSequence=startup.m_tareFrame.sequence;phase=3;
            waitFor([&]{return startup.m_pointTable->item(0,1)->text()=="400";});
            check(startup.m_tareFrame.sequence==zeroSequence&&startup.m_pointTable->item(0,1)->text()=="400",
                "later frames retain the first zero rather than repeatedly taring the sensor");
            startup.m_connectionSettings->setChecked(true);startup.m_interval->setValue(20);
            waitFor([&]{return startup.m_serialSample.settings.intervalMs==20;});
            startup.m_interval->setValue(10);waitFor([&]{return startup.m_serialSample.settings.intervalMs==10;});
            check(startup.m_interval->isEnabled()&&startup.m_serialSample.settings.intervalMs==10
                &&startup.m_tareFrame.sequence==zeroSequence&&startup.m_connectionSettings->isChecked(),
                "live interval edits reach the worker without replacing zero or collapsing the open card");
            startup.disconnectSerial();check(startup.m_serial.wait(2000),"default-port worker stops asynchronously and joins in the offline check");
            wait(20);phase=2;startup.m_connect->click();
            waitFor([&]{return startup.hasDisplayBaseline()&&!startup.m_waitingForFirstHardwareFrame;});
            check(startup.hasDisplayBaseline()&&startup.m_connectionSettings->isChecked()
                &&startup.m_tareFrame.generation>1&&unexpectedWrites.load()==0,
                "manual reconnect keeps settings open, zeroes its new session once and never writes device registers");
            const auto generation=startup.m_serial.generation();
            startup.m_connectionSettings->click();startup.m_displaySettings->click();
            phase=4;
            waitFor([&]{return startup.m_serialSample.timeouts>=2;});
            const auto scan=[&]{QMetaObject::invokeMethod(&startup.m_portScanTimer,"timeout",Qt::DirectConnection);};
            scan();
            check(startup.m_portScanTimer.isActive()&&startup.m_serialSample.timeouts>=2
                &&startup.m_disconnect->isEnabled()&&!startup.m_connect->isEnabled(),
                "response timeouts with the port still present preserve manual disconnect");
            available={"OFFLINE_OTHER_DEVICE"};
            scan();
            check(startup.m_disconnect->isEnabled(),"one missing port scan leaves the session open");
            scan();
            waitFor([&]{return startup.m_connect->isEnabled();});
            check(!startup.m_serial.isRunning()&&startup.m_connect->isEnabled()&&!startup.m_disconnect->isEnabled()
                &&!startup.m_connectionSettings->isChecked()&&startup.m_displaySettings->isChecked()
                &&!startup.m_tareFrame.valid&&!startup.m_adapter->valid()
                &&startup.m_port->currentIndex()==-1,
                "confirmed port removal preserves both panel choices and clears the hardware session");
            available.append("OFFLINE_TACTILE_DEFAULT");scan();
            wait(5250);
            check(startup.m_serial.generation()==generation+1&&startup.m_serial.isRunning()
                &&startup.m_waitingForFirstHardwareFrame&&!startup.hasDisplayBaseline(),
                "hotplug probe keeps waiting past the old five-second cutoff while the sensor boots");
            phase=2;
            waitFor([&]{return startup.hasDisplayBaseline()&&!startup.m_waitingForFirstHardwareFrame;});
            check(startup.m_port->currentText()=="OFFLINE_TACTILE_DEFAULT"&&startup.m_serial.generation()==generation+1
                &&startup.m_disconnect->isEnabled()&&!startup.m_connect->isEnabled()&&startup.hasDisplayBaseline()
                &&!startup.m_connectionSettings->isChecked()&&startup.m_displaySettings->isChecked(),
                "replug reconnects and zeroes the remembered port without changing either settings panel");
            startup.m_disconnect->click();
            waitFor([&]{return !startup.m_serial.isRunning()&&startup.m_connect->isEnabled();});
            scan();
            check(!startup.m_serial.isRunning()&&startup.m_serial.generation()==generation+1,
                "explicit disconnect suppresses reconnect while the port remains present");
            startup.close();check(startup.m_serial.wait(2000)
                &&!startup.m_connectionSettings->isChecked()&&startup.m_displaySettings->isChecked(),
                "closing the startup widget releases its worker without changing either panel choice");
        }
        settings.sync();
        check(settings.value("tactile/serial/port").toString()=="OFFLINE_TACTILE_DEFAULT"
            &&settings.value("tactile/serial/baud").toInt()==921600&&settings.value("tactile/serial/slave").toInt()==1,
            "only a successful sensor response persists the next default connection");
        {
            TactileVisualizationWidget missing;missing.setAttribute(Qt::WA_DontShowOnScreen);
            QStringList available{"OFFLINE_OTHER_DEVICE"};
            missing.m_availableSerialPorts=[&]{return available;};
            std::atomic_int requestedId{-1},requests{0};
            std::atomic_bool ready{false};
            missing.m_serial.m_exchangeForTests=[&](const QByteArray &tx){
                ++requests;
                if(!tx.isEmpty()&&tx[0]!=0)requestedId=quint8(tx.at(0));
                if(!ready.load())return QByteArray{};
                return modelReply(tx).isEmpty()?good:modelReply(tx);
            };
            missing.show();wait(30);missing.connectDefaultSerial();
            check(!missing.m_serial.isRunning()&&missing.m_connectionSettings->isChecked()&&missing.m_port->currentIndex()<0,
                "a missing remembered port does not auto-connect a different enumerated device");
            missing.m_baud->setCurrentText("115200");missing.m_slave->setValue(7);
            missing.m_connectionSettings->setChecked(false);
            const auto scan=[&]{QMetaObject::invokeMethod(&missing.m_portScanTimer,"timeout",Qt::DirectConnection);};
            available.append("OFFLINE_ANOTHER_DEVICE");scan();
            check(!missing.m_serial.isRunning(),"late arrival does not probe an unrelated port when a target is remembered");
            available.append("OFFLINE_TACTILE_DEFAULT");scan();
            waitFor([&]{return requestedId.load()==7;});
            check(requestedId.load()==7&&missing.m_connectedSettings.baud==115200
                &&missing.m_connectedSettings.port=="OFFLINE_TACTILE_DEFAULT"&&!missing.m_connectionSettings->isChecked(),
                "late arrival probes the remembered port with the currently edited baud and slave ID");
            waitFor([&]{return !missing.m_serial.isRunning()&&missing.m_connect->isEnabled();},11000);
            const auto generation=missing.m_serial.generation();
            const int fastRequests=requests.load();
            scan();scan();
            check(!missing.m_serial.isRunning()&&missing.m_serial.generation()==generation
                &&missing.m_connect->isEnabled()&&missing.m_disconnect->isEnabled()&&missing.m_serialRetryTimer.isActive()
                &&!missing.m_connectionSettings->isChecked()&&missing.m_baud->currentText()=="115200"
                &&missing.m_slave->value()==7&&!missing.hasDisplayBaseline()&&missing.m_serialRetryTimer.interval()==3000,
                "fast probe timeout preserves collapsed settings and waits before retrying");
            // Advance the verified backoff explicitly. A busy renderer must not
            // consume two backoff periods between observations of this fixture.
            missing.m_serialRetryTimer.stop();
            QMetaObject::invokeMethod(&missing.m_serialRetryTimer,"timeout",Qt::DirectConnection);
            waitFor([&]{return missing.m_serial.generation()==generation+1&&!missing.m_serial.isRunning()
                &&missing.m_serialRetryTimer.isActive();});
            check(missing.m_serial.generation()==generation+1&&requests.load()==fastRequests+2
                &&missing.m_connect->isEnabled()&&missing.m_serialRetryTimer.isActive(),
                "failed low-rate retry tries the configured and public addresses once and releases the port");
            missing.m_baud->setCurrentText("921600");missing.m_slave->setValue(1);ready=true;
            missing.m_serialRetryTimer.stop();
            QMetaObject::invokeMethod(&missing.m_serialRetryTimer,"timeout",Qt::DirectConnection);
            check(waitFor([&]{return missing.hasDisplayBaseline();})
                &&missing.m_serial.generation()==generation+2&&missing.m_connectedSettings.baud==921600
                &&missing.m_connectedSettings.slave==1&&!missing.m_serialRetryTimer.isActive()
                &&!missing.m_connectionSettings->isChecked(),
                "later retry uses the edited settings and promotes the same port to normal acquisition");
            ready=false;available.removeAll("OFFLINE_TACTILE_DEFAULT");scan();scan();
            waitFor([&]{return !missing.m_serial.isRunning()&&missing.m_connect->isEnabled();});
            available.append("OFFLINE_TACTILE_DEFAULT");scan();
            QMetaObject::invokeMethod(&missing.m_initialResponseTimer,"timeout",Qt::DirectConnection);
            waitFor([&]{return !missing.m_serial.isRunning()&&missing.m_disconnect->isEnabled();});
            const auto retryGeneration=missing.m_serial.generation();
            missing.m_disconnect->click();scan();scan();
            check(!missing.m_serialRetryTimer.isActive()&&missing.m_serialRetryPort.isEmpty()
                &&missing.m_serial.generation()==retryGeneration&&!missing.m_disconnect->isEnabled(),
                "explicit disconnect cancels pending low-rate retries on the same attachment");
            missing.close();
        }
        {
            TactileVisualizationWidget remembered;remembered.setAttribute(Qt::WA_DontShowOnScreen);
            remembered.m_availableSerialPorts=[]{return QStringList{"OFFLINE_OTHER_DEVICE","OFFLINE_TACTILE_DEFAULT"};};
            std::atomic_int delivered{0};
            remembered.m_serial.m_exchangeForTests=[&](const QByteArray &tx){
                if(!modelReply(tx).isEmpty())return modelReply(tx);
                ++delivered;return good;
            };
            remembered.show();wait(30);remembered.connectDefaultSerial();
            QElapsedTimer deadline;deadline.start();
            while(delivered.load()<55&&deadline.elapsed()<1000)QThread::msleep(1);
            QMetaObject::invokeMethod(&remembered.m_initialResponseTimer,"timeout",Qt::DirectConnection);
            check(remembered.hasDisplayBaseline()&&!remembered.m_waitingForFirstHardwareFrame,
                "delayed GUI timeout consumes an already valid reply before deciding the connection failed");
            wait(60);
            check(remembered.m_connectedSettings.port=="OFFLINE_TACTILE_DEFAULT"&&remembered.hasDisplayBaseline(),
                "next startup chooses the confirmed default port among multiple devices");
            remembered.disconnectSerial();remembered.m_serial.wait(2000);wait(20);
            const auto generation=remembered.m_serial.generation();remembered.connectDefaultSerial();wait(30);
            check(!remembered.m_serial.isRunning()&&remembered.m_serial.generation()==generation,
                "default connection is attempted once and does not undo an explicit disconnect");
            remembered.close();
        }
        settings.remove("tactile/serial");settings.sync();
        {
            LanguageManager language;
            SensorMainWindow window(&language);
            window.setAttribute(Qt::WA_DontShowOnScreen);
            auto &serial=*window.tactile()->display();
            serial.m_availableSerialPorts=[]{return QStringList{"COM241"};};
            serial.m_serial.m_exchangeForTests=[&](const QByteArray &tx){return modelReply(tx).isEmpty()?good:modelReply(tx);};
            window.show();wait(30);serial.connectDefaultSerial();
            check(waitFor([&]{return serial.hasDisplayBaseline();}),"native hotplug fixture starts on its synthetic COM port");
            const auto generation=serial.m_serial.generation();
            settings.setValue("tactile/serial/saveReminderSeen",true);
            const auto editBaud=[&](const QString &baud){
                serial.m_baud->setCurrentText(baud);
                serial.m_baud->activated(serial.m_baud->currentIndex());
            };
            editBaud("115200");
            auto *firstReminder=serial.findChild<QMessageBox *>("tactileSaveReminder");
            check(firstReminder&&firstReminder->isVisible(),"legacy saved flag cannot suppress a new session reminder");
            if(firstReminder){firstReminder->accept();wait(10);}
            editBaud("921600");
            serial.m_disconnect->click();
            waitFor([&]{return !serial.m_serial.isRunning()&&serial.m_connect->isEnabled();});
            serial.m_portScanTimer.stop();
            serial.scanIdleSerialPorts();
            check(serial.m_serial.generation()==generation&&!serial.m_serial.isRunning(),
                "manual disconnect alone does not start another connection");

            struct PortNotice {
                DEV_BROADCAST_HDR header{sizeof(PortNotice),DBT_DEVTYP_PORT,0};
                wchar_t name[7]=L"COM241";
            } notice;
            const auto sendPortEvent=[&](WPARAM event){
                SendMessageW(reinterpret_cast<HWND>(window.winId()),WM_DEVICECHANGE,event,reinterpret_cast<LPARAM>(&notice));
                wait(10);
            };
            sendPortEvent(DBT_DEVICEREMOVECOMPLETE);
            serial.scanIdleSerialPorts();
            check(serial.m_serial.generation()==generation&&!serial.m_serial.isRunning(),
                "removal notification suppresses stale enumeration of the unplugged port");
            sendPortEvent(DBT_DEVICEARRIVAL);
            check(waitFor([&]{return serial.hasDisplayBaseline();})&&serial.m_serial.generation()==generation+1
                &&serial.m_connectionSettings->isChecked(),
                "Windows removal and arrival reconnect the same COM name even when no enumeration sees it absent");
            serial.m_connectionSettings->setChecked(true);
            editBaud("115200");
            auto *reconnectReminder=serial.findChild<QMessageBox *>("tactileSaveReminder");
            check(reconnectReminder&&reconnectReminder->isVisible(),"first baud change after hotplug reconnect prompts again");
            if(reconnectReminder){reconnectReminder->accept();wait(10);}
            editBaud("38400");
            check(!serial.findChild<QMessageBox *>("tactileSaveReminder"),"another edit in the reconnected session does not prompt");
            editBaud("921600");
            serial.m_disconnect->click();
            waitFor([&]{return !serial.m_serial.isRunning()&&serial.m_connect->isEnabled();});
            sendPortEvent(DBT_DEVICEARRIVAL);
            check(serial.m_serial.generation()==generation+1&&!serial.m_serial.isRunning(),
                "duplicate arrival without removal does not override manual disconnect");
            serial.stop();serial.m_serial.wait(2000);window.hide();
        }
        settings.remove("tactile/serial");settings.sync();
        {
            TactileVisualizationWidget silent;silent.setAttribute(Qt::WA_DontShowOnScreen);
            silent.m_availableSerialPorts=[]{return QStringList{"OFFLINE_SILENT_SENSOR"};};
            silent.m_serial.m_exchangeForTests=[](const QByteArray &){return QByteArray{};};
            silent.show();wait(30);silent.connectDefaultSerial();
            waitFor([&]{return !silent.m_serial.isRunning()&&!silent.m_waitingForFirstHardwareFrame;},2000);
            check(!silent.m_serial.isRunning()&&!silent.m_waitingForFirstHardwareFrame
                &&silent.m_connectionSettings->isChecked()&&!silent.m_tareFrame.valid&&silent.m_connect->isEnabled()
                &&!silent.m_serialSample.error.isEmpty(),
                "no valid response times out asynchronously and leaves editable connection settings open");
            check(!settings.contains("tactile/serial/port"),"failed startup does not replace the confirmed default port");
            silent.close();silent.m_serial.wait(2000);
        }
        settings.remove("tactile/serial");
        {
            TactileVisualizationWidget stable;stable.setAttribute(Qt::WA_DontShowOnScreen);
            stable.m_availableSerialPorts=[]{return QStringList{"OFFLINE_STABLE_ZERO"};};
            std::atomic_int count{0};std::atomic_bool unstable{false},readingsReady{false};QElapsedTimer elapsed;
            stable.m_serial.m_exchangeForTests=[&](const QByteArray &tx){
                if(!modelReply(tx).isEmpty())return modelReply(tx);
                if(!readingsReady)return badCrc;
                const int n=++count;
                return reply(unstable?(n%2?800:1600):elapsed.elapsed()<150?8000:1000+(n%2?10:-10),600);
            };
            stable.show();wait(30);stable.refreshSerialPorts();stable.m_port->setCurrentText("OFFLINE_STABLE_ZERO");
            stable.m_slave->setValue(1);
            stable.m_interval->setValue(100);elapsed.start();stable.m_connect->click();
            wait(30);stable.m_interval->setValue(1000);
            check(stable.m_waitingForFirstHardwareFrame&&!stable.m_adapter->valid(),"waiting for valid startup data keeps raw arrows hidden during interval edits");
            // Debug rendering may outlast a nominal wait(200). Release valid
            // data only after the edit so the pending-state check is causal.
            elapsed.restart();readingsReady=true;
            check(waitFor([&]{return stable.hasDisplayBaseline();})&&elapsed.elapsed()<1000
                &&std::abs(stable.m_tareFrame.fingers[0].modules[0].points[0].force.x-1000)<=1,
                "startup spike is discarded and interval edits during zero retain the stable mean within one second");
            check(stable.m_serialSample.settings.intervalMs==1000&&stable.m_tareButton->isEnabled()
                &&stable.m_autoZeroResult.sampleCount>=6&&stable.m_zeroStatus->isVisible()
                &&!stable.m_statusToggle->isChecked(),"completion status and sample count stay visible with communication details hidden");
            const auto &raw=stable.m_displayFilter.last;
            check(raw.valid&&std::abs(raw.fingers[0].modules[0].points[0].force.x-1000)==10
                &&stable.m_serialHistory.samples().size()==1
                &&stable.m_serialHistory.samples().back().pointForces[0][0].x==raw.fingers[0].modules[0].points[0].force.x,
                "averaged display baseline never replaces the acquired raw frame or history measurement");
            stable.disconnectSerial();stable.m_serial.wait(1000);wait(20);
            unstable=true;stable.m_interval->setValue(60000);elapsed.restart();stable.m_connect->click();
            check(waitFor([&]{return !stable.m_waitingForFirstHardwareFrame;})&&elapsed.elapsed()<1100
                &&stable.m_autoZeroResult.phase==tactile::AutoZeroPhase::Complete&&stable.hasDisplayBaseline()
                &&stable.m_autoZeroResult.sampleCount==1&&stable.m_autoZeroResult.elapsedMs>=900
                &&stable.m_adapter->valid()&&stable.m_connectionSettings->isChecked()&&stable.m_tareButton->isEnabled(),
                "unstable data receives one single-frame zero at the deadline without collapsing settings");
            check(stable.m_tareFrame.sequence==stable.m_serialSample.frame.sequence
                &&stable.m_pointTable->item(0,1)->text()=="0"&&stable.m_zeroStatus->isVisible()
                &&stable.m_zeroStatus->toolTip()==stable.tr("Auto zero complete · %1 frames · %2 ms")
                    .arg(1).arg(stable.m_autoZeroResult.elapsedMs),
                "deadline fallback zeroes the current displayed frame and shows only the ordinary completed status");
            const auto sequence=stable.m_serialSample.frame.sequence;
            wait(60);check(stable.m_serialSample.frame.sequence==sequence,"deadline fallback restores the long user interval");
            unstable=false;stable.m_interval->setValue(10);
            waitFor([&]{return stable.m_serialSample.frame.sequence>sequence;});
            check(stable.m_tareFrame.sequence==sequence&&stable.m_autoZeroResult.sampleCount==1,
                "later frames do not automatically repeat the deadline zero");
            stable.m_tareButton->click();
            check(stable.hasDisplayBaseline()&&stable.m_autoZeroResult.phase==tactile::AutoZeroPhase::Inactive,
                "manual zero remains available after the automatic deadline fallback");
            stable.stop();check(stable.m_serial.wait(500),"stop during long normal polling releases the auto-zero session");stable.close();
            settings.remove("tactile/serial");settings.sync();
        }
        for(int model=1;model<=3;++model){
            settings.remove("tactile/serial");settings.sync();
            TactileVisualizationWidget detected;detected.setAttribute(Qt::WA_DontShowOnScreen);
            detected.m_availableSerialPorts=[]{return QStringList{"OFFLINE_MODEL_DETECTION"};};
            std::atomic_int address{1},writes{0};
            detected.m_serial.m_exchangeForTests=[&](const QByteArray &tx){
                if(tx[1]!=3){++writes;return QByteArray{};}
                // This fixture deliberately does not support public address 0.
                if(quint8(tx[0])!=address.load())return QByteArray{};
                const auto identity=modelReply(tx,model);
                if(!identity.isEmpty())return identity;
                auto data=good;data[0]=tx[0];
                if(model==3){
                    // S has 16 native points. Unused F/G point slots must not
                    // participate in range checks or become synthetic points.
                    for(int i=48;i<60;++i){data[3+2*i]=char(0xff);data[4+2*i]=char(0xff);}
                }
                const auto crc=tactile::tactileCrc(data.first(131));data[131]=char(crc);data[132]=char(crc>>8);
                return data;
            };
            detected.m_fingers->button(model%3)->click();
            detected.show();wait(30);detected.connectDefaultSerial();
            check(waitFor([&]{return detected.hasDisplayBaseline();})
                &&detected.selectedFinger()==model-1&&detected.m_hardwareType==model-1
                &&detected.m_fingers->checkedId()==model-1&&detected.m_serialSample.modelIdentified
                &&detected.frame().fingers[model-1].modules[0].points.size()==(model==3?16:20),
                "startup from the wrong selected model identifies F/G/S and uses its native data layout");
            detected.m_disconnect->click();detected.m_serial.wait(2000);wait(20);
            address=9;
            detected.m_fingers->button(model%3)->click();detected.m_slave->setValue(7);detected.m_connect->click();
            check(waitFor([&]{return !detected.m_serial.isRunning()&&!detected.m_waitingForFirstHardwareFrame;},2500)
                &&detected.m_connectionSettings->isChecked()&&!detected.hasDisplayBaseline()
                &&detected.selectedFinger()==model%3&&detected.m_configStatus->isVisible(),
                "failed configured and public addresses leave the chosen model and editable connection settings visible");
            detected.m_baud->setCurrentText("115200");detected.m_slave->setValue(9);detected.m_connect->click();
            check(waitFor([&]{return detected.hasDisplayBaseline();})
                &&detected.selectedFinger()==model-1&&detected.m_hardwareType==model-1
                &&detected.m_connectedSettings.slave==9&&detected.m_connectedSettings.baud==115200
                &&detected.m_connectionSettings->isChecked()&&detected.m_adapter->valid()
                &&detected.m_tareFrame.hardwareType==model-1&&writes.load()==0,
                "manual correction from the wrong model routes F/G/S to its matching 3D view and zeroes once");
            settings.sync();
            check(settings.value("tactile/serial/type").toInt()==model-1
                &&settings.value("tactile/serial/slave").toInt()==9,
                "confirmed model and actual communication ID replace the old saved selection");
            detected.stop();detected.m_serial.wait(2000);detected.close();
        }
        settings.remove("tactile/serial");settings.sync();
        {
            TactileVisualizationWidget recovered;recovered.setAttribute(Qt::WA_DontShowOnScreen);
            recovered.m_availableSerialPorts=[]{return QStringList{"OFFLINE_PUBLIC_RECOVERY"};};
            std::atomic_int writes{0};
            recovered.m_serial.m_exchangeForTests=[&](const QByteArray &tx){
                if(tx[1]!=3){++writes;return QByteArray{};}
                if(tx[0]!=0&&tx[0]!=9)return QByteArray{};
                const auto identity=modelReply(tx,2);
                if(!identity.isEmpty())return identity;
                QByteArray bytes;
                if(tx.mid(2,2)==QByteArray::fromHex("466c"))bytes=QByteArray::fromHex("0003020009");
                else bytes=good.first(131);
                bytes[0]=tx[0];const auto crc=tactile::tactileCrc(bytes);bytes.append(char(crc));bytes.append(char(crc>>8));return bytes;
            };
            recovered.show();wait(30);recovered.connectDefaultSerial();
            check(waitFor([&]{return recovered.hasDisplayBaseline();})
                &&recovered.selectedFinger()==1&&recovered.m_connectedSettings.slave==9&&recovered.m_slave->value()==9
                &&recovered.m_serialSample.settings.slave==9&&recovered.m_serialSample.rx[0]==9
                &&!recovered.m_serial.requestDeviceConfig(9,tactile::SaveCandidate::Decimal18013)&&writes.load()==0,
                "public-address discovery updates UI, data reads and configuration validation to the actual device ID");
            recovered.stop();recovered.m_serial.wait(2000);recovered.close();
        }
        settings.remove("tactile/serial");
        if(previousNoise.isValid())settings.setValue("tactile/display/noiseEnabled",previousNoise);
        else settings.remove("tactile/display/noiseEnabled");
        settings.sync();
    }
    {
        QSettings layoutSettings(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::organizationName(),QCoreApplication::applicationName());
        const auto oldHorizontal=layoutSettings.value("tactile/layout/horizontal"),oldVertical=layoutSettings.value("tactile/layout/vertical");
        layoutSettings.remove("tactile/layout");layoutSettings.sync();
        double horizontal=-1,vertical=-1;
        {
            TactileVisualizationWidget layout;layout.setAttribute(Qt::WA_DontShowOnScreen);layout.setAttribute(Qt::WA_ShowWithoutActivating);
            layout.resize(1500,1000);layout.show();wait(40);
            check(layout.findChild<QSplitter *>("tactileContentSplitter")==layout.m_splitter&&layout.m_splitterReady,"named tactile splitter restores after initial layout");
            layout.m_splitter->setSizes({850,550});wait(10);horizontal=splitRatio(layout.m_splitter->sizes());
            QMetaObject::invokeMethod(layout.m_splitter,"splitterMoved",Qt::DirectConnection,Q_ARG(int,layout.m_splitter->sizes()[0]),Q_ARG(int,1));
            layoutSettings.sync();check(!layoutSettings.value("tactile/layout/horizontal").toByteArray().isEmpty(),"tactile split movement saves immediately");
            layout.resize(600,1450);wait(30);layout.m_splitter->setSizes({450,750});wait(10);vertical=splitRatio(layout.m_splitter->sizes());
            QMetaObject::invokeMethod(layout.m_splitter,"splitterMoved",Qt::DirectConnection,Q_ARG(int,layout.m_splitter->sizes()[0]),Q_ARG(int,1));
            check(layout.m_splitter->orientation()==Qt::Vertical,"narrow tactile layout remains vertical");
            layout.resize(1500,1000);wait(30);
            check(layout.m_splitter->orientation()==Qt::Horizontal&&std::abs(splitRatio(layout.m_splitter->sizes())-horizontal)<.02,"tactile wide narrow wide preserves the horizontal split");
            layout.close();layout.show();wait(20);
            check(std::abs(splitRatio(layout.m_splitter->sizes())-horizontal)<.02&&!layout.running()&&!layout.m_serial.isRunning(),"reopening tactile layout retains split without acquisition");
            layout.close();
        }
        {
            TactileVisualizationWidget restored;restored.setAttribute(Qt::WA_DontShowOnScreen);restored.setAttribute(Qt::WA_ShowWithoutActivating);
            restored.resize(1500,1000);restored.show();wait(40);
            check(std::abs(splitRatio(restored.m_splitter->sizes())-horizontal)<.02,"new tactile widget restores horizontal split");
            restored.resize(600,1450);wait(30);
            check(restored.m_splitter->orientation()==Qt::Vertical&&std::abs(splitRatio(restored.m_splitter->sizes())-vertical)<.02,"new tactile widget restores independent vertical split");
            restored.close();
        }
        layoutSettings.setValue("tactile/layout/horizontal",QByteArray("invalid splitter state"));layoutSettings.sync();
        {
            TactileVisualizationWidget invalid;invalid.setAttribute(Qt::WA_DontShowOnScreen);invalid.setAttribute(Qt::WA_ShowWithoutActivating);
            invalid.resize(1500,1000);invalid.show();wait(40);const auto sizes=invalid.m_splitter->sizes();
            check(sizes.size()==2&&sizes[0]>0&&sizes[1]>0&&!invalid.m_splitter->childrenCollapsible()
                &&invalid.m_splitter->orientation()==Qt::Horizontal&&!invalid.running(),"bad tactile split state falls back to visible noncollapsible panes and stays stopped");
            invalid.close();
        }
        layoutSettings.remove("tactile/layout");
        if(oldHorizontal.isValid())layoutSettings.setValue("tactile/layout/horizontal",oldHorizontal);
        if(oldVertical.isValid())layoutSettings.setValue("tactile/layout/vertical",oldVertical);
        layoutSettings.sync();
    }
    using namespace tactile;
    auto frame=tactileTestFrame();
    check(validateFrame(frame),"canonical 20 / 20 / 16 frame and all resultants");
    auto bad=frame; bad.fingers.removeLast(); check(!validateFrame(bad),"wrong finger count");
    bad=frame; bad.fingers[1].id=bad.fingers[0].id; check(!validateFrame(bad),"duplicate finger ID");
    bad=frame; bad.fingers[0].modules[0].id="unknown"; check(!validateFrame(bad),"unknown module ID");
    bad=frame; bad.fingers[0].modules.append(bad.fingers[0].modules[0]); check(!validateFrame(bad),"duplicate module ID");
    bad=frame; bad.fingers[0].modules[0].points[1].id="P1"; check(!validateFrame(bad),"duplicate point ID");
    bad=frame; bad.fingers[0].modules[0].points[0].id="P9"; check(!validateFrame(bad),"unknown point ID");
    bad=frame; bad.fingers[0].modules.removeLast(); check(!validateFrame(bad),"wrong module count");
    bad=frame; bad.fingers[0].modules[0].points.removeLast(); check(!validateFrame(bad),"wrong point count");
    bad=frame; bad.fingers[0].modules[0].points[0].force.x=std::numeric_limits<double>::quiet_NaN(); check(!validateFrame(bad),"NaN point");
    bad=frame; bad.fingers[0].resultant.z=std::numeric_limits<double>::infinity(); check(!validateFrame(bad),"infinite resultant");
    bad=frame; bad.fingers[0].modules[0].resultant.x+=1; check(!validateFrame(bad),"inconsistent module sum");
    bad=frame; bad.fingers[0].resultant.y+=1; check(!validateFrame(bad),"inconsistent fingertip sum");
    bad=frame; bad.fingers[0].modules[0].points[0].valid=false; check(!validateFrame(bad),"invalid measurement state");
    bad=frame; bad.schemaVersion=2; check(!validateFrame(bad),"unknown schema");
    bad=frame; bad.layoutId="other"; check(!validateFrame(bad),"unknown layout");
    FrameGate gate; gate.reset(frame.sessionId,frame.generation);
    check(gate.accept(frame)&&!gate.accept(frame),"duplicate sequence rejected");
    bad=frame; ++bad.sequence; check(!gate.accept(bad),"non-increasing monotonic time rejected");
    ++bad.monotonicNs; ++bad.generation; check(!gate.accept(bad),"old/wrong generation rejected");
    bad=frame; bad.sessionId=QUuid::createUuid(); ++bad.sequence; ++bad.monotonicNs; check(!gate.accept(bad),"wrong session rejected");
    TactileHistory history; history.begin(frame.sessionId,frame.generation);
    bool historyOk=true;
    for(int i=0;i<1000;++i) { auto sample=frame; sample.sequence=i+1; sample.monotonicNs=qint64(i)*33000000;
        historyOk &= history.append(sample); }
    check(historyOk && history.samples().size()<=TactileHistory::Capacity && history.samples().back().timeNs-history.samples().front().timeNs<=TactileHistory::WindowNs,"history bounded by count and ten seconds");
    bool mapped=true;
    for(int f=0;f<3;++f) { if(!frame.fingers[f].valid){mapped &= history.samples().back().pointForces[f].isEmpty();continue;} mapped &= sameForce(history.samples().back().forces[f][0],frame.fingers[f].resultant);
        for(int m=0;m<1;++m) mapped &= sameForce(history.samples().back().forces[f][m+1],frame.fingers[f].modules[m].resultant); }
    check(mapped,"history maps all sensor resultant scopes");
    auto next=frame; next.sequence=1001;next.monotonicNs=33000000000LL;
    history.breakSegment();check(history.append(next)&&history.samples().back().startsSegment,"pause marks a curve discontinuity");
    check(!history.samples().back().bridgeFromPrevious,"pause discontinuity cannot be interpolated");
    ++next.sequence;next.monotonicNs+=TactileHistory::BridgeWindowNs;history.breakSegment(true);
    check(history.append(next)&&history.samples().back().startsSegment&&history.samples().back().bridgeFromPrevious,"missing-data gap up to 500 ms permits only a marked display bridge");
    ++next.sequence;next.monotonicNs+=TactileHistory::BridgeWindowNs+1;history.breakSegment(true);
    check(history.append(next)&&!history.samples().back().bridgeFromPrevious,"missing-data gap beyond 500 ms remains disconnected");
    ++next.sequence;next.monotonicNs+=100000000;history.breakSegment();history.breakSegment(true);
    check(history.append(next)&&!history.samples().back().bridgeFromPrevious,"subsequent data errors cannot override a hard pause boundary");
    history.begin(QUuid::createUuid(),2);check(history.samples().empty()&&!history.append(next),"new session clears history and rejects previous identity");
    {
        TactileVisualizationWidget preferences;
        preferences.resize(1000,900);preferences.show();wait(30);
        auto *drawer=preferences.findChild<QPushButton *>("tactileDisplaySettings");
        auto *controls=preferences.findChild<QWidget *>("tactileDisplayControls");
        auto *statusToggle=preferences.findChild<QPushButton *>("tactileShowStatus");
        check(drawer&&controls&&statusToggle,"display settings drawer and communication-status toggle exist");
        if(drawer&&controls&&statusToggle) {
            check(drawer->isCheckable()&&!drawer->isChecked()&&controls->isHidden()
                &&drawer->geometry().right()<preferences.m_tareButton->geometry().left()
                &&!preferences.findChild<QPushButton *>("tactileRawData")
                &&!statusToggle->isChecked()&&preferences.m_serialStatus->isHidden(),
                "display settings start collapsed with communication status off and no raw diagnostics button");
            drawer->click();wait(30);
            {
                auto *minimizeConnection=preferences.findChild<QToolButton *>("tactileMinimizeConnection");
                auto *minimizeDisplay=preferences.findChild<QToolButton *>("tactileMinimizeDisplay");
                check(minimizeConnection&&minimizeDisplay&&!minimizeConnection->icon().isNull()&&!minimizeDisplay->icon().isNull()
                    &&!minimizeConnection->accessibleName().isEmpty()&&!minimizeDisplay->accessibleName().isEmpty(),
                    "both settings cards have accessible minimize icon buttons");
                if(minimizeConnection&&minimizeDisplay) {
                    preferences.m_connectionSettings->setChecked(true);wait(20);
                    const int baud=preferences.m_baud->currentIndex(),slave=preferences.m_slave->value();
                    const double gain=preferences.m_arrowGain->value();
                    check(minimizeConnection->mapTo(preferences.m_serialControls,QPoint()).x()>preferences.m_serialControls->width()/2
                        &&minimizeConnection->mapTo(preferences.m_serialControls,QPoint()).y()<12
                        &&minimizeDisplay->mapTo(controls,QPoint()).x()>controls->width()/2
                        &&minimizeDisplay->mapTo(controls,QPoint()).y()<12,
                        "minimize buttons occupy the upper right of their own cards");
                    minimizeConnection->click();wait(20);
                    check(!preferences.m_connectionSettings->isChecked()&&preferences.m_serialControls->isHidden()
                        &&drawer->isChecked()&&controls->isVisible()&&!preferences.m_portScanTimer.isActive(),
                        "connection minimize closes only its card and stops collapsed port discovery");
                    preferences.m_connectionSettings->click();wait(20);
                    check(preferences.m_serialControls->isVisible()&&preferences.m_portScanTimer.isActive()
                        &&preferences.m_baud->currentIndex()==baud&&preferences.m_slave->value()==slave&&!preferences.m_serial.isRunning(),
                        "top connection button reopens unchanged settings without connecting a device");
                    minimizeDisplay->click();wait(20);
                    check(!drawer->isChecked()&&controls->isHidden()&&preferences.m_serialControls->isVisible()
                        &&preferences.m_arrowGain->value()==gain,
                        "display minimize closes only its card without changing display preferences");
                    drawer->click();wait(20);
                    check(controls->isVisible()&&preferences.m_arrowGain->value()==gain&&!preferences.running(),
                        "top display button reopens unchanged settings without starting acquisition");
                    minimizeConnection->click();wait(20);
                }
            }
            bool switches=true;
            for(auto *toggle:{preferences.m_reverseZArrows,preferences.m_noiseEnabled,preferences.m_pointsToggle,
                preferences.m_statusToggle,preferences.m_hideReadingsToggle}) {
                auto *caption=preferences.findChild<QLabel *>(toggle->objectName()+"Label");
                const bool checked=toggle->isChecked();toggle->click();
                switches &= toggle->isCheckable()&&toggle->isChecked()!=checked&&toggle->size()==preferences.m_resultantToggle->size()
                    &&caption&&caption->buddy()==toggle&&!caption->text().isEmpty();
                toggle->click();switches &= toggle->isChecked()==checked;
            }
            wait(20);
            check(switches,"all five display options use labeled compact switches and retain on/off interaction");
            bool allMoved=controls->isVisible();
            for(QWidget *control:QList<QWidget *>{preferences.m_jumpThreshold,preferences.m_arrowGain,preferences.m_arrowWidthGain,
                preferences.m_displayMode,preferences.m_density,preferences.m_opacitySlider,preferences.m_pointsToggle,
                preferences.m_resultantToggle,preferences.m_componentsToggle,statusToggle})
                allMoved &= controls->isAncestorOf(control)&&control->isVisible();
            check(allMoved,"display drawer contains threshold, length and thickness gains, interpolation, opacity, arrow toggles and status visibility");
            check(preferences.m_displayMode->count()==2&&preferences.m_displayMode->currentData().toString()=="xyz"
                &&!preferences.m_adapter->dotMatrix(),"older preferences default to XYZ axes with the new dot matrix option available");
            check(preferences.m_arrowWidthGain->minimum()==0.1&&preferences.m_arrowWidthGain->maximum()==10
                &&preferences.m_arrowWidthGain->value()==1&&preferences.m_adapter->arrowWidthGain()==1,
                "existing preferences without thickness restore the original arrow widths");
            check(preferences.m_resultantSizeGain->minimum()==0.1&&preferences.m_resultantSizeGain->maximum()==10
                &&preferences.m_resultantSizeGain->value()==1&&preferences.m_adapter->resultantSizeGain()==1,
                "existing preferences default to the original resultant arrow size");
            check(preferences.grab().save(QFileInfo(path).absolutePath()+"/tactile_display_settings.png"),"save expanded display settings capture");
            {
                const auto captureFolder=QFileInfo(path).absolutePath();
                const bool connectionOpen=preferences.m_connectionSettings->isChecked();
                const QRect sceneBefore(preferences.m_canvas->mapTo(&preferences,QPoint()),preferences.m_canvas->size());
                preferences.m_connectionSettings->setChecked(true);wait(20);
                auto *sidebar=preferences.findChild<QScrollArea *>("tactileDetailsScroll");
                auto *readings=preferences.findChild<QWidget *>("tactileReadingsPanel");
                check(sidebar&&sidebar->isAncestorOf(controls)&&sidebar->isAncestorOf(preferences.m_serialControls)
                    &&sidebar->isAncestorOf(readings)
                    &&preferences.m_serialControls->mapTo(sidebar->widget(),QPoint()).y()<controls->mapTo(sidebar->widget(),QPoint()).y()
                    &&controls->mapTo(sidebar->widget(),QPoint(0,controls->height())).y()<=readings->mapTo(sidebar->widget(),QPoint()).y()
                    &&controls->mapTo(&preferences,QPoint()).x()>=sceneBefore.right()
                    &&QRect(preferences.m_canvas->mapTo(&preferences,QPoint()),preferences.m_canvas->size())==sceneBefore,
                    "both settings cards sit above readings in the right sidebar without moving the model viewport");
                sidebar->verticalScrollBar()->setValue(sidebar->verticalScrollBar()->maximum());wait(10);
                check(sidebar->verticalScrollBar()->maximum()>0
                    &&QRect(preferences.m_canvas->mapTo(&preferences,QPoint()),preferences.m_canvas->size())==sceneBefore,
                    "scrolling the settings sidebar preserves the model viewport");
                sidebar->verticalScrollBar()->setValue(0);
                const QList<QWidget *> fixedControls={preferences.m_port,preferences.m_baud,preferences.m_slave,
                    preferences.m_timeout,preferences.m_interval,preferences.m_jumpThreshold,preferences.m_noiseThreshold,
                    preferences.m_arrowGain,preferences.m_arrowWidthGain,preferences.m_displayMode,preferences.m_density,
                    preferences.m_connect,preferences.m_disconnect,
                    preferences.m_writeAddress,preferences.m_scanPorts,preferences.m_resultantToggle,preferences.m_componentsToggle,
                    preferences.m_resetView,preferences.m_frontView,preferences.m_tareButton};
                QList<QSize> sizes;for(auto *control:fixedControls)sizes.append(control->size());
                preferences.resize(1600,1100);wait(30);
                bool fixed=true;
                for(int i=0;i<fixedControls.size();++i)fixed &= fixedControls[i]->size()==sizes[i];
                check(fixed,"dropdowns, numeric editors, switches and action buttons keep their size when the dock grows");
                bool fullOptions=true;
                for(auto *combo:{preferences.m_baud,preferences.m_displayMode,preferences.m_density}) {
                    QStyleOptionComboBox option;option.initFrom(combo);option.editable=combo->isEditable();
                    const auto textArea=combo->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,combo);
                    for(int i=0;i<combo->count();++i)fullOptions &= combo->fontMetrics().horizontalAdvance(combo->itemText(i))<=textArea.width();
                }
                check(fullOptions,"fixed dropdowns show complete baud rates and translated option names beside the chevron");
                const auto editorRight=[&](QWidget *control){return control->mapTo(controls,QPoint(control->width(),0)).x();};
                bool aligned=true;
                for(QWidget *control:QList<QWidget *>{preferences.m_displayMode,preferences.m_jumpThreshold,preferences.m_noiseThreshold,
                    preferences.m_arrowGain,preferences.m_arrowWidthGain,preferences.m_resultantSizeGain,preferences.m_density}) {
                    auto *caption=preferences.findChild<QLabel *>(control->objectName()+"Label");
                    auto *unit=preferences.findChild<QLabel *>(control->objectName()+"Unit");
                    aligned &= caption&&caption->buddy()==control&&!caption->text().isEmpty()
                        &&caption->mapTo(controls,QPoint()).x()<control->mapTo(controls,QPoint()).x()
                        &&editorRight(unit?static_cast<QWidget *>(unit):control)==editorRight(preferences.m_displayMode);
                }
                check(aligned,"display captions stay on the left and editor groups including trailing units share the right edge");
                bool numericOnly=true,stepButtons=true;
                const auto clickSpin=[](QAbstractSpinBox *spin,const QPoint &point) {
                    QMouseEvent press(QEvent::MouseButtonPress,QPointF(point),QPointF(spin->mapToGlobal(point)),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                    QApplication::sendEvent(spin,&press);
                    QMouseEvent release(QEvent::MouseButtonRelease,QPointF(point),QPointF(spin->mapToGlobal(point)),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                    QApplication::sendEvent(spin,&release);
                };
                for(auto *spin:preferences.findChildren<QAbstractSpinBox *>()) {
                    auto *caption=preferences.findChild<QLabel *>(spin->objectName()+"Label");
                    auto *unit=preferences.findChild<QLabel *>(spin->objectName()+"Unit");
                    if(auto *integer=qobject_cast<QSpinBox *>(spin))numericOnly &= integer->prefix().isEmpty()&&integer->suffix().isEmpty();
                    if(auto *decimal=qobject_cast<QDoubleSpinBox *>(spin))numericOnly &= decimal->prefix().isEmpty()&&decimal->suffix().isEmpty();
                    numericOnly &= caption&&caption->buddy()==spin&&!caption->text().isEmpty()
                        &&(spin==preferences.m_slave||(unit&&!unit->text().isEmpty()
                            &&unit->mapTo(spin->parentWidget(),QPoint()).x()>spin->geometry().right()));
                    const QSignalBlocker block(spin);const bool enabled=spin->isEnabled();spin->setEnabled(true);
                    QStyleOptionSpinBox option;option.initFrom(spin);option.buttonSymbols=spin->buttonSymbols();
                    option.stepEnabled=QAbstractSpinBox::StepUpEnabled|QAbstractSpinBox::StepDownEnabled;
                    const auto up=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxUp,spin);
                    const auto down=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxDown,spin);
                    stepButtons &= spin->buttonSymbols()==QAbstractSpinBox::UpDownArrows&&!up.isEmpty()&&!down.isEmpty();
                    if(auto *integer=qobject_cast<QSpinBox *>(spin)) {
                        const int value=integer->value();clickSpin(spin,up.center());stepButtons &= integer->value()==value+integer->singleStep();
                        clickSpin(spin,down.center());stepButtons &= integer->value()==value;
                    }
                    if(auto *decimal=qobject_cast<QDoubleSpinBox *>(spin)) {
                        const double value=decimal->value();clickSpin(spin,up.center());stepButtons &= std::abs(decimal->value()-value-decimal->singleStep())<1e-6;
                        clickSpin(spin,down.center());stepButtons &= std::abs(decimal->value()-value)<1e-6;
                    }
                    spin->setEnabled(enabled);
                }
                check(numericOnly,"all tactile numeric inputs contain numbers alone with captions on the left and external units after the editors");
                check(stepButtons,"every numeric editor retains clickable up and down arrows with its original increment");
                const auto displayY=[&](QWidget *control){return control->mapTo(controls,QPoint()).y();};
                check(displayY(preferences.m_jumpThreshold)<displayY(preferences.m_noiseThreshold)
                    &&displayY(preferences.m_noiseThreshold)<displayY(preferences.m_arrowGain)
                    &&displayY(preferences.m_arrowGain)<displayY(preferences.m_arrowWidthGain)
                    &&displayY(preferences.m_arrowWidthGain)<displayY(preferences.m_resultantSizeGain)
                    &&displayY(preferences.m_resultantSizeGain)<displayY(preferences.m_density)
                    &&displayY(preferences.m_resultantToggle)==displayY(preferences.m_componentsToggle),
                    "wider settings preserve separate numeric rows and the existing shared arrow row");
                const auto serialY=[&](QWidget *control){return control->mapTo(preferences.m_serialControls,QPoint(0,control->height()/2)).y();};
                check(serialY(preferences.m_port)==serialY(preferences.m_scanPorts)
                    &&preferences.m_port->mapTo(preferences.m_serialControls,QPoint(preferences.m_port->width(),0)).x()
                        <preferences.m_baud->mapTo(preferences.m_serialControls,QPoint(preferences.m_baud->width(),0)).x()
                    &&preferences.m_scanPorts->mapTo(preferences.m_serialControls,QPoint(preferences.m_scanPorts->width(),0)).x()
                        ==preferences.m_slave->mapTo(preferences.m_serialControls,QPoint(preferences.m_slave->width(),0)).x()
                    &&serialY(preferences.m_baud)<serialY(preferences.m_slave)
                    &&serialY(preferences.m_baud)>serialY(preferences.m_port)
                    &&serialY(preferences.m_timeout)<serialY(preferences.m_interval)
                    &&serialY(preferences.m_timeout)>serialY(preferences.m_slave)
                    &&serialY(preferences.m_connect)==serialY(preferences.m_disconnect)
                    &&serialY(preferences.m_writeAddress)==serialY(preferences.m_connect)
                    &&serialY(preferences.m_connect)>serialY(preferences.m_interval)
                    &&preferences.m_configStatus->text().isEmpty()&&preferences.m_configStatus->isHidden(),
                    "connection settings give ID and request gap their own rows, group three actions and omit static hint space");
                const int gap1=preferences.m_disconnect->x()-preferences.m_connect->geometry().right()-1;
                const int gap2=preferences.m_writeAddress->x()-preferences.m_disconnect->geometry().right()-1;
                const auto serialMargins=preferences.m_serialControls->layout()->contentsMargins();
                check(qAbs(gap1-gap2)<=1
                    &&preferences.m_connect->x()==serialMargins.left()
                    &&preferences.m_writeAddress->geometry().right()+1==preferences.m_serialControls->width()-serialMargins.right(),
                    "three fixed-size connection actions span the row with equal gaps from left to right");
                check(!preferences.findChild<QWidget *>("tactileUseMock")
                    &&!preferences.findChild<QWidget *>("scenario")&&!preferences.findChild<QWidget *>("pauseSimulation"),
                    "simulation actions and their bottom controls are removed from the widget tree");
                const int choicesWidth=preferences.m_fingerBox->width(),selectedFinger=preferences.m_finger;
                const int sidebarLeft=sidebar->mapTo(&preferences,QPoint()).x();
                const auto expandedTip=preferences.m_foldSensors->toolTip();
                const int foldTop=preferences.m_foldSensors->y();
                const bool foldAbove=preferences.m_foldSensors->geometry().bottom()<preferences.m_fingers->button(0)->y();
                preferences.m_fingerBox->grab(QRect(0,0,choicesWidth,preferences.m_fingers->button(2)->geometry().bottom()+1)).save(captureFolder+"/tactile_sensor_choices_expanded.png");
                preferences.m_foldSensors->click();wait(30);
                bool collapsed=true;for(auto *button:preferences.m_fingers->buttons())collapsed &= button->isHidden();
                check(foldAbove&&collapsed&&preferences.m_foldSensors->isVisible()&&preferences.m_foldSensors->isChecked()
                    &&preferences.m_foldSensors->y()==foldTop
                    &&preferences.m_foldSensors->size()==QSize(32,32)&&preferences.m_fingerBox->width()<choicesWidth
                    &&sidebar->mapTo(&preferences,QPoint()).x()<sidebarLeft&&preferences.m_finger==selectedFinger
                    &&preferences.m_foldSensors->toolTip()!=expandedTip&&!preferences.m_serial.isRunning()
                    &&preferences.grab().save(captureFolder+"/tactile_sensor_choices_collapsed_wide.png")
                    &&preferences.m_fingerBox->grab(QRect(0,0,preferences.m_fingerBox->width(),32)).save(captureFolder+"/tactile_sensor_choices_collapsed.png"),
                    "top fold button keeps its vertical position while hiding model choices and reclaiming sidebar width");
                preferences.m_foldSensors->click();wait(30);
                bool expanded=true;for(auto *button:preferences.m_fingers->buttons())expanded &= button->isVisible();
                check(expanded&&!preferences.m_foldSensors->isChecked()&&preferences.m_fingerBox->width()==choicesWidth
                    &&preferences.m_foldSensors->y()==foldTop
                    &&preferences.m_fingers->checkedId()==selectedFinger&&preferences.m_foldSensors->toolTip()==expandedTip,
                    "expanding sensor choices restores the column and selected model below the fixed top button");
                preferences.m_resultantToggle->click();preferences.m_componentsToggle->click();
                check(!preferences.m_adapter->resultantArrow()&&!preferences.m_adapter->componentArrows(),
                    "both compact arrow switches turn their existing display options off");
                preferences.m_resultantToggle->click();preferences.m_componentsToggle->click();
                check(preferences.m_adapter->resultantArrow()&&preferences.m_adapter->componentArrows(),
                    "both compact arrow switches restore their existing display options");
                for(auto *spin:preferences.findChildren<QAbstractSpinBox *>())
                    if(auto *editor=spin->findChild<QLineEdit *>())editor->deselect();
                check(preferences.grab().save(captureFolder+"/tactile_settings_wide.png")
                    &&preferences.m_serialControls->grab().save(captureFolder+"/tactile_connection_card.png")
                    &&controls->grab().save(captureFolder+"/tactile_display_card.png"),"capture compact settings cards with both drawers expanded");
                preferences.resize(700,1000);wait(30);
                for(int i=0;i<fixedControls.size();++i)fixed &= fixedControls[i]->size()==sizes[i];
                check(fixed,"shrinking the dock also preserves fixed editor and button sizes");
                check(preferences.grab().save(captureFolder+"/tactile_settings_narrow.png"),"capture unchanged settings rows in a narrow scrollable dock");
                preferences.m_connectionSettings->setChecked(connectionOpen);preferences.resize(1000,900);wait(20);
            }
            check(preferences.m_connectionSettings->isChecked()==preferences.m_serialControls->isVisible(),
                "opening display settings leaves connection drawer state consistent");
            check(!preferences.findChild<QComboBox *>("compactFinger")&&preferences.m_tareButton->isEnabled()
                &&preferences.findChild<QPushButton *>("finger0")&&preferences.findChild<QPushButton *>("finger1")
                &&preferences.findChild<QPushButton *>("finger2")
                &&preferences.m_resetView->parentWidget()==preferences.m_tareButton->parentWidget()
                &&preferences.m_frontView->parentWidget()==preferences.m_tareButton->parentWidget()
                &&preferences.m_resetView->geometry().left()>preferences.m_tareButton->geometry().right()
                &&preferences.m_frontView->geometry().left()>preferences.m_resetView->geometry().right()
                &&preferences.m_frontView->geometry().top()==preferences.m_tareButton->geometry().top()
                &&preferences.m_tareButton->geometry().top()==preferences.m_displaySettings->geometry().top()
                &&preferences.m_tareButton->height()==preferences.m_displaySettings->height()
                &&preferences.m_tareButton->height()==preferences.m_connectionSettings->height()
                &&preferences.m_tareButton->styleSheet().isEmpty(),
                "sensor buttons remain and enabled tare shares the top settings row, compact height and standard style");
            preferences.m_tareButton->click();
            check(!preferences.hasDisplayBaseline()&&preferences.history().samples().empty(),
                "tare without any displayed frame is a harmless no-op");
            preferences.m_jumpThreshold->setValue(6500);preferences.m_arrowGain->setValue(12.5);preferences.m_arrowWidthGain->setValue(2.5);
            preferences.m_density->setCurrentIndex(preferences.m_density->findData(4));
            preferences.m_opacitySlider->setValue(63);preferences.m_pointsToggle->setChecked(true);
            preferences.m_resultantToggle->setChecked(false);preferences.m_componentsToggle->setChecked(false);
            statusToggle->setChecked(false);wait(30);
            check(preferences.m_serialStatus->isHidden()&&preferences.m_filterStatus->isHidden()
                &&!preferences.m_adapter->communicationStatusVisible(),"status visibility control hides communication and display-filter status");
            check(preferences.grab().save(QFileInfo(path).absolutePath()+"/tactile_status_hidden.png"),"save hidden communication status capture");
            const auto expandedRatio=splitRatio(preferences.m_splitter->sizes());
            preferences.m_hideReadingsToggle->setChecked(true);wait(30);
            report<<"hidden details split="<<preferences.m_splitter->sizes()[0]<<','<<preferences.m_splitter->sizes()[1]<<'\n';
            check(preferences.m_tareButton->isVisible()&&preferences.m_tareButton->isEnabled()&&preferences.m_resetView->isVisible()
                &&preferences.m_frontView->isVisible()
                &&!preferences.m_pointTable->isVisible()
                &&preferences.m_fingerBox->isVisible()&&preferences.m_canvas->isVisible()
                &&controls->isVisible()&&preferences.findChild<QScrollArea *>("tactileDetailsScroll")->isVisible()
                &&preferences.m_hideReadingsToggle->isVisible(),
                "hiding readings preserves accessible settings and top-row tare while hiding detail controls");
            preferences.m_minimizeDisplay->click();preferences.resize(600,740);wait(30);
            auto *toolbarScroll=preferences.findChild<QScrollArea *>("tactilePlotsScroll");
            const QRect tareBounds(preferences.m_tareButton->mapTo(toolbarScroll->viewport(),QPoint()),preferences.m_tareButton->size());
            check(preferences.m_splitter->orientation()==Qt::Horizontal&&preferences.m_splitter->sizes()[1]<=90
                &&preferences.m_tareButton->isVisible()&&preferences.m_tareButton->height()==preferences.m_displaySettings->height()
                &&toolbarScroll->viewport()->rect().contains(tareBounds),
                "compact narrow display keeps sensor choices beside the scene and tare visible with the settings buttons");
            const int compactChoicesWidth=preferences.m_splitter->sizes()[1],compactSceneWidth=preferences.m_canvas->width();
            preferences.m_foldSensors->click();wait(30);
            const bool compactFolded=preferences.m_splitter->sizes()[1]<compactChoicesWidth&&preferences.m_canvas->width()>compactSceneWidth
                &&preferences.m_foldSensors->isVisible()&&preferences.m_fingers->button(0)->isHidden();
            preferences.grab().save(QFileInfo(path).absolutePath()+"/tactile_compact_choices_collapsed.png");
            preferences.m_foldSensors->click();wait(30);
            check(compactFolded&&preferences.m_splitter->sizes()[1]==compactChoicesWidth&&preferences.m_fingers->button(0)->isVisible(),
                "compact display reclaims model width when choices collapse and restores it when expanded");
            check(preferences.grab().save(QFileInfo(path).absolutePath()+"/tactile_details_hidden.png"),"save compact display capture");
            preferences.resize(1000,900);preferences.m_hideReadingsToggle->setChecked(false);wait(30);
            check(preferences.m_tareButton->isVisible()&&preferences.m_resetView->isVisible()
                &&preferences.m_frontView->isVisible()
                &&preferences.m_pointTable->isVisible()
                &&std::abs(splitRatio(preferences.m_splitter->sizes())-expandedRatio)<.02,
                "leaving compact display restores controls and previous split proportions");
            preferences.m_displaySettings->setChecked(true);
            preferences.m_displayMode->setCurrentIndex(preferences.m_displayMode->findData("dots"));
            preferences.close();
        }
    }
    {
        QSettings saved(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::organizationName(),QCoreApplication::applicationName());
        saved.sync();
        check(saved.status()==QSettings::NoError&&QFileInfo::exists(saved.fileName())
            &&saved.value("tactile/display/jumpThreshold").toInt()==6500
            &&saved.value("tactile/display/arrowGain").toDouble()==12.5
            &&saved.value("tactile/display/arrowWidthGain").toDouble()==2.5
            &&saved.value("tactile/display/visualization").toString()=="dots"
            &&saved.value("tactile/display/modelOpacity").toInt()==63
            &&saved.value("tactile/display/pointArrows").toBool()
            &&!saved.value("tactile/display/resultantArrow",true).toBool()
            &&!saved.value("tactile/display/componentArrows",true).toBool()
            &&!saved.contains("tactile/display/statusVisible")
            &&!saved.contains("tactile/display/expanded"),
            "display preferences persist without saving the panel's expanded state");
        saved.setValue("tactile/display/expanded",true);saved.sync();
        TactileVisualizationWidget restored;restored.show();wait(30);
        auto *drawer=restored.findChild<QPushButton *>("tactileDisplaySettings");
        auto *controls=restored.findChild<QWidget *>("tactileDisplayControls");
        auto *statusToggle=restored.findChild<QPushButton *>("tactileShowStatus");
        check(restored.m_jumpThreshold->value()==6500&&restored.m_arrowGain->value()==12.5
            &&restored.m_displayMode->currentData().toString()=="dots"&&restored.m_adapter->dotMatrix()
            &&restored.m_arrowWidthGain->value()==2.5
            &&restored.m_density->currentData().toInt()==4&&restored.m_opacitySlider->value()==63
            &&restored.m_pointsToggle->isChecked()&&!restored.m_resultantToggle->isChecked()&&!restored.m_componentsToggle->isChecked()
            &&drawer&&!drawer->isChecked()&&controls&&controls->isHidden()
            &&restored.m_connectionSettings->isChecked()&&restored.m_serialControls->isVisible()
            &&statusToggle&&!statusToggle->isChecked()&&restored.m_serialStatus->isHidden()
            &&restored.m_filterStatus->isHidden()&&!restored.m_adapter->communicationStatusVisible(),
            "new widget restores display values but uses startup panel visibility even with a legacy expanded preference");
        if(drawer)drawer->click();
        check(restored.m_adapter->gridSize()==4&&restored.m_adapter->arrowGain()==12.5&&std::abs(restored.m_adapter->modelOpacity()-0.63)<1e-6
            &&restored.m_adapter->arrowWidthGain()==2.5
            &&restored.m_adapter->pointArrows()&&!restored.m_adapter->resultantArrow()&&!restored.m_adapter->componentArrows(),
            "restored settings apply to the scene adapter before capture starts");
        auto *restoredLegend=restored.m_canvas->rootObject()?restored.m_canvas->rootObject()->findChild<QObject *>("tactileLegend"):nullptr;
        check(!restoredLegend,"restored display settings do not recreate the model bottom-left legend");
        if(statusToggle)statusToggle->setChecked(true);
        check(!restored.m_serialStatus->isHidden()&&restored.m_filterStatus->isHidden()
            &&restored.m_adapter->communicationStatusVisible()
            &&!restoredLegend,
            "communication status shows the serial row without an empty filter row or model legend");
        if(drawer)drawer->click();
        wait(30);
        check(controls&&controls->isHidden(),"display drawer collapses without changing its settings");
        const int statusBottom=restored.m_serialStatus->mapTo(&restored,QPoint(0,restored.m_serialStatus->height())).y();
        const int sceneTop=restored.m_splitter->mapTo(&restored,QPoint()).y();
        check(sceneTop>=statusBottom&&sceneTop<=statusBottom+16,"collapsed display drawer leaves only normal spacing above the scene");
        check(restored.grab().save(QFileInfo(path).absolutePath()+"/tactile_display_collapsed.png"),"save collapsed display settings capture");
        // Keep later independent fixtures on their documented defaults.
        restored.m_jumpThreshold->setValue(5000);restored.m_arrowGain->setValue(1);restored.m_arrowWidthGain->setValue(1);
        restored.m_density->setCurrentIndex(restored.m_density->findData(3));
        restored.m_noiseEnabled->setChecked(false);restored.m_noiseThreshold->setValue(150);
        restored.m_reverseZArrows->setChecked(false);
        restored.m_displayMode->setCurrentIndex(restored.m_displayMode->findData("xyz"));
        restored.m_density->setCurrentIndex(restored.m_density->findData(2));restored.m_opacitySlider->setValue(45);
        restored.m_pointsToggle->setChecked(false);restored.m_resultantToggle->setChecked(true);restored.m_componentsToggle->setChecked(true);
        restored.close();
    }
    {
        TactileVisualizationWidget options;options.resize(1000,640);options.show();
        options.m_availableSerialPorts=[]{return QStringList{"WHEEL_PORT_A","WHEEL_PORT_B"};};
        options.m_connectionSettings->setChecked(true);options.m_displaySettings->setChecked(true);
        options.refreshSerialPorts();options.m_portScanTimer.stop();options.m_noiseEnabled->setChecked(true);
        options.m_displayMode->setCurrentIndex(options.m_displayMode->findData("xyz"));wait(30);
        QList<QWidget *> controls;
        for(auto *combo:options.findChildren<QComboBox *>())controls.append(combo);
        for(auto *spin:options.findChildren<QAbstractSpinBox *>())controls.append(spin);
        for(auto *slider:options.findChildren<QSlider *>())controls.append(slider);
        const auto optionValue=[](QWidget *control)->QVariant{
            if(auto *combo=qobject_cast<QComboBox *>(control))return QStringList{combo->currentText(),QString::number(combo->currentIndex())};
            return control->property("value");
        };
        const auto wheel=[](QWidget *target,const QPoint &pixels,const QPoint &angle,Qt::KeyboardModifiers modifiers){
            const QPointF at=target->rect().center();
            QWheelEvent event(at,target->mapToGlobal(at.toPoint()),pixels,angle,Qt::NoButton,modifiers,Qt::NoScrollPhase,false);
            QApplication::sendEvent(target,&event);
        };
        bool unchanged=true;
        for(auto *control:controls) {
            const auto before=optionValue(control);
            for(bool focused:{false,true}) {
                if(focused)control->setFocus();else options.m_resetView->setFocus();
                wheel(control,{},QPoint(0,120),Qt::NoModifier);
                wheel(control,{},QPoint(0,-120),Qt::ControlModifier);
                wheel(control,QPoint(0,15),{},Qt::NoModifier);
                wheel(control,{},QPoint(120,0),Qt::ShiftModifier);
                unchanged &= optionValue(control)==before;
                if(auto *edit=control->findChild<QLineEdit *>())wheel(edit,{},QPoint(0,120),Qt::NoModifier);
                unchanged &= optionValue(control)==before;
            }
            if(optionValue(control)!=before)report<<"Wheel changed "<<control->objectName()<<'\n';
        }
        check(controls.size()>=13&&unchanged,"all tactile option values ignore focused and unfocused mouse and touchpad wheels, including editable children");
        auto *combo=options.m_density;combo->showPopup();wait(20);
        const int index=combo->currentIndex();const auto highlighted=combo->view()->currentIndex();
        wheel(combo->view()->viewport(),{},QPoint(0,-120),Qt::NoModifier);
        check(combo->currentIndex()==index&&combo->view()->currentIndex()==highlighted,
            "open option popup wheels do not change the selected or highlighted option");
        combo->hidePopup();
        auto *scroll=options.findChild<QScrollArea *>("tactileDetailsScroll");
        scroll->verticalScrollBar()->setValue(0);wait(10);
        wheel(options.m_jumpThreshold,{},QPoint(0,-120),Qt::NoModifier);
        report<<"option wheel page maximum="<<scroll->verticalScrollBar()->maximum()<<" value="<<scroll->verticalScrollBar()->value()<<'\n';
        check(scroll->verticalScrollBar()->maximum()>0&&scroll->verticalScrollBar()->value()>0,
            "wheel over an option scrolls the surrounding page without editing the value");
        const auto key=[](QWidget *target,int code){QKeyEvent event(QEvent::KeyPress,code,Qt::NoModifier);QApplication::sendEvent(target,&event);};
        options.m_jumpThreshold->setValue(5000);key(options.m_jumpThreshold,Qt::Key_Up);
        options.m_opacitySlider->setValue(50);key(options.m_opacitySlider,Qt::Key_Right);
        combo->setCurrentIndex(0);key(combo,Qt::Key_Down);
        check(options.m_jumpThreshold->value()==5500&&options.m_opacitySlider->value()==51&&combo->currentIndex()==1,
            "keyboard editing still changes spin boxes sliders and dropdown options");
        check(!options.m_serial.isRunning()&&!options.m_serial.configBusy(),"option-wheel checks never open or configure hardware");
        options.m_noiseEnabled->setChecked(false);options.m_jumpThreshold->setValue(5000);
        options.m_opacitySlider->setValue(45);combo->setCurrentIndex(0);options.m_displaySettings->setChecked(false);options.close();
    }
    {
        QStringList available;int scans=0;
        TactileVisualizationWidget hotplug;
        hotplug.setAttribute(Qt::WA_DontShowOnScreen);hotplug.setAttribute(Qt::WA_ShowWithoutActivating);
        hotplug.m_availableSerialPorts=[&]{++scans;return available;};
        hotplug.m_port->clear();hotplug.refreshSerialPorts();hotplug.show();wait(30);
        check(hotplug.m_port->count()==0&&!hotplug.m_serial.isRunning()&&!hotplug.m_port->isEditable()&&!hotplug.m_port->lineEdit(),
            "serial port uses a selection-only dropdown and no devices at startup does not open hardware");
        hotplug.m_connectionSettings->setChecked(false);
        available={"COM3"};hotplug.m_connectionSettings->setChecked(true);
        check(hotplug.m_port->findText("COM3")>=0&&hotplug.m_port->currentText()=="COM3",
            "opening connection settings discovers a device inserted after startup");
        hotplug.m_baud->setCurrentText("115200");
        int selectionChanges=0;
        QObject::connect(hotplug.m_port,&QComboBox::currentTextChanged,&hotplug,[&]{++selectionChanges;});
        available={"COM9","COM3"};hotplug.m_scanPorts->setFocus();
        QElapsedTimer discovery;discovery.start();
        while(hotplug.m_port->findText("COM9")<0&&discovery.elapsed()<1800)wait(20);
        check(hotplug.m_port->findText("COM9")>=0&&hotplug.m_port->currentText()=="COM3"
            &&hotplug.m_baud->currentText()=="115200"&&selectionChanges==0&&!hotplug.m_serial.isRunning(),
            "periodic discovery adds hotplugged ports without changing the selected port, baud or opening hardware");
        available={"COM3","COM9","COM12"};hotplug.m_port->showPopup();
        check(hotplug.m_port->findText("COM12")>=0,"opening the serial dropdown refreshes immediately before the next timer tick");
        hotplug.m_port->hidePopup();
        hotplug.m_port->setCurrentText("COM9");hotplug.m_port->setEditText("MANUAL_TACTILE_PORT");
        check(hotplug.m_port->currentText()=="COM9","serial dropdown rejects arbitrary text and retains its selected available port");
        available={"COM3"};hotplug.m_scanPorts->click();
        check(hotplug.m_port->currentIndex()==-1&&hotplug.m_port->findText("COM9")<0
            &&hotplug.m_port->findText("COM12")<0,"manual refresh removes unplugged entries without silently switching the selected target");
        hotplug.m_port->setCurrentText("COM3");QApplication::setActiveWindow(&hotplug);hotplug.m_port->setFocus();
        const int beforeFocused=scans;available={"COM3","COM15"};
        QMetaObject::invokeMethod(&hotplug.m_portScanTimer,"timeout",Qt::DirectConnection);
        check(hotplug.m_port->hasFocus()&&scans==beforeFocused+1&&hotplug.m_port->currentText()=="COM3"
            &&hotplug.m_port->findText("COM15")>=0,
            "background port discovery preserves the focused dropdown selection while adding available ports");
        available={"COM3","COM15","COM18"};hotplug.m_scanPorts->setFocus();hotplug.m_port->setEnabled(false);
        const int beforeLocked=scans;hotplug.refreshSerialPorts();
        check(scans==beforeLocked&&hotplug.m_port->findText("COM18")<0,
            "locked connection controls do not enumerate or alter the active port selection");
        hotplug.m_port->setEnabled(true);hotplug.m_connectionSettings->setChecked(false);
        check(!hotplug.m_portScanTimer.isActive(),"collapsing connection settings stops periodic port discovery");
        hotplug.m_connectionSettings->setChecked(true);
        check(hotplug.m_port->findText("COM18")>=0,"reopening connection settings refreshes devices immediately");
        hotplug.hide();available={"COM22"};hotplug.show();wait(10);
        check(hotplug.m_port->findText("COM22")>=0&&hotplug.m_port->findText("COM3")<0,
            "showing the tactile module refreshes ports after devices change while hidden");
        hotplug.close();
        check(!hotplug.m_portScanTimer.isActive()&&!hotplug.running()&&!hotplug.m_serial.isRunning(),
            "closing the tactile module stops discovery without starting any source");
    }
    TactileVisualizationWidget widget;
    widget.m_density->setCurrentIndex(widget.m_density->findData(2));
    widget.show(); wait(30); check(!widget.running()&&widget.refreshCount()==0,"construction and show do not start acquisition");
    check(widget.m_slave->prefix().isEmpty()&&widget.m_slave->suffix().isEmpty()
        &&widget.m_slaveLabel->buddy()==widget.m_slave&&!widget.m_slaveLabel->text().isEmpty()
        &&!widget.m_writeAddress->isEnabled(),"ID is numeric only with a separate label and disconnected save disabled");
    check(!widget.findChild<QWidget *>("tactileNewSlave")&&!widget.findChild<QWidget *>("tactileNewBaud")
        &&!widget.findChild<QWidget *>("tactileChangeAddress")&&!widget.findChild<QWidget *>("tactileChangeBaud")
        &&!widget.findChild<QWidget *>("tactileReadConfig")&&!widget.findChild<QWidget *>("tactileReconnectAddress"),
        "duplicate setting controls, read button and power-cycle reconnect button are removed");
    check(widget.m_baud->count()==3&&!widget.m_baud->isEditable(),"unified baud input offers only supported values");
    {
        const QSignalBlocker blocker(widget.m_slave);widget.m_slave->setValue(0);
        check(widget.m_slave->minimum()==0&&widget.m_slave->maximum()==127&&widget.m_slave->value()==0
            &&widget.m_slave->text()=="0","initial public address 0 remains accepted and displayed numerically");
        widget.m_slave->setValue(1);
    }
    widget.m_connectedSettings.slave=1;widget.m_connectedSettings.baud=921600;
    widget.m_baud->setCurrentText("115200");widget.m_baud->activated(widget.m_baud->currentIndex());wait(10);
    check(!widget.findChild<QMessageBox *>("tactileSaveReminder")&&widget.m_saveReminderGeneration==0,
        "selecting connection parameters offline neither opens nor consumes the save reminder");
    {
        const auto priorSettings=widget.m_connectedSettings;
        QSemaphore workerStarted,releaseWorker;
        const auto startupGate=QObject::connect(&widget.m_serial,&tactile::TactileSerialSource::started,[&]{workerStarted.release();releaseWorker.acquire();});
        // The default empty port prevents serial I/O after this startup gate is released.
        tactile::SerialSettings emptySettings;emptySettings.port.clear();
        widget.m_serial.begin(emptySettings);const bool active=workerStarted.tryAcquire(1,1000);
        check(active,"connected-button fixture holds the serial thread without opening a device");
        widget.m_serialSample.portOpen=true;widget.m_connectedSettings.port="TACTILE_TEST_PORT";
        struct EditCase {int connected,selected,baud;bool enabled;const char *name;};
        const EditCase edits[]={
            {0,1,921600,true,"public address 0 to 1 enables saving"},
            {0,127,115200,true,"public address 0 to 127 plus baud change enables saving"},
            {0,0,115200,true,"public address 0 baud-only change enables saving"},
            {0,0,921600,false,"unchanged public connection cannot save"},
            {1,0,115200,false,"writing nonzero ID back to 0 remains forbidden even with baud change"},
            {1,0,921600,false,"address-only change back to 0 remains forbidden"},
            {1,2,921600,true,"nonzero address change enables saving"},
            {1,1,115200,true,"nonzero baud-only change enables saving"},
            {1,1,921600,false,"restoring both original values disables saving"}
        };
        if(active){
            widget.m_baud->activated(widget.m_baud->currentIndex());wait(10);
            auto *editNotice=widget.findChild<QMessageBox *>("tactileSaveReminder");
            check(editNotice&&editNotice->isVisible(),"first connected baud edit prompts saving and power cycling");
            if(editNotice){
                check(editNotice->grab().save(QFileInfo(path).absolutePath()+"/tactile_edit_reminder.png"),"capture edit reminder");
                editNotice->accept();wait(10);
            }
            widget.m_slave->setValue(2);
            widget.m_baud->setCurrentText("38400");widget.m_baud->activated(widget.m_baud->currentIndex());wait(10);
            check(widget.m_saveReminderGeneration==widget.m_serial.generation()
                &&!widget.findChild<QMessageBox *>("tactileSaveReminder")&&widget.m_writeAddress->isEnabled(),
                "reminder is limited to once per session across both fields while saving remains available");
            for(const auto &edit:edits){
                const QSignalBlocker blockId(widget.m_slave),blockBaud(widget.m_baud);
                widget.m_connectedSettings.slave=edit.connected;widget.m_connectedSettings.baud=921600;
                widget.m_slave->setValue(edit.selected);widget.m_baud->setCurrentText(QString::number(edit.baud));
                widget.updateSerialStatus();check(widget.m_writeAddress->isEnabled()==edit.enabled,edit.name);
            }
            widget.m_connectedSettings.slave=0;
            {const QSignalBlocker blocker(widget.m_slave);widget.m_slave->setValue(0);}
            widget.m_baud->setCurrentText("115200");widget.updateSerialStatus();
            widget.m_serialSample.portOpen=false;widget.updateSerialStatus();
            check(!widget.m_writeAddress->isEnabled(),"public configuration still requires an open port");
            widget.m_serialSample.portOpen=true;widget.m_configAwaitClose=true;widget.updateSerialStatus();
            check(!widget.m_writeAddress->isEnabled()&&!widget.m_slave->isEnabled()&&!widget.m_baud->isEnabled(),
                "public settings remain locked until the previous connection closes");
            widget.writeDeviceAddress();check(!widget.m_serial.configBusy(),"direct write entry also rejects pending connection close");
            widget.m_configAwaitClose=false;
            bool confirmationSeen=false;QTimer decline;
            QObject::connect(&decline,&QTimer::timeout,[&]{
                for(auto *dialog:widget.findChildren<QMessageBox *>())if(dialog->isVisible()&&dialog->standardButtons().testFlag(QMessageBox::Yes)){
                    confirmationSeen=true;
                    check(dialog->text().contains(QCoreApplication::translate("TactileVisualizationWidget",
                        "Keep the device ID read from the sensor; only the baud rate will be changed.")),
                        "public baud-only confirmation promises to preserve the actual device ID");
                    dialog->done(QMessageBox::No);decline.stop();break;
                }
            });
            decline.start(10);widget.writeDeviceAddress();decline.stop();
            check(confirmationSeen&&!widget.m_serial.configBusy(),"public save requires explicit single-device confirmation and No leaves acquisition intact");
            widget.m_connectedSettings.slave=1;
            widget.writeDeviceAddress();
            check(!widget.m_serial.configBusy()&&widget.m_configStatus->text().contains("1-127")&&!widget.m_configStatus->isHidden(),
                "direct write entry refuses a target ID 0 before configuration is queued");
            check(widget.m_serial.requestDeviceConfig(2,tactile::SaveCandidate::Decimal18013),"configuration fixture enters busy state");
            widget.updateSerialStatus();
            check(!widget.m_writeAddress->isEnabled()&&!widget.m_slave->isEnabled()&&!widget.m_baud->isEnabled(),
                "busy configuration disables saving and both editable device fields");
        }
        widget.m_statusToggle->setChecked(true);
        widget.disconnectSerial();
        check(!active||(widget.m_serial.stopping()&&!widget.m_connect->isEnabled()&&!widget.m_disconnect->isEnabled()
            &&!widget.m_slave->isEnabled()&&!widget.m_baud->isEnabled()&&widget.m_serialStatus->isVisible()),
            "pending driver close is visible and blocks reconnect and configuration without blocking the UI");
        if(active) {
            widget.m_statusToggle->setChecked(false);
            check(widget.m_serialStatus->isHidden()&&!widget.m_connect->isEnabled()&&!widget.m_disconnect->isEnabled()
                &&widget.m_disconnect->text()==QCoreApplication::translate("TactileVisualizationWidget","Disconnecting..."),
                "hidden communication status keeps pending-close feedback in the disconnect button and still blocks reconnect");
            widget.m_statusToggle->setChecked(true);
        }
        releaseWorker.release();check(widget.m_serial.wait(2000),"cancelled UI fixture worker exits within deadline");
        QObject::disconnect(startupGate);wait(10);
        if(active)check(widget.m_configStatus->text()==QCoreApplication::translate("TactileVisualizationWidget",
            "Configuration interrupted; device state may have changed. Reconnect and read parameters before another write."),
            "disconnecting during queued configuration warns that device state may have changed");
        widget.m_connectedSettings=priorSettings;widget.m_serialSample={};widget.m_configAwaitClose=false;
        {const QSignalBlocker blocker(widget.m_slave);widget.m_slave->setValue(2);}
        widget.m_baud->setCurrentText("115200");widget.updateSerialStatus();
        check(!widget.m_writeAddress->isEnabled(),"disconnected public configuration cannot save");
    }
    check(widget.m_connectedSettings.slave==1&&widget.m_connectedSettings.baud==921600&&!widget.m_serial.isRunning(),
        "editing both values leaves actual connection settings unchanged and performs no device write");
    widget.m_connectionSettings->setChecked(true);widget.resize(1100,1000);wait(30);
    check(widget.grab().save(QFileInfo(path).absolutePath()+"/tactile_address_controls.png"),"save device address controls capture");
    tactile::DeviceConfigResult configured;configured.requestedAddress=2;configured.originalBaud=921600;configured.writeAttempted=true;
    configured.writeAcknowledged=true;configured.addressVerified=true;configured.saveAcknowledged=true;
    widget.receiveDeviceConfig(configured);
    check(!widget.findChild<QMessageBox *>("tactilePowerCycleNotice")&&widget.m_configStatus->isVisible()
        &&widget.m_configStatus->text().contains("921600"),
        "successful address save shows the power-cycle reminder inline without a popup");
    auto uncertain=configured;uncertain.saveAcknowledged=false;uncertain.error="fixture: save timeout";
    widget.receiveDeviceConfig(uncertain);
    auto *warning=widget.findChild<QMessageBox *>("tactilePowerCycleNotice");
    check(warning&&warning->isVisible()&&warning->icon()==QMessageBox::Warning,"uncertain save shows warning instead of success");
    if(warning)warning->accept();wait(10);
    check(widget.m_pendingAddress==2&&widget.m_slave->value()==2&&!widget.m_verifyingAddress
        &&widget.m_connect->isEnabled()&&!widget.m_serial.isRunning(),
        "save acknowledgement only arms manual power-cycle verification, never auto connects");
    widget.m_verifyingAddress=true;widget.m_serialSample.accepted=true;widget.m_serialSample.crcOk=false;widget.m_serialSample.settings.slave=2;
    widget.m_serialSample.settings.port=widget.m_pendingPort;
    widget.updateSerialStatus();check(widget.m_verifyingAddress&&widget.m_pendingAddress==2,"bad CRC cannot verify new address");
    widget.m_serialSample.crcOk=true;widget.m_serialSample.settings.slave=1;
    widget.updateSerialStatus();check(widget.m_verifyingAddress,"old address response cannot verify new address");
    widget.m_serialSample.settings.slave=2;widget.updateSerialStatus();
    check(!widget.m_verifyingAddress&&widget.m_pendingAddress==0,"only CRC-valid new-address data completes explicit verification");
    configured.requestedBaud=115200;configured.baudVerified=true;
    widget.receiveDeviceConfig(configured);
    check(!widget.findChild<QMessageBox *>("tactilePowerCycleNotice")&&widget.m_configStatus->isVisible()
        &&widget.m_configStatus->text().contains("115200")
        &&widget.m_pendingAddress==2&&widget.m_pendingBaud==115200&&!widget.m_serial.isRunning(),
        "combined save reports final ID and baud inline and waits for manual reconnect");
    widget.m_verifyingAddress=true;widget.m_serialSample.accepted=true;widget.m_serialSample.crcOk=true;
    widget.m_serialSample.settings.slave=2;widget.m_serialSample.settings.baud=921600;
    widget.updateSerialStatus();check(widget.m_verifyingAddress,"old baud cannot verify combined settings");
    widget.m_serialSample.settings.baud=115200;widget.m_serialSample.settings.port="OTHER_PORT";
    widget.updateSerialStatus();check(widget.m_verifyingAddress,"another serial port cannot verify the saved sensor");
    widget.m_serialSample.settings.port=widget.m_pendingPort;widget.updateSerialStatus();
    check(!widget.m_verifyingAddress&&widget.m_pendingBaud==0,"combined verification requires new ID, new baud, original port and valid CRC");
    configured.baudVerified=false;configured.saveAcknowledged=false;configured.error="fixture: second parameter failed";
    widget.receiveDeviceConfig(configured);warning=widget.findChild<QMessageBox *>("tactilePowerCycleNotice");
    check(warning&&warning->icon()==QMessageBox::Warning&&!widget.m_verifyingAddress,
        "partial combined write warns without reporting success or automatically reconnecting");
    if(warning)warning->accept();wait(10);
    {
        tactile::DeviceConfigResult baudOnly;baudOnly.requestedBaud=115200;baudOnly.originalBaud=921600;
        baudOnly.configuredBaud=115200;baudOnly.writeAttempted=baudOnly.writeAcknowledged=baudOnly.baudVerified=baudOnly.saveAcknowledged=true;
        widget.m_connectedSettings.slave=0;widget.m_connectedSettings.port="TACTILE_TEST_PORT";
        for(int storedId:{0,7}){
            baudOnly.configuredAddress=storedId;widget.receiveDeviceConfig(baudOnly);
            check(!widget.findChild<QMessageBox *>("tactilePowerCycleNotice")&&widget.m_pendingAddress==storedId
                &&widget.m_slave->value()==storedId&&widget.m_pendingBaud==115200
                &&widget.m_pendingPort=="TACTILE_TEST_PORT"&&!widget.m_verifyingAddress&&!widget.m_serial.isRunning(),
                "public baud-only save displays the actual stored ID, including 0, and waits for manual power cycling");
            check(!widget.findChild<QMessageBox *>("tactileSaveReminder"),"readback updates reconnect ID without opening another edit reminder");
            widget.m_verifyingAddress=true;widget.m_serialSample.accepted=true;widget.m_serialSample.crcOk=false;
            widget.m_serialSample.settings.slave=storedId;widget.m_serialSample.settings.baud=115200;
            widget.m_serialSample.settings.port="TACTILE_TEST_PORT";widget.updateSerialStatus();
            check(widget.m_verifyingAddress,"public baud-only reconnect cannot be verified with invalid CRC");
            widget.m_serialSample.crcOk=true;widget.m_serialSample.settings.baud=921600;widget.updateSerialStatus();
            check(widget.m_verifyingAddress,"public baud-only reconnect cannot be verified with the original baud");
            widget.m_serialSample.settings.baud=115200;widget.m_serialSample.settings.port="OTHER_PORT";widget.updateSerialStatus();
            check(widget.m_verifyingAddress,"public baud-only reconnect cannot be verified on another port");
            widget.m_serialSample.settings.port="TACTILE_TEST_PORT";widget.updateSerialStatus();
            check(!widget.m_verifyingAddress&&widget.m_pendingBaud==0,
                "public baud-only reconnect verifies actual stored ID 0 or nonzero only with new baud, original port and valid CRC");
        }
    }
    widget.m_pendingAddress=0;widget.m_pendingBaud=0;widget.m_pendingPort.clear();
    widget.m_serialSample={};widget.m_connectionSettings->setChecked(false);widget.updateSerialStatus();wait(30);
    auto *status=widget.findChild<QLabel*>("tactileSerialStatus");
    const int statusHeight=status->height();
    auto *scene=widget.findChild<QQuickWidget*>("tactileQuickView");
    const auto sceneOrigin=scene->mapTo(&widget,QPoint());
    status->setText(QString(500,QChar('W'))+" Values outside manual range; raw values retained");wait(30);
    check(status->height()==statusHeight&&!status->wordWrap()&&scene->mapTo(&widget,QPoint())==sceneOrigin,"long transient serial warning cannot change layout height");
    status->setText("OK");wait(30);
    check(status->height()==statusHeight&&scene->mapTo(&widget,QPoint())==sceneOrigin,"clearing serial warning cannot shift scene");
    widget.start();wait(150);
    const auto idleRefreshes=widget.refreshCount();
    check(widget.running()&&!widget.frame().valid&&widget.history().samples().empty(),
        "starting view refresh creates no measurements without a device");
    auto *unavailable=scene->rootObject()->findChild<QObject *>("tactileNoDataMessage");
    for(int type=0;type<3;++type) {
        widget.m_fingers->button(type)->click();wait(40);
        check(!widget.frame().valid&&!widget.m_adapter->valid()&&widget.history().samples().empty()
            &&widget.m_pointTable->rowCount()==(type==2?16:20)&&widget.m_pointTable->item(0,1)->text()=="--"
            &&unavailable&&unavailable->property("visible").toBool()
            &&unavailable->property("text").toString()==QCoreApplication::translate("TactileView","Device disconnected / data unavailable"),
            "every disconnected model keeps geometry with the unavailable message and no manufactured readings");
    }
    check(widget.refreshCount()==idleRefreshes,"idle view does not produce frames or curve samples");
    check(widget.grab().save(QFileInfo(path).absolutePath()+"/tactile_disconnected.png"),"capture hardware-only disconnected view");
    widget.close();const auto closed=widget.refreshCount();wait(70);
    check(!widget.running()&&!widget.frame().valid&&widget.refreshCount()==closed,"close stops view refresh without leaving values");
    widget.show();wait(40);check(!widget.running()&&!widget.frame().valid,"show does not create data or start acquisition");
    widget.start();wait(60);check(widget.running()&&!widget.frame().valid&&!widget.m_serial.isRunning(),"reopen starts only view refresh until a device is connected");widget.hide();
    QPointer<TactileVisualizationWidget> owned=new TactileVisualizationWidget; owned->show(); owned->start(); delete owned; wait(70);
    check(owned.isNull(),"destroy active widget safely");
    TactileVisualizationWidget serialUi;serialUi.show();serialUi.start();wait(100);
    check(serialUi.findChild<QCheckBox*>("tactileVerifyCrc")->isChecked()
        &&serialUi.findChild<QSpinBox*>("tactileTimeout")->value()==250&&serialUi.findChild<QSpinBox*>("tactileInterval")->value()==10,
        "serial defaults CRC enabled, 250 ms timeout, 10 ms post-request gap");
    serialUi.m_availableSerialPorts=[] {return QStringList{"NONEXISTENT_TACTILE_TEST_PORT"};};serialUi.refreshSerialPorts();
    serialUi.m_port->setCurrentIndex(0);
    serialUi.findChild<QPushButton*>("tactileConnect")->click();wait(200);
    check(!serialUi.frame().valid&&serialUi.findChild<QTableWidget*>("tactilePointValues")->item(0,1)->text()=="--","failed F connection does not fall back to simulated values");
    serialUi.findChild<QPushButton*>("finger1")->click();wait(80);check(!serialUi.frame().valid&&!serialUi.m_adapter->valid(),"G stays unavailable after F connection failure");
    serialUi.findChild<QCheckBox*>("tactileVerifyCrc")->setChecked(false);serialUi.findChild<QPushButton*>("finger0")->click();
    check(!serialUi.frame().valid,"CRC toggle cannot revive rejected hardware data");serialUi.close();
    {
    using namespace tactile;
    SerialSettings settings;settings.port="TEST";settings.verifyCrc=false;
    SerialSample sample;sample.accepted=true;sample.verifyCrc=false;sample.crcOk=true;sample.registers.fill(0,64);
    sample.registers[0]=10;sample.registers[2]=100;
    const auto session=QUuid::createUuid();
    auto good=serialFrame(sample,settings,session,1,1,1);
    sample.registers[0]=5011;auto spike=serialFrame(sample,settings,session,1,3,3);
    sample.registers[0]=5010;auto boundary=serialFrame(sample,settings,session,1,4,4);
    sample.registers[0]=10;sample.registers[2]=50;auto release=serialFrame(sample,settings,session,1,5,5);
    TactileSceneAdapter deltaAdapter;deltaAdapter.publish(boundary,0,false,0,&good);
    check(deltaAdapter.valid()&&deltaAdapter.points()[0].toMap()["components"].toList()[0].toMap()["value"].toDouble()==5000,"adapter renders delta while retaining raw frame domain");
    deltaAdapter.publish(good,0,false,0,&good);
    check(deltaAdapter.valid()&&!deltaAdapter.points()[0].toMap()["arrowVisible"].toBool(),"all-zero baseline removes force arrow");
    deltaAdapter.publish(release,0,false,0,&good);
    check(deltaAdapter.valid()&&deltaAdapter.points()[0].toMap()["components"].toList()[2].toMap()["value"].toDouble()==-50,"negative Z delta retained without allowing negative raw Z");
    TactileVisualizationWidget ui;ui.m_hardwareType=0;ui.m_crc->setChecked(false);
    ui.m_serialHistory.begin(session,1);
    const auto feed=[&](const Frame &frame){SerialSample data;data.accepted=true;data.portOpen=true;data.verifyCrc=false;data.frame=frame;data.requests=frame.sequence;
        QElapsedTimer now;now.start();data.receivedMs=now.msecsSinceReference();
        ui.receiveSerialSample(data);ui.updateValues();};
    feed(good);ui.setDisplayZero();
    check(sameForce(ui.chartForce(),{})&&ui.m_serialHistory.samples().size()==1,"hardware zero starts relative curve from current trusted frame");
    feed(spike);
    check(ui.frame().fingers[0].modules[0].points[0].force.x==5011&&sameForce(ui.chartForce(),{})
        &&ui.m_serialHistory.samples().size()==1&&ui.m_tareButton->isEnabled()
        &&ui.m_adapter->points()[0].toMap()["fx"].toDouble()==0,"spike stays in raw table source; curve and arrows held; tare remains available");
    feed(release);
    check(ui.m_serialHistory.samples().size()==2&&ui.m_serialHistory.samples().back().startsSegment&&ui.m_serialHistory.samples().back().bridgeFromPrevious
        &&ui.chartForce().z==-50,"recovered sample retains a segmented genuine curve with marked short-gap interpolation");
    const double initialArrow=ui.m_adapter->points()[0].toMap()["fz"].toDouble();
    const auto recoveryHistory=ui.history().samples().size();const auto recoveryRefresh=ui.refreshCount();
    QThread::msleep(35);ui.updateValues();
    const double middleArrow=ui.m_adapter->points()[0].toMap()["fz"].toDouble();
    check(initialArrow>-50&&middleArrow<=initialArrow&&middleArrow>=-50&&validateFrame(ui.arrowFrame())
        &&ui.chartForce().z==-50&&ui.frame().fingers[0].modules[0].points[0].force.z==50
        &&ui.history().samples().size()==recoveryHistory&&ui.refreshCount()==recoveryRefresh,"arrow transition is bounded and preserves raw values, canonical chart values, history and sample metrics");
    wait(110);ui.updateValues();
    check(ui.m_adapter->points()[0].toMap()["fz"].toDouble()==-50,"arrow transition converges exactly to signed delta within 100 ms");
    const auto curveBefore=ui.chartForce();const auto rawBefore=ui.frame();const auto historySize=ui.history().samples().size();
    const auto originalLength=ui.m_adapter->points()[0].toMap()["components"].toList()[2].toMap()["length"].toDouble();
    ui.m_arrowGain->setValue(10);
    const auto enlarged=ui.m_adapter->points()[0].toMap()["components"].toList()[2].toMap();
    check(std::abs(enlarged["length"].toDouble()-10*originalLength)<1e-6&&enlarged["value"].toDouble()==-50
        &&sameForce(ui.chartForce(),curveBefore)&&sameForce(ui.frame().fingers[0].resultant,rawBefore.fingers[0].resultant)
        &&ui.history().samples().size()==historySize,"arrow gain changes only geometry, not delta, raw values or history");
    ui.m_arrowGain->setValue(1);
    check(std::abs(ui.m_adapter->points()[0].toMap()["components"].toList()[2].toMap()["length"].toDouble()-originalLength)<1e-6,"1x restores original arrow length");
    const auto pointsBeforeWidth=ui.m_adapter->points();const auto resultantBeforeWidth=ui.m_adapter->resultant();
    ui.m_arrowWidthGain->setValue(2.5);
    check(ui.m_adapter->arrowWidthGain()==2.5&&ui.m_adapter->arrowGain()==1
        &&ui.m_adapter->points()==pointsBeforeWidth&&ui.m_adapter->resultant()==resultantBeforeWidth
        &&sameForce(ui.chartForce(),curveBefore)&&ui.frame().sequence==rawBefore.sequence
        &&ui.history().samples().size()==historySize,"thickness changes preserve arrow lengths, signed delta, raw frame and history");
    ui.m_arrowWidthGain->setValue(1);
    // Give the hold test its own fresh sample after potentially slow Debug UI work.
    release.sequence=6;release.monotonicNs=6;feed(release);
    const auto holdHistory=ui.history().samples().size();
    auto corrupt=release;corrupt.sequence=7;corrupt.monotonicNs=7;corrupt.crcOk=false;
    feed(corrupt);
    check(ui.m_adapter->valid()&&ui.m_adapter->sourceLabel().contains("DISPLAY HELD")&&ui.chartForce().z==-50,"CRC-off corruption holds the recent trusted delta display");
    ui.m_trustedReceivedMs-=1001;corrupt.sequence=8;corrupt.monotonicNs=8;feed(corrupt);
    check(ui.frame().valid&&!ui.visualFrame().valid&&!ui.m_adapter->valid()
        &&ui.m_displayFilter.last.sequence==release.sequence&&ui.history().samples().size()==holdHistory
        &&ui.hasDisplayBaseline()&&ui.curveForce(release.fingers[0].modules[0].points[0].force,0).z==-50,"new corrupt raw samples cannot extend trusted display age or change historical delta baseline");
    release.sequence=9;release.monotonicNs=9;feed(release);
    check(ui.m_adapter->valid()&&!ui.m_serialHistory.samples().back().bridgeFromPrevious,"recovery after trusted expiry starts without bridging the unavailable period");
    ui.setSuspended(true);
    check(!ui.m_adapter->valid()&&!ui.visualFrame().valid,"pause immediately removes held hardware arrows");
    ui.setSuspended(false);release.sequence=10;release.monotonicNs=10;feed(release);
    check(ui.m_adapter->valid()&&!ui.m_serialHistory.samples().back().bridgeFromPrevious,"resume requires fresh data and retains a hard curve boundary");
    ui.disconnectSerial();check(!ui.m_adapter->valid()&&!ui.hasDisplayBaseline()&&!ui.m_displayFilter.last.valid,"disconnect clears held arrows and baseline");
    TactileVisualizationWidget checked;checked.m_hardwareType=0;checked.m_serialHistory.begin(session,1);
    auto checkedGood=good;checkedGood.crcVerified=true;
    const auto feedChecked=[&](const Frame &frame,bool accepted){SerialSample data;data.accepted=accepted;data.portOpen=true;data.verifyCrc=true;data.crcOk=accepted;
        data.frame=frame;data.requests=frame.sequence;data.error=accepted?QString():QStringLiteral("CRC mismatch");
        QElapsedTimer now;now.start();data.receivedMs=now.msecsSinceReference();checked.receiveSerialSample(data);checked.updateValues();};
    feedChecked(checkedGood,true);
    {
        TactileVisualizationWidget readings;
        readings.m_hardwareType=0;readings.m_hideReadingsToggle->setChecked(false);
        readings.m_serialHistory.begin(checkedGood.sessionId,checkedGood.generation);
        const auto feedReading=[&](const Frame &frame){SerialSample data;data.accepted=true;data.portOpen=true;data.verifyCrc=true;data.crcOk=true;
            data.frame=frame;data.requests=frame.sequence;QElapsedTimer clock;clock.start();data.receivedMs=clock.msecsSinceReference();
            readings.receiveSerialSample(data);readings.updateValues();};
        feedReading(checkedGood);
        check(readings.m_liveFrame.valid,"hidden readout fixture starts with a valid hardware frame");
        readings.m_noiseEnabled->setChecked(false);readings.setDisplayZero();
        const auto visibleReading=readings.m_pointTable->item(0,1)->text();
        readings.m_hideReadingsToggle->setChecked(true);
        auto nextReading=checkedGood;nextReading.sequence=2;nextReading.monotonicNs=2;
        nextReading.fingers[0].modules[0].points[0].force.x+=20;
        nextReading.fingers[0].modules[0].resultant.x+=20;
        nextReading.fingers[0].resultant.x+=20;
        feedReading(nextReading);
        check(readings.m_pointTable->item(0,1)->text()==visibleReading,
            "hidden readings stop table formatting while accepting the latest hardware data");
        readings.m_hideReadingsToggle->setChecked(false);
        check(readings.m_pointTable->item(0,1)->text()==QString::number(
            nextReading.fingers[0].modules[0].points[0].force.x-checkedGood.fingers[0].modules[0].points[0].force.x,'f',0),
            "revealing readings immediately shows the latest hardware zero-point delta");
    }
    checked.m_statusToggle->setChecked(true);
    const auto liveTableLabel=checked.m_tableLabel->text();
    auto checkedBad=checkedGood;checkedBad.sequence=2;checkedBad.monotonicNs=2;checkedBad.crcOk=false;checkedBad.valid=false;
    feedChecked(checkedBad,false);
    check(!checked.frame().valid&&checked.m_adapter->valid()&&checked.m_adapter->sourceLabel().contains("DISPLAY HELD")
        &&checked.m_pointTable->item(0,1)->text()=="--"&&checked.m_tareButton->isEnabled()
        &&sameForce(checked.chartForce(),checkedGood.fingers[0].modules[0].points[0].force)&&checked.history().samples().size()==1,
        "CRC-on rejection retains the previous displayed values while raw frame stays invalid and history stays unchanged");
    auto *heldLegend=checked.m_canvas->rootObject()?checked.m_canvas->rootObject()->findChild<QObject *>("tactileLegend"):nullptr;
    const auto heldTableLabel=checked.m_tableLabel->text();
    const auto heldSource=checked.m_adapter->sourceLabel();
    const auto heldCurve=checked.chartForce();
    const auto tableText=[&]{QStringList values;for(int row=0;row<checked.m_pointTable->rowCount();++row)
        for(int col=0;col<checked.m_pointTable->columnCount();++col)values<<checked.m_pointTable->item(row,col)->text();return values;};
    const auto heldTableText=tableText();
    check(checked.m_statusToggle->isChecked()&&checked.m_adapter->communicationStatusVisible()&&checked.m_filterStatus->isHidden()
        &&heldTableLabel==liveTableLabel&&!heldLegend
        &&checked.m_filterStatus->toolTip().isEmpty()&&checked.m_displayFilter.rejected==1,
        "enabled communication status omits the skip counter and empty row while filtering still counts rejects");
    checked.m_statusToggle->setChecked(false);
    check(checked.m_tableLabel->text()==liveTableLabel&&checked.m_serialStatus->isHidden()&&checked.m_filterStatus->isHidden()
        &&!checked.m_adapter->communicationStatusVisible()&&!heldLegend
        &&tableText()==heldTableText&&checked.m_adapter->sourceLabel()==heldSource
        &&!checked.frame().valid&&sameForce(checked.chartForce(),heldCurve)&&checked.history().samples().size()==1,
        "hiding communication status keeps fixed headings, colors, data and history");
    checked.m_statusToggle->setChecked(true);
    check(checked.m_tableLabel->text()==heldTableLabel&&checked.m_filterStatus->isHidden()
        &&checked.m_adapter->communicationStatusVisible()&&!heldLegend
        &&tableText()==heldTableText&&checked.m_adapter->sourceLabel()==heldSource,
        "showing communication status restores counters without model or held annotations and without changing data");
    checked.m_trustedReceivedMs-=1001;checkedBad.sequence=3;checkedBad.monotonicNs=3;feedChecked(checkedBad,false);
    check(!checked.m_adapter->valid()&&!checked.frame().valid&&checked.m_pointTable->item(0,1)->text()=="--"
        &&checked.history().samples().size()==1,"continuous CRC failure keeps the last displayed table even after arrow hold expires");
    checked.m_tareButton->click();
    bool heldZero=true;
    for(int row=0;row<checked.m_pointTable->rowCount();++row)
        for(int col=1;col<4;++col)heldZero &= checked.m_pointTable->item(row,col)->text()=="0";
    check(heldZero&&checked.hasDisplayBaseline()&&checked.m_tareFrame.sequence==checkedGood.sequence
        &&sameForce(checked.m_tareFrame.fingers[0].resultant,checkedGood.fingers[0].resultant)
        &&!checked.frame().valid&&!checked.m_adapter->valid(),
        "tare during prolonged CRC failure uses the displayed previous frame without reviving raw data or expired arrows");
    checkedGood.sequence=4;checkedGood.monotonicNs=4;feedChecked(checkedGood,true);
    checked.m_connectionSettings->setChecked(false);
    SerialSample portFailure;portFailure.requests=5;portFailure.verifyCrc=true;portFailure.error="Port removed";
    checked.receiveSerialSample(portFailure);checked.updateValues();
    check(!checked.m_adapter->valid()&&!checked.visualFrame().valid&&checked.m_pointTable->item(0,1)->text()=="--"
        &&!checked.m_connectionSettings->isChecked()&&!checked.hasDisplayBaseline(),
        "terminal port error preserves collapsed settings and immediately clears held arrows, table and baseline");
    checked.m_tareButton->click();
    check(checked.m_tareButton->isEnabled()&&!checked.hasDisplayBaseline(),"tare remains harmless and clickable after disconnect");
    checked.m_hardwareType=0;checked.m_serialHistory.begin(session,1);
    checkedGood.sequence=6;checkedGood.monotonicNs=6;feedChecked(checkedGood,true);
    auto checkedSpike=spike;checkedSpike.crcVerified=true;checkedSpike.sequence=7;checkedSpike.monotonicNs=7;feedChecked(checkedSpike,true);
    const auto trustedSequence=checked.m_displayFilter.last.sequence;
    checked.m_tareButton->click();
    bool onlyTrustedHistory=true;
    for(const auto &entry:checked.history().samples())onlyTrustedHistory &= entry.timeNs!=checkedSpike.monotonicNs;
    check(checked.m_tareFrame.sequence==checkedSpike.sequence&&checked.m_pointTable->item(0,1)->text()=="0"
        &&checked.m_displayFilter.last.sequence==trustedSequence&&onlyTrustedHistory,
        "CRC-valid latest displayed step can become zero without changing the jump-filter reference or entering trusted history");
    checkedBad.sequence=8;checkedBad.monotonicNs=8;feedChecked(checkedBad,false);
    check(checked.m_pointTable->item(0,1)->text()=="0",
        "CRC failure retains the latest CRC-good table even when that sample exceeded the arrow jump threshold");
    SerialSample timeout;timeout.portOpen=true;timeout.verifyCrc=true;timeout.requests=9;timeout.error="Response timeout";
    checked.receiveSerialSample(timeout);checked.updateValues();
    check(checked.m_pointTable->item(0,1)->text()=="--", "timeout ends held table continuity");
    checkedBad.sequence=10;checkedBad.monotonicNs=10;feedChecked(checkedBad,false);
    check(checked.m_pointTable->item(0,1)->text()=="--", "CRC failure after timeout cannot revive a retired table snapshot");
    TactileVisualizationWidget unverified;unverified.m_hardwareType=0;unverified.m_crc->setChecked(false);unverified.m_serialHistory.begin(session,1);
    SerialSample unchecked;unchecked.accepted=true;unchecked.portOpen=true;unchecked.verifyCrc=false;unchecked.frame=good;unchecked.requests=1;
    QElapsedTimer now;now.start();unchecked.receivedMs=now.msecsSinceReference();
    unverified.receiveSerialSample(unchecked);unverified.updateValues();
    unchecked.frame=spike;unchecked.frame.crcOk=false;unchecked.requests=3;unchecked.receivedMs=now.msecsSinceReference();
    unverified.receiveSerialSample(unchecked);unverified.updateValues();unverified.m_tareButton->click();
    check(unverified.frame().fingers[0].modules[0].points[0].force.x==5011&&unverified.m_tareFrame.sequence==good.sequence
        &&unverified.m_tareFrame.crcOk&&unverified.m_pointTable->item(0,1)->text()=="5001",
        "CRC-off corrupt raw display cannot become zero; tare uses the last CRC-good snapshot");
    {
        TactileVisualizationWidget status;status.resize(837,640);status.show();wait(30);
        status.m_hardwareType=0;status.m_connectedSettings.port="OFFLINE_STATUS_FIXTURE";
        status.m_serialHistory.begin(session,1);
        quint64 sequence=0;
        const auto feedStatus=[&](const QString &error){
            auto frame=checkedGood;frame.sequence=++sequence;frame.monotonicNs=sequence;
            frame.crcOk=error.isEmpty();frame.valid=error.isEmpty();
            SerialSample data;data.frame=frame;data.accepted=error.isEmpty();data.portOpen=error!=QStringLiteral("Port removed");
            data.verifyCrc=true;data.crcOk=error.isEmpty();data.requests=sequence;data.error=error;
            QElapsedTimer clock;clock.start();data.receivedMs=clock.msecsSinceReference();
            status.receiveSerialSample(data);status.updateValues();wait(5);
        };
        const auto sceneGeometry=[&]{return QRect(status.m_canvas->mapTo(&status,QPoint()),status.m_canvas->size());};
        bool stable=true,hidden=true,heldCorrect=true,spaceRules=true;
        for(bool compact:{false,true}) {
            status.m_hideReadingsToggle->setChecked(compact);status.m_statusToggle->setChecked(true);
            feedStatus({});const auto before=sceneGeometry();
            spaceRules &= !status.m_serialStatus->sizePolicy().retainSizeWhenHidden()
                &&!status.m_filterStatus->sizePolicy().retainSizeWhenHidden();
            status.m_statusToggle->setChecked(false);wait(10);
            const auto withoutStatus=sceneGeometry();
            spaceRules &= withoutStatus.top()<before.top()&&withoutStatus.height()>before.height();
            for(int i=0;i<8;++i) {
                feedStatus(i%2?QString():QStringLiteral("CRC mismatch"));
                stable &= sceneGeometry()==withoutStatus;
                hidden &= status.m_serialStatus->isHidden()&&status.m_filterStatus->isHidden();
            }
            status.m_statusToggle->setChecked(true);wait(10);
            stable &= sceneGeometry()==before;
            feedStatus(QStringLiteral("CRC mismatch"));
            const auto statusText=status.m_serialStatus->toolTip();
            heldCorrect &= !statusText.contains(status.tr("Showing last valid hardware frame"))
                &&!statusText.contains(status.tr("Hardware data"))
                &&!statusText.contains("CRC mismatch")&&!status.m_serialStatus->text().contains("CRC mismatch")
                &&statusText.contains(status.tr(" | %1 ID %2 | RX accepted %3/%4 | CRC errors %5 | Timeouts %6 | Protocol errors %7")
                    .arg(status.m_connectedSettings.port).arg(status.m_connectedSettings.slave)
                    .arg(status.m_serialSample.acceptedCount).arg(status.m_serialSample.requests)
                    .arg(status.m_serialSample.crcErrors).arg(status.m_serialSample.timeouts).arg(status.m_serialSample.protocolErrors))
                &&status.m_tableLabel->text()==status.tr("Zero-point delta per output · counts")&&status.m_adapter->valid();
            stable &= sceneGeometry()==before;
            feedStatus({});stable &= sceneGeometry()==before;
        }
        check(hidden,"alternating good and CRC-rejected frames never reopen hidden communication status");
        check(stable,"changing frame validity never shifts either layout and explicitly restoring status recovers the same scene rectangle");
        check(heldCorrect,"rejected frames retain trusted display and serial counters without CRC mismatch or removed frame descriptions");
        check(spaceRules,"hidden communication status reclaims space in both layouts");
        status.m_statusToggle->setChecked(false);feedStatus(QStringLiteral("Port removed"));
        check(status.m_serialStatus->isHidden()&&!status.m_adapter->valid(),
            "hiding port-error text still revokes invalid hardware display");
        check(status.grab().save(folder+"/tactile_status_stable_hidden.png"),"capture compact hidden status without a reserved blank row using an offline fixture");
        status.close();status.m_statusToggle->setChecked(true);status.m_hideReadingsToggle->setChecked(false);
    }
    {
        QSettings noisePreferences(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::organizationName(),QCoreApplication::applicationName());
        noisePreferences.remove("tactile/display/noiseEnabled");noisePreferences.sync();
        TactileVisualizationWidget noise;noise.resize(1100,900);noise.show();wait(50);
        noise.m_hardwareType=0;noise.m_serialHistory.begin(session,1);
        check(noise.m_noiseEnabled->isChecked()&&noise.m_noiseThreshold->value()==150&&noise.m_noiseThreshold->isEnabled()
            &&noise.m_noiseThreshold->minimum()==1&&noise.m_noiseThreshold->maximum()==5000,
            "noise suppression defaults on with a manual 150-count reference and bounded input");
        check(!noise.m_reverseZArrows->isChecked()&&!noise.m_adapter->reverseZArrows()
            &&noise.m_displayControls->isAncestorOf(noise.m_reverseZArrows),
            "Z arrow reversal defaults off and belongs to display settings");
        quint64 sequence=0;
        const auto noiseFeed=[&](Force value,bool crcOk=true,bool rangeOk=true){
            auto f=good;f.sequence=++sequence;f.monotonicNs=sequence;f.crcVerified=true;f.crcOk=crcOk;f.rangeOk=rangeOk;
            auto &finger=f.fingers[0];finger.resultant={};finger.modules[0].resultant={};
            for(auto &point:finger.modules[0].points)point.force={};
            finger.modules[0].points[0].force=value;finger.modules[0].resultant=value;finger.resultant=value;
            SerialSample reading;reading.accepted=crcOk;reading.portOpen=true;reading.verifyCrc=true;reading.crcOk=crcOk;
            reading.rangeOk=rangeOk;reading.frame=f;reading.requests=sequence;reading.registers.fill(0,64);
            reading.registers[0]=quint16(int(value.x));reading.registers[1]=quint16(int(value.y));reading.registers[2]=quint16(int(value.z));
            if(!crcOk)reading.error="CRC mismatch";
            QElapsedTimer received;received.start();reading.receivedMs=received.msecsSinceReference();
            noise.receiveSerialSample(reading);noise.m_arrowTransition.invalidate();noise.updateValues();
        };
        noise.m_noiseEnabled->setChecked(true);noiseFeed({-150,150,150});
        check(sameForce(noise.chartForce(),{0,0,150})&&noise.m_pointTable->item(0,1)->text()=="--"
            &&noise.m_pointTable->item(0,2)->text()=="--"&&noise.m_pointTable->item(0,3)->text()=="--"
            &&noise.m_adapter->points()[0].toMap()["arrowVisible"].toBool()
            &&noise.m_adapter->resultant()["magnitude"].toDouble()==150
            &&noise.m_adapter->points()[0].toMap()["color"].value<QColor>()==TactileSceneAdapter::forceColor(150),
            "inclusive deadband suppresses X/Y while Z remains visible and untared list entries stay unavailable");
        check(noise.m_pointTable->horizontalHeaderItem(1)->text()=="X"
            &&noise.m_pointTable->horizontalHeaderItem(2)->text()=="Y"&&noise.m_pointTable->horizontalHeaderItem(3)->text()=="Z"
            &&noise.m_chartLabel->text()==noise.tr("P%1 · displayed counts · time (s)").arg(1),
            "delta-only table uses plain X Y Z headers");
        check(noise.frame().fingers[0].modules[0].points[0].force.x==-150
            &&noise.m_displayFilter.last.fingers[0].modules[0].points[0].force.y==150
            &&noise.history().samples().back().pointForces[0][0].x==-150,
            "noise suppression preserves acquisition, the raw jump reference and trusted history");
        noise.m_connectionSettings->setChecked(false);noise.m_displaySettings->setChecked(false);noise.m_hideReadingsToggle->setChecked(false);wait(20);
        check(noise.grab().save(folder+"/tactile_noise_enabled.png"),"capture suppressed model and numeric readings using an offline fixture");
        noise.m_noiseThreshold->setValue(149);
        check(sameForce(noise.chartForce(),{-150,150,150})&&noise.m_adapter->points()[0].toMap()["fx"].toDouble()==-150
            &&noise.m_adapter->points()[0].toMap()["color"].value<QColor>()==TactileSceneAdapter::forceColor(150),
            "manual threshold changes reveal 3D force and pressure color while keeping the same full numeric signal");
        noise.m_noiseThreshold->setValue(150);noise.m_noiseEnabled->setChecked(false);
        check(noise.chartForce().x==-150&&noise.m_pointTable->item(0,1)->text()=="--",
            "disabling suppression preserves the full numeric readings");
        noise.m_noiseEnabled->setChecked(true);noiseFeed({});noise.setDisplayZero();noiseFeed({151,-151,151});
        check(sameForce(noise.chartForce(),{151,-151,151})&&noise.m_adapter->points()[0].toMap()["fz"].toDouble()==151,
            "values outside the deadband retain their full amplitude");
        noise.m_density->setCurrentIndex(noise.m_density->findData(4));
        int smallOutput=-1;Force interpolated;
        for(int point=20;point<noise.displayPointCount();++point){
            const auto value=sampleTactileDisplayPoint(noise.frame().fingers[0].modules[0].points,0,4,point);
            if(value.x>0&&value.x<=150){smallOutput=point;interpolated=value;break;}
        }
        check(smallOutput>=20,"nonuniform measurements produce a small interpolated display component");
        if(smallOutput>=20){
            noise.selectPoint(smallOutput);
            check(sameForce(noise.chartForce(),{0,0,interpolated.z})&&noise.m_pointTable->item(smallOutput,1)->text()=="0"
                &&noise.m_pointTable->item(smallOutput,3)->text()==QString::number(interpolated.z,'f',0)
                &&std::abs(noise.m_adapter->points()[smallOutput].toMap()["fz"].toDouble()-interpolated.z)<1e-9,
                "interpolated Z bypasses suppression in model chart and delta-only list while X/Y keep their deadband");
            noise.m_noiseEnabled->setChecked(false);
            check(sameForce(noise.chartForce(),interpolated)
                &&noise.m_pointTable->item(smallOutput,1)->text()==QString::number(interpolated.x,'f',0),
                "disabling suppression immediately restores the complete interpolated readout");
            noise.m_noiseEnabled->setChecked(true);noise.selectPoint(0);
        }
        const auto originalPoints=noise.m_adapter->points();const auto originalSum=noise.m_adapter->resultant();
        const auto originalTable=noise.m_pointTable->item(0,3)->text();const auto originalHistory=noise.history().samples().size();
        const auto direction=[](const QVariantMap &arrow){return arrow["rotation"].value<QQuaternion>().rotatedVector(QVector3D(0,1,0));};
        const auto reversedDirection=[&](const QVariantMap &before,const QVariantMap &after){
            const auto a=direction(before),b=direction(after);
            return (b-QVector3D(a.x(),a.y(),-a.z())).length()<1e-5&&before["length"]==after["length"]&&before["magnitude"]==after["magnitude"];
        };
        noise.m_reverseZArrows->setChecked(true);
        const auto reversedPoints=noise.m_adapter->points();bool allReversed=originalPoints.size()==136&&reversedPoints.size()==originalPoints.size();
        for(int p=0;p<originalPoints.size()&&allReversed;++p){
            const auto before=originalPoints[p].toMap(),after=reversedPoints[p].toMap();
            const auto beforeAxes=before["components"].toList(),afterAxes=after["components"].toList();
            allReversed &= reversedDirection(before,after)&&reversedDirection(beforeAxes[2].toMap(),afterAxes[2].toMap())
                &&beforeAxes[0]==afterAxes[0]&&beforeAxes[1]==afterAxes[1]&&before["fz"]==after["fz"]
                &&before["position"]==after["position"]&&before["dotPosition"]==after["dotPosition"]&&before["color"]==after["color"];
        }
        check(allReversed&&reversedDirection(originalSum,noise.m_adapter->resultant()),
            "Z reversal mirrors raw and interpolated force arrows and the resultant without changing XY directions lengths colors or dot positions");
        check(sameForce(noise.chartForce(),{151,-151,151})&&noise.frame().fingers[0].modules[0].points[0].force.z==151
            &&noise.m_pointTable->item(0,3)->text()==originalTable&&noise.history().samples().size()==originalHistory,
            "Z arrow reversal preserves raw frames numeric readouts curves and trusted history");
        noise.m_reverseZArrows->setChecked(false);
        check(noise.m_adapter->points()==originalPoints&&noise.m_adapter->resultant()==originalSum,
            "clearing Z reversal immediately restores all original arrow geometry");
        noise.m_density->setCurrentIndex(noise.m_density->findData(2));
        noiseFeed({1000,-2000,3000});noise.setDisplayZero();noiseFeed({1150,-1850,3150});
        check(sameForce(noise.chartForce(),{0,0,150})&&noise.m_adapter->resultant()["magnitude"].toDouble()==150
            &&noise.m_pointTable->item(0,1)->text()=="0"&&noise.m_pointTable->item(0,3)->text()=="150"
            &&noise.m_adapter->points()[0].toMap()["color"].value<QColor>()==TactileSceneAdapter::forceColor(150)
            &&noise.m_tareFrame.fingers[0].modules[0].points[0].force.x==1000,
            "tared X/Y deadband retains Z and the original zero baseline across all readouts");
        for(int z:{0,1,-1,149,-149,150,-150,151,-151}){
            noiseFeed({1100,-1900,double(3000+z)});
            const auto point=noise.m_adapter->points()[0].toMap();
            const auto zArrow=point["components"].toList()[2].toMap();
            check(sameForce(noise.chartForce(),{0,0,double(z)})&&point["fz"].toDouble()==z
                &&zArrow["value"].toDouble()==z&&zArrow["arrowVisible"].toBool()==(z!=0)
                &&noise.m_pointTable->item(0,1)->text()=="0"&&noise.m_pointTable->item(0,2)->text()=="0"
                &&noise.m_pointTable->item(0,3)->text()==QString::number(z),
                "suppression-on preserves signed Z delta at zero inside on and outside the threshold across model chart and list");
            noise.m_noiseEnabled->setChecked(false);
            check(sameForce(noise.chartForce(),{100,100,double(z)})
                &&noise.m_adapter->points()[0].toMap()["fz"].toDouble()==z
                &&noise.m_pointTable->item(0,1)->text()=="100"&&noise.m_pointTable->item(0,2)->text()=="100"
                &&noise.m_pointTable->item(0,3)->text()==QString::number(z),
                "suppression-off restores X/Y without changing signed Z delta");
            noise.m_noiseEnabled->setChecked(true);
        }
        noise.m_noiseThreshold->setValue(5000);noiseFeed({1001,-1999,3001});
        check(sameForce(noise.chartForce(),{0,0,1})&&noise.m_adapter->points()[0].toMap()["fz"].toDouble()==1
            &&noise.m_pointTable->item(0,3)->text()=="1","even the largest noise threshold leaves a one-count Z delta visible");
        noise.m_noiseThreshold->setValue(150);
        noiseFeed({1151,-1849,3151});
        check(sameForce(noise.chartForce(),{151,151,151}),"a real change just outside the tared deadband is retained");
        noiseFeed({1151,-1849,2700});const auto negativeDelta=noise.m_adapter->resultant();
        noise.m_reverseZArrows->setChecked(true);
        check(reversedDirection(negativeDelta,noise.m_adapter->resultant())&&noise.chartForce().z==-300
            &&noise.m_tareFrame.fingers[0].modules[0].points[0].force.z==3000,
            "Z arrow reversal also handles signed tare deltas without reversing numeric values or the baseline");
        noise.m_reverseZArrows->setChecked(false);
        noise.m_tareFrame={};noise.m_noiseThreshold->setValue(5000);noiseFeed({-3000,0,0});
        const auto trusted=noise.m_displayFilter.last.sequence;const auto samples=noise.history().samples().size();
        noiseFeed({3000,0,0});
        check(noise.m_displayFilter.last.sequence==trusted&&noise.history().samples().size()==samples
            &&noise.frame().fingers[0].modules[0].points[0].force.x==3000,
            "a raw 6000-count jump remains rejected even when both endpoints would be noise-zeroed");
        noiseFeed({-2999,0,0},false);noiseFeed({-2998,0,0},true,false);
        check(noise.m_displayFilter.last.sequence==trusted&&noise.history().samples().size()==samples,
            "CRC and manual-range errors remain rejected before noise suppression");
        noise.disconnectSerial();check(!noise.m_adapter->valid()&&!noise.frame().valid,"noise suppression cannot invent data after disconnect");
        noise.close();noise.m_noiseEnabled->setChecked(false);noise.m_noiseThreshold->setValue(150);
    }
    {
        QTranslator chinese;
        check(chinese.load(":/i18n/hand_zh_CN.qm")&&QCoreApplication::installTranslator(&chinese),"load compiled Chinese translation for display methods");
        TactileVisualizationWidget dense;dense.resize(1450,1000);dense.show();wait(40);TactileTestAccess::showSnapshot(dense);
        check(dense.findChild<QLabel *>("tactileDensityLabel")->text()==QString::fromUtf8("显示方式"),"Chinese caption is Display method");
        auto canonical=dense.frame();
        check(validateFrame(canonical),"dense readout fixture has a canonical raw frame");
        const auto expectedPoint=[&](int type,int grid,int point){
            const auto &raw=canonical.fingers[type].modules[0].points;
            if(point<raw.size())return raw[point].force;
            Force value;const auto &sample=tactileSurfaceModel(type).interpolation[grid-3][point-raw.size()];
            for(int i=0;i<sample.sources.size();++i){
                const auto f=raw[sample.sources[i]].force;const double w=sample.weights[i];
                value.x+=f.x*w;value.y+=f.y*w;value.z+=f.z*w;
            }
            return value;
        };
        for(int type=0;type<3;++type){
            TactileTestAccess::showSnapshot(dense,type);canonical=dense.frame();
            const int counts[3]={type==2?16:20,type==2?49:65,type==2?100:136};
            for(int mode=0;mode<3;++mode){
                const int grid=mode+2,count=counts[mode];dense.m_tareFrame={};
                dense.m_density->setCurrentIndex(mode);dense.updateValues();
                check(dense.m_density->itemText(mode)==dense.tr("%1 tactile output points").arg(count)
                    &&dense.displayPointCount()==count&&dense.m_pointTable->rowCount()==count
                    &&dense.m_adapter->points().size()==count,"display option, model and list share the actual output count for every sensor and mode");
                check(dense.m_density->itemText(mode)==QString::number(count)+QString::fromUtf8(" 触觉输出点"),
                    "Chinese choices display the actual depth output count");
                dense.m_pointTable->setCurrentCell(count-1,0);
                const auto expected=expectedPoint(type,grid,count-1);
                check(dense.selectedPoint()==count-1&&sameForce(dense.chartForce(),expected)
                    &&dense.m_pointTable->selectionModel()->isRowSelected(count-1,QModelIndex())
                    &&dense.m_pointTable->selectionModel()->selectedRows().size()==1
                    &&dense.m_pointTable->item(count-1,0)->text()==QString("P%1").arg(count)
                    &&dense.m_pointTable->item(count-1,1)->text()==QString("--"),
                    "last output row selects its complete interpolated XYZ chart and list values");
                const auto &history=dense.history().samples();
                check(!history.empty()&&history.back().pointForces[type].size()==(type==2?16:20)
                    &&sameForce(dense.displayPointForce(history.back().pointForces[type],count-1),expected)
                    &&dense.frame().sequence==canonical.sequence,
                    "historical charts sample existing weights without expanding or modifying raw history");
                dense.setDisplayZero();
                check(dense.hasDisplayBaseline()&&sameForce(dense.chartForce(),{})
                    &&dense.m_pointTable->item(count-1,1)->text()==QString("0"),
                    "interpolated delta uses the same original tare frame and zeroes the selected output point");
            }
            dense.m_tareFrame={};dense.m_density->setCurrentIndex(0);dense.updateValues();
            check(dense.selectedPoint()<counts[0]&&dense.m_pointTable->currentRow()==dense.selectedPoint()
                &&dense.m_pointTable->selectionModel()->isRowSelected(dense.selectedPoint(),QModelIndex())
                &&dense.m_pointTable->selectionModel()->selectedRows().size()==1,
                "reducing the output count keeps table and chart selection in range");
        }
        TactileTestAccess::showSnapshot(dense,0);canonical=dense.frame();
        dense.m_serialHistory.begin(canonical.sessionId,canonical.generation);bool replayed=true;
        for(int i=0;i<81;++i){
            auto sample=tactileTestFrame(0,{double(600+i),double(-400+i),double(1000+i)});
            sample.sessionId=canonical.sessionId;sample.generation=canonical.generation;
            sample.sequence=canonical.sequence+i+1;sample.monotonicNs=canonical.monotonicNs+qint64(i+1)*50000000;
            if(i==0)dense.m_tareFrame=sample;
            replayed &= dense.m_serialHistory.append(sample);dense.m_liveFrame=dense.m_heldTableFrame=sample;dense.m_displayFilter.last=sample;
        }
        check(replayed&&dense.m_serialHistory.samples().size()==81,"continuous offline history supplies the expanded output curves");
        dense.m_fingers->button(0)->click();dense.m_density->setCurrentIndex(1);dense.updateValues();
        dense.selectPoint(64);dense.m_displaySettings->setChecked(true);dense.m_connectionSettings->setChecked(false);
        wait(20);check(dense.grab().save(folder+"/tactile_65_outputs.png"),"capture 65 output point display method");
        dense.m_displaySettings->setChecked(false);wait(20);
        dense.m_pointTable->scrollToItem(dense.m_pointTable->item(64,0),QAbstractItemView::PositionAtBottom);wait(10);
        check(dense.grab().save(folder+"/tactile_65_readings.png"),"capture P65 curve and selected output row with model labels removed");
        TactileTestAccess::showSnapshot(dense,2);dense.setDisplayZero();dense.m_density->setCurrentIndex(2);dense.selectPoint(99);
        check(dense.grab().save(folder+"/tactile_100_outputs.png"),"capture ultrathin 100 output points");
        dense.m_density->setCurrentIndex(0);dense.close();QCoreApplication::removeTranslator(&chinese);
    }
    }
    report<<(failures?"RESULT FAIL\n":"RESULT PASS\n"); return failures?4:0;
}
