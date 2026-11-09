#pragma once
#include "RvCalibration.h"
#include <QtCore/QString>
#include <QtCore/QJsonArray>
#include <QtCore/QSize>

// Immutable virtual cameras. Physical K/D/M/R/t and native rays are shared;
// each inference graph, rectification map and projection uses one whole profile.
namespace RvCameraView {
inline constexpr const char *UniDepthVgaNative16="unidepth_rgb672_vga_fp16_tof16";
inline bool isUniDepthVga(const QString &model){return model==QLatin1String(UniDepthVgaNative16);}
inline bool isUniDepthNative16(const QString &model){return isUniDepthVga(model);}
inline QString runtimeModel(const QString &model){return isUniDepthVga(model)?model:QString();}
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
inline bool isUniDepth(const QString &model){return isUniDepthNative16(model);}
inline bool isHostDepth(const QString &model){return isUniDepthNative16(model);}
inline bool isWide(const QString &model){return isUniDepthNative16(model);}
inline bool supportsNativeGrid(const QString &model,int columns,int rows){
    return isUniDepthNative16(model)&&columns==16&&rows==16;
}
inline const Profile &profile(const QString &model){
    return Rgb672VgaFp16;
}
inline QJsonArray intrinsics(const Profile &view){return {view.focal,view.focal,view.cx(),view.cy()};}
}
