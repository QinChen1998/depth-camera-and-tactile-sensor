#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>

namespace DtofBoard {
constexpr int Pixels=384*256;
struct Packed {std::vector<uint8_t> depth,coverage;bool runs=false;};
inline void put32(std::vector<uint8_t>& b,uint32_t v){for(int k=0;k<4;++k)b.push_back(uint8_t(v>>(k*8)));}
inline uint32_t get32(const uint8_t* b){return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);}
inline Packed pack(const float* depth,const uint8_t* coverage,bool metric){
    Packed out;out.runs=true;
    for(int i=0;i<Pixels;){if(!coverage[i]){++i;continue;}int begin=i;while(i<Pixels&&coverage[i])++i;
        put32(out.coverage,begin);put32(out.coverage,i-begin);out.coverage.insert(out.coverage.end(),coverage+begin,coverage+i);
        if(metric){const auto* bytes=reinterpret_cast<const uint8_t*>(depth);out.depth.insert(out.depth.end(),bytes+begin*4,bytes+i*4);}
    }
    if(out.depth.size()+out.coverage.size()>=size_t(Pixels*(metric?5:1))){out.runs=false;out.coverage.assign(coverage,coverage+Pixels);if(metric){const auto* bytes=reinterpret_cast<const uint8_t*>(depth);out.depth.assign(bytes,bytes+Pixels*4);}}
    return out;
}
inline Packed unpack(const uint8_t* depth,size_t nd,const uint8_t* coverage,size_t nc,bool metric,bool runs){
    auto require=[](bool value){if(!value)throw std::invalid_argument("Malformed DTF2 payload");};
    require(nd<=Pixels*4&&nc<=Pixels*5&&nd+nc<=Pixels*5&&(!nd||metric));
    Packed out;out.depth.resize(metric?Pixels*4:0);out.coverage.resize(Pixels);
    if(!runs){require(nd==size_t(metric?Pixels*4:0)&&nc==Pixels);if(nd)std::memcpy(out.depth.data(),depth,nd);std::memcpy(out.coverage.data(),coverage,nc);return out;}
    size_t at=0,values=0;uint32_t previous=0;
    while(at<nc){require(nc-at>=8);uint32_t start=get32(coverage+at),count=get32(coverage+at+4);at+=8;
        require(count>0&&start>=previous&&start<Pixels&&count<=Pixels-start&&count<=nc-at);previous=start+count;
        for(uint32_t i=0;i<count;++i)require(coverage[at+i]>0);
        std::memcpy(out.coverage.data()+start,coverage+at,count);at+=count;
        if(metric){require(values+size_t(count)*4<=nd);std::memcpy(out.depth.data()+start*4,depth+values,count*4);values+=count*4;}
    }
    require(values==nd);return out;
}
}
