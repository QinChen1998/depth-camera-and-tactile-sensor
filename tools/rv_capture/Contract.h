#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

// Shared by the board programs, SSH worker and desktop stream receiver.
namespace RvCaptureContract {
constexpr int NativeHd=14, Native48=24, Native48HighAccuracy=25;
constexpr int FreshMs=1000;
// These LSG receipt fields are independent of the native measurement grid.
constexpr int ReceiptWidth=256, ReceiptHeight=256;
constexpr char NativeEngine[]="native-hd-capture-v1";
constexpr const char* Protocol="LSG1";
constexpr const char* PairedProtocol="LSG2";
constexpr const char* NativeCaptureProtocolV2="native-m48-m16-m8-v2";
constexpr const char* NativeHoleFillProtocol="native48-hole-fill-v1";
constexpr char CandidateRecoveryProtocol[]="clean-candidate-restart-v1";
constexpr uint32_t RequestMagic=0x3151534c, ResponseMagic=0x3147534c;
constexpr uint32_t PairedRequestMagic=0x3251534c, PairedResponseMagic=0x3247534c;
// Optional LSG2 extension. Zero request and size fields retain the raw-only layout.
constexpr int HoleFillRequestOffset=24, HoleFillSizeOffset=32;
constexpr uint32_t HoleFillRequest=1;
constexpr int Native48Columns=48, Native48Rows=32, HoleFillBytes=Native48Columns*Native48Rows*2;
inline bool supports(int mode){return mode==NativeHd||mode==Native48||mode==Native48HighAccuracy;}
inline bool supportsHoleFill(int mode){return mode==Native48||mode==Native48HighAccuracy;}
inline int columns(int mode){return mode==NativeHd?16:Native48Columns;}
inline int rows(int mode){return mode==NativeHd?16:Native48Rows;}
inline int sensorMode(int mode){return mode==NativeHd?5:mode==Native48HighAccuracy?12:11;}
inline void validate(int mode,const std::string& token,const std::string& options){
    if(!supports(mode)||token.size()!=32||token.find_first_not_of("0123456789abcdef")!=std::string::npos
       ||options!="1,1")throw std::runtime_error("Invalid board imaging session");
}
}
