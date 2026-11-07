#pragma once
#include "RvTofFrame.h"
#include "../tools/litesr_board/Contract.h"
#include <QtGui/QImage>

namespace RvDtofCandidates {
constexpr int TinyNv12=TinyNv12Contract::Mode;
inline bool isTinyNv12(int mode){return mode==TinyNv12;}
constexpr int LiteSrFull=LiteSrBoardContract::Full, LiteSrContour=LiteSrBoardContract::Contour, LiteSrFullHd=LiteSrBoardContract::FullHd, LiteSrNearFullHd=LiteSrBoardContract::NearFullHd;
constexpr int LiteSrRectFullHd=LiteSrBoardContract::RectFullHd;
constexpr int LiteSrHighResolution=LiteSrBoardContract::HighResolution;
constexpr int LiteSrNative16Depth=LiteSrBoardContract::Native16Depth;
constexpr int NativeHd=LiteSrBoardContract::NativeHd;
inline bool isCaptureOnly(int mode){return LiteSrBoardContract::isCaptureOnly(mode);}
inline bool isLiteSrHd(int mode){return LiteSrBoardContract::isHd(mode);}
inline bool isLiteSrFull(int mode){return LiteSrBoardContract::isFull(mode);}
inline bool isLiteSr(int mode){return LiteSrBoardContract::supports(mode);}
inline bool supports(int mode){return isTinyNv12(mode)||mode==LiteSrRectFullHd||mode==NativeHd;}
inline int outputWidth(int mode){return isTinyNv12(mode)?80:isLiteSr(mode)?LiteSrBoardContract::outputWidth(mode):384;}
inline int outputHeight(int mode){return isTinyNv12(mode)?128:isLiteSr(mode)?LiteSrBoardContract::outputHeight(mode):256;}
inline int freshnessMs(int mode){return (isTinyNv12(mode)||isLiteSr(mode))?LiteSrBoardContract::FreshMs:220;}
struct Settings {
    int liteSrHoles=1;
    bool liteSrOutliers=true;
};
inline Settings defaults(int =LiteSrRectFullHd){return {};}
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
