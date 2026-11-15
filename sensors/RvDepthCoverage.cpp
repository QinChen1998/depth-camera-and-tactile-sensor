#include "RvDepthCoverage.h"
#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include "RvInferenceRuntime.h"
#include <QtCore/QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RvDepthCoverage {
namespace {
bool fieldIdentity(const QByteArray &depth,const QJsonObject &metadata){
    const auto model=metadata["assist_host_model"].toString();
    const auto &profile=RvCameraView::outputProfile(model);
    const int width=profile.outputWidth,height=profile.outputHeight;
    if(!RvCameraView::isWide(model)||!RvCameraView::validOutputGrid(metadata)||metadata["width"]!=width||metadata["height"]!=height
       ||depth.size()!=profile.depthBytes()||metadata["depth_unit"]!="camera-Z_m_float32"
       ||metadata["assist_output_policy"]!=QLatin1String(RvAssistFusion::FieldOfViewPolicy)
       ||metadata["assist_camera_profile"]!="wide"||metadata["assist_output_restored_to_calibrated_grid"]!=true
       ||(metadata["assist_input_rotation_degrees"]!=0&&metadata["assist_input_rotation_degrees"]!=180))return false;
    const bool hasNativeFov=metadata.contains("dtof_fov_correction")||metadata.contains("tof_fov_correction");
    // RGB-only predictions retain the selected geometric calibration, but no
    // current native-measurement fields. Do not turn that into a new crop or
    // guess geometry when a paired measurement's FOV is missing.
    const auto fov=hasNativeFov?(metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"])
        :metadata["assist_tof_fov_correction"];
    if((fov!=1&&fov!=2&&fov!=6)||(!hasNativeFov&&metadata["dtof_present"]!=false)
       ||(metadata.contains("assist_tof_fov_correction")&&metadata["assist_tof_fov_correction"]!=fov)
       ||(metadata.contains("dtof_fov_correction")&&metadata.contains("tof_fov_correction")
       &&metadata["dtof_fov_correction"]!=metadata["tof_fov_correction"])
       ||metadata["rectify_map_sha256"]!=QLatin1String(profile.mapSha)
       ||!RvInferenceRuntime::validGraphMetadata(metadata)
       ||metadata["depth_K"].toArray()!=RvCameraView::intrinsics(profile))return false;
    const auto fit=metadata["assist_fusion"].toObject();
    return fit["output_policy"]==QLatin1String(RvAssistFusion::FieldOfViewPolicy)&&fit["fov_geometry_only"]==true;
}
DepthField scan(const QByteArray &depth,const QJsonObject &metadata,std::vector<float> *decoded,bool &finite){
    finite=false;DepthField result;
    const bool admitted=fieldIdentity(depth,metadata);
    if(!decoded&&!admitted)return result;
    const int width=metadata["width"].toInt(),height=metadata["height"].toInt();
    if(width<=0||height<=0||qint64(width)*height*4!=depth.size())return result;
    if(decoded)decoded->resize(size_t(width)*height);
    auto *values=decoded?decoded->data():nullptr;
    bool nonnegative=true;
    int count=0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const quint32 bits=qFromLittleEndian<quint32>(depth.constData()+4*(y*width+x));float z;std::memcpy(&z,&bits,4);
        if(!std::isfinite(z)){if(decoded)decoded->clear();return {};}
        if(values)values[y*width+x]=z;
        if(admitted){
            nonnegative&=z>=0;
            if(z>0)++count;
        }
    }
    finite=true;
    if(!admitted||!nonnegative||count==0||metadata["assist_fusion"].toObject()["fov_pixels"].toDouble(-1)!=count)return result;
    // Both schemes keep the full calibrated viewport across current and held scale.
    result.sourceRect=QRect(0,0,width,height);
    return result;
}
}
DepthField depthField(const QByteArray &depth,const QJsonObject &metadata){bool finite=false;return scan(depth,metadata,nullptr,finite);}
bool decodeDepth(const QByteArray &depth,const QJsonObject &metadata,std::vector<float> &values,DepthField &field){
    bool finite=false;field=scan(depth,metadata,&values,finite);return finite;
}
}
