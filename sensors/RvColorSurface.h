#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// Current-frame derived surface, NOT additional sensor measurements.
// Inputs use normalized coordinates on the rectified camera image boundaries.
namespace RvColorSurface {
struct Point {double u=0,v=0,z=0,range=0;bool valid=false;};
struct Grid {
    int width,height;std::vector<float> z;std::vector<unsigned char> measured;
    Grid(int w,int h):width(w),height(h),z(w*h,0),measured(w*h,0){}
    void put(int x,int y,double value,bool raw=false){
        if(x<0||y<0||x>=width||y>=height||!std::isfinite(value)||value<=0)return;
        const int i=y*width+x;
        if(z[i]==0||value<z[i]){z[i]=float(value);measured[i]=raw;}
    }
};
inline bool usable(const Point &p){return p.valid&&std::isfinite(p.u)&&std::isfinite(p.v)&&std::isfinite(p.z)&&std::isfinite(p.range)&&p.z>0&&p.range>=.01&&p.range<=1.4;}
inline void point(Grid &g,const Point &p){if(usable(p)&&p.u>=0&&p.v>=0&&p.u<1&&p.v<1)g.put(int(p.u*g.width),int(p.v*g.height),p.z,true);}
inline double cross(double ax,double ay,double bx,double by){return ax*by-ay*bx;}
inline void triangle(Grid &g,const Point &a,const Point &b,const Point &c){
    const double area=cross(b.u-a.u,b.v-a.v,c.u-a.u,c.v-a.v);
    if(!std::isfinite(area)||std::abs(area)<1e-12)return;
    const int x0=std::max(0,int(std::ceil(std::min({a.u,b.u,c.u})*g.width-.5)));
    const int x1=std::min(g.width-1,int(std::floor(std::max({a.u,b.u,c.u})*g.width-.5)));
    const int y0=std::max(0,int(std::ceil(std::min({a.v,b.v,c.v})*g.height-.5)));
    const int y1=std::min(g.height-1,int(std::floor(std::max({a.v,b.v,c.v})*g.height-.5)));
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
        const double u=(x+.5)/g.width,v=(y+.5)/g.height;
        const double wb=cross(u-a.u,v-a.v,c.u-a.u,c.v-a.v)/area;
        const double wc=cross(b.u-a.u,b.v-a.v,u-a.u,v-a.v)/area,wa=1-wb-wc;
        if(std::min({wa,wb,wc}) < -1e-9)continue;
        // Perspective-correct surface interpolation; nearest visible Z wins.
        g.put(x,y,1/(wa/a.z+wb/b.z+wc/c.z));
    }
}
inline bool connected(const Point &a,const Point &b){
    return usable(a)&&usable(b)&&std::abs(a.range-b.range)<=std::max(.02,.08*std::min(a.range,b.range));
}
inline int raster(const std::array<Point,1536>&p,Grid &low,Grid &high){
    for(const auto &v:p){point(low,v);point(high,v);}
    int quads=0;
    for(int y=0;y<31;++y)for(int x=0;x<47;++x){
        const auto &a=p[y*48+x],&b=p[y*48+x+1],&c=p[(y+1)*48+x],&d=p[(y+1)*48+x+1];
        // Do not bridge a missing native return, a depth step, or folded mesh.
        if(!connected(a,b)||!connected(a,c)||!connected(b,d)||!connected(c,d)||!connected(a,d)||!connected(b,c))continue;
        const double s1=cross(b.u-a.u,b.v-a.v,d.u-a.u,d.v-a.v),s2=cross(d.u-a.u,d.v-a.v,c.u-a.u,c.v-a.v);
        if(s1*s2<=1e-20)continue;
        for(auto *g:{&low,&high}){triangle(*g,a,b,d);triangle(*g,a,d,c);}++quads;
    }return quads;
}
inline void regression(){
    const auto require=[](bool ok,const char *why){if(!ok)throw std::runtime_error(why);};
    std::array<Point,1536> p{};
    // Dense plane whose projected spacing skips cells under old point splats.
    for(int y=0;y<32;++y)for(int x=0;x<48;++x)p[y*48+x]={(x+.5)/32.,(y+.5)/24.,.6,.6,true};
    Grid low(48,32),high(256,256),old(48,32);for(const auto &v:p)point(old,v);raster(p,low,high);
    int oldHoles=0;for(int y=3;y<28;++y)for(int x=3;x<44;++x){oldHoles+=old.z[y*48+x]==0;require(low.z[y*48+x]>.59,"Continuous plane has LR sampling holes");}
    require(oldHoles>100,"Fixture does not reproduce old sampling holes");
    for(int y=20;y<230;++y)for(int x=20;x<230;++x)require(high.z[y*256+x]>.59,"Continuous plane has HR holes");
    p[12*48+16].valid=false;Grid holeLow(48,32),holeHigh(256,256);raster(p,holeLow,holeHigh);
    require(holeHigh.z[int(p[12*48+16].v*256)*256+int(p[12*48+16].u*256)]==0,"Missing return was bridged");
    for(int y=0;y<32;++y)for(int x=0;x<48;++x)p[y*48+x]={(x+.5)/48.,(y+.5)/32.,x<24?.3:1.,x<24?.3:1.,true};
    Grid edgeLow(48,32),edgeHigh(256,256);raster(p,edgeLow,edgeHigh);
    require(edgeHigh.z[128*256+127]==0&&edgeHigh.z[128*256+128]==0,"Foreground/background discontinuity bridged");
    Grid ztest(16,16);Point a{.1,.1,1,1,true},b{.9,.1,1,1,true},c{.1,.9,1,1,true};triangle(ztest,a,b,c);a.z=b.z=c.z=.3;triangle(ztest,a,b,c);require(std::abs(ztest.z[4*16+4]-.3)<1e-6,"Near surface lost z-buffer");
    a.z=b.z=c.z=1;triangle(ztest,a,b,c);require(std::abs(ztest.z[4*16+4]-.3)<1e-6,"Far surface overwrote near surface");
    p={};Grid emptyLow(48,32),emptyHigh(256,256);raster(p,emptyLow,emptyHigh);require(std::all_of(emptyHigh.z.begin(),emptyHigh.z.end(),[](float z){return z==0;}),"Empty input acquired support");
}
}
