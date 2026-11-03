#pragma once
#include <QtGui/QColor>
#include <algorithm>
#include <cmath>

// One distance palette for model camera-Z and native radial range.
// Both values use the float32 metre domain; sensor data is never altered.
namespace DepthColorScale {
inline QRgb color(float metres,double minimum,double maximum,bool valid=true,bool blackOutside=false)
{
    if(!valid||!std::isfinite(metres)||metres<=0||!std::isfinite(minimum)
        ||!std::isfinite(maximum)||maximum<=minimum)return qRgb(0,0,0);
    if(blackOutside&&(metres<float(minimum)||metres>float(maximum)))return qRgb(0,0,0);
    const double t=std::clamp((double(metres)-minimum)/(maximum-minimum),0.0,1.0);
    return QColor::fromHsvF((1-t)*2/3,1,1).rgb();
}
}
