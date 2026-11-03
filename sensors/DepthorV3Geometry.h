#pragma once
#include "RvAssistFusion.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

// Current standard virtual camera -> centred 4:3 model camera. Native ranges
// are projected individually through the device's GEO2 rays and extrinsics.
namespace DepthorV3 {
inline constexpr int W=640,H=480,N=W*H,RW=576,RH=324,RN=RW*RH;
inline void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
inline bool supported(const QString &name){return name=="depthor_v3_fp32"||name=="depthor_v3_fp16"||name=="depthor_v3_int8";}

inline std::vector<float> prepareRgb(const QByteArray &planar,int rotation){
    require(planar.size()==3*RN&&(rotation==0||rotation==180),"Invalid DEPTHOR RGB shape or orientation");
    std::vector<float> result(3*N);
    // Match the established OpenCV uint8 INTER_LINEAR contract, including its
    // integer coefficient rounding. Resize RGB; never resize ToF index grids.
    for(int y=0;y<H;++y){float fy=float((y+.5)*RH/H-.5);int sy=int(std::floor(fy));fy-=sy;
        int b1=int(std::nearbyint(fy*2048.f)),b0=2048-b1,y0=std::clamp(sy,0,RH-1),y1=std::clamp(sy+1,0,RH-1);
        for(int x=0;x<W;++x){float fx=float((x+.5)*432/W-.5);int sx=int(std::floor(fx));fx-=sx;
            if(sx<0){sx=0;fx=0;}if(sx>=431){sx=431;fx=0;}
            int a1=int(std::nearbyint(fx*2048.f)),a0=2048-a1,x0=sx+72,x1=std::min(sx+1,431)+72;
            const int out=rotation?N-1-y*W-x:y*W+x;
            for(int c=0;c<3;++c){const auto *p=reinterpret_cast<const unsigned char*>(planar.constData())+c*RN;
                const int h0=p[y0*RW+x0]*a0+p[y0*RW+x1]*a1,h1=p[y1*RW+x0]*a0+p[y1*RW+x1]*a1;
                const int value=((((h0>>4)*b0)>>16)+(((h1>>4)*b1)>>16)+2)>>2;
                result[c*N+out]=float(value)/255.f;}}
    }return result;
}

inline std::vector<float> project(const RvTofFrame &tof,int rotation,QJsonObject &diagnostic){
    require(tof.isValid(true)&&((tof.columns()==48&&tof.rows()==32)||(tof.columns()==8&&tof.rows()==8))&&(rotation==0||rotation==180),
            "DEPTHOR V3 requires a current native 48x32 or 8x8 measurement");
    const auto a=tof.metadata.value("dtof_fov_correction"),b=tof.metadata.value("tof_fov_correction");
    require(a.isUndefined()||b.isUndefined()||a==b,"Conflicting DEPTHOR native FOV bytes");
    const auto value=a.isUndefined()?b:a;
    require(value.isDouble()&&(value==1||value==2),"DEPTHOR requires the actual calibrated FOV byte");
    const auto &cal=RvAssistFusion::calibrationForFov(value.toInt());
    require(cal.valid,"DEPTHOR device calibration unavailable");
    const auto *rays=tof.columns()==8?RvAssistFusion::native8RaysForFov(value.toInt()).data():cal.cameraRays.data();
    require(rays[0][2]>0,"DEPTHOR native ray calibration unavailable");
    std::vector<double> nearest(N,std::numeric_limits<double>::infinity());
    int measured=0;
    for(int i=0;i<tof.sampleCount();++i){const double r=tof.rangeUq2(i)*.00025;
        if(!tof.valid(i)||tof.snr(i)<6||r<.01||r>10.)continue;
        ++measured;const auto &q=rays[i];
        const double px=r*q[0]+cal.translation[0],py=r*q[1]+cal.translation[1],z=r*q[2]+cal.translation[2];
        if(!std::isfinite(z)||z<=0)continue;
        double u=((574.*px/z+287.5)-72.+.5)*(640./432.)-.5;
        double v=(574.*py/z+161.5+.5)*(480./324.)-.5;
        if(rotation){u=639-u;v=479-v;}
        if(!std::isfinite(u)||!std::isfinite(v)||u<-.5||u>639.5||v<-.5||v>479.5)continue;
        const int x=int(std::nearbyint(u)),y=int(std::nearbyint(v));
        if(x<0||x>=W||y<0||y>=H)continue;
        nearest[y*W+x]=std::min(nearest[y*W+x],z);
    }
    std::vector<float> result(N);int count=0;
    for(int i=0;i<N;++i)if(std::isfinite(nearest[i])){result[i]=float(nearest[i]);++count;}
    diagnostic={{"native_measurements",measured},{"projected_points",count},{"actual_fov_correction",value},
        {"native_columns",tof.columns()},{"native_rows",tof.rows()},
        {"geometry","standard_crop432_to640_camera_z_v1"},{"derived_values_are_measurements",false}};
    require(count>0,"No current native dToF points project into the DEPTHOR field");
    return result;
}

inline std::vector<float> restore(const std::vector<float> &prediction,int rotation){
    require(prediction.size()==N&&(rotation==0||rotation==180),"Invalid DEPTHOR output grid");
    for(float z:prediction)require(std::isfinite(z)&&z>0,"Invalid DEPTHOR absolute camera-Z output");
    std::vector<float> out(RN,0);
    for(int y=0;y<RH;++y){const double dy=(y+.5)*(double(H)/RH)-.5;const int sy=int(std::floor(dy));
        const float fy=float(dy-sy),b0=1.f-fy,b1=fy;
        for(int x=0;x<432;++x){const double dx=(x+.5)*(double(W)/432)-.5;const int sx=int(std::floor(dx));
            const float fx=float(dx-sx),a0=1.f-fx,a1=fx;
            const auto at=[&](int xx,int yy){const int i=std::clamp(yy,0,H-1)*W+std::clamp(xx,0,W-1);return prediction[rotation?N-1-i:i];};
            const float top=at(sx,sy)*a0+at(sx+1,sy)*a1,bottom=at(sx,sy+1)*a0+at(sx+1,sy+1)*a1;
            out[y*RW+x+72]=top*b0+bottom*b1;}}
    return out;
}
}
