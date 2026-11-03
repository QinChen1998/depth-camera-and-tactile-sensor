#pragma once
// Private application adapter for the SPI board. Native D/I from one v5
// measurement keeps its original order, range encoding and signal words.
#include "NativePacket.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace RvSpiNative48 {
constexpr std::size_t PacketBytes=10995;
inline LiteSrBoardNative::Frame decode(const uint8_t* source,std::size_t size,bool captureOnly){
 const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
 require(size==PacketBytes&&LiteSrBoardNative::read32(source)==0x39464d54&&source[4]==5&&source[5]==0
  &&source[6]==44&&source[7]==0&&LiteSrBoardNative::read32(source+8)==PacketBytes,"Native ToF fault: SPI v5 header");
 const bool m48=source[28]==32&&source[30]==48,m16=source[28]==16&&source[30]==16;
 require((m48||(captureOnly&&m16))&&!source[29]&&!source[31]&&source[32]==1&&!source[33],"Native ToF fault: SPI native grid/target");
 require(source[10994]==unsigned(!captureOnly),"Native ToF fault: SPI Signal must follow selected capture/model mode");
 require(!(LiteSrBoardNative::read32(source+36)&~8u)&&!(unsigned(source[7920])&~9u),"Native ToF fault: SPI warning/status outside observe-0x08 policy");
 std::array<uint8_t,7922> legacy{};std::memcpy(legacy.data(),source,legacy.size());legacy[4]=4;
 for(unsigned i=0;i<4;++i)legacy[8+i]=uint8_t(legacy.size()>>(8*i));
 auto result=LiteSrBoardNative::decode(legacy.data(),legacy.size(),captureOnly);
 result.spiSignalPresent=source[10994]!=0;
 if(result.spiSignalPresent)for(unsigned i=0;i<unsigned(result.columns)*result.rows;++i)
  result.spiSignal[i]=uint16_t(source[7922+2*i])|(uint16_t(source[7923+2*i])<<8);
 return result;
}
}
