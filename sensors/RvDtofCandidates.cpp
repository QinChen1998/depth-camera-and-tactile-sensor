#include "RvDtofCandidates.h"
#include <QtCore/QJsonDocument>
namespace RvDtofCandidates {
QString name(int mode){return mode==LiteSrHighResolution?QStringLiteral("board_litesr_near_high_resolution"):mode==NativeHd?QStringLiteral("native_hd_capture"):mode==LiteSrNearFullHd?QStringLiteral("board_litesr_near_full_host_contour_hd"):mode==LiteSrFullHd?QStringLiteral("board_litesr_full_host_contour_hd"):mode==LiteSrFull?QStringLiteral("board_litesr_area_full"):mode==LiteSrContour?QStringLiteral("board_litesr_area_full_contour"):QString();}
QString identity(const QJsonObject& m){
    QJsonObject result;for(const char* key:{"generation","dtof_source","dtof_range_unit","dtof_coordinate","work_mode","model","rectify_map_sha256","boot_id","session_id","simulation","tof_filter_raw_preserved"})result[key]=m[key];
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}
QJsonObject parameters(int mode,const Settings& s){
    if(isLiteSr(mode))return {{"hole_mode",s.liteSrHoles},{"remove_outliers",s.liteSrOutliers},{"contour",mode==LiteSrContour},{"fresh_ms",LiteSrBoardContract::FreshMs}};
    if(mode!=FastDepth)return {};
    return {{"near_m",s.nearM},{"sigma_cells",s.sigma},{"current_weight",s.currentWeight},
        {"minimum_component",s.minimumComponent},{"fill_hole_cells",s.fillHoles},
        {"bezier",s.bezier},{"presentation_smoothing",s.presentation},{"depth_usage",s.presentation>0?"presentation":"current_frame_reconstruction"},{"fresh_ms",220},{"layer_m",s.metric.layerM},
        {"interpolation",s.fastInterpolation==0?"bilinear":"nearest"},
        {"hole_policy","small_enclosed_unknown_only"}};
}
}
