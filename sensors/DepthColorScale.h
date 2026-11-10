#pragma once
#include <QtGui/QColor>
#include <algorithm>
#include <array>
#include <cmath>

// One distance palette for model camera-Z and native radial range.
// Both values use the float32 metre domain; sensor data is never altered.
namespace DepthColorScale {
inline QRgb hueColor(float hue)
{
    // QColor 6.11 stores HSV hue in hundredths of a degree. Cache every
    // representable hue used by this palette; retain its float rounding and
    // let Qt generate the RGB values, with no additional quantization.
    static const auto colors=[] {
        std::array<QRgb,24001> result{};
        for(int i=0;i<int(result.size());++i)
            result[i]=QColor::fromHsvF(float(i)/36000.0f,1,1).rgb();
        return result;
    }();
    return colors[qRound(hue*36000.0f)];
}
inline QRgb color(float metres,double minimum,double maximum,bool valid=true,bool blackOutside=false)
{
    if(!valid||!std::isfinite(metres)||metres<=0||!std::isfinite(minimum)
        ||!std::isfinite(maximum)||maximum<=minimum)return qRgb(0,0,0);
    if(blackOutside&&(metres<float(minimum)||metres>float(maximum)))return qRgb(0,0,0);
    const double t=std::clamp((double(metres)-minimum)/(maximum-minimum),0.0,1.0);
    return hueColor(float((1-t)*2/3));
}
}
