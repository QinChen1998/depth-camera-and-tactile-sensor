#include "RvDtofCandidates.h"
#include <QtCore/QJsonDocument>
namespace RvDtofCandidates {
QString name(int mode){return isTinyNv12(mode)?QString::fromLatin1(TinyNv12Contract::Scheme):mode==LiteSrRectFullHd?QStringLiteral("board_litesr_near_384x256_host_contour_hd"):mode==NativeHd?QStringLiteral("native_hd_capture"):QString();}
QString identity(const QJsonObject& m){
    QJsonObject result;for(const char* key:{"generation","dtof_source","dtof_range_unit","dtof_coordinate","work_mode","model","rectify_map_sha256","boot_id","session_id","simulation","tof_filter_raw_preserved"})result[key]=m[key];
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}
QJsonObject parameters(int mode,const Settings& s){
    if(isLiteSr(mode))return {{"hole_mode",s.liteSrHoles},{"remove_outliers",s.liteSrOutliers},{"contour",mode==LiteSrContour},{"fresh_ms",LiteSrBoardContract::FreshMs}};
    return {};
}
}
