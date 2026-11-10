#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QRect>
#include <vector>

namespace RvDepthCoverage {
struct DepthField {
    QRect sourceRect;
    bool valid()const{return !sourceRect.isEmpty();}
};
// Only the admitted wide-view geometric output policy defines the viewport.
// A/G/H use the fixed calibrated grid; B keeps its geometric display crop.
// Color range, native validity and selected correction points do not define it.
DepthField depthField(const QByteArray &depth,const QJsonObject &metadata);
// Decode metric values and derive the same viewport in a single validated scan.
// A finite array may have no admitted field, as with native/raw previews.
bool decodeDepth(const QByteArray&,const QJsonObject&,std::vector<float>&,DepthField&);
}
