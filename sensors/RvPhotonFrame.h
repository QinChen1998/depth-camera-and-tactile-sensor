#pragma once
#include "RvTofFrame.h"
#include "../tools/rv_photon/Wire.h"

struct RvPhotonFrame {
    QByteArray signalWords;
    RvTofFrame tof;
    QString bootId;
    int resultFormat=0;
    double ageMs=0;
    int fovCorrection=-1; // Actual PHC1 header byte; never inferred from display.
    bool isValid() const {
        return tof.isValid() && signalWords.size()==tof.sampleCount()*2
            && (resultFormat&8) && (resultFormat&7)==1 && !(resultFormat&0xc0)
            && QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(bootId).hasMatch()
            && std::isfinite(ageMs) && ageMs>=0 && ageMs<500;
    }
    quint16 encodedSignal(int index) const { return qFromLittleEndian<quint16>(signalWords.constData()+2*index); }
    quint32 photonCount(int index) const { return rv_photon_count(encodedSignal(index)); }
};
Q_DECLARE_METATYPE(RvPhotonFrame)
