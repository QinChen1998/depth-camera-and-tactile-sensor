#pragma once
#ifndef LITESR_NATIVE_FOV
#define LITESR_NATIVE_FOV 1
#endif
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace LiteSrBoardNative {
constexpr std::size_t PacketBytes=7922;
struct Frame {
 std::array<uint16_t,1536> spiSignal{};bool spiSignalPresent=false;
 std::array<uint8_t,6144> bytes{};uint32_t sequence=0,warnings=0;uint64_t stamp=0;uint8_t fov=0;
 uint16_t columns=48,rows=32;
 std::size_t byteCount() const{return std::size_t(columns)*rows*4;}
};
inline uint32_t read32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint64_t read64(const uint8_t* p){return read32(p)|(uint64_t(read32(p+4))<<32);}
inline Frame decode(const uint8_t* b,std::size_t size,bool allowM8=false){
 const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
 require(size==PacketBytes&&read32(b)==0x39464d54&&b[4]==4&&b[5]==0&&b[6]==44&&b[7]==0&&read32(b+8)==PacketBytes,"TMF9 v4 packet contract");
 // The frame-valid bit remains required. The existing observe-0x08 policy
 // treats 0x09 like 0x01 for acceptance; retain the real warning in metadata.
 const bool m8=b[28]==8&&b[30]==8,m16=b[28]==16&&b[30]==16;
 require(((b[28]==32&&b[30]==48)||(allowM8&&(m8||m16)))&&b[29]==0&&b[31]==0&&b[32]>0&&b[33]==0&&(unsigned(b[7920])&~8u)==1&&b[7921]==0,"TMF9 native mode/status");
 const unsigned flags=b[34]|(unsigned(b[35])<<8),warnings=read32(b+36);
 require((flags&1)&&!(warnings&~8u)&&(!(flags&2)||warnings),"Native ToF fault: observe-0x08 only");
 Frame result;result.sequence=read32(b+12);result.stamp=read64(b+16);result.warnings=warnings;result.fov=b[7916];
 result.rows=b[28];result.columns=b[30];
 require(result.sequence&&result.stamp&&result.fov==LITESR_NATIVE_FOV,"Native identity/current pinned FOV");
 for(unsigned i=0;i<unsigned(result.rows)*result.columns;++i){result.bytes[4*i]=b[3308+2*i];result.bytes[4*i+1]=b[3309+2*i];result.bytes[4*i+2]=b[6380+i];result.bytes[4*i+3]=(b[44+i/8]>>(i%8))&1;}
 return result;
}
}
