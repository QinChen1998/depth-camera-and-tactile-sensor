#include "RvDepthCoverage.h"
#include "RvAssistFusion.h"
#include "RvCameraView.h"
#include "RvInferenceRuntime.h"
#include <QtCore/QtEndian>
#include <cmath>
#include <cstring>

namespace RvDepthCoverage {
namespace {
bool fieldIdentity(const QJsonObject &metadata){
    const auto model=metadata["assist_host_model"].toString();
    const auto &profile=RvCameraView::Portrait;
    if(!RvCameraView::validOutputGrid(metadata)||!RvCameraView::isWide(model)
       ||metadata["depth_unit"]!="camera-Z_m_float32"
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
}

bool decodeDepth(const QByteArray &depth,const QJsonObject &metadata,
                 std::vector<float> &values,DepthField &field){
    field={};
    values.clear();
    const int width=metadata["width"].toInt(),height=metadata["height"].toInt();
    constexpr qsizetype bytesPerPixel=sizeof(float);
    const auto pixels=depth.size()/bytesPerPixel;
    if(width<=0||height<=0||qint64(width)*height!=pixels||depth.size()%bytesPerPixel!=0)return false;

    values.resize(pixels);
    const auto *data=depth.constData();
    qsizetype positive=0;
    bool nonnegative=true;
    for(qsizetype i=0;i<pixels;++i){
        const quint32 bits=qFromLittleEndian<quint32>(data+i*bytesPerPixel);
        float z;
        std::memcpy(&z,&bits,bytesPerPixel);
        if(!std::isfinite(z)){values.clear();return false;}
        values[i]=z;
        nonnegative&=z>=0;
        positive+=z>0;
    }
    // Keep the calibrated viewport fixed as the parallax mask moves with depth.
    if(nonnegative&&positive>0&&fieldIdentity(metadata)
       &&metadata["assist_fusion"].toObject()["fov_pixels"].toDouble(-1)==double(positive))
        field.sourceRect=QRect(0,0,width,height);
    return true;
}
}
