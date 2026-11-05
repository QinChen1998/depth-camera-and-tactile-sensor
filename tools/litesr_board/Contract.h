#pragma once
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
constexpr const char* Engine="litesr117-int8-v1";
constexpr const char* ModelSha="156791e6dab758f3fd4bd66a64466208dfd06e1da12efeeba104e0a9bfda8828";
constexpr const char* NearEngine="litesr-near-official10-int8-v1";
constexpr const char* NearModelSha="a916354dce57bd4cf4559cde91270cbd50bb2b14302ed277e8653e02b525268b";
constexpr const char* NearCheckpointSha="b9896dd1ef172e4393d32fa191ad2b0d3c941bb6f7441de77992dcd16dfd226d";
constexpr const char* RectEngine="litesr-near-official10-384x256-int8-normal-v1";
constexpr const char* RectModelSha="cccfdfa4c0f48796aacd81ec308cbddb50511e1e9edc4cb16807655096ca4784";
constexpr const char* Native16Engine="litesr-near-official10-native16-128x128-int8-v1";
constexpr const char* Native16ModelSha="b4e983eef4928bbcdd2dc1fe308681fa0f8eda86dc0e38e2dc6662e3496730bf";
inline bool isNative16Depth(int mode){return mode==Native16Depth;}
inline bool isRectangular(int mode){return mode==RectFullHd;}
inline bool isHighResolution(int mode){return mode==HighResolution;}
inline int outputSide(int mode){return isNative16Depth(mode)?128:isHighResolution(mode)?512:256;}
inline int outputWidth(int mode){return isRectangular(mode)?384:outputSide(mode);}
inline int outputHeight(int mode){return outputSide(mode);}
inline bool isNear(int mode){return isNative16Depth(mode)||mode==NearFullHd||isHighResolution(mode)||isRectangular(mode);}
inline bool isCaptureOnly(int mode){return mode==NativeHd;}
inline bool isHd(int mode){return mode==FullHd||mode==NearFullHd||mode==NativeHd||isRectangular(mode);}
constexpr const char* HighEngine="litesr-near-official10-512-int8-normal-v1";
constexpr const char* HighModelSha="c37785c5e4df3b84e837d0edb52d6c6cdf0d84a1f0ce8dd263328c4eca53dd29";
inline const char* engine(int mode){return isNative16Depth(mode)?Native16Engine:isRectangular(mode)?RectEngine:isHighResolution(mode)?HighEngine:isCaptureOnly(mode)?"native-hd-capture-v1":isNear(mode)?NearEngine:Engine;}
inline const char* modelSha(int mode){return isNative16Depth(mode)?Native16ModelSha:isRectangular(mode)?RectModelSha:isHighResolution(mode)?HighModelSha:isCaptureOnly(mode)?"":isNear(mode)?NearModelSha:ModelSha;}
inline bool isFull(int mode){return isNative16Depth(mode)||mode==Full||isHd(mode)||isHighResolution(mode);}
inline bool supports(int mode){return isFull(mode)||mode==Contour;}
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
