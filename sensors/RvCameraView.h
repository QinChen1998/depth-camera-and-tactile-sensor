#pragma once
#include "RvCalibration.h"
#include <QtCore/QString>
#include <QtCore/QJsonArray>
#include <QtCore/QSize>
#include <QtCore/QPointF>
#include <QtCore/QJsonObject>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <algorithm>
#include <vector>

// Immutable virtual cameras. Physical K/D/M/R/t and native rays are shared;
// each inference graph, rectification map and projection uses one whole profile.
namespace RvCameraView {
inline constexpr const char *UniDepthVgaNative48="unidepth_rgb672_vga_fp16_tof48";
inline constexpr const char *DepthPro2Model="depthpro2_s_480x640_fp16";
inline constexpr const char *DepthPro2VgaModel="depthpro2_s_vga_fp16";
inline constexpr const char *DepthPro2Backend="native-tensorrt-fp16";
inline constexpr const char *DepthPro2Worker="DctsDepthPro2Worker.exe";
inline constexpr const char *DepthPro2Engine="depthpro2-s-480x640-fp16.engine";
inline constexpr const char *DepthPro2ProtectedEngine="depthpro2-s-480x640-fp16.dctsm";
inline constexpr const char *DepthPro2ProtectionLibrary="DctsDepthPro2Runtime.dll";
inline constexpr const char *DepthPro2Protection="aes-256-gcm";
inline constexpr const char *DepthPro2EngineSha="9039371e303b5f348551493b6143374e27d6903a3d98a3c7c02eb64db01e3335";
inline constexpr const char *DepthPro2TensorRt="10.16.1.11";
inline constexpr const char *LegacyNative16Model="unidepth_rgb672_vga_fp16_tof16";
inline bool isUniDepthNative48(const QString &model){return model==QLatin1String(UniDepthVgaNative48);}
inline bool isUniDepthVga(const QString &model){return isUniDepthNative48(model);}
inline int nativeColumns(const QString &){return 48;}
inline int nativeRows(const QString &){return 32;}
inline int nativeCount(const QString &model){return nativeColumns(model)*nativeRows(model);}
inline int captureMode(const QString &){return 24;}
struct Profile {double focal;const char *mapSha,*sourceSha,*graphSha,*name,*conversion;
    const char *weightsSha="93705cb3295dd7476b44911b8a55f5215bf74e8d5eccd27cecdb1b338270a648";
    const char *precision="fp16-mixed-trial",*modelName="UniDepthV2-Small",*graphFile="unidepthv2-small-rgb672-vga.onnx";
    int networkWidth=672,networkHeight=504,resolutionLevel=-1;
    const char *networkProfile="rgb672_vga_crop_v1";
    double parityMeanLimit=.0025,parityMaxLimit=.01;
    bool provisional=true;
    int inputWidth=672,inputHeight=504,outputWidth=640,outputHeight=480;
    QSize inputSize() const {return {inputWidth,inputHeight};}
    QSize outputSize() const {return {outputWidth,outputHeight};}
    constexpr int rgbBytes() const {return inputWidth*inputHeight*3;}
    constexpr int depthBytes() const {return outputWidth*outputHeight*4;}
    double cx() const {return (outputWidth-1)*.5;}
    double cy() const {return (outputHeight-1)*.5;}
};
inline constexpr Profile Rgb672VgaFp16{260.*480/324,
    RvCalibration::MapSha,
    "a35dbea69611e21c866b1600569fb8a44c9df2061b505477366d020fef443b6f",
    "50d22cd33e126832793d905788a1efadeb4c9298de72cbfa634d49fdcdbc2dc1","wide","ort-fp16-rgb672-vga-head-fp32-v1",
    "93705cb3295dd7476b44911b8a55f5215bf74e8d5eccd27cecdb1b338270a648",
    "fp16-mixed-trial","UniDepthV2-Small","unidepthv2-small-rgb672-vga.onnx",672,504,-1,"rgb672_vga_crop_v1",.0025,.01,true,
    672,504,640,480};
inline constexpr QRect VgaInputRoi{16,12,640,480};
inline constexpr QRect TofPortraitRoi{156,12,360,480};
inline constexpr char DepthPro2Grid[]="calibrated_tof_portrait_v1";
inline constexpr char RecordedVgaGrid[]="calibrated_vga";
inline constexpr Profile DepthPro2Portrait{
    Rgb672VgaFp16.focal*480./TofPortraitRoi.width(),RvCalibration::MapSha,
    "9e4dffeece1a77bac996bc12fbf69267fee78de138e75b90683703472031274b",
    "9e4dffeece1a77bac996bc12fbf69267fee78de138e75b90683703472031274b",
    "wide","strongly-typed-fp16",
    "583c4d630779fd231f969c738733624963ad34bd3c29f9c64088257e3436f97c",
    "fp16","Depth Pro 2-S",DepthPro2Engine,480,640,-1,DepthPro2Grid,0.,0.,false,
    480,640,480,640};
// Retired geometry and engine identity are needed to open existing recordings.
inline constexpr Profile DepthPro2Vga{
    Rgb672VgaFp16.focal,RvCalibration::MapSha,
    "5ccf784840a8d6a8a4360eb8b2bb9f26f887110828d3b3e5fdac26eb0678a393",
    "5ccf784840a8d6a8a4360eb8b2bb9f26f887110828d3b3e5fdac26eb0678a393",
    "wide","strongly-typed-fp16",
    "583c4d630779fd231f969c738733624963ad34bd3c29f9c64088257e3436f97c",
    "fp16","Depth Pro 2-S","depthpro2-s-vga-fp16.engine",640,480,-1,RecordedVgaGrid,0.,0.,false,
    640,480,640,480};
struct DepthPro2Runtime {
    const Profile &view;
    const char *engineSha,*directory,*overrideKey;
};
inline constexpr DepthPro2Runtime DepthPro2SRuntime{
    DepthPro2Portrait,DepthPro2EngineSha,"DepthPro2Runtime","RV_DEPTHPRO2_ROOT"};
inline constexpr DepthPro2Runtime RecordedDepthPro2Vga{
    DepthPro2Vga,"cd01ebaa9f8376c4c71788cba583e1627eba8fa62c42c4e0ab2e94a24b04bc2c",nullptr,nullptr};
inline const DepthPro2Runtime *depthPro2Runtime(const QString &model){
    return model==QLatin1String(DepthPro2Model)?&DepthPro2SRuntime:nullptr;
}
inline const DepthPro2Runtime *recordedDepthPro2Runtime(const QString &model){
    return model==QLatin1String(DepthPro2VgaModel)?&RecordedDepthPro2Vga:depthPro2Runtime(model);
}
inline bool isDepthPro2(const QString &model){return recordedDepthPro2Runtime(model)!=nullptr;}
inline bool isUniDepth(const QString &model){return isUniDepthVga(model);}
inline bool isHostDepth(const QString &model){return isUniDepthVga(model)||isDepthPro2(model);}
inline bool isWide(const QString &model){return isUniDepthVga(model);}
inline bool supportsNativeGrid(const QString &model,int columns,int rows){
    return isHostDepth(model)&&columns==nativeColumns(model)&&rows==nativeRows(model);
}
inline const Profile &profile(const QString &model){
    const auto *runtime=recordedDepthPro2Runtime(model);
    return runtime?runtime->view:Rgb672VgaFp16;
}
// The graph remains 640x480. Its central 360x480 camera is resampled with
// square pixels to the portrait ToF/RGB overlap, without rotating the sensor.
inline constexpr char PortraitGrid[]="tof_portrait_crop_v1";
inline constexpr int PortraitCropX=140,PortraitCropY=0,PortraitCropWidth=360,PortraitCropHeight=480;
inline constexpr Profile Portrait=[] {auto p=Rgb672VgaFp16;p.focal*=4./3.;p.outputWidth=480;p.outputHeight=640;return p;}();
inline const Profile &outputProfile(const QString &model){
    const auto *runtime=recordedDepthPro2Runtime(model);
    return runtime?runtime->view:Portrait;
}
inline bool isPortrait(const QJsonObject &metadata){return metadata["assist_output_grid"]==QLatin1String(PortraitGrid);}
inline bool validOutputGrid(const QJsonObject &m){
    if(const auto *runtime=recordedDepthPro2Runtime(m["assist_host_model"].toString())){
        const auto &view=runtime->view;
        return m["assist_output_grid"]==QLatin1String(view.networkProfile)
            &&m["width"]==view.outputWidth&&m["height"]==view.outputHeight
            &&m["depth_stride"]==view.outputWidth*int(sizeof(float));
    }
    return isPortrait(m)&&m["assist_model_depth_size"].toArray()==QJsonArray{Rgb672VgaFp16.outputWidth,Rgb672VgaFp16.outputHeight}
        &&m["assist_model_crop_xywh"].toArray()==QJsonArray{PortraitCropX,PortraitCropY,PortraitCropWidth,PortraitCropHeight}
        &&m["width"]==Portrait.outputWidth&&m["height"]==Portrait.outputHeight;
}
inline QPointF inputPixel(QPointF output,const QSize &size){
    if(size==Portrait.outputSize())return {156+(output.x()+.5)*.75-.5,12+(output.y()+.5)*.75-.5};
    return {output.x()+16,output.y()+12};
}
inline QImage pairedImage(const QImage &input,const QJsonObject &metadata){
    if(const auto *runtime=recordedDepthPro2Runtime(metadata["assist_host_model"].toString()))
        return input.size()==runtime->view.inputSize()?input:QImage();
    return isPortrait(metadata)?input.copy(16+PortraitCropX,12+PortraitCropY,PortraitCropWidth,PortraitCropHeight)
        .scaled(Portrait.outputSize(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation):input.copy(16,12,640,480);
}
inline std::vector<float> portraitDepth(const std::vector<float> &modelDepth){
    if(modelDepth.size()!=640*480)return {};
    std::vector<float> out(480*640);
    for(int y=0;y<640;++y)for(int x=0;x<480;++x){
        const double u=std::clamp(140+(x+.5)*.75-.5,140.,499.);
        const double v=std::clamp((y+.5)*.75-.5,0.,479.);
        const int ix=int(u),iy=int(v),jx=std::min(ix+1,499),jy=std::min(iy+1,479);
        const double a=u-ix,b=v-iy;
        out[y*480+x]=float((1-b)*((1-a)*modelDepth[iy*640+ix]+a*modelDepth[iy*640+jx])
            +b*((1-a)*modelDepth[jy*640+ix]+a*modelDepth[jy*640+jx]));
    }
    return out;
}
inline QJsonArray intrinsics(const Profile &view){return {view.focal,view.focal,view.cx(),view.cy()};}
inline QJsonObject depthPro2Identity(const QString &model,const DepthPro2Runtime &runtime){
    const auto &view=runtime.view;
    return {{"graph_sha256",view.graphSha},{"source_graph_sha256",view.sourceSha},
        {"weights_sha256",view.weightsSha},{"precision",view.precision},
        {"conversion",view.conversion},{"host_model",model},
        {"engine_sha256",runtime.engineSha}};
}
}
