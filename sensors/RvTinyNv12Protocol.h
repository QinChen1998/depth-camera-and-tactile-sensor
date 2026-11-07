#pragma once
#include "RvDtofCandidates.h"
#include "RvPhotonFrame.h"
#include "../tools/tiny_nv12_board/Receipt.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace RvTinyNv12 {
struct Identity { quint32 output=0,tof=0,rgb=0; quint64 tofStamp=0,rgbStamp=0,ready=0; };
struct Frame {
    RvDtofCandidates::Result result;
    RvTofFrame native;
    RvPhotonFrame photon;
    QImage camera;
    QJsonObject cameraMetadata;
    Identity identity;
    double rawAge=0,rgbAge=0;
};
inline void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
inline quint16 u16(const char*p){return qFromLittleEndian<quint16>(p);}
inline quint32 u32(const char*p){return qFromLittleEndian<quint32>(p);}
inline quint64 u64(const char*p){return qFromLittleEndian<quint64>(p);}
inline double f64(const char*p){const auto bits=u64(p);double v;std::memcpy(&v,&bits,8);return v;}
inline float f32(const char*p){const auto bits=u32(p);float v;std::memcpy(&v,&bits,4);return v;}
inline Frame decode(const QByteArray& packet,const QJsonObject& receipt,Identity previous,quint64 boardNow){
    using namespace TinyNv12Contract;
    require(tinyNv12Receipt(receipt,receipt["generation"].toString(),receipt["options"].toString()),"Tiny NV12 receipt identity");
    require(packet.size()==HeaderBytes+PayloadBytes,"Tiny NV12 packet length");
    const auto*p=packet.constData();
    require(u32(p)==0x32594e54&&u32(p+4)==6&&u32(p+8)==PayloadBytes&&u32(p+12)==0&&u32(p+44)==(80u|(128u<<16)),"Tiny NV12 wire contract");
    Frame f;auto&i=f.identity;i.output=u32(p+16);i.tof=u32(p+20);i.rgb=u32(p+24);
    i.tofStamp=u64(p+48);i.rgbStamp=u64(p+56);const auto started=u64(p+64);i.ready=u64(p+72);const auto sent=u64(p+80);
    require(i.output>previous.output&&i.tof>previous.tof&&i.tofStamp>previous.tofStamp&&i.ready>previous.ready,"Tiny NV12 repeated/regressed depth");
    require(i.rgb>=previous.rgb&&i.rgbStamp>=previous.rgbStamp&&(!previous.output||((i.rgb==previous.rgb)==(i.rgbStamp==previous.rgbStamp))),"Tiny NV12 RGB identity");
    require(i.tofStamp&&i.rgbStamp&&started>=i.tofStamp&&i.ready>=started&&sent>=i.ready&&boardNow>=sent,"Tiny NV12 clock order");
    require(std::abs(qint64(i.rgbStamp)-qint64(i.tofStamp))<=50000000,"Tiny NV12 unpaired input");
    for(int n=0;n<22;++n)require(std::isfinite(f64(p+88+8*n))&&f64(p+88+8*n)>=0,"Tiny NV12 invalid timing");
    require(f64(p+96)==0&&f64(p+112)>0,"Tiny NV12 inference timing");
    const auto*depth=p+HeaderBytes;const auto*rgb=depth+Pixels*4;const auto*t=rgb+Pixels*3/2;
    const unsigned warning=u32(t+36),status=quint8(t[7920]);
    require(u32(t)==0x39464d54&&u16(t+4)==5&&u16(t+6)==44&&u32(t+8)==10995
        &&u32(t+12)==i.tof&&u64(t+16)==i.tofStamp&&u16(t+28)==16&&u16(t+30)==16&&quint8(t[32])==1&&t[33]==0,"Tiny NV12 native packet identity");
    require((u16(t+34)&1)&&(warning==0||warning==8||warning==16)&&status==(1|warning)
        &&quint8(t[7918])==status&&(t[7919]==0||quint8(t[7919])==status)&&t[7921]==0&&t[7916]==1&&t[10994]==1,"Tiny NV12 native status/FOV");
    f.rawAge=double(boardNow-i.tofStamp)/1e6;f.rgbAge=double(boardNow-i.rgbStamp)/1e6;
    auto&m=f.result.metadata;m=receipt;
    m["model"]=Scheme;m["work_mode"]="dtof_candidate";m["frame_id"]=qint64(i.output);
    m["tof_sequence"]=qint64(i.tof);m["tof_timestamp_ns"]=qint64(i.tofStamp);m["rgb_sequence"]=qint64(i.rgb);m["rgb_timestamp_ns"]=qint64(i.rgbStamp);
    m["tof_reused"]=false;m["rgb_reused"]=previous.output&&i.rgb==previous.rgb;m["pair_delta_ms"]=double(qint64(i.rgbStamp)-qint64(i.tofStamp))/1e6;
    m["candidate_age_ms"]=f.rawAge;m["compute_ms"]=double(i.ready-started)/1e6;m["board_compute_ms"]=m["compute_ms"];
    m["npu_ms"]=f64(p+112);m["geometry_ms"]=f64(p+88);m["pack_ms"]=f64(p+104);m["hole_ms"]=f64(p+232);m["hole_filled"]=f64(p+240);
    m["publish_count"]=double(i.output);m["publish_monotonic_ms"]=double(i.ready)/1e6;m["send_dropped"]=int(u32(p+40));
    m["depth_native_warning_flags"]=int(warning);m["tof_warning_flags"]=int(warning);m["dtof_fov_correction"]=1;
    m["depth_format"]="float32_le";m["depth_coordinate"]="camera-Z";m["depth_location"]="rv1103";m["compute_location"]="rv1103";
    m["host_contour_enabled"]=false;m["simulation"]=false;m["depth_present"]=true;m["tof_filter_raw_preserved"]=true;
    m["coverage_semantics"]="model_output_validity_including_derived_hole_predictions";m["native_measurement_preserved"]=true;
    m["photon_data"]=true;m["input_columns"]=16;m["input_rows"]=16;
    f.result.depth.resize(Pixels*4);f.result.coverage.resize(Pixels);
    for(int n=0;n<Pixels;++n){
        float z=f32(depth+4*n);require(std::isnan(z)||(std::isfinite(z)&&z>=.01f&&z<=3.f),"Tiny NV12 invalid camera-Z value");
        const bool valid=std::isfinite(z);if(!valid)z=0;
        quint32 bits;std::memcpy(&bits,&z,4);qToLittleEndian(bits,f.result.depth.data()+4*n);
        // RvDepthView consumes byte opacity, not a boolean support flag.
        f.result.coverage[n]=valid?char(255):char(0);
    }
    auto&native=f.native;native.samples.resize(1024);native.metadata=m;auto&nm=native.metadata;
    nm["frame_id"]=qint64(i.tof);nm["dtof_width"]=16;nm["dtof_height"]=16;nm["dtof_bytes"]=1024;
    nm["dtof_format"]="range_uq2_u16le_snr_u8_valid_u8";nm["dtof_range_unit"]="0.25_mm";nm["dtof_source"]="tmf8829_ipc_first_peak";nm["dtof_coordinate"]="sensor_radial_native";
    for(int n=0;n<256;++n){qToLittleEndian(u16(t+3308+2*n),native.samples.data()+4*n);native.samples[4*n+2]=t[6380+n];native.samples[4*n+3]=(quint8(t[44+n/8])>>(n%8))&1;}
    require(native.isValid(),"Tiny NV12 native frame contract");
    f.photon.tof=native;f.photon.signalWords=QByteArray(t+7922,512);f.photon.resultFormat=9;f.photon.fovCorrection=1;f.photon.bootId=receipt["boot_id"].toString();f.photon.ageMs=f.rawAge;
    // Raw NV12 remains in physical camera orientation. The shared session applies
    // its verified 180-degree mount exactly once; depth already uses that frame.
    f.camera=QImage(Width,Height,QImage::Format_RGB32);
    for(int y=0;y<Height;++y){auto*row=reinterpret_cast<QRgb*>(f.camera.scanLine(y));for(int x=0;x<Width;++x){
        const int uv=Pixels+(y/2)*Width+(x/2)*2;const double yy=quint8(rgb[y*Width+x]),u=quint8(rgb[uv])-128.,v=quint8(rgb[uv+1])-128.;
        const auto c=[](double value){return std::clamp(int(std::lround(value)),0,255);};
        row[x]=qRgb(c(yy+1.402*v),c(yy-.344136*u-.714136*v),c(yy+1.772*u));
    }}
    f.cameraMetadata=m;f.cameraMetadata["rgb_age_ms"]=f.rgbAge;
    f.result.fresh=f.rawAge<1000;return f;
}
}
