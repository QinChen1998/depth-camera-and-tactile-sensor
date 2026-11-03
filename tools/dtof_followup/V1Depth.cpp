#include "V1Depth.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace DtofV1 {
namespace {
using Mask=std::array<uint8_t,1536>;
using Field=std::array<double,1536>;
template<class F>void neighbours(int i,bool diagonal,F f){
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){int x=i%48+dx,y=i/48+dy;
        if((dx||dy)&&(diagonal||!dx||!dy)&&x>=0&&x<48&&y>=0&&y<32)f(y*48+x);}
}
std::vector<std::vector<int>> components(const Mask& mask,bool diagonal){
    Mask seen{};std::vector<std::vector<int>> groups;
    for(int i=0;i<1536;++i)if(mask[i]&&!seen[i]){std::vector<int> q{i};seen[i]=1;
        for(size_t j=0;j<q.size();++j)neighbours(q[j],diagonal,[&](int p){if(mask[p]&&!seen[p]){seen[p]=1;q.push_back(p);}});
        groups.push_back(std::move(q));}return groups;
}
Field gaussian(const Mask& mask,double sigma){
    int radius=int(4*sigma+.5);std::vector<double> k(2*radius+1);double sum=0;
    for(int i=-radius;i<=radius;++i)sum+=k[i+radius]=std::exp(-i*i/(2*sigma*sigma));
    for(auto& v:k)v/=sum;
    auto reflect=[](int i,int n){while(i<0||i>=n)i=i<0?-i-1:2*n-i-1;return i;};
    Field tmp{},out{};
    for(int y=0;y<32;++y)for(int x=0;x<48;++x)for(int j=-radius;j<=radius;++j)tmp[y*48+x]+=mask[y*48+reflect(x+j,48)]*k[j+radius];
    for(int y=0;y<32;++y)for(int x=0;x<48;++x)for(int j=-radius;j<=radius;++j)out[y*48+x]+=tmp[reflect(y+j,32)*48+x]*k[j+radius];
    return out;
}
void simplify(const std::vector<Point>& p,int a,int b,std::vector<Point>& out,double tolerance=.10){
    if(b<=a)return;auto v=p[b]-p[a];double length=std::hypot(v.x,v.y),distance=0;int split=a;
    for(int i=a+1;i<b;++i){auto u=p[i]-p[a];double d=length<1e-9?std::hypot(u.x,u.y):std::abs(v.x*u.y-v.y*u.x)/length;if(d>distance){distance=d;split=i;}}
    if(distance>tolerance){simplify(p,a,split,out,tolerance);simplify(p,split,b,out,tolerance);}else out.push_back(p[a]);
}
std::vector<std::vector<Point>> contours(const Field& field,bool bezier,double presentation=0){
    constexpr int W=50,H=34;constexpr double level=.43;
    std::array<double,W*H> z{};for(int y=0;y<32;++y)for(int x=0;x<48;++x)z[(y+1)*W+x+1]=field[y*48+x];
    struct Node{Point point;std::vector<int> next;};std::map<int,Node> nodes;
    auto edge=[&](int a,int b){int id=2*std::min(a,b)+(std::abs(b-a)==W);double t=(level-z[a])/(z[b]-z[a]);nodes[id].point={a%W+(b%W-a%W)*t-1,a/W+(b/W-a/W)*t-1};return id;};
    auto join=[&](int a,int b){nodes[a].next.push_back(b);nodes[b].next.push_back(a);};
    for(int y=0;y<H-1;++y)for(int x=0;x<W-1;++x){int a=y*W+x,b=a+1,c=b+W,d=a+W,p[4]={a,b,c,d},ids[4],n=0;
        for(int k=0;k<4;++k)if((z[p[k]]>=level)!=(z[p[(k+1)%4]]>=level))ids[n++]=edge(p[k],p[(k+1)%4]);
        if(n==2)join(ids[0],ids[1]);else if(n==4){bool centre=(z[a]+z[b]+z[c]+z[d])*.25>=level;
            if(centre==(z[a]>=level)){join(ids[0],ids[1]);join(ids[2],ids[3]);}else{join(ids[0],ids[3]);join(ids[1],ids[2]);}}}
    std::vector<std::vector<Point>> paths;std::map<int,bool> seen;
    for(const auto& e:nodes){int start=e.first;if(seen[start])continue;std::vector<Point> line;int prev=-1,at=start;
        do{if(seen[at]||nodes[at].next.size()!=2)break;seen[at]=true;line.push_back(nodes[at].point);int next=nodes[at].next[0]==prev?nodes[at].next[1]:nodes[at].next[0];prev=at;at=next;}while(at!=start&&line.size()<=nodes.size());
        if(at!=start||line.size()<3)continue;line.push_back(line.front());std::vector<Point> points;simplify(line,0,int(line.size())-1,points,.10+.20*presentation);if(points.size()<3)continue;
        for(auto& p:points)p=p*8+Point{3.5,3.5};std::vector<Point> poly;int n=int(points.size());
        for(int i=0;i<n;++i){auto p0=points[(i+n-1)%n],p1=points[i],p2=points[(i+1)%n],p3=points[(i+2)%n];if(presentation>0&&bezier){
                // Bound corner rounding to at most half a native cell for
                // the middle preset. Long edges must not amplify shrinkage.
                const double trim=3+2*presentation;
                double l0=std::hypot(p0.x-p1.x,p0.y-p1.y),l1=std::hypot(p2.x-p1.x,p2.y-p1.y);
                auto start=p1+(p0-p1)*std::min(.5,trim/std::max(l0,1e-9));
                auto end=p1+(p2-p1)*std::min(.5,trim/std::max(l1,1e-9));
                auto c1=start+(p1-start)*(2./3),c2=end+(p1-end)*(2./3);
                double length=std::hypot(c1.x-start.x,c1.y-start.y)+std::hypot(c2.x-c1.x,c2.y-c1.y)+std::hypot(end.x-c2.x,end.y-c2.y);
                int steps=std::clamp(int(std::ceil(length*2)),2,256);
                for(int j=0;j<steps;++j){double t=double(j)/steps,u=1-t;poly.push_back(start*(u*u*u)+c1*(3*u*u*t)+c2*(3*u*t*t)+end*(t*t*t));}
                continue;
            }
            poly.push_back(p1);
            if(bezier){auto c1=p1+(p2-p0)*.125,c2=p2-(p3-p1)*.125;
                // Bounded subdivision at <= 0.5 output-pixel control-polygon spacing.
                double length=std::hypot(c1.x-p1.x,c1.y-p1.y)+std::hypot(c2.x-c1.x,c2.y-c1.y)+std::hypot(p2.x-c2.x,p2.y-c2.y);
                int steps=std::clamp(int(std::ceil(length*2)),2,256);
                for(int j=1;j<steps;++j){double t=double(j)/steps,u=1-t;poly.push_back(p1*(u*u*u)+c1*(3*u*u*t)+c2*(3*u*t*t)+p2*(t*t*t));}}}
        paths.push_back(std::move(poly));}return paths;
}
void raster(const std::vector<std::vector<Point>>& paths,std::array<uint8_t,DtofM0::P>& coverage){
    coverage.fill(0);
    for(int y=0;y<256;++y)for(int sy=0;sy<2;++sy){double py=y-.25+sy*.5;std::vector<double> crossings;
        for(const auto& p:paths)for(size_t i=0;i<p.size();++i){auto a=p[i],b=p[(i+1)%p.size()];if((a.y>py)!=(b.y>py))crossings.push_back(a.x+(py-a.y)*(b.x-a.x)/(b.y-a.y));}
        std::sort(crossings.begin(),crossings.end());
        for(size_t j=0;j+1<crossings.size();j+=2){int a=std::max(0,int(std::floor(crossings[j]))),b=std::min(383,int(std::ceil(crossings[j+1])));
            for(int x=a;x<=b;++x)for(int sx=0;sx<2;++sx)if(x-.25+sx*.5>=crossings[j]&&x-.25+sx*.5<crossings[j+1])++coverage[y*384+x];}}
    for(auto& c:coverage)c=uint8_t(c*255/4);
}
bool pathClear(int owner,int target,double reference,const Mask& blocked,const Mask& near,const DtofM0::Frame& f,double layer){
    int dx=target%48-owner%48,dy=target/48-owner/48,steps=2*std::max(std::abs(dx),std::abs(dy));
    for(int k=1;k<steps;++k){double x=owner%48+double(dx)*k/steps,y=owner/48+double(dy)*k/steps;
        for(int yy=int(std::floor(y));yy<=int(std::ceil(y));++yy)for(int xx=int(std::floor(x));xx<=int(std::ceil(x));++xx){int i=yy*48+xx;if(blocked[i]||(near[i]&&std::abs(f.uq[i]*.00025-reference)>layer+1e-12))return false;}}
    return true;
}
// Same sample positions and parity rule as raster(), with edges bucketed into
// only the subpixel rows that they actually cross. Original V1 stays unchanged.
void rasterFast(const std::vector<std::vector<Point>>& paths,std::array<uint8_t,DtofM0::P>& coverage){
    std::array<std::vector<double>,512> rows;
    for(const auto& p:paths)for(size_t i=0;i<p.size();++i){auto a=p[i],b=p[(i+1)%p.size()];
        int first=std::max(0,int(std::floor(2*std::min(a.y,b.y)+.5))-1),last=std::min(511,int(std::ceil(2*std::max(a.y,b.y)+.5))+1);
        for(int row=first;row<=last;++row){double py=row*.5-.25;if((a.y>py)!=(b.y>py))rows[row].push_back(a.x+(py-a.y)*(b.x-a.x)/(b.y-a.y));}}
    coverage.fill(0);
    for(int row=0;row<512;++row){auto& crossings=rows[row];std::sort(crossings.begin(),crossings.end());
        for(size_t j=0;j+1<crossings.size();j+=2){int a=std::max(0,int(std::floor(crossings[j]))),b=std::min(383,int(std::ceil(crossings[j+1])));
            for(int x=a;x<=b;++x)for(int sx=0;sx<2;++sx)if(x-.25+sx*.5>=crossings[j]&&x-.25+sx*.5<crossings[j+1])++coverage[row/2*384+x];}}
    for(auto& c:coverage)c=uint8_t(c*255/4);
}
void fastDepth(Result& out,const DtofM0::Frame& f,const Mask& near,const Mask& filled,const Mask& blocked,const Mask& keepVoid,const Mask& hard,const Settings& s){
    std::array<float,1536> grid{};std::array<int,1536> label{};int group=0;
    for(const auto& component:components(filled,false)){++group;for(int i:component)label[i]=group;}
    // Multi-source flood fills only eligible enclosed holes, using CURRENT
    // measured seeds. Explicit background, open gaps and large holes are walls.
    std::vector<int> queue;queue.reserve(1536);
    for(int i=0;i<1536;++i)if(near[i]){grid[i]=float(f.uq[i]*.00025);queue.push_back(i);}
    for(size_t at=0;at<queue.size();++at){int i=queue[at];neighbours(i,false,[&](int j){if(filled[j]&&!grid[j]&&!blocked[j]&&label[j]==label[i]){grid[j]=grid[i];queue.push_back(j);}});}
    // One-cell exterior support keeps the Bezier boundary smooth. This is a
    // single pass from the filled grid, never recursive growth into background.
    if(s.presentation>0&&s.interpolation==0){
        // Low-cost 3x3 current-frame surface smoothing, within the original
        // component and depth-layer guard. Native input samples stay intact.
        auto smooth=grid;
        for(int i=0;i<1536;++i)if(grid[i]){
            double sum=4*grid[i],weight=4;
            neighbours(i,true,[&](int j){if(!grid[j]||label[j]!=label[i]||std::abs(grid[j]-grid[i])>s.layerM)return;
                double dz=(grid[j]-grid[i])/.03;int dx=j%48-i%48,dy=j/48-i/48;
                double w=((dx&&dy)?1.:2.)/(1+dz*dz);sum+=w*grid[j];weight+=w;});
            smooth[i]=float(grid[i]*(1-.5*s.presentation)+(sum/weight)*(.5*s.presentation));
        }
        grid=smooth;
    }
    auto extended=grid;auto labels=label;
    for(int i=0;i<1536;++i)if(!grid[i]&&(s.presentation>0?!hard[i]:!blocked[i]&&!keepVoid[i])){int best=3,seed=-1;neighbours(i,true,[&](int j){int dx=j%48-i%48,dy=j/48-i/48,d=dx*dx+dy*dy;if(grid[j]&&d<best){best=d;seed=j;}});if(seed>=0){extended[i]=grid[seed];labels[i]=label[seed];}}
    // The 8x expansion has 64 exact binary phases. A native owner's layer,
    // component and invalid-support gates are shared by its four quadrants.
    // Keep the original 00,10,01,11 accumulation order and double arithmetic.
    static const auto weights=[](){std::array<std::array<double,4>,64> w{};
        for(int py=0;py<8;++py)for(int px=0;px<8;++px){
            double tx=(px<4?px+4.5:px-3.5)/8,ty=(py<4?py+4.5:py-3.5)/8;
            w[py*8+px]={{(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty}};
        }return w;}();
    for(int owner=0;owner<1536;++owner){
        const int ox=owner%48,oy=owner/48,base=oy*8*384+ox*8;
        const float reference=extended[owner];
        if((s.presentation>0?hard[owner]:blocked[owner]||keepVoid[owner])||!reference){
            for(int py=0;py<8;++py)std::fill_n(out.coverage.data()+base+py*384,8,uint8_t(0));
            continue;
        }
        if(s.interpolation==1){
            for(int py=0;py<8;++py)for(int px=0;px<8;++px){int p=base+py*384+px;if(out.coverage[p])out.depth[p]=reference;}
            continue;
        }
        struct Support {std::array<float,4> z{};unsigned mask=0;};
        std::array<Support,4> support{};
        for(int q=0;q<4;++q)for(int k=0;k<4;++k){
            int xx=ox-1+q%2+k%2,yy=oy-1+q/2+k/2;
            if(xx<0||xx>=48||yy<0||yy>=32)continue;
            int i=yy*48+xx;
            if(!extended[i]||(s.presentation>0?hard[i]:blocked[i]||keepVoid[i])||labels[i]!=labels[owner]||std::abs(extended[i]-reference)>s.layerM+1e-7)continue;
            support[q].z[k]=extended[i];support[q].mask|=1u<<k;
        }
        for(int py=0;py<8;++py)for(int px=0;px<8;++px){
            int p=base+py*384+px;if(!out.coverage[p])continue;
            const auto& g=support[(py/4)*2+px/4];const auto& w=weights[py*8+px];
            double sum=0,total=0;
            for(int k=0;k<4;++k)if(g.mask&(1u<<k)){sum+=w[k]*g.z[k];total+=w[k];}
            // No eligible neighbour still falls back to the current owner.
            out.depth[p]=total>1e-9?float(sum/total):reference;
        }
    }
}
}
Processor::Processor(Settings settings):s(settings){
    if(!std::isfinite(s.presentation)||s.presentation<0||s.presentation>1)throw std::invalid_argument("Invalid presentation smoothing");
    if(s.interpolation<0||s.interpolation>1)throw std::invalid_argument("Invalid fast interpolation");
    if(!std::isfinite(s.nearM)||s.nearM<.1||s.nearM>1.4||!std::isfinite(s.sigma)||s.sigma<.1||s.sigma>2||!std::isfinite(s.currentWeight)||s.currentWeight<.1||s.currentWeight>1||s.minimumComponent<1||s.minimumComponent>64||s.fillHoles<0||s.fillHoles>64||!std::isfinite(s.layerM)||s.layerM<.005||s.layerM>.2||!std::isfinite(s.sigmaM)||s.sigmaM<.005||s.sigmaM>.15||!std::isfinite(s.spreadM)||s.spreadM<0||s.spreadM>.2)throw std::invalid_argument("Invalid V1 parameters");
}
void Processor::reset(){out={};field={};sequence=0;capture=0;received=lastHost=-1;identity.clear();history=false;}
const Result& Processor::expire(int64_t t){if(t<0||(lastHost>=0&&t<lastHost)){reset();return out;}lastHost=t;if(received>=0&&t-received>=220){out={};field={};history=false;}return out;}
const Result& Processor::process(const DtofM0::Frame& f,const std::array<uint8_t,1536>& snr,int64_t t,bool withDepth){
    if(t<0||!f.sequence||f.captureNs<=0||f.identity.empty()){reset();return out;}expire(t);if(identity!=f.identity){reset();identity=f.identity;}
    if(f.sequence<=sequence)return out;
    if(capture&&(f.captureNs<=capture||f.captureNs-capture>=500000000||t-received>=500)){field={};history=false;}
    sequence=f.sequence;capture=f.captureNs;received=lastHost=t;out={};Mask near{},hard{},blocked{};
    // Only mode 9 accepts every positive native range. Original V1 keeps its
    // 100 mm floor; zero/no-return and contradictory slots keep their policy.
    const int minimumUq=s.fast?1:400;
    for(int i=0;i<1536;++i){auto u=f.uq[i];bool v=f.valid[i]!=0;hard[i]=v&&((u&&(u<minimumUq||u>5600))||(!u&&snr[i])||(u>=minimumUq&&u<=5600&&!snr[i]));near[i]=v&&u>=minimumUq&&u<=std::lround(s.nearM*4000)&&snr[i];blocked[i]=hard[i]||(v&&u>std::lround(s.nearM*4000));}
    for(const auto& c:components(near,true))if(int(c.size())<s.minimumComponent)for(int i:c)near[i]=0;
    Mask filled=near,holes{},outside{};for(int i=0;i<1536;++i)outside[i]=!near[i];
    for(const auto& c:components(outside,false)){bool border=false;for(int i:c)border|=i%48==0||i%48==47||i/48==0||i/48==31;if(!border)for(int i:c)holes[i]=1;}
    Mask keepVoid{};
    for(const auto& c:components(holes,true)){
        bool eligible=int(c.size())<=s.fillHoles;
        if(s.fast)for(int i:c)if(blocked[i])eligible=false;
        for(int i:c)if(eligible)filled[i]=1;else if(s.fast)keepVoid[i]=1;
    }
    auto current=gaussian(filled,s.sigma);Mask support=near;for(int i=0;i<1536;++i)if(near[i])neighbours(i,false,[&](int j){support[j]=1;});
    if(history)for(int i=0;i<1536;++i)if(support[i])current[i]=s.currentWeight*current[i]+(1-s.currentWeight)*field[i];
    for(int i=0;i<1536;++i)if(hard[i])current[i]=0;field=current;history=std::any_of(near.begin(),near.end(),[](auto v){return v!=0;});if(!history)field={};
    out.contours=contours(field,s.bezier,s.fast?s.presentation:0);if(s.fast)rasterFast(out.contours,out.coverage);else raster(out.contours,out.coverage);out.fresh=true;
    if(s.fast&&withDepth){fastDepth(out,f,near,filled,blocked,keepVoid,hard,s);return out;}
    // Exactly the same 5x5 support and nearest current seed as the direct
    // implementation. Reuse cell-level path/layer gates across its 8x8 pixels;
    // factor the Gaussian into spatial and radial terms. No change to history.
    static const auto spatial=[](){std::array<std::array<double,25>,64> weights{};
        for(int py=0;py<8;++py)for(int px=0;px<8;++px)for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){double x=dx-(px-3.5)/8,y=dy-(py-3.5)/8;weights[py*8+px][(dy+2)*5+dx+2]=std::exp(-(x*x+y*y)/2);}return weights;}();
    for(int owner=0;owner<1536;++owner){
        bool active=false;for(int py=0;py<8;++py)for(int px=0;px<8;++px)active|=out.coverage[(owner/48*8+py)*384+owner%48*8+px]!=0;if(!active)continue;
        if(hard[owner]||(withDepth&&blocked[owner])){for(int py=0;py<8;++py)for(int px=0;px<8;++px)out.coverage[(owner/48*8+py)*384+owner%48*8+px]=0;continue;}if(!withDepth)continue;
        int ids[25],offsets[25],count=0;double values[25];bool seedOk[25];
        for(int yy=std::max(0,owner/48-2);yy<=std::min(31,owner/48+2);++yy)for(int xx=std::max(0,owner%48-2);xx<=std::min(47,owner%48+2);++xx){int i=yy*48+xx;if(!near[i])continue;ids[count]=i;offsets[count]=(yy-owner/48+2)*5+xx-owner%48+2;values[count]=f.uq[i]*.00025;seedOk[count]=pathClear(owner,i,values[count],blocked,near,f,s.layerM);++count;}
        double radial[25][25]{};bool valid[25]{},computed[25]{};int ownSeed=-1;
        for(int a=0;a<count;++a)if(ids[a]==owner)ownSeed=a;
        auto prepareSeed=[&](int a){double lo=2,hi=0;int supports=0;
            for(int b=0;b<count;++b){double dz=values[b]-values[a];if(std::abs(dz)>s.layerM+1e-12||!pathClear(owner,ids[b],values[a],blocked,near,f,s.layerM))continue;
                radial[a][b]=std::exp(-dz*dz/(2*s.sigmaM*s.sigmaM));lo=std::min(lo,values[b]);hi=std::max(hi,values[b]);++supports;}
            valid[a]=(supports>=2||near[owner])&&hi-lo<=s.spreadM+1e-12;computed[a]=true;};
        for(int py=0;py<8;++py)for(int px=0;px<8;++px){int p=(owner/48*8+py)*384+owner%48*8+px;if(!out.coverage[p])continue;double best=1e10;int seed=-1;
            // Pixel centres lie strictly inside the owner's Voronoi cell.
            if(ownSeed>=0){seed=ownSeed;best=((px-3.5)*(px-3.5)+(py-3.5)*(py-3.5))/64;}
            else for(int a=0;a<count;++a)if(seedOk[a]){double dx=ids[a]%48-owner%48-(px-3.5)/8,dy=ids[a]/48-owner/48-(py-3.5)/8,d=dx*dx+dy*dy;if(d<best){best=d;seed=a;}}
            if(seed<0||best>2.25){out.coverage[p]=0;continue;}if(!computed[seed])prepareSeed(seed);
            if(!valid[seed]){out.coverage[p]=0;continue;}double sum=0,total=0;
            for(int b=0;b<count;++b){double w=spatial[py*8+px][offsets[b]]*radial[seed][b];sum+=w*values[b];total+=w;}
            double v=total>1e-9?sum/total:0;if(!std::isfinite(v)||v<.1-1e-12||v>s.nearM+1e-12){out.coverage[p]=0;continue;}out.depth[p]=float(std::clamp(v,.1,s.nearM));
        }
    }return out;
}
}
