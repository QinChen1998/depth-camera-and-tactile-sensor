#pragma once
#include "RvTofFrame.h"
#include "../tools/dtof_m0/M0.h"
#include "../tools/litesr_board/Contract.h"
#include <QtGui/QImage>

namespace RvDtofCandidates {
constexpr int FastDepth=9; // Stable DTF3 board mode, not a GUI index.
constexpr int LiteSrFull=LiteSrBoardContract::Full, LiteSrContour=LiteSrBoardContract::Contour, LiteSrFullHd=LiteSrBoardContract::FullHd, LiteSrNearFullHd=LiteSrBoardContract::NearFullHd;
constexpr int LiteSrRectFullHd=LiteSrBoardContract::RectFullHd;
constexpr int LiteSrHighResolution=LiteSrBoardContract::HighResolution;
constexpr int LiteSrNative16Depth=LiteSrBoardContract::Native16Depth;
constexpr int NativeHd=LiteSrBoardContract::NativeHd;
inline bool isCaptureOnly(int mode){return LiteSrBoardContract::isCaptureOnly(mode);}
inline bool isLiteSrHd(int mode){return LiteSrBoardContract::isHd(mode);}
inline bool isLiteSrFull(int mode){return LiteSrBoardContract::isFull(mode);}
inline bool isLiteSr(int mode){return LiteSrBoardContract::supports(mode);}
inline bool supports(int mode){return isLiteSr(mode);}
inline int outputWidth(int mode){return isLiteSr(mode)?LiteSrBoardContract::outputWidth(mode):384;}
inline int outputHeight(int mode){return isLiteSr(mode)?LiteSrBoardContract::outputHeight(mode):256;}
inline int freshnessMs(int mode){return isLiteSr(mode)?LiteSrBoardContract::FreshMs:220;}
struct Settings {
    DtofM0::Config metric;
    double nearM=.6, sigma=.5, currentWeight=.6;
    int minimumComponent=5, fillHoles=32, palette=0;
    bool bezier=true;
    int fastInterpolation=0;
    double presentation=.5; // Medium visual smoothing; 0 retains the original fast-depth path.
    int liteSrHoles=1;
    bool liteSrOutliers=true;
};
inline Settings defaults(int =FastDepth){return {};}
QString name(int mode);
QString identity(const QJsonObject&);
QJsonObject parameters(int mode,const Settings&);
struct Result {
    QByteArray depth,coverage;
    QJsonObject metadata;
    QString error;
    qint64 receivedMs=-1;
    bool fresh=false;
};
}
