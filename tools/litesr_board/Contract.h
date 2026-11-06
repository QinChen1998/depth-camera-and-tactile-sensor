#pragma once
#include "../tiny_nv12_board/Contract.h"
#include <stdexcept>
#include <string>
#include <cstdint>

// Shared by the native desktop receiver, SSH worker and RV1103 runner.
namespace LiteSrBoardContract {
constexpr int Full=10, Contour=11, FullHd=12, NearFullHd=13, NativeHd=14, HighResolution=15, RectFullHd=16, Native16Depth=17, Width=256, Height=256, Pixels=Width*Height;
constexpr int FreshMs=1000;
constexpr const char* Protocol="LSG1";
constexpr uint32_t RequestMagic=0x3151534c, ResponseMagic=0x3147534c;
// Opt-in response with the exact native measurement for this depth:
// 6144 bytes for 48x32, 1024 bytes for Native16.
// LSG1 clients retain the original header and independent display streams.
constexpr const char* PairedProtocol="LSG2";
constexpr const char* NativeCaptureProtocol="native-m48-m8-v1";
constexpr const char* NativeCaptureProtocolV2="native-m48-m16-m8-v2";
constexpr uint32_t PairedRequestMagic=0x3251534c, PairedResponseMagic=0x3247534c;
constexpr const char* NearCheckpointSha="b9896dd1ef172e4393d32fa191ad2b0d3c941bb6f7441de77992dcd16dfd226d";
constexpr const char* RectEngine="litesr-near-official10-384x256-int8-normal-v1";
constexpr const char* RectModelSha="cccfdfa4c0f48796aacd81ec308cbddb50511e1e9edc4cb16807655096ca4784";
inline bool isNative16Depth(int mode){return false;}
inline bool isRectangular(int mode){return mode==RectFullHd;}
inline bool isHighResolution(int mode){return false;}
inline int outputSide(int mode){return 256;}
inline int outputWidth(int mode){return isRectangular(mode)?384:outputSide(mode);}
inline int outputHeight(int mode){return outputSide(mode);}
inline bool isNear(int mode){return isRectangular(mode);}
inline bool isCaptureOnly(int mode){return mode==NativeHd;}
inline bool isHd(int mode){return mode==NativeHd;}
inline const char* engine(int mode){return mode==RectFullHd?RectEngine:mode==NativeHd?"native-hd-capture-v1":"";}
inline const char* modelSha(int mode){return mode==RectFullHd?RectModelSha:"";}
inline bool isFull(int mode){return mode==RectFullHd||mode==NativeHd;}
inline bool supports(int mode){return mode==RectFullHd||mode==NativeHd;}
inline bool isDepthOnly(int mode){return mode==RectFullHd;}
struct Options {
    int holes=1; bool outliers=true;
    explicit Options(const std::string& csv){
        if(csv.size()!=3||csv[0]<'0'||csv[0]>'2'||csv[1]!=','||(csv[2]!='0'&&csv[2]!='1'))
            throw std::runtime_error("Invalid LiteSR repair options");
        holes=csv[0]-'0';outliers=csv[2]=='1';
    }
};
inline void validate(int mode,const std::string& token,const std::string& csv){
    if(!supports(mode)||token.size()!=32||token.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::runtime_error("Invalid LiteSR session identity");
    Options checked(csv);
}
}
