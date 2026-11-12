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
// The host viewport uses the fixed calibrated grid.
// Color range, native validity and selected correction points do not define it.
// Decode metric values and derive the viewport in a single scan.
// Native depth and other finite arrays can be valid without a host field.
bool decodeDepth(const QByteArray &depth,const QJsonObject &metadata,
                 std::vector<float> &values,DepthField &field);
}
