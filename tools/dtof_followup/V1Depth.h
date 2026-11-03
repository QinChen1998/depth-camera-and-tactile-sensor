#pragma once
#include "../dtof_m0/M0.h"
#include <vector>

// Portable V1 geometry and current-frame radial reconstruction. No Qt/RGB.
namespace DtofV1 {
struct Settings {
    double nearM=.6, sigma=.5, currentWeight=.6;
    int minimumComponent=5, fillHoles=5;
    bool bezier=true;
    double layerM=.05, sigmaM=.03, spreadM=.06;
    bool fast=false;
    double presentation=0; // 0 original; (0,1] presentation smoothing. Native measurements stay intact.
    int interpolation=0; // Fast only: 0 bilinear, 1 nearest native cell.
};
struct Point {
    double x=0,y=0;
    Point operator+(Point p)const{return {x+p.x,y+p.y};}
    Point operator-(Point p)const{return {x-p.x,y-p.y};}
    Point operator*(double s)const{return {x*s,y*s};}
};
struct Result {
    std::array<float,DtofM0::P> depth{};
    std::array<uint8_t,DtofM0::P> coverage{};
    std::vector<std::vector<Point>> contours;
    bool fresh=false;
};
class Processor {
public:
    explicit Processor(Settings settings={});
    const Result& process(const DtofM0::Frame&,const std::array<uint8_t,1536>& snr,int64_t hostMs,bool withDepth=true);
    const Result& expire(int64_t hostMs);
    void reset();
private:
    Settings s;
    Result out;
    std::array<double,1536> field{};
    uint64_t sequence=0;
    int64_t capture=0,received=-1,lastHost=-1;
    std::string identity;
    bool history=false;
};
}
