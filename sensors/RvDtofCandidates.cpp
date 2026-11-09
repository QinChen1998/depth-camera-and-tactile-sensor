#include "RvDtofCandidates.h"
namespace RvDtofCandidates {
QString name(int mode){return mode==Native48?QStringLiteral("board_native_tof48"):
    mode==Native48HighAccuracy?QStringLiteral("board_native_tof48_high_accuracy"):
    mode==NativeHd?QStringLiteral("native_hd_capture"):QString();}
}
