#include "M0.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace DtofM0 {
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point t) {return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
bool inside(int x,int y) {return x>=0&&x<48&&y>=0&&y<32;}
double metres(uint16_t uq) {return uq*.00025;}
bool near(double a,double b,double threshold) {return std::abs(a-b)<=threshold+1e-12;}
template<class F> void neighbours(int i,int radius,F visit) {
    for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx) {
        const int x=i%48+dx,y=i/48+dy;
        if(inside(x,y)&&(dx||dy))visit(y*48+x,dx,dy);
    }
}
// Supercover: a diagonal cannot cut through a forbidden corner. Ordinary
// unknown is not a hard obstacle. Endpoints are checked by the caller.
bool clearPath(int i,int j,double reference,const Frame& f,const Result& o,const Config& c) {
    const int dx=j%48-i%48,dy=j/48-i/48,steps=2*std::max(std::abs(dx),std::abs(dy));
    for(int s=1;s<steps;++s) {
        const double x=i%48+double(dx)*s/steps,y=i/48+double(dy)*s/steps;
        for(int yy=int(std::floor(y));yy<=int(std::ceil(y));++yy)
            for(int xx=int(std::floor(x));xx<=int(std::ceil(x));++xx) {
                const int k=yy*48+xx;if(k==i||k==j)continue;
                if(o.observation[k]==Outside||o.observation[k]==Malformed)return false;
                if(o.observation[k]==InRange&&!near(metres(f.uq[k]),reference,c.layerM))return false;
            }
    }
    return true;
}
bool channel(int i,const Result& o) {
    for(const auto& d:std::array<std::array<int,2>,4>{{{1,0},{0,1},{1,1},{1,-1}}}) {
        const int x=i%48,y=i/48;
        if(inside(x+d[0],y+d[1])&&inside(x-d[0],y-d[1])&&
           o.observation[(y+d[1])*48+x+d[0]]==Unknown&&
           o.observation[(y-d[1])*48+x-d[0]]==Unknown)return true;
    }
    return false;
}
void spatialContour(const std::array<uint8_t,N>& shape,std::array<double,N>& contour) {
    std::array<double,N> distance{};
    for(int i=0;i<N;++i) {
        double squared=64;
        for(int j=0;j<N;++j)if(bool(shape[i])!=bool(shape[j])) {
            const double dx=i%48-j%48,dy=i/48-j/48;squared=std::min(squared,dx*dx+dy*dy);
        }
        distance[i]=(shape[i]?1:-1)*(std::sqrt(squared)-.5);
    }
    constexpr double kernel[3]={.15,.70,.15};
    for(int i=0;i<N;++i) {
        double value=0;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
            value+=kernel[dx+1]*kernel[dy+1]*distance[std::clamp(i/48+dy,0,31)*48+std::clamp(i%48+dx,0,47)];
        contour[i]=shape[i]?std::max(.2,value):std::min(-.2,value);
    }
}
}
Processor::Processor(Config c) { configure(c); }
void Processor::configure(Config c) {
    if(!std::isfinite(c.layerM)||c.layerM<=0||!std::isfinite(c.changeM)||c.changeM<=0||
       !std::isfinite(c.sigmaM)||c.sigmaM<=0||!std::isfinite(c.holeSpreadM)||c.holeSpreadM<0||
       c.edgeMisses<0||c.edgeMisses>10||c.innerMisses<0||c.innerMisses>10||
       c.initialPeriodMs<1||c.initialPeriodMs>200||c.jitterMs<0||c.jitterMs>100||
       c.edgeMaxMs<1||c.edgeMaxMs>150||c.innerMaxMs<1||c.innerMaxMs>220||
       c.freshMs<1||c.freshMs>500||c.resetGapMs<c.freshMs||c.resetGapMs>10000)
        throw std::invalid_argument("Invalid M0 experimental parameters");
    config_=c;reset();
}
void Processor::reset() {
    cells_={};current_={};out_=Result{};periods_={};periodsCount_=periodCursor_=0;lastHost_=-1;
}
double Processor::periodMs() const {
    if(!periodsCount_)return config_.initialPeriodMs;
    auto a=periods_;std::sort(a.begin(),a.begin()+periodsCount_);
    return a[periodsCount_/2];
}
const Result& Processor::expire(int64_t now) {
    out_.newMeasurement=false;out_.timing={};
    if(now<0||(lastHost_>=0&&now<lastHost_)) {reset();out_.event="host_time_reset";return out_;}
    lastHost_=now;
    if(!out_.sequence)return out_;
    if(now-out_.receiveMs>=config_.freshMs) {
        out_.depth.fill(0);out_.coverage.fill(0);out_.source.fill(0);out_.shape.fill(0);
        out_.prepared.fill(0);out_.smooth.fill(0);out_.contour.fill(0);out_.estimated.fill(0);
        out_.fresh=false;out_.event="source_expired";
        for(auto& c:cells_)if(c.phase==HoldEdge||c.phase==HoldInner)c.phase=Off;
        if(now-out_.receiveMs>=config_.resetGapMs) {
            // Keep the sequence watermark, so a stale duplicate cannot revive it.
            cells_={};periodsCount_=periodCursor_=0;
        }
        return out_;
    }
    bool changed=false;
    for(int i=0;i<N;++i)if((cells_[i].phase==HoldEdge||cells_[i].phase==HoldInner)&&now>=cells_[i].deadlineMs) {
        cells_[i].phase=Off;out_.shape[i]=out_.estimated[i]=0;out_.reason[i]|=Age;changed=true;
    }
    if(changed){render();out_.event="hold_expired";}
    return out_;
}
const Result& Processor::onFrame(const Frame& f,int64_t now) {
    const auto start=Clock::now();
    expire(now);
    if(now<0||!f.sequence||f.captureNs<=0||f.identity.empty()||
       std::any_of(f.valid.begin(),f.valid.end(),[](auto v){return v>1;})) {
        reset();out_.event="invalid_frame";return out_;
    }
    if(current_.identity!=f.identity){reset();out_.event="identity_reset";}
    if(out_.sequence&&f.sequence<=out_.sequence) {
        out_.event=f.sequence==out_.sequence?"duplicate":"out_of_order";return out_;
    }
    const double dt=out_.sequence?(f.captureNs-out_.captureNs)/1e6:0;
    const double arrivalDt=out_.sequence?double(now-out_.receiveMs):0;
    if(out_.sequence&&(dt<=0||dt>=config_.resetGapMs||arrivalDt>=config_.resetGapMs)) {
        reset();out_.event="capture_or_gap_reset";
    }
    const double period=periodMs(); // Freeze using PRIOR normal independent intervals.
    const auto previous=cells_;
    const auto event=out_.event;
    out_=Result{};out_.event=event;out_.sequence=f.sequence;out_.captureNs=f.captureNs;
    out_.receiveMs=now;out_.fresh=out_.newMeasurement=true;lastHost_=now;current_=f;
    for(int i=0;i<N;++i) {
        out_.observation[i]=!f.valid[i]?Unknown:!f.uq[i]?Malformed:
            (f.uq[i]>=400&&f.uq[i]<=5600?InRange:Outside);
        if(out_.observation[i]!=InRange)continue;
        bool neighbour=false;
        neighbours(i,1,[&](int j,int,int){if(f.valid[j]&&f.uq[j]>=400&&f.uq[j]<=5600&&
            near(metres(f.uq[i]),metres(f.uq[j]),config_.layerM))neighbour=true;});
        auto& c=cells_[i];
        const bool recent=c.rawHostMs>=0&&now-c.rawHostMs<config_.innerMaxMs&&
            (f.captureNs-c.rawCaptureNs)/1e6<config_.innerMaxMs;
        const bool continued=recent&&near(metres(c.lastUq),metres(f.uq[i]),config_.changeM);
        const bool second=c.hits==1&&near(metres(c.pendingUq),metres(f.uq[i]),config_.layerM)&&
            dt>0&&dt<config_.freshMs&&arrivalDt<config_.freshMs;
        if(neighbour||continued||second)out_.accepted[i]=1;
        else {const auto everRaw=c.rawCaptureNs;c=Cell{};c.rawCaptureNs=everRaw;c.phase=Pending;c.pendingUq=f.uq[i];c.hits=1;}
    }
    // Short-lived surface overlap, rederived from previous RAW evidence each
    // frame. Labels are scratch data, never stable object IDs. Four-connected
    // surfaces avoid joining objects through a single diagonal corner.
    std::array<uint8_t,N> seen{};
    std::array<int,N> queue{};
    for(int seed=0;seed<N;++seed) {
        if(seen[seed]||!previous[seed].lastUq||now-previous[seed].rawHostMs>=config_.innerMaxMs)continue;
        int count=1;queue[0]=seed;seen[seed]=1;bool overlap=false;
        for(int h=0;h<count;++h) {
            const int i=queue[h];
            overlap|=out_.accepted[i]&&near(metres(f.uq[i]),metres(previous[i].lastUq),config_.changeM);
            neighbours(i,1,[&](int j,int dx,int dy){
                if(std::abs(dx)+std::abs(dy)!=1||seen[j]||!previous[j].lastUq||
                   now-previous[j].rawHostMs>=config_.innerMaxMs)return;
                if(near(metres(previous[j].lastUq),metres(previous[seed].lastUq),config_.layerM)) {
                    seen[j]=1;queue[count++]=j;
                }
            });
        }
        for(int h=0;h<count;++h)out_.overlap[queue[h]]=overlap;
    }
    // Unknown components use the CURRENT raw classification, including hard
    // invalid cells as blockers in components so a hard hole cannot be repaired.
    std::array<uint32_t,N> holeReason{};
    std::array<uint8_t,N> holeOk{};
    seen.fill(0);
    for(int seed=0;seed<N;++seed) {
        if(seen[seed]||out_.observation[seed]==InRange)continue;
        int count=1;queue[0]=seed;seen[seed]=1;int minX=48,maxX=0,minY=32,maxY=0,ringCount=0;
        uint32_t reason=0;double lo=2,hi=0;std::array<uint8_t,N> ring{};
        for(int h=0;h<count;++h) {
            int i=queue[h],x=i%48,y=i/48;
            minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);
            if(!x||x==47||!y||y==31)reason|=HoleOpen;
            if(out_.observation[i]!=Unknown)reason|=Hard;
            neighbours(i,1,[&](int j,int,int){
                if(out_.observation[j]==InRange) {
                    if(!ring[j]) {ring[j]=1;if(out_.accepted[j]){++ringCount;lo=std::min(lo,metres(f.uq[j]));hi=std::max(hi,metres(f.uq[j]));}}
                } else if(!seen[j]){seen[j]=1;queue[count++]=j;}
            });
        }
        if(count>4)reason|=HoleArea;
        if(maxX-minX>=3||maxY-minY>=3)reason|=HoleBox;
        if(ringCount<8)reason|=HoleRing;
        if(hi-lo>config_.holeSpreadM+1e-12)reason|=HoleLayer;
        for(int h=0;h<count;++h){holeReason[queue[h]]=reason;holeOk[queue[h]]=!reason;}
    }
    for(int i=0;i<N;++i) {
        auto& c=cells_[i];const auto& old=previous[i];
        if(out_.observation[i]==Outside||out_.observation[i]==Malformed) {
            c=Cell{};c.rawCaptureNs=old.rawCaptureNs;c.phase=HardOut;out_.reason[i]=Hard;continue;
        }
        if(out_.accepted[i]) {
            c=Cell{};c.phase=On;c.lastUq=f.uq[i];c.rawHostMs=now;c.rawCaptureNs=f.captureNs;
            c.inner=i%48>0&&i%48<47&&i/48>0&&i/48<31;
            neighbours(i,1,[&](int j,int,int){c.inner=c.inner&&out_.accepted[j]&&near(metres(f.uq[i]),metres(f.uq[j]),config_.layerM);});
            out_.shape[i]=1;continue;
        }
        if(out_.observation[i]!=Unknown)continue;
        c.hits=0;c.pendingUq=0;c.phase=Off;
        c.misses=uint8_t(std::min(255,int(old.misses)+1));
        const bool hasHistory=old.lastUq&&old.rawHostMs>=0;
        const bool inner=hasHistory&&old.inner;
        // Cold-hole policy ablation uses CURRENT ring only; never creates raw history.
        if(!config_.holeHistory&&holeOk[i]&&!hasHistory&&old.rawCaptureNs==0) {
            neighbours(i,2,[&](int j,int dx,int dy){if(out_.accepted[j]&&
                clearPath(i,j,metres(f.uq[j]),f,out_,config_))out_.estimateSupport[i]|=1u<<((dy+2)*5+dx+2);});
            out_.shape[i]=out_.estimated[i]=1;out_.reason[i]=ColdHole;continue;
        }
        uint32_t reject=0;
        if(!config_.temporal||!hasHistory)reject|=NoHistory;
        if(inner)reject|=holeReason[i];
        else if(holeOk[i])reject|=ColdHole; // prior boundary is not interior evidence
        else if(channel(i,out_))reject|=Channel;
        if(c.misses>(inner?config_.innerMisses:config_.edgeMisses))reject|=Count;
        if(!old.misses&&hasHistory) {
            const int limit=std::min(int(std::floor((inner?3:2)*period+config_.jitterMs)),inner?config_.innerMaxMs:config_.edgeMaxMs);
            const double captureAge=(f.captureNs-old.rawCaptureNs)/1e6;
            c.deadlineMs=std::min(old.rawHostMs+limit,now+int64_t(std::floor(limit-captureAge)));
        }
        if(c.deadlineMs<0||now>=c.deadlineMs)reject|=Age;
        if(!out_.overlap[i])reject|=LostSurface;
        int supports=0;bool close=false;
        neighbours(i,2,[&](int j,int dx,int dy){
            if(std::max(std::abs(dx),std::abs(dy))<=1) {
                if(out_.observation[j]==Outside||out_.observation[j]==Malformed)reject|=Changed;
                if(out_.accepted[j]&&previous[j].lastUq&&
                   !near(metres(f.uq[j]),metres(previous[j].lastUq),config_.changeM))reject|=Changed;
            }
            if(out_.accepted[j]&&hasHistory&&near(metres(f.uq[j]),metres(old.lastUq),config_.layerM)&&
               clearPath(i,j,metres(old.lastUq),f,out_,config_)) {
                ++supports;close|=std::max(std::abs(dx),std::abs(dy))<=1;
                out_.estimateSupport[i]|=1u<<((dy+2)*5+dx+2);
            }
        });
        if(supports<2||!close)reject|=NoSupport;
        out_.reason[i]=reject;
        if(!reject){c.phase=inner?HoldInner:HoldEdge;out_.shape[i]=out_.estimated[i]=1;}
        // Rejection revokes this evidence; a later change cannot resurrect it.
        else {const auto misses=c.misses;c=Cell{};c.misses=misses;c.rawCaptureNs=old.rawCaptureNs;}
    }
    if(dt>=10&&dt<=200&&dt<=1.5*period&&arrivalDt<config_.resetGapMs) {
        periods_[periodCursor_++%8]=dt;periodsCount_=std::min(8,periodsCount_+1);
    }
    out_.timing.stateMs=ms(start);render();out_.timing.totalMs=ms(start);
    return out_;
}
void Processor::render() {
    const auto start=Clock::now();
    out_.depth.fill(0);out_.coverage.fill(0);out_.source.fill(0);
    out_.prepared.fill(0);out_.smooth.fill(0);
    std::array<double,N> lo{},hi{},smoothLo{},smoothHi{};
    std::array<uint8_t,N> smoothFlags{};
    for(int i=0;i<N;++i)if(out_.shape[i]) {
        if(out_.accepted[i]) {out_.prepared[i]=lo[i]=hi[i]=metres(current_.uq[i]);continue;}
        double sum=0,weights=0;int supports=0;lo[i]=2;
        // The state stage already selected current raw IDs. No historical
        // distance is read by numerical reconstruction, even as a reference.
        neighbours(i,2,[&](int j,int dx,int dy){
            if(!(out_.estimateSupport[i]&(1u<<((dy+2)*5+dx+2)))||!out_.accepted[j])return;
            const double v=metres(current_.uq[j]);
            const double weight=1./(dx*dx+dy*dy);sum+=weight*v;weights+=weight;++supports;
            lo[i]=std::min(lo[i],v);hi[i]=std::max(hi[i],v);
        });
        if(supports>=2&&weights>1e-12&&hi[i]-lo[i]<=config_.holeSpreadM+1e-12)out_.prepared[i]=sum/weights;
        else {out_.shape[i]=out_.estimated[i]=0;out_.reason[i]|=NoSupport;cells_[i].phase=Off;}
    }
    for(int i=0;i<N;++i)if(out_.prepared[i]>0) {
        double sum=0,weights=0;smoothLo[i]=2;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx) {
            const int x=i%48+dx,y=i/48+dy;if(!inside(x,y))continue;const int j=y*48+x;
            const double v=out_.prepared[j];if(!v)continue;
            if(config_.hardLayer&&(!near(lo[j],out_.prepared[i],config_.layerM)||!near(hi[j],out_.prepared[i],config_.layerM)))continue;
            if(!clearPath(i,j,out_.prepared[i],current_,out_,config_))continue;
            const double delta=(v-out_.prepared[i])/config_.sigmaM;
            const double weight=std::exp(-.5*(dx*dx+dy*dy)/2.25-.5*delta*delta);
            sum+=weight*v;weights+=weight;smoothLo[i]=std::min(smoothLo[i],lo[j]);smoothHi[i]=std::max(smoothHi[i],hi[j]);
            if(out_.estimated[j])smoothFlags[i]|=SpatialEstimate;
        }
        if(weights>1e-12)out_.smooth[i]=sum/weights;
    }
    out_.timing.prepareMs=ms(start);const auto contourStart=Clock::now();
    // Deliberately the same search, truncation, short kernel and sign clamp as R0.
    spatialContour(out_.shape,out_.contour);
    auto noHistoryMask=out_.shape;bool hasHistory=false;
    for(int i=0;i<N;++i)if(cells_[i].phase==HoldEdge||cells_[i].phase==HoldInner){noHistoryMask[i]=0;hasHistory=true;}
    std::array<double,N> noHistoryContour{};
    // Diagnostic counterfactual only: correctly attribute nonlocal distance
    // field influence, not just held cells among the four interpolation taps.
    if(hasHistory)spatialContour(noHistoryMask,noHistoryContour);
    out_.timing.contourMs=ms(contourStart);const auto outputStart=Clock::now();
    for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
        const int owner=(y/8)*48+x/8,p=y*W+x;
        if(out_.observation[owner]==Malformed||(config_.ownerOutside&&out_.observation[owner]==Outside))continue;
        const double sx=std::clamp((x+.5)/8-.5,0.,47.),sy=std::clamp((y+.5)/8-.5,0.,31.);
        const int x0=int(sx),y0=int(sy);int indices[4],anchor=-1;double spatial[4],best=-1,phi=0,noHistoryPhi=0;
        for(int dy=0;dy<=1;++dy)for(int dx=0;dx<=1;++dx) {
            const int k=dy*2+dx,i=std::min(y0+dy,31)*48+std::min(x0+dx,47);
            indices[k]=i;spatial[k]=(dx?sx-x0:1-(sx-x0))*(dy?sy-y0:1-(sy-y0));
            phi+=spatial[k]*out_.contour[i];
            if(hasHistory)noHistoryPhi+=spatial[k]*noHistoryContour[i];
            if(out_.prepared[i]>0&&spatial[k]>best){best=spatial[k];anchor=i;}
        }
        const int alpha=int(std::lround(255*std::clamp(.5+phi*8,0.,1.)));
        if(!alpha||anchor<0)continue;
        // A supported owner has unambiguous layer identity; otherwise ties
        // across different current surfaces are left black.
        if(out_.prepared[owner]>0)anchor=owner;
        bool ambiguous=false;
        for(int k=0;k<4;++k)if(config_.hardLayer&&out_.prepared[indices[k]]>0&&
            std::abs(spatial[k]-best)<1e-12&&!near(out_.prepared[indices[k]],out_.prepared[anchor],config_.layerM))ambiguous=true;
        if(ambiguous){out_.reason[owner]|=Ambiguous;continue;}
        double sum=0,weights=0;uint8_t flags=Numeric|((hasHistory&&std::abs(phi-noHistoryPhi)>1e-12)?HistoryShape:0);
        for(int k=0;k<4;++k) {
            const int i=indices[k];if(!out_.prepared[i]||!out_.smooth[i]||spatial[k]<=0)continue;
            if(config_.hardLayer&&(!near(smoothLo[i],out_.prepared[anchor],config_.layerM)||
               !near(smoothHi[i],out_.prepared[anchor],config_.layerM)))continue;
            const double delta=(out_.smooth[i]-out_.smooth[anchor])/config_.sigmaM;
            const double weight=spatial[k]*std::exp(-.5*delta*delta);
            sum+=weight*out_.smooth[i];weights+=weight;flags|=smoothFlags[i];
        }
        if(weights<=1e-12)continue;
        const double value=sum/weights;
        if(!std::isfinite(value)||value<.1-1e-12||value>1.4+1e-12)continue;
        out_.depth[p]=float(value);out_.coverage[p]=uint8_t(alpha);out_.source[p]=flags;
    }
    out_.timing.outputMs=ms(outputStart);
    out_.timing.totalMs=out_.timing.stateMs+ms(start);
}
}
