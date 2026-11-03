#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace LiteSrBoard {
#if defined(LITESR_NEAR_MODEL) && LITESR_NEAR_MODEL
constexpr bool NearModel=true;
constexpr float TrainingMinimum=.01f;
#else
constexpr bool NearModel=false;
constexpr float TrainingMinimum=.1f;
#endif
#if defined(LITESR_HIGH_RESOLUTION) && LITESR_HIGH_RESOLUTION
constexpr bool HighResolution=true;
constexpr int Work=128,Output=512;
#else
constexpr bool HighResolution=false;
constexpr int Work=64,Output=256;
#endif
constexpr int NativeW=48,NativeH=32,NativeN=1536;
constexpr float DepthMean=1.9940122413254844f,DepthStd=2.578946957776752f;
constexpr float IntensityMean=.32854884130555023f,IntensityStd=.2716072995236189f;
// Live depth publication does not consume the upsampled intensity base.
struct DepthPrepared {
 std::array<float,Work*Work> depthWork{},intensityWork{};
 std::array<float,Output*Output> depthBase{};
 std::array<uint8_t,Output*Output> eligible{};
 int validNative=0;
};
struct Prepared:DepthPrepared {std::array<float,Output*Output> intensityBase{};};
inline void resize(const float* value,const uint8_t* valid,int size,float* out){
 for(int y=0;y<size;++y){
  const float fy=std::max(0.f,(y+.5f)*(float(NativeH)/size)-.5f);
  const int y0=int(fy),y1=std::min(y0+1,NativeH-1);const float wy=fy-y0;
  for(int x=0;x<size;++x){
   const float fx=std::max(0.f,(x+.5f)*(float(NativeW)/size)-.5f);
   const int x0=int(fx),x1=std::min(x0+1,NativeW-1);const float wx=fx-x0;
   const int a=y0*NativeW+x0,b=y0*NativeW+x1,c=y1*NativeW+x0,d=y1*NativeW+x1;
   const float top=(1-wx)*value[a]+wx*value[b],bottom=(1-wx)*value[c]+wx*value[d];
   const float numerator=(1-wy)*top+wy*bottom;
   const float denominator=(1-wy)*((1-wx)*valid[a]+wx*valid[b])+wy*((1-wx)*valid[c]+wx*valid[d]);
   out[y*size+x]=denominator>1e-6f?numerator/denominator:0.f;
  }
 }
}
inline void prepareImpl(const uint8_t* native,const uint16_t* words,int rotation,float white,DepthPrepared& p,float* intensityBase){
 if((rotation!=0&&rotation!=180)||!std::isfinite(white)||white<1||white>8388352)
  throw std::runtime_error("Invalid LiteSR input orientation or fixed white count");
 std::array<float,NativeN> depth{},intensity{};std::array<uint8_t,NativeN> dv{},iv{};iv.fill(1);
 p.validNative=0;
 for(int i=0;i<NativeN;++i){
  const int j=rotation?NativeN-1-i:i;const auto* s=native+4*j;
  if(s[3]>1)throw std::runtime_error("Nonbinary native validity");
  const unsigned uq=s[0]|(unsigned(s[1])<<8);const float z=uq*.00025f;
  dv[i]=s[3]&&s[2]>=6&&uq&&z>=TrainingMinimum&&z<=1.4f;p.validNative+=dv[i]!=0;
  if(dv[i])depth[i]=(z-DepthMean)/DepthStd;
  const unsigned word=words[j],counts=(word&0x8000)?(word&0x7fff)*256:word;
  intensity[i]=(std::min(float(counts)/white,1.f)-IntensityMean)/IntensityStd;
 }
 resize(depth.data(),dv.data(),Work,p.depthWork.data());
 resize(intensity.data(),iv.data(),Work,p.intensityWork.data());
 resize(depth.data(),dv.data(),Output,p.depthBase.data());
 if(intensityBase)resize(intensity.data(),iv.data(),Output,intensityBase);
 for(int y=0;y<Output;++y)for(int x=0;x<Output;++x)p.eligible[y*Output+x]=dv[(y*NativeH/Output)*NativeW+x*NativeW/Output];
}
inline void prepareDepth(const uint8_t* native,const uint16_t* words,int rotation,float white,DepthPrepared& p){
 prepareImpl(native,words,rotation,white,p,nullptr);
}
inline void prepare(const uint8_t* native,const uint16_t* words,int rotation,float white,Prepared& p){
 prepareImpl(native,words,rotation,white,p,p.intensityBase.data());
}
inline float publishValue(const DepthPrepared& p,int i,float residual,bool allowPositiveBelowTrainingRange){
 const float z=(p.depthBase[i]+residual)*DepthStd+DepthMean;
 const bool good=p.eligible[i]&&std::isfinite(z)&&(NearModel?z>=TrainingMinimum:(allowPositiveBelowTrainingRange?z>0:z>=TrainingMinimum))&&z<=1.4f;
 return good?z:0;
}
inline int publish(const DepthPrepared& p,const float* residual,int rotation,float* out,bool allowPositiveBelowTrainingRange=false){
 int valid=0;
 for(int i=0;i<Output*Output;++i){
  const float z=publishValue(p,i,residual[i],allowPositiveBelowTrainingRange);
  out[rotation?Output*Output-1-i:i]=z;valid+=z>0;
 }
 return valid;
}
}
