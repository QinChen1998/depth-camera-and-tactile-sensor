#include "RvLiteSrContour.h"
#include "../tools/litesr_board/Contour.h"
#include "../tools/litesr_board/Contour384.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace RvLiteSrContour {
Result refineBoard(const QByteArray &input,const QByteArray &native,const RepairSettings &settings){
    const bool rect=input.size()==384*256*4;
    const int Pixels=rect?384*256:256*256;
    if(input.size()!=Pixels*4||native.size()!=48*32*4)
        throw std::runtime_error("Board LiteSR refinement requires a complete paired depth/native frame");
    std::vector<float> values(Pixels);
    for(int i=0;i<Pixels;++i){const auto bits=qFromLittleEndian<quint32>(input.constData()+4*i);std::memcpy(&values[i],&bits,4);}
    const auto finish=[&](const auto& filtered){
    Result out;out.depth.resize(Pixels*4);out.coverage=QByteArray(reinterpret_cast<const char*>(filtered.coverage.data()),Pixels);
    for(int i=0;i<Pixels;++i){quint32 bits;std::memcpy(&bits,&filtered.depth[i],4);qToLittleEndian(bits,out.depth.data()+4*i);}
    out.diagnostic={{"algorithm",rect?"litesr_board_contour_384x256_v1":"litesr_board_contour_fast_v2"},{"location","host_cpu_background"},{"base_model","board_area_full_int8"},
        {"base_depth_sha256",QString::fromLatin1(QCryptographicHash::hash(input,QCryptographicHash::Sha256).toHex())},
        {"hole_mode",settings.holeMode},{"maximum_hole_native_cells",settings.holeMode==1?4:settings.holeMode==2?64:0},
        {"isolated_outlier_repair",settings.removeOutliers},{"filled_native_cells",filtered.repairs.filledCells},{"outlier_native_cells",filtered.repairs.outlierCells},
        {"filled_pixels",filtered.repairs.filledPixels},{"outlier_pixels",filtered.repairs.outlierPixels},{"new_valid_pixels",filtered.repairs.newSupportPixels},
        {"repair_ms",filtered.repairs.ms},{"postprocess_ms",filtered.totalMs},{"max_adjustment_m",.012},{"tangent_passes",4},
        {"native_measurements_modified",false},{"rgb_used",false},{"temporal_history_used",false},{"minimum_exclusive_m",0},
        {"coverage_semantics","display_opacity_only_never_multiplied_into_metres"}};
    return out;};
    if(rect)return finish(LiteSrBoardContour384::refineFast(values.data(),reinterpret_cast<const uint8_t*>(native.constData()),0,{settings.holeMode,settings.removeOutliers}));
    return finish(LiteSrBoardContour::refineFast(values.data(),reinterpret_cast<const uint8_t*>(native.constData()),0,{settings.holeMode,settings.removeOutliers}));
}
namespace {
constexpr int W=256,N=W*W;
constexpr float RangeSigma=.012f,LayerGuard=.04f,MaxChange=.012f;
constexpr std::array<int,7> Spatial{{1,6,15,20,15,6,1}};
constexpr std::array<int,5> MaskKernel{{1,4,6,4,1}};
int at(int x,int y){return std::clamp(y,0,W-1)*W+std::clamp(x,0,W-1);}

// Bilinear interpolation along a contour needs all contributing support.
// Missing corners are not renormalized into a bridge over an unknown gap.
bool sample(const std::vector<float> &z,float x,float y,float reference,float &value){
    if(x<0||y<0||x>W-1||y>W-1)return false;
    const int ix=int(x),iy=int(y);const float fx=x-ix,fy=y-iy;
    const std::array<float,4> weights{{(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy}};
    double sum=0;
    for(int k=0;k<4;++k)if(weights[k]>1e-7f){
        const float v=z[at(ix+k%2,iy+k/2)];
        if(v==0||std::abs(v-reference)>LayerGuard)return false;
        sum+=weights[k]*v;
    }
    value=float(sum);return true;
}
struct Region {int kind=0,cells=0;float mean=0,low=0,high=0;};
struct Repairs {int filledCells=0,outlierCells=0,filledPixels=0,outlierPixels=0,newSupportPixels=0;double ms=0;};
Repairs repair(std::vector<float> &depth,const QByteArray &native,int rotation,const RepairSettings &settings){
    Repairs result;QElapsedTimer clock;clock.start();
    if(settings.holeMode==0&&!settings.removeOutliers)return result;
    if(native.size()!=48*32*4)throw std::runtime_error("LiteSR repair requires the same complete native measurement");
    constexpr int C=48,R=32,M=C*R;
    std::array<float,M> measured{},low{},sum{};
    std::array<int,M> count{},kind{},group{};
    std::vector<int> owner(N);
    for(int i=0;i<M;++i){
        const auto *p=native.constData()+4*i;const int valid=quint8(p[3]),snr=quint8(p[2]);
        if(valid>1)throw std::runtime_error("LiteSR repair rejected a nonbinary native valid bit");
        const float z=qFromLittleEndian<quint16>(p)*.00025f;
        const bool successful=valid&&snr>=6&&z>0;
        if(successful){measured[i]=z;if(z<.1f||z>1.4f)kind[i]=3;}
        else kind[i]=1;
    }
    for(int y=0;y<W;++y)for(int x=0;x<W;++x){
        // Match nearest eligibility, including inverse restoration. W48 ->
        // W256 is noninteger, so 180 degrees changes the owner phases.
        const int nx=rotation?C-1-(W-1-x)*C/W:x*C/W;
        const int ny=rotation?R-1-(W-1-y)*R/W:y*R/W;
        const int i=y*W+x,k=ny*C+nx;owner[i]=k;
        if(depth[i]>0){sum[k]+=depth[i];++count[k];}
    }
    for(int i=0;i<M;++i)if(count[i])low[i]=sum[i]/count[i];
    const auto neighbours=[&](int i,bool diagonal,const auto &fn){
        const int x=i%C,y=i/C;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
            if((dx==0&&dy==0)||(!diagonal&&dx&&dy))continue;
            if(x+dx>=0&&x+dx<C&&y+dy>=0&&y+dy<R)fn((y+dy)*C+x+dx);
        }
    };
    if(settings.removeOutliers){
        std::array<int,M> candidates{};
        for(int i=0;i<M;++i)if(measured[i]>0){
            std::array<float,8> values{};int n=0;
            neighbours(i,true,[&](int j){if(kind[j]==0&&measured[j]>0)values[n++]=measured[j];});
            if(n<6)continue;
            std::sort(values.begin(),values.begin()+n);const float median=values[n/2];
            if(std::abs(measured[i]-median)<std::max(.15f,.2f*median))continue;
            int coherent=0;for(int j=0;j<n;++j)coherent+=std::abs(values[j]-median)<=.04f;
            if(coherent>=6)candidates[i]=2;
        }
        for(int i=0;i<M;++i)if(candidates[i])kind[i]=2;
    }
    const int maximum=settings.holeMode==1?4:settings.holeMode==2?64:0;
    std::array<bool,M> seen{};std::vector<Region> regions(1);
    for(int start=0;start<M;++start){
        const int type=kind[start];if(seen[start]||(type!=1&&type!=2))continue;
        std::vector<int> component{start};seen[start]=true;
        for(size_t n=0;n<component.size();++n)neighbours(component[n],false,[&](int j){
            if(!seen[j]&&kind[j]==type){seen[j]=true;component.push_back(j);}
        });
        if(component.size()>size_t(type==2?2:maximum))continue;
        std::array<bool,M> boundarySeen{};float lo=2,hi=0,rawLo=2,rawHi=0;double total=0;int support=0;
        for(int i:component)neighbours(i,true,[&](int j){
            if(boundarySeen[j]||kind[j]!=0||low[j]<=0||measured[j]<=0)return;
            boundarySeen[j]=true;lo=std::min(lo,low[j]);hi=std::max(hi,low[j]);
            rawLo=std::min(rawLo,measured[j]);rawHi=std::max(rawHi,measured[j]);total+=low[j];++support;
        });
        // A larger area allowance never relaxes the one-surface guard.
        if(support<(type==2?6:3)||hi-lo>.06f||rawHi-rawLo>.08f)continue;
        const int id=int(regions.size());regions.push_back({type,int(component.size()),float(total/support),lo,hi});
        for(int i:component)group[i]=id;
    }
    std::vector<int> target(N);std::vector<float> interpolated(N),weights(N);
    for(int i=0;i<N;++i){const int g=group[owner[i]];
        if(g&&(regions[g].kind==2||depth[i]==0))target[i]=g;
    }
    const auto source=[&](int i,int g){return i>=0&&i<N&&!target[i]&&depth[i]>0
        &&std::abs(depth[i]-regions[g].mean)<=.06f;};
    // Two O(H*W) scanline passes. One-sided border support has lower weight;
    // every bracket reads immutable LiteSR values, never a previous repair.
    for(int axis=0;axis<2;++axis)for(int line=0;line<W;++line){
        const auto index=[&](int t){return axis?t*W+line:line*W+t;};
        int begin=0;
        while(begin<W){const int g=target[index(begin)];if(!g){++begin;continue;}
            int end=begin+1;while(end<W&&target[index(end)]==g)++end;
            const bool a=begin>0&&source(index(begin-1),g),b=end<W&&source(index(end),g);
            if(a||b){
                const float left=a?depth[index(begin-1)]:depth[index(end)];
                const float right=b?depth[index(end)]:left;
                const float weight=(a&&b?1.f:.25f)/float(end-begin+1);
                for(int t=begin;t<end;++t){const int i=index(t);
                    const float fraction=float(t-begin+1)/float(end-begin+1);
                    interpolated[i]+=weight*(left+(right-left)*fraction);weights[i]+=weight;
                }
            }
            begin=end;
        }
    }
    std::array<bool,M> touched{};
    for(int i=0;i<N;++i)if(target[i]){
        const auto &g=regions[target[i]];
        result.newSupportPixels+=depth[i]==0;
        depth[i]=std::clamp(weights[i]>0?interpolated[i]/weights[i]:g.mean,g.low,g.high);
        if(g.kind==2)++result.outlierPixels;else ++result.filledPixels;
        if(!touched[owner[i]]){touched[owner[i]]=true;if(g.kind==2)++result.outlierCells;else ++result.filledCells;}
    }
    result.ms=clock.nsecsElapsed()/1e6;return result;
}
}
Result refine(const QByteArray &input){return refine(input,{},0,{0,false});}
Result refine(const QByteArray &input,const QByteArray &native,int rotation,const RepairSettings &settings){
    if(input.size()!=N*4)throw std::runtime_error("LiteSR contour requires a 256 x 256 float32 grid");
    if((rotation!=0&&rotation!=180)||settings.holeMode<0||settings.holeMode>2)
        throw std::runtime_error("LiteSR repair settings or input orientation invalid");
    QElapsedTimer clock;clock.start();
    std::vector<float> base(N),surface(N),next(N);
    int valid=0;
    for(int i=0;i<N;++i){
        const auto bits=qFromLittleEndian<quint32>(input.constData()+4*i);
        std::memcpy(&base[i],&bits,4);
        if(!std::isfinite(base[i])||(base[i]!=0&&(base[i]<.1f||base[i]>1.4f)))
            throw std::runtime_error("LiteSR contour rejected invalid radial depth");
        valid+=base[i]>0;
    }
    const auto repaired=repair(base,native,rotation,settings);
    // Current-frame normalized bilateral interpolation: suppress surface noise,
    // but use the immutable LiteSR depth as the range guide on every pass.
    for(int y=0;y<W;++y)for(int x=0;x<W;++x){
        const int i=y*W+x;const float centre=base[i];if(centre==0)continue;
        double sum=0,weight=0;
        for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx){
            const float v=base[at(x+dx,y+dy)],difference=v-centre;
            if(v==0||std::abs(difference)>LayerGuard)continue;
            const double d=difference/RangeSigma;
            const double w=double(Spatial[dx+3]*Spatial[dy+3])/(1+d*d);
            sum+=w*v;weight+=w;
        }
        surface[i]=std::clamp(float(sum/weight),centre-MaxChange,centre+MaxChange);
    }
    // Tangential diffusion rounds stair steps in isodepth contours instead of
    // blurring straight across foreground/background boundaries. Four bounded
    // subpixel steps retain planes, thin supports and the exact validity mask.
    for(int pass=0;pass<4;++pass){
        next=surface;
        for(int y=1;y<W-1;++y)for(int x=1;x<W-1;++x){
            const int i=y*W+x;const float c=surface[i];if(c==0)continue;
            const float l=surface[i-1],r=surface[i+1],u=surface[i-W],d=surface[i+W];
            if(l==0||r==0||u==0||d==0)continue;
            const float gx=(r-l)*.5f,gy=(d-u)*.5f,length=std::hypot(gx,gy);
            if(length<.0001f)continue;
            const float tx=-gy/length*1.25f,ty=gx/length*1.25f;
            float a=0,b=0;
            if(!sample(surface,x+tx,y+ty,base[i],a)||!sample(surface,x-tx,y-ty,base[i],b))continue;
            next[i]=std::clamp(.5f*c+.25f*(a+b),base[i]-MaxChange,base[i]+MaxChange);
        }
        surface.swap(next);
    }
    Result result;result.depth.resize(N*4);result.coverage=QByteArray(N,0);
    int changed=0,feathered=0;double total=0,maximum=0;
    for(int y=0;y<W;++y)for(int x=0;x<W;++x){
        const int i=y*W+x;const float z=base[i]>0?std::clamp(surface[i],.1f,1.4f):0;
        quint32 bits;std::memcpy(&bits,&z,4);qToLittleEndian(bits,result.depth.data()+4*i);
        if(base[i]==0)continue;
        const double delta=std::abs(double(z)-base[i]);total+=delta;maximum=std::max(maximum,delta);changed+=delta>1e-6;
        int occupied=0;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)
            if(base[at(x+dx,y+dy)]>0)occupied+=MaskKernel[dx+2]*MaskKernel[dy+2];
        const int alpha=(occupied*255+128)/256;
        result.coverage[i]=char(alpha);feathered+=alpha<255;
    }
    result.diagnostic={{"algorithm",native.isEmpty()?"litesr_contour_tangent_v1":"litesr_contour_repair_v2"},{"location","host_cpu_background"},
        {"base_model","area_full"},{"base_depth_sha256",QString::fromLatin1(QCryptographicHash::hash(input,QCryptographicHash::Sha256).toHex())},
        {"validity_mask_preserved",repaired.newSupportPixels==0},{"hole_filling",settings.holeMode>0},{"hole_mode",settings.holeMode},
        {"maximum_hole_native_cells",settings.holeMode==1?4:settings.holeMode==2?64:0},{"isolated_outlier_repair",settings.removeOutliers},
        {"filled_native_cells",repaired.filledCells},{"outlier_native_cells",repaired.outlierCells},
        {"filled_pixels",repaired.filledPixels},{"outlier_pixels",repaired.outlierPixels},{"new_valid_pixels",repaired.newSupportPixels},{"repair_ms",repaired.ms},
        {"repair_method","two_axis_linear_from_current_litesr_boundary"},{"repair_values_are_measurements",false},
        {"native_measurements_modified",false},{"rgb_used",false},{"temporal_history_used",false},
        {"range_sigma_m",RangeSigma},{"layer_guard_m",LayerGuard},{"max_adjustment_m",MaxChange},
        {"tangent_passes",4},{"valid_pixels",valid+repaired.newSupportPixels},{"changed_pixels",changed},{"antialiased_pixels",feathered},
        {"smoothing_change_reference","after_optional_repairs"},
        {"mean_absolute_change_m",valid+repaired.newSupportPixels?total/(valid+repaired.newSupportPixels):0},{"maximum_absolute_change_m",maximum},
        {"postprocess_ms",clock.nsecsElapsed()/1e6},{"coverage_semantics","display_opacity_only_never_multiplied_into_metres"}};
    return result;
}
}
