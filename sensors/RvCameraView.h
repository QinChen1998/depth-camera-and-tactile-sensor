#pragma once
#include <QtCore/QString>
#include <QtCore/QJsonArray>

// Two immutable virtual cameras. Physical K/D/M/R/t and native rays are shared;
// each inference graph, rectification map and projection uses one whole profile.
namespace RvCameraView {
inline constexpr const char *UniDepthFp16Native16="unidepth_wide_576_fp16_tof16";
inline bool isUniDepthNative16(const QString &model){return model==QLatin1String(UniDepthFp16Native16);}
inline QString runtimeModel(const QString &model){return isUniDepthNative16(model)?QStringLiteral("unidepth_wide_576_fp16"):model;}
struct Profile {double focal;const char *mapSha,*sourceSha,*graphSha,*name,*conversion;
    const char *weightsSha="93705cb3295dd7476b44911b8a55f5215bf74e8d5eccd27cecdb1b338270a648";
    const char *precision="fp16-mixed",*modelName="UniDepthV2-Small",*graphFile="unidepthv2-small-rv576.onnx";
    int networkWidth=1008,networkHeight=574,resolutionLevel=9;
    const char *networkProfile=nullptr;
    double parityMeanLimit=.001,parityMaxLimit=.01;
    bool provisional=false;};
inline constexpr Profile Standard{574.,
    "00de89f4e6d95f09758b0d8274c2694168a27b5f601bdfeda667b0b179126b5d",
    "46488b81c0fbf75c6277fd9d4da5d29bbf7dcd6f499a67a87ff7e7aa33594368",
    "5b2df8c511708e890d5922ed31fd552aa2526e4a82a76173b3fe0c9faa5f328b","standard","ort-fp16-cast-fixed-v1"};
inline constexpr Profile Wide{260.,
    "0317f36006702df342d78a0ff19ed8250d58b91424b4b09f11327d81f31c6191",
    "63941069172b0095fac8a70b1c387c02c2dca63f3b376906b2a0bd15a1e46c7e",
    "1ce04189bf527a6dc016ff9af4f61776d4a03c887a26301187ea25ecda87234a","wide","ort-fp16-cast-fixed-head-fp32-v1"};
inline constexpr Profile WideRgb576{260.,
    "0317f36006702df342d78a0ff19ed8250d58b91424b4b09f11327d81f31c6191",
    "729f64664bd2502df1a5e97f93724513df24c516c7538aa1e4ba04bdf378d281",
    "729f64664bd2502df1a5e97f93724513df24c516c7538aa1e4ba04bdf378d281","wide","ort-fp32-rgb576-pad6-tf32-v1",
    "93705cb3295dd7476b44911b8a55f5215bf74e8d5eccd27cecdb1b338270a648",
    "fp32-tf32","UniDepthV2-Small","unidepthv2-small-rv576.onnx",588,336,-1,"rgb576_pad6_v1"};
inline constexpr Profile WideRgb576Fp16Trial{260.,
    "0317f36006702df342d78a0ff19ed8250d58b91424b4b09f11327d81f31c6191",
    "729f64664bd2502df1a5e97f93724513df24c516c7538aa1e4ba04bdf378d281",
    "2912174428afd1b5b6ceb503b716d58166184529909cb28b9a83f1d60cf686ca","wide","ort-fp16-rgb576-pad6-head-fp32-v1",
    "93705cb3295dd7476b44911b8a55f5215bf74e8d5eccd27cecdb1b338270a648",
    "fp16-mixed-trial","UniDepthV2-Small","unidepthv2-small-rv576.onnx",588,336,-1,"rgb576_pad6_v1",.0025,.01,true};
inline constexpr Profile ColorRgb{574., Standard.mapSha,
    "06069e977d45604e82f0acee1bba5bcc63bb0b82f6b9f39fb94486532730bd5c", "0f93f909f7cd64973a97016e607fcb5154f90356b3af8183bf1fcc903547911c", "standard", "litesr-color-host-v1",
    "06069e977d45604e82f0acee1bba5bcc63bb0b82f6b9f39fb94486532730bd5c", "fp32", "LiteSR-RGB", "litesr_rgb_raw_v2.onnx"};
inline constexpr Profile ColorNv12{574., Standard.mapSha,
    "42def7790b9b46712a907b6804ed9db6fc98fba19057e2a4221ebfe5ba2aea09", "b51fb4a0e599c138bf558e3783a520ef4a59157ba53a2fa7757ceb56dcc384c8", "standard", "litesr-color-host-v1",
    "42def7790b9b46712a907b6804ed9db6fc98fba19057e2a4221ebfe5ba2aea09", "fp32", "LiteSR-NV12", "litesr_nv12_raw_v2.onnx"};
inline constexpr Profile DepthorV3Fp32{574., Standard.mapSha,
    "ae8e42c8afeca46db6befba96072bcbc6179acac4084f694585e5d106655b08d", "252a53af4e3bcd39e0407a09b39181d0c30b3ce837d6bd16a794871004a5e76a", "standard", "depthor-v3-fp32-cspn-fused-cudagraph-v1",
    "8116f4f1120578e2f339ac91aa350645877ca34579eba9332dd32157c5ddfb7a", "fp32", "DEPTHOR Small V3", "depthor_v3.onnx",640,480,-1};
inline constexpr Profile DepthorV3Fp16{574., Standard.mapSha,
    "ae8e42c8afeca46db6befba96072bcbc6179acac4084f694585e5d106655b08d", "a197ba499148393a4a14ff29e4c6139f1fb8d73292867cdcacd4a101ccb1e44a", "standard", "depthor-v3-fp16-head-cspn-fp32-v1",
    "8116f4f1120578e2f339ac91aa350645877ca34579eba9332dd32157c5ddfb7a", "fp16-mixed", "DEPTHOR Small V3", "depthor_v3.onnx",640,480,-1};
inline constexpr Profile DepthorV3Int8{574., Standard.mapSha,
    "ae8e42c8afeca46db6befba96072bcbc6179acac4084f694585e5d106655b08d", "b7d3a0195c9f91c7ee77cd70001359c9c6eb41858ea2870c0115ced88394d3ae", "standard", "depthor-v3-s8s8-qdq-train32-v1",
    "8116f4f1120578e2f339ac91aa350645877ca34579eba9332dd32157c5ddfb7a", "int8-qdq-mixed", "DEPTHOR Small V3", "depthor_v3.onnx",640,480,-1};
inline constexpr double DepthorMaxPairDeltaMs=100.;
inline constexpr const char *DepthorCspnDllSha="a15441e1f8e087a4d69e35da4e23f726b5734c46dc836459ab53f08208a51596";
inline constexpr const char *DepthorCspnPtxSha="dde6964a663062d3493b640c11ea8993fbb0f36a46f09fd410582e45cae545b9";
inline constexpr const char *DepthorPairingPolicy="latest_native_reuse_100ms_v1";
inline bool isDepthorV3(const QString &model){return model=="depthor_v3_fp32"||model=="depthor_v3_fp16"||model=="depthor_v3_int8";}
inline bool isColorLiteSR(const QString &model){return model=="colorsr_rgb"||model=="colorsr_nv12";}
inline bool isUniDepth(const QString &model){return model=="unidepth"||model=="unidepth_wide"||model=="unidepth_wide_576"||model=="unidepth_wide_576_fp16"||isUniDepthNative16(model);}
inline bool isHostDepth(const QString &model){return isUniDepth(model)||isColorLiteSR(model)||isDepthorV3(model);}
inline bool isWide(const QString &model){return model=="unidepth_wide"||model=="unidepth_wide_576"||model=="unidepth_wide_576_fp16"||isUniDepthNative16(model);}
inline bool supportsNativeGrid(const QString &model,int columns,int rows){
    return isUniDepthNative16(model)?columns==16&&rows==16:
        isHostDepth(model)&&((columns==48&&rows==32)||(isDepthorV3(model)&&columns==8&&rows==8));
}
inline const Profile &profile(bool wide){return wide?Wide:Standard;}
inline const Profile &profile(const QString &model){return model=="depthor_v3_fp32"?DepthorV3Fp32:model=="depthor_v3_fp16"?DepthorV3Fp16:model=="depthor_v3_int8"?DepthorV3Int8:model=="colorsr_rgb"?ColorRgb:model=="colorsr_nv12"?ColorNv12:(model=="unidepth_wide_576_fp16"||isUniDepthNative16(model))?WideRgb576Fp16Trial:(model=="unidepth_wide_576"?WideRgb576:profile(isWide(model)));}
inline QJsonArray intrinsics(bool wide){const double f=profile(wide).focal;return {f,f,287.5,161.5};}
}
