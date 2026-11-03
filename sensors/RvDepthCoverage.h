#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QRect>

namespace RvDepthCoverage {
struct DepthField {
    QRect sourceRect;
    bool valid()const{return !sourceRect.isEmpty();}
};
// Only the admitted wide-view geometric output policy defines the viewport.
// A/G/H use the fixed calibrated grid; B keeps its geometric display crop.
// Color range, native validity and selected correction points do not define it.
DepthField depthField(const QByteArray &depth,const QJsonObject &metadata);
}
