// Included inside the runner namespace. Latest-only real native D/I processing.
uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint64_t u64(const uint8_t* p){return u32(p)|(uint64_t(u32(p+4))<<32);}
void put32(uint8_t* p,uint32_t value){for(int i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
void put64(uint8_t* p,uint64_t value){for(int i=0;i<8;++i)p[i]=uint8_t(value>>(8*i));}
using Native=LiteSrBoardNative::Frame;
Native readNative(int fd,bool allowM8=false){
 std::array<uint8_t,RvSpiNative48::PacketBytes> b{};const auto n=recv(fd,b.data(),b.size(),MSG_TRUNC);
 if(n==RvSpiNative48::PacketBytes)return RvSpiNative48::decode(b.data(),std::size_t(n),allowM8);
 return LiteSrBoardNative::decode(b.data(),std::size_t(n),allowM8);
}
void io(int fd,void* data,size_t size,bool writing){
 char* p=static_cast<char*>(data);const double start=now();
 while(size){require(now()-start<500,"Bounded photon IO timeout");pollfd event{fd,short(writing?POLLOUT:POLLIN),0};
  const int ready=poll(&event,1,25);if(ready<0&&errno==EINTR)continue;require(ready>=0,"Photon poll");if(!ready)continue;
  const auto n=writing?send(fd,p,size,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,p,size,MSG_DONTWAIT);
  if(n<0&&(errno==EINTR||errno==EAGAIN))continue;require(n>0,"Photon connection closed");p+=n;size-=size_t(n);
 }
}
bool photon(const Native& native,const std::string& boot,std::array<uint16_t,1536>& words,int* format=nullptr){
 if(native.spiSignalPresent){
  if(native.columns!=48||native.rows!=32||boot.size()!=36||now()-native.stamp/1e6>=500||now()-native.stamp/1e6<0)return false;
  words=native.spiSignal;if(format)*format=9;return true;
 }
 int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);if(fd<0)return false;
 try{
  sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(8833);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  const int connected=connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a));
  require(connected==0||errno==EINPROGRESS,"Photon loopback connect");
  if(connected){pollfd event{fd,POLLOUT,0};require(poll(&event,1,500)>0,"Photon bounded connect");int error=0;socklen_t size=sizeof(error);require(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)==0&&error==0,"Photon connect result");}
  std::array<uint8_t,16> request{};put32(request.data(),0x31514850);put32(request.data()+4,native.sequence);put64(request.data()+8,native.stamp);io(fd,request.data(),request.size(),true);
  std::array<uint8_t,72> header{};io(fd,header.data(),header.size(),false);
  const auto signalBytes=native.byteCount()/2;
  require(u32(header.data())==0x31434850&&header[4]==1&&header[5]==0&&header[6]==0&&header[7]==0&&u32(header.data()+8)==signalBytes,"PHC1 format/measurement");
  require(u32(header.data()+12)==native.sequence&&u64(header.data()+16)==native.stamp,"PHC1 exact native identity");
  require(header[24]==native.columns&&header[25]==0&&header[26]==native.rows&&header[27]==0&&header[29]==native.fov,"PHC1 actual shape/FOV");
  require((header[28]&8)&&(header[28]&7)==1&&!(header[28]&0xc0)&&header[30]==0&&header[31]==0&&std::memcmp(header.data()+32,boot.data(),36)==0,"PHC1 signal presence/boot");
  require(now()-native.stamp/1e6>=0&&now()-native.stamp/1e6<500,"PHC1 freshness");
  std::array<uint8_t,3072> payload{};io(fd,payload.data(),signalBytes,false);for(unsigned i=0;i<unsigned(native.rows)*native.columns;++i)words[i]=payload[2*i]|(uint16_t(payload[2*i+1])<<8);
  if(format)*format=header[28];close(fd);return true;
 }catch(const std::exception&){close(fd);return false;}
}

