#include "RvDepthDiagnostic.h"

#include <cmath>
#include <cstring>

namespace RvDepthDiagnostic {
QByteArray encode(const std::vector<float> &mono,const RvAssistFusion::Result &result){
    if(mono.size()!=Pixels)return {};
    const int depthBytes=int(mono.size())*4;
    QByteArray out(depthBytes+2*ReasonBytes,Qt::Uninitialized);auto *data=out.data();
    qToLittleEndian<quint32>(mono.data(),qsizetype(mono.size()),data);
    const auto reasons=[&](const QByteArray &source,int offset){
        std::memset(data+offset,RvAssistFusion::NoMeasurement,ReasonBytes);
        if(source.size()==ReasonBytes)std::memcpy(data+offset,source.constData(),ReasonBytes);
    };
    reasons(result.globalReasons,depthBytes);reasons(result.localReasons,depthBytes+ReasonBytes);return out;
}
bool validPayload(const QByteArray &p,int depthBytes){
    if(depthBytes!=DepthBytes||p.size()!=depthBytes+2*ReasonBytes)return false;
    const auto *data=p.constData();
    for(int i=0;i<depthBytes/4;++i){const auto bits=qFromLittleEndian<quint32>(data+4*i);float z;std::memcpy(&z,&bits,4);
        if(!std::isfinite(z)||z<=0)return false;}
    for(int i=depthBytes;i<p.size();++i)if(quint8(data[i])>RvAssistFusion::AlreadySupported)return false;
    return true;
}
}
