#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

// Capture transport only. LSG1/LSG2 and the envelope dimensions are retained
// for wire compatibility; the board never supplies a model depth image.
namespace RvCaptureContract {
constexpr int NativeHd=14, Native48=24, Native48HighAccuracy=25, Width=256, Height=256, FreshMs=1000;
constexpr const char* Protocol="LSG1";
constexpr const char* PairedProtocol="LSG2";
constexpr const char* NativeCaptureProtocolV2="native-m48-m16-m8-v2";
constexpr uint32_t RequestMagic=0x3151534c, ResponseMagic=0x3147534c;
constexpr uint32_t PairedRequestMagic=0x3251534c, PairedResponseMagic=0x3247534c;
inline bool supports(int mode){return mode==NativeHd||mode==Native48||mode==Native48HighAccuracy;}
inline int columns(int mode){return mode==NativeHd?16:48;}
inline int rows(int mode){return mode==NativeHd?16:32;}
inline int sensorMode(int mode){return mode==NativeHd?5:mode==Native48?11:12;}
inline const char* engine(int mode){return supports(mode)?"native-hd-capture-v1":"";}
inline void validate(int mode,const std::string& token,const std::string& options){
    if(!supports(mode)||token.size()!=32||token.find_first_not_of("0123456789abcdef")!=std::string::npos
       ||options!="1,1")throw std::runtime_error("Invalid native capture session");
}
}
