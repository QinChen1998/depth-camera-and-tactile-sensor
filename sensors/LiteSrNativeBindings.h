#pragma once
#include <QtCore/QString>
struct LiteSrNativeBinding {const char* model;const char* file;const char* graph;const char* checkpoint;};
inline const LiteSrNativeBinding* liteSrNativeBinding(const QString& model){
    static const LiteSrNativeBinding bindings[]={
        {"area_full","graphs/area_full.onnx","9cb0061f579f31e1737eec3bbc99a4ed72124129c6c7f9c5eeda9147e91925ed","519cb9b3efe3ebd30aac6a9ff832ee287e761f8af5eb907c9614b30b69623943"},
        {"area_no_guidance","graphs/area_no_guidance.onnx","f3196af1364230ecfb61458119801c657fa6f8505b05987326764783a3b579c3","54ed33a88c36844a56738901fb83420d21b4fc310cabed4a1f56052f4a7c3dd2"},
        {"wta_full","graphs/wta_full.onnx","278cd87f5070f2264dc60086c74789219d1f34185db8a7a0a8970c2dd5955384","b4971ed45b5c8cd872fec4a0ec84777e35fd13fc43146a863ae20b8cf9fcf689"},
        {"interpolation","contracts/interpolation.json","bdc6c6205116afbea4e0d6f6ed7ee243b679a98a49208560f17bc1cb698e106e","519cb9b3efe3ebd30aac6a9ff832ee287e761f8af5eb907c9614b30b69623943"},
        {"near_full_quality12","graphs/near_full_quality12.onnx","7d023320ec2a27bc09af63d3df8d88969fd4f3e37b56c94c3d6e6732fb7deaf1","ba4e3ea6bb79139bd3eb998c0f0e74edb5c9da3a059c25476ef5d4d68a459bfb"},
        {"near_no_guidance_quality12","graphs/near_no_guidance_quality12.onnx","3553b9b0aceec327e43cee40b24a7fcacf22d6a01e39ef79e97908707abb3026","bc6b97896d98c0432551f0e2d67e8a342c8179d79b740fc4f9f9aa2550e0bcb5"},
    };
    for(const auto& binding:bindings)if(model==QLatin1String(binding.model))return &binding;
    return nullptr;
}
