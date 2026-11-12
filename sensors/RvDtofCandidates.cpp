#include "RvDtofCandidates.h"
namespace RvDtofCandidates {
QString name(int mode){return mode==LiteSrNear?QStringLiteral("board_litesr_near_256"):
    mode==Native48?QStringLiteral("board_native_tof48"):
    mode==Native48HighAccuracy?QStringLiteral("board_native_tof48_high_accuracy"):
    mode==NativeHd?QStringLiteral("native_hd_capture"):QString();}
int modeForScheme(const QString &scheme){
    for(int mode:{Native48,Native48HighAccuracy,LiteSrNear})if(scheme==name(mode))return mode;
    return -1;
}
}
