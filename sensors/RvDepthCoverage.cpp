#include "RvDepthCoverage.h"
#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include <QtCore/QJsonArray>
#include <QtCore/QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RvDepthCoverage {
DepthField depthField(const QByteArray &depth,const QJsonObject &metadata){
    DepthField result;const auto model=metadata["assist_host_model"].toString();
    if(!RvCameraView::isWide(model)||metadata["width"]!=576||metadata["height"]!=324
       ||depth.size()!=576*324*4||metadata["depth_unit"]!="camera-Z_m_float32"
       ||metadata["assist_output_policy"]!=QLatin1String(RvAssistFusion::FieldOfViewPolicy)
       ||metadata["assist_camera_profile"]!="wide"||metadata["assist_output_restored_to_calibrated_grid"]!=true
       ||(metadata["assist_input_rotation_degrees"]!=0&&metadata["assist_input_rotation_degrees"]!=180))return result;
    const auto &profile=RvCameraView::profile(model);
    const bool hasNativeFov=metadata.contains("dtof_fov_correction")||metadata.contains("tof_fov_correction");
    // RGB-only predictions retain the selected geometric calibration, but no
    // current native-measurement fields. Do not turn that into a new crop or
    // guess geometry when a paired measurement's FOV is missing.
    const auto fov=hasNativeFov?(metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"])
        :metadata["assist_tof_fov_correction"];
    if((fov!=1&&fov!=2)||(!hasNativeFov&&metadata["dtof_present"]!=false)
       ||(metadata.contains("assist_tof_fov_correction")&&metadata["assist_tof_fov_correction"]!=fov)
       ||(metadata.contains("dtof_fov_correction")&&metadata.contains("tof_fov_correction")
       &&metadata["dtof_fov_correction"]!=metadata["tof_fov_correction"])
       ||metadata["rectify_map_sha256"]!=QLatin1String(profile.mapSha)
       ||metadata["assist_graph_sha256"]!=QLatin1String(profile.graphSha)
       ||metadata["depth_K"].toArray()!=RvCameraView::intrinsics(true))return result;
    const auto fit=metadata["assist_fusion"].toObject();
    if(fit["output_policy"]!=QLatin1String(RvAssistFusion::FieldOfViewPolicy)||fit["fov_geometry_only"]!=true)return result;
    int count=0,left=576,top=324,right=-1,bottom=-1;
    for(int y=0;y<324;++y)for(int x=0;x<576;++x){
        const quint32 bits=qFromLittleEndian<quint32>(depth.constData()+4*(y*576+x));float z;std::memcpy(&z,&bits,4);
        if(!std::isfinite(z)||z<0)return {};
        if(z>0){++count;left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);}
    }
    if(count==0||fit["fov_pixels"].toDouble(-1)!=count)return result;
    // RGB-driven A/G/H keep one calibrated viewport across current/held scale
    // and RGB-only output. Their positive bounding boxes move with depth and
    // parallax; fitting those boxes produces visible zoom. B keeps its crop.
    result.sourceRect=(metadata["assist_experiment"]=="A"||metadata["assist_experiment"]=="G"||metadata["assist_experiment"]=="H")
        ?QRect(0,0,576,324):QRect(QPoint(left,top),QPoint(right,bottom));return result;
}
}
