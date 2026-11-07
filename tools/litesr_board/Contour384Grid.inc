// Independent rectangular grid algorithm; W/H are supplied by the enclosing namespace.
struct RepairSettings {int holeMode=1;bool removeOutliers=true;};
struct Repairs {int filledCells=0,outlierCells=0,filledPixels=0,outlierPixels=0,newSupportPixels=0;double ms=0;};
namespace {
struct Timer {
 std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
 double ms()const{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
};
constexpr int N=W*H;
constexpr float RangeSigma=.012f,LayerGuard=.04f,MaxChange=.012f;
// Pixel distances are expressed relative to the unchanged256 reference grid.
// Metric thresholds and native48x32 repair limits do not scale with output size.
constexpr int PixelScale=W==512?2:1;
constexpr int SpatialRadius=3*PixelScale,MaskRadius=2*PixelScale,PaddedWidth=W+2*SpatialRadius,PaddedHeight=H+2*SpatialRadius;
constexpr auto Spatial=[](){
 if constexpr(W==512)return std::array<int,13>{{2,7,12,21,30,35,40,35,30,21,12,7,2}};
 else return std::array<int,7>{{1,6,15,20,15,6,1}};
}();
constexpr auto MaskKernel=[](){
 if constexpr(W==512)return std::array<int,9>{{2,5,8,10,12,10,8,5,2}};
 else return std::array<int,5>{{1,4,6,4,1}};
}();
constexpr int MaskSum=[](){int total=0;for(int v:MaskKernel)total+=v;return total;}();
constexpr int MaskNormalization=MaskSum*MaskSum;
int at(int x,int y){return std::clamp(y,0,H-1)*W+std::clamp(x,0,W-1);}

// Bilinear interpolation along a contour needs all contributing support.
// Missing corners are not renormalized into a bridge over an unknown gap.
template<bool Fast=false> bool sample(const std::vector<float> &z,float x,float y,float reference,float &value){
    if(x<0||y<0||x>W-1||y>H-1)return false;
    const int ix=int(x),iy=int(y);const float fx=x-ix,fy=y-iy;
    const std::array<float,4> weights{{(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy}};
#if defined(__ARM_NEON)
    double sum=0;
#else
    typename std::conditional<Fast,float,double>::type sum=0;
#endif
    for(int k=0;k<4;++k)if(weights[k]>1e-7f){
        // Bounds were checked above. A corner outside the image has zero
        // interpolation weight and is skipped, so Fast needs no clamp here.
        const float v=z[Fast?(iy+k/2)*W+ix+k%2:at(ix+k%2,iy+k/2)];
        if(v==0||std::abs(v-reference)>LayerGuard)return false;
        sum+=weights[k]*v;
    }
    value=float(sum);return true;
}
struct Region {int kind=0,cells=0;float mean=0,low=0,high=0;};
#if defined(__ARM_NEON)
float32x4_t reciprocal(float32x4_t x){
 auto r=vrecpeq_f32(x);r=vmulq_f32(r,vrecpsq_f32(x,r));return vmulq_f32(r,vrecpsq_f32(x,r));
}
void bilateralNeon(const std::vector<float>& base,const std::vector<float>& padded,std::vector<float>& surface){
 const auto zero=vdupq_n_f32(0),one=vdupq_n_f32(1),sigma=vdupq_n_f32(1/RangeSigma),guard=vdupq_n_f32(LayerGuard);
 for(int y=0;y<H;++y)for(int x=0;x<W;x+=4){
  const auto centre=vld1q_f32(base.data()+y*W+x);
  auto sum=zero,weight=zero,sumError=zero,weightError=zero;
  for(int dy=-SpatialRadius;dy<=SpatialRadius;++dy)for(int dx=-SpatialRadius;dx<=SpatialRadius;++dx){
   const auto v=vld1q_f32(padded.data()+(y+dy+SpatialRadius)*PaddedWidth+x+dx+SpatialRadius),difference=vsubq_f32(v,centre);
   const auto mask=vandq_u32(vcgtq_f32(v,zero),vcleq_f32(vabsq_f32(difference),guard));
   const auto d=vmulq_f32(difference,sigma),denominator=vaddq_f32(one,vmulq_f32(d,d));
   auto w=vmulq_n_f32(reciprocal(denominator),float(Spatial[dx+SpatialRadius]*Spatial[dy+SpatialRadius]));w=vbslq_f32(mask,w,zero);
   // Compensated accumulation keeps shallow-gradient tangent decisions stable.
   const auto add=vsubq_f32(vmulq_f32(w,v),sumError),nextSum=vaddq_f32(sum,add);
   sumError=vsubq_f32(vsubq_f32(nextSum,sum),add);sum=nextSum;
   const auto addWeight=vsubq_f32(w,weightError),nextWeight=vaddq_f32(weight,addWeight);
   weightError=vsubq_f32(vsubq_f32(nextWeight,weight),addWeight);weight=nextWeight;
  }
  float sums[4],weights[4];vst1q_f32(sums,sum);vst1q_f32(weights,weight);
  for(int k=0;k<4;++k){const float c=base[y*W+x+k];surface[y*W+x+k]=c>0?std::clamp(sums[k]/weights[k],c-MaxChange,c+MaxChange):0;}
 }
}
#endif
Repairs repair(std::vector<float> &depth,const uint8_t *native,int rotation,const RepairSettings &settings){
    Repairs result;Timer clock;
    if(settings.holeMode==0&&!settings.removeOutliers)return result;
    if(!native)throw std::runtime_error("LiteSR repair requires the same complete native measurement");
    constexpr int C=48,R=32,M=C*R;
    std::array<float,M> measured{},low{},sum{};
    std::array<int,M> count{},kind{},group{};
    std::vector<int> owner(N);
    for(int i=0;i<M;++i){
        const auto *p=native+4*i;const int valid=p[3],snr=p[2];
        if(valid>1)throw std::runtime_error("LiteSR repair rejected a nonbinary native valid bit");
        const float z=(uint16_t(p[0])|(uint16_t(p[1])<<8))*.00025f;
        const bool successful=valid&&snr>=6&&z>0;
        if(successful){measured[i]=z;if(z>1.4f)kind[i]=3;}
        else kind[i]=1;
    }
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        // Match nearest eligibility, including inverse restoration. W48 ->
        // W256 is noninteger, so 180 degrees changes the owner phases.
        const int nx=rotation?C-1-(W-1-x)*C/W:x*C/W;
        const int ny=rotation?R-1-(H-1-y)*R/H:y*R/H;
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
        // A near return with no model support is not an outlier repair target.
        // Preserve existing positive predictions, including values below 0.1 m.
        for(int i=0;i<M;++i)if(measured[i]>0&&!(measured[i]<.1f&&count[i]==0)){
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
    for(int axis=0;axis<2;++axis)for(int line=0;line<(axis?W:H);++line){
        const int length=axis?H:W;
        const auto index=[&](int t){return axis?t*W+line:line*W+t;};
        int begin=0;
        while(begin<length){const int g=target[index(begin)];if(!g){++begin;continue;}
            int end=begin+1;while(end<length&&target[index(end)]==g)++end;
            const bool a=begin>0&&source(index(begin-1),g),b=end<length&&source(index(end),g);
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
    result.ms=clock.ms();return result;
}
}
struct Result {std::vector<float> depth;std::vector<uint8_t> coverage;Repairs repairs;int valid=0,changed=0,feathered=0;double totalMs=0,maxChange=0,bilateralMs=0,tangentMs=0,coverageMs=0;};
template<bool Fast=false> inline Result refineImpl(const float* input,const uint8_t* native,int rotation,const RepairSettings& settings){
 if(!input||(rotation!=0&&rotation!=180)||settings.holeMode<0||settings.holeMode>2)
  throw std::runtime_error("Invalid contour grid/settings/orientation");
 Timer clock;
 std::vector<float> base(input,input+N);
 int valid=0;
 for(float z:base){if(!std::isfinite(z)||z<0||z>1.4f)throw std::runtime_error("Invalid radial depth");valid+=z>0;}
 const auto repaired=repair(base,native,rotation,settings);
 const double filterBegin=clock.ms();
 // Repair scratch is already released. Reuse the bilateral padding allocation
 // for the next tangent pass, which only needs the unpadded output grid.
 std::vector<float> surface(N),next(Fast?PaddedWidth*PaddedHeight:N);
 if constexpr(Fast){for(int y=0;y<PaddedHeight;++y)for(int x=0;x<PaddedWidth;++x)next[y*PaddedWidth+x]=base[at(x-SpatialRadius,y-SpatialRadius)];}
 // Interpolated reciprocal LUT replaces millions of divisions. No changes to
 // the 7x7 footprint, layer exclusion, four tangent passes or 12 mm bound.
 static const auto rangeWeight=[](){std::array<float,4098> values{};for(int i=0;i<4098;++i){const double d=(double(i)*.04/4096)/RangeSigma;values[i]=float(1/(1+d*d));}return values;}();
    // Current-frame normalized bilateral interpolation: suppress surface noise,
    // but use the immutable LiteSR depth as the range guide on every pass.
#if defined(__ARM_NEON)
    if constexpr(Fast)bilateralNeon(base,next,surface);else
#endif
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        const int i=y*W+x;const float centre=base[i];if(centre==0)continue;
        typename std::conditional<Fast && W==256,float,double>::type sum=0,weight=0;
        for(int dy=-SpatialRadius;dy<=SpatialRadius;++dy)for(int dx=-SpatialRadius;dx<=SpatialRadius;++dx){
            const float v=Fast?next[(y+dy+SpatialRadius)*PaddedWidth+x+dx+SpatialRadius]:base[at(x+dx,y+dy)],difference=v-centre;
            if(v==0||std::abs(difference)>LayerGuard)continue;
            if constexpr(Fast){
                const float position=std::abs(difference)*102400.f;const int k=std::min(int(position),4096);
                const float reciprocal=rangeWeight[k]+(rangeWeight[k+1]-rangeWeight[k])*(position-k);
                const float w=float(Spatial[dx+SpatialRadius]*Spatial[dy+SpatialRadius])*reciprocal;sum+=w*v;weight+=w;
            }else{
                const double d=difference/RangeSigma;
                const double w=double(Spatial[dx+SpatialRadius]*Spatial[dy+SpatialRadius])/(1+d*d);sum+=w*v;weight+=w;
            }
        }
        surface[i]=std::clamp(float(sum/weight),centre-MaxChange,centre+MaxChange);
    }
    const double bilateralMs=clock.ms()-filterBegin,tangentBegin=clock.ms();
    next.resize(N);
    // Tangential diffusion rounds stair steps in isodepth contours instead of
    // blurring straight across foreground/background boundaries. Four bounded
    // subpixel steps retain planes, thin supports and the exact validity mask.
    for(int pass=0;pass<4;++pass){
        next=surface;
        for(int y=1;y<H-1;++y)for(int x=1;x<W-1;++x){
            const int i=y*W+x;const float c=surface[i];if(c==0)continue;
            const float l=surface[i-1],r=surface[i+1],u=surface[i-W],d=surface[i+W];
            if(l==0||r==0||u==0||d==0)continue;
            const float gx=(r-l)*.5f,gy=(d-u)*.5f;
            const float length=Fast?std::sqrt(gx*gx+gy*gy):std::hypot(gx,gy);
            if(length*PixelScale<.0001f)continue;
            const float tx=-gy/length*(1.25f*PixelScale),ty=gx/length*(1.25f*PixelScale);
            float a=0,b=0;
            if(!sample<Fast>(surface,x+tx,y+ty,base[i],a)||!sample<Fast>(surface,x-tx,y-ty,base[i],b))continue;
            next[i]=std::clamp(.5f*c+.25f*(a+b),base[i]-MaxChange,base[i]+MaxChange);
        }
        surface.swap(next);
    }
    const double tangentMs=clock.ms()-tangentBegin,coverageBegin=clock.ms();
    std::vector<uint16_t> horizontal;
    if constexpr(Fast){horizontal.resize(N);for(int y=0;y<H;++y)for(int x=0;x<W;++x){int occupied=0;for(int dx=-MaskRadius;dx<=MaskRadius;++dx)if(base[at(x+dx,y)]>0)occupied+=MaskKernel[dx+MaskRadius];horizontal[y*W+x]=uint16_t(occupied);}}
    Result result;result.depth=std::move(surface);result.coverage.resize(N);result.repairs=repaired;
    int changed=0,feathered=0;double total=0,maximum=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        const int i=y*W+x;const float z=base[i]>0?std::clamp(result.depth[i],std::numeric_limits<float>::min(),1.4f):0;
        result.depth[i]=z;
        if(base[i]==0)continue;
        const double delta=std::abs(double(z)-base[i]);total+=delta;maximum=std::max(maximum,delta);changed+=delta>1e-6;
        int occupied=0;
        if constexpr(Fast){for(int dy=-MaskRadius;dy<=MaskRadius;++dy)occupied+=horizontal[at(x,y+dy)]*MaskKernel[dy+MaskRadius];}
        else {for(int dy=-MaskRadius;dy<=MaskRadius;++dy)for(int dx=-MaskRadius;dx<=MaskRadius;++dx)
            if(base[at(x+dx,y+dy)]>0)occupied+=MaskKernel[dx+MaskRadius]*MaskKernel[dy+MaskRadius];}
        const int alpha=(occupied*255+MaskNormalization/2)/MaskNormalization;
        result.coverage[i]=uint8_t(alpha);feathered+=alpha<255;
    }

 result.valid=valid+repaired.newSupportPixels;result.changed=changed;result.feathered=feathered;
 result.bilateralMs=bilateralMs;result.tangentMs=tangentMs;result.coverageMs=clock.ms()-coverageBegin;
 result.totalMs=clock.ms();result.maxChange=maximum;return result;
}
inline Result refine(const float* input,const uint8_t* native,int rotation,const RepairSettings& settings){return refineImpl<false>(input,native,rotation,settings);}
inline Result refineFast(const float* input,const uint8_t* native,int rotation,const RepairSettings& settings){return refineImpl<true>(input,native,rotation,settings);}
