#pragma once
#include "../sensors/TactileVisualizationWidget.h"
#include <QButtonGroup>
#include <QAbstractButton>

// Fixed native-frame snapshots for geometry/readout tests only. No waveform,
// acquisition source, scenario selection, or hardware I/O is provided here.
inline tactile::Frame tactileTestFrame(int type=0, tactile::Force force={600,-400,1000})
{
    tactile::Frame frame;
    frame.valid=true;frame.hardwareType=type;
    frame.sessionId=QUuid("{6f0c0e93-4838-4c52-a418-4ed876c72327}");
    frame.generation=1;frame.sequence=1;frame.monotonicNs=1;
    for(int slot=0;slot<3;++slot) {
        tactile::Finger finger;finger.id=tactile::sensorLayout().fingerIds[slot];finger.valid=slot==type;
        if(finger.valid) {
            tactile::Module module;module.id="M1";
            for(const auto &point:tactile::sensorLayout(type).points) {
                module.points.append({point.pointId,force});module.resultant+=force;
            }
            finger.modules.append(module);finger.resultant=module.resultant;
        }
        frame.fingers.append(finger);
    }
    return frame;
}

struct TactileTestAccess {
    static void publish(TactileVisualizationWidget &widget,const tactile::Frame &frame)
    {
        const bool same=tactile::matchingBaseline(widget.m_liveFrame,frame);
        widget.m_hardwareType=frame.hardwareType;
        widget.m_liveFrame=widget.m_heldTableFrame=frame;
        widget.m_displayFilter.last=frame;widget.m_displayFilter.reason.clear();
        widget.m_serialSample.frame=frame;widget.m_serialSample.portOpen=true;
        widget.m_serialSample.accepted=true;widget.m_serialSample.verifyCrc=true;
        widget.m_waitingForFirstHardwareFrame=false;
        widget.m_arrowFrom={};widget.m_arrowTransition.invalidate();
        QElapsedTimer clock;clock.start();widget.m_trustedReceivedMs=clock.msecsSinceReference();
        if(!same)widget.m_serialHistory.begin(frame.sessionId,frame.generation);
        widget.m_serialHistory.append(frame);
        ++widget.m_refreshCount;
        widget.m_fingers->button(frame.hardwareType)->click();
        widget.updateValues();
    }
    static void showSnapshot(TactileVisualizationWidget &widget,int type=0,tactile::Force force={600,-400,1000})
    {
        widget.start();widget.m_timer.stop();
        auto frame=tactileTestFrame(type,force);frame.sessionId=QUuid::createUuid();
        widget.m_tareFrame={};publish(widget,frame);
        // Keep a frozen render fixture current while camera/layout assertions
        // run. Real acquisition, expiry and suspension are covered by serial tests.
        if(!widget.findChild<QTimer *>("tactileSnapshotClock")) {
            auto *timer=new QTimer(&widget);timer->setObjectName("tactileSnapshotClock");
            QObject::connect(timer,&QTimer::timeout,&widget,[&widget] {
                if(widget.running()&&widget.m_liveFrame.valid) {
                    QElapsedTimer clock;clock.start();widget.m_trustedReceivedMs=clock.msecsSinceReference();
                }
            });
            timer->start(20);
        }
    }
    static void advance(TactileVisualizationWidget &widget)
    {
        auto frame=widget.m_liveFrame;++frame.sequence;frame.monotonicNs+=33000000;
        publish(widget,frame);
    }
};
