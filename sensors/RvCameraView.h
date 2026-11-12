#pragma once
#include "RvCalibration.h"
#include <QtCore/QString>
#include <QtCore/QJsonArray>
#include <QtCore/QSize>
#include <QtCore/QPointF>
#include <QtCore/QJsonObject>
#include <QtGui/QImage>
#include <algorithm>
#include <vector>

// Immutable virtual cameras. Physical K/D/M/R/t and native rays are shared;
// each inference graph, rectification map and projection uses one whole profile.
namespace RvCameraView {
inline constexpr const char *UniDepthVgaNative48="unidepth_rgb672_vga_fp16_tof48";
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
    int rgbBytes() const {return inputWidth*inputHeight*3;}
    int depthBytes() const {return outputWidth*outputHeight*4;}
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
inline bool isUniDepth(const QString &model){return isUniDepthVga(model);}
inline bool isHostDepth(const QString &model){return isUniDepthVga(model);}
inline bool isWide(const QString &model){return isUniDepthVga(model);}
inline bool supportsNativeGrid(const QString &model,int columns,int rows){
    return isUniDepthVga(model)&&columns==nativeColumns(model)&&rows==nativeRows(model);
}
inline const Profile &profile(const QString &model){
    return Rgb672VgaFp16;
}
// The graph remains 640x480. Its central 360x480 camera is resampled with
// square pixels to the portrait ToF/RGB overlap, without rotating the sensor.
inline constexpr char PortraitGrid[]="tof_portrait_crop_v1";
inline constexpr int PortraitCropX=140,PortraitCropY=0,PortraitCropWidth=360,PortraitCropHeight=480;
inline constexpr Profile Portrait=[] {auto p=Rgb672VgaFp16;p.focal*=4./3.;p.outputWidth=480;p.outputHeight=640;return p;}();
inline const Profile &outputProfile(const QString &model){return Portrait;}
inline bool isPortrait(const QJsonObject &metadata){return metadata["assist_output_grid"]==QLatin1String(PortraitGrid);}
inline bool validOutputGrid(const QJsonObject &m){
    return isPortrait(m)&&m["assist_model_depth_size"].toArray()==QJsonArray{640,480}
        &&m["assist_model_crop_xywh"].toArray()==QJsonArray{140,0,360,480}
        &&m["width"]==480&&m["height"]==640;
}
inline QPointF inputPixel(QPointF output,const QSize &size){
    if(size==Portrait.outputSize())return {156+(output.x()+.5)*.75-.5,12+(output.y()+.5)*.75-.5};
    return {output.x()+16,output.y()+12};
}
inline QImage pairedImage(const QImage &input,const QJsonObject &metadata){
    return isPortrait(metadata)?input.copy(156,12,360,480).scaled(480,640,Qt::IgnoreAspectRatio,Qt::SmoothTransformation)
        :input.copy(16,12,640,480);
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
}
