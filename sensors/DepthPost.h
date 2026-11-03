#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdint>

// Final camera-Z metres only. Half-pixel, replicated borders, positive finite
// support renormalization. No confidence is created; unsupported output is zero.
namespace depthpost {
struct Axis { int lo,hi; float f; };
class Bilinear {
 std::vector<Axis> xs,ys;
 mutable std::vector<uint8_t> valid,support;
 static std::vector<Axis> axis(int n,int src) {
  std::vector<Axis> a(n);
  for(int i=0;i<n;++i){double p=std::max(0.,std::min(double(src-1),(i+.5)*src/n-.5));int l=int(p);a[i]={l,std::min(l+1,src-1),float(p-l)};}return a;
 }
public:
 int width=0,height=0;
 void configure(int w,int h){if(w==width&&h==height)return;if(!((w==144&&h==81)||(w==288&&h==162)||(w==384&&h==256)||(w==576&&h==324)))throw std::runtime_error("Unsupported depth size");width=w;height=h;xs=axis(w,144);ys=axis(h,81);valid.resize(11664);support.resize(11664);}
 void run(const float* src,float* dst,const float* guideLow=nullptr,const float* guideHigh=nullptr,const float* a=nullptr,const float* b=nullptr)const {
  if(!(a&&guideLow&&guideHigh)){
   // Validity belongs to the 144x81 measurements, not to every enlarged
   // output pixel. Reuse four-bit support without changing weights or sums.
   for(int i=0;i<11664;++i)valid[i]=std::isfinite(src[i])&&src[i]>0;
   for(int y=0;y<81;++y)for(int x=0;x<144;++x){int i=y*144+x,j=std::min(y+1,80)*144+x,dx=x<143?1:0;
    support[i]=valid[i]|(valid[i+dx]<<1)|(valid[j]<<2)|(valid[j+dx]<<3);
   }
   for(int y=0;y<height;++y){const auto Y=ys[y];const float* top=src+Y.lo*144;const float* bottom=src+Y.hi*144;
    for(int x=0;x<width;++x){const auto X=xs[x];const unsigned mask=support[Y.lo*144+X.lo];
     if(!mask){dst[y*width+x]=0;continue;}
     const float w0=(1-X.f)*(1-Y.f),w1=X.f*(1-Y.f),w2=(1-X.f)*Y.f,w3=X.f*Y.f;
     float sum=0,weight=0;
     if(mask==15){sum+=w0*top[X.lo];sum+=w1*top[X.hi];sum+=w2*bottom[X.lo];sum+=w3*bottom[X.hi];weight+=w0;weight+=w1;weight+=w2;weight+=w3;}
     else{
      if((mask&1)&&w0>0){sum+=w0*top[X.lo];weight+=w0;}
      if((mask&2)&&w1>0){sum+=w1*top[X.hi];weight+=w1;}
      if((mask&4)&&w2>0){sum+=w2*bottom[X.lo];weight+=w2;}
      if((mask&8)&&w3>0){sum+=w3*bottom[X.hi];weight+=w3;}
     }
     dst[y*width+x]=weight>0?sum/weight:0;
    }
   }
   return;
  }
  for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto X=xs[x],Y=ys[y];int ids[4]={Y.lo*144+X.lo,Y.lo*144+X.hi,Y.hi*144+X.lo,Y.hi*144+X.hi};float ws[4]={(1-X.f)*(1-Y.f),X.f*(1-Y.f),(1-X.f)*Y.f,X.f*Y.f};float sum=0,weight=0,lo=1e30f,hi=0,aa=0,bb=0;bool all=true;
   for(int k=0;k<4;++k){float z=src[ids[k]];bool ok=std::isfinite(z)&&z>0;all=all&&ok;if(ok&&ws[k]>0){sum+=ws[k]*z;weight+=ws[k];lo=std::min(lo,z);hi=std::max(hi,z);}if(a){aa+=ws[k]*a[ids[k]];bb+=ws[k]*b[ids[k]];}}
   float z=weight>0?sum/weight:0;
   // Guarded Fast Guided Filter: only all-valid, non-flat local support;
   // nonnegative local range prevents overshoot. Does not certify accuracy.
   if(a&&guideLow&&guideHigh&&all&&hi-lo>.02f){float q=aa*guideHigh[y*width+x]+bb;if(std::isfinite(q))z=std::max(lo,std::min(hi,q));}
   dst[y*width+x]=z;
  }
 }
};
// Low-resolution radius-2 valid-weighted linear regression, eps in [0,1] Y.
// Scratch allocated once and reused; no high-resolution coefficient images.
class Guided {
 std::vector<float> a,b;
public:
 std::vector<float> meanA,meanB;
 Guided():a(11664),b(11664),meanA(11664),meanB(11664){}
 void fit(const float* z,const float* g){
  for(int y=0;y<81;++y)for(int x=0;x<144;++x){double n=0,si=0,sp=0,sii=0,sip=0;
   for(int v=std::max(0,y-2);v<=std::min(80,y+2);++v)for(int u=std::max(0,x-2);u<=std::min(143,x+2);++u){int i=v*144+u;if(!(z[i]>0&&std::isfinite(z[i])))continue;++n;si+=g[i];sp+=z[i];sii+=g[i]*g[i];sip+=g[i]*z[i];}
   int i=y*144+x;a[i]=n?float((sip/n-si*sp/(n*n))/(std::max(0.,sii/n-si*si/(n*n))+.0025)):0;b[i]=n?float(sp/n-a[i]*si/n):0;
  }
  for(int y=0;y<81;++y)for(int x=0;x<144;++x){float sa=0,sb=0,n=0;for(int v=std::max(0,y-2);v<=std::min(80,y+2);++v)for(int u=std::max(0,x-2);u<=std::min(143,x+2);++u){sa+=a[v*144+u];sb+=b[v*144+u];++n;}meanA[y*144+x]=sa/n;meanB[y*144+x]=sb/n;}
 }
};
}
