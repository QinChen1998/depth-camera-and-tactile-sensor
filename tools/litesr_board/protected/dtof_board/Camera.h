#pragma once
// Camera capture only, adapted from the read-only RV project; no model or ToF link.
// RGB-only V4L2 + existing Rockchip AIQ. No OpenCV, ToF or frame files.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <dlfcn.h>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif


namespace DtofRgb {
static volatile sig_atomic_t running=1;
static void quit(int){running=0;}
static uint64_t ns(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static void check(bool ok,const char* text){if(!ok)throw std::runtime_error(std::string(text)+": "+strerror(errno));}
static void call(int fd,unsigned long op,void* value){int r;do{r=ioctl(fd,op,value);}while(r<0&&errno==EINTR&&running);if(r<0)fprintf(stderr,"IOCTL_FAILED op=0x%lx\n",op);check(r>=0,"camera ioctl");}
static void sendall(int fd,const void* ptr,size_t length){auto p=(const char*)ptr;while(length&&running){ssize_t n=send(fd,p,length,MSG_NOSIGNAL);check(n>0,"network send");p+=n;length-=n;}check(length==0,"shutdown");}
static const int W=2304,H=1296;
struct Buffer{void* ptr=MAP_FAILED;size_t length=0;int dma=-1;};
struct Camera{
 int fd=-1;void* lib=nullptr;void* aiq=nullptr;bool started=false,streaming=false;
 unsigned stride=0,quantization=0,requested_buffers=1;std::vector<Buffer> buffers;
 uint64_t last_poll_ns=0,last_dq_ns=0,last_select_ns=0;unsigned last_dropped=0;
 template<class T>T symbol(const char* name){void*p=dlsym(lib,name);check(p!=nullptr,name);return reinterpret_cast<T>(p);}
 explicit Camera(unsigned buffer_count=1):requested_buffers(buffer_count){
  check(buffer_count>=1&&buffer_count<=3,"bounded camera buffer request");
  fd=open("/dev/video11",O_RDWR|O_NONBLOCK);check(fd>=0,"open camera");
  lib=dlopen("librkaiq.so",RTLD_NOW|RTLD_GLOBAL);check(lib!=nullptr,"load AIQ");
  const char*s=symbol<const char*(*)(const char*)>("rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd")("/dev/video11");check(s!=nullptr,"sensor");
  check(symbol<int(*)(const char*,const char*,const char*)>("rk_aiq_uapi2_sysctl_preInit_scene")(s,"normal","day")>=0,"scene");
  aiq=symbol<void*(*)(const char*,const char*,void*,void*)>("rk_aiq_uapi2_sysctl_init")(s,"/oem/usr/share/iqfiles",nullptr,nullptr);check(aiq!=nullptr,"AIQ init");
  check(symbol<int(*)(void*,uint32_t,uint32_t,int)>("rk_aiq_uapi2_sysctl_prepare")(aiq,W,H,0)>=0,"AIQ prepare");
  check(symbol<int(*)(void*)>("rk_aiq_uapi2_sysctl_start")(aiq)>=0,"AIQ start");started=true;
  v4l2_format format{};format.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;format.fmt.pix_mp.width=W;format.fmt.pix_mp.height=H;format.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV12;format.fmt.pix_mp.field=V4L2_FIELD_NONE;
  call(fd,VIDIOC_S_FMT,&format);
  check(format.fmt.pix_mp.width==W&&format.fmt.pix_mp.height==H&&format.fmt.pix_mp.pixelformat==V4L2_PIX_FMT_NV12&&format.fmt.pix_mp.num_planes==1,"camera format");
  stride=format.fmt.pix_mp.plane_fmt[0].bytesperline;quantization=format.fmt.pix_mp.quantization;
  check(stride==W&&format.fmt.pix_mp.plane_fmt[0].sizeimage==W*H*3/2,"packed layout");
  v4l2_selection crop{};crop.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;crop.target=V4L2_SEL_TGT_CROP;crop.r.width=W;crop.r.height=H;call(fd,VIDIOC_S_SELECTION,&crop);
  resume();
 }
 void resume(){
  v4l2_requestbuffers req{};req.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;req.memory=V4L2_MEMORY_MMAP;req.count=requested_buffers;call(fd,VIDIOC_REQBUFS,&req);check(req.count>=1&&req.count<=4,"buffer count");buffers.resize(req.count);
  for(unsigned i=0;i<req.count;i++){v4l2_buffer b{};v4l2_plane plane{};b.type=req.type;b.memory=req.memory;b.index=i;b.length=1;b.m.planes=&plane;call(fd,VIDIOC_QUERYBUF,&b);auto&z=buffers[i];z.length=plane.length;z.ptr=mmap(nullptr,z.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,plane.m.mem_offset);check(z.ptr!=MAP_FAILED,"mmap");v4l2_exportbuffer e{};e.type=req.type;e.index=i;e.flags=O_CLOEXEC;call(fd,VIDIOC_EXPBUF,&e);z.dma=e.fd;call(fd,VIDIOC_QBUF,&b);}
  int type=req.type;call(fd,VIDIOC_STREAMON,&type);streaming=true;
 }
 void pause(){
  if(streaming){int t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;call(fd,VIDIOC_STREAMOFF,&t);streaming=false;}
  for(auto&b:buffers){if(b.ptr!=MAP_FAILED)munmap(b.ptr,b.length);if(b.dma>=0)close(b.dma);}buffers.clear();
  v4l2_requestbuffers req{};req.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;req.memory=V4L2_MEMORY_MMAP;req.count=0;call(fd,VIDIOC_REQBUFS,&req);
 }
 ~Camera(){if(streaming){int t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;ioctl(fd,VIDIOC_STREAMOFF,&t);}if(started)symbol<int(*)(void*,bool)>("rk_aiq_uapi2_sysctl_stop")(aiq,false);for(auto&b:buffers){if(b.ptr!=MAP_FAILED)munmap(b.ptr,b.length);if(b.dma>=0)close(b.dma);}if(fd>=0)close(fd);if(aiq)symbol<void(*)(void*)>("rk_aiq_uapi2_sysctl_deinit")(aiq);}
 void drain(){
  for(size_t i=0;i<buffers.size();i++){
   v4l2_buffer b{};v4l2_plane plane{};b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;b.memory=V4L2_MEMORY_MMAP;b.length=1;b.m.planes=&plane;
   if(ioctl(fd,VIDIOC_DQBUF,&b)<0){if(errno==EAGAIN)return;check(false,"idle dequeue");}
   call(fd,VIDIOC_QBUF,&b);
  }
 }
 bool acquire(v4l2_buffer& b,v4l2_plane& plane){
  uint64_t poll_start=ns();pollfd p{fd,POLLIN,0};int ready=poll(&p,1,100);last_poll_ns=ns()-poll_start;
  if(ready<=0){last_dq_ns=0;return false;}
  b={};plane={};b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;b.memory=V4L2_MEMORY_MMAP;b.length=1;b.m.planes=&plane;
  uint64_t dq_start=ns();int rc=ioctl(fd,VIDIOC_DQBUF,&b);last_dq_ns=ns()-dq_start;
  if(rc<0){if(errno==EAGAIN)return false;check(false,"dequeue");}
  check(b.index<buffers.size()&&plane.bytesused==W*H*3/2&&plane.data_offset==0,"frame layout");
  check((b.flags&V4L2_BUF_FLAG_TIMESTAMP_MASK)==V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,"clock");
  check(quantization==V4L2_QUANTIZATION_FULL_RANGE,"requires full range NV12 contract");
  if(b.flags&V4L2_BUF_FLAG_ERROR){call(fd,VIDIOC_QBUF,&b);return false;}return true;
 }
 void release(v4l2_buffer& b){call(fd,VIDIOC_QBUF,&b);}
 bool acquire_latest(v4l2_buffer& b,v4l2_plane& plane){
  last_select_ns=0;last_dropped=0;
  if(!acquire(b,plane))return false;
  // Retain the newest buffer already ready; never wait for a future frame.
  uint64_t select_start=ns();
  for(size_t i=1;i<buffers.size();i++){
   pollfd p{fd,POLLIN,0};if(poll(&p,1,0)<=0)break;
   v4l2_buffer next{};v4l2_plane next_plane{};
   next.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;next.memory=V4L2_MEMORY_MMAP;next.length=1;next.m.planes=&next_plane;
   if(ioctl(fd,VIDIOC_DQBUF,&next)<0){if(errno==EAGAIN)break;release(b);check(false,"latest dequeue");}
   try{check(next.index<buffers.size()&&next_plane.bytesused==W*H*3/2&&next_plane.data_offset==0,"latest frame layout");
    check((next.flags&V4L2_BUF_FLAG_TIMESTAMP_MASK)==V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,"latest clock");}
   catch(...){release(next);release(b);throw;}
   release(b);++last_dropped;plane=next_plane;b=next;b.m.planes=&plane;
   if(b.flags&V4L2_BUF_FLAG_ERROR){release(b);return false;}
  }
  last_select_ns=ns()-select_start;
  return true;
 }
};

inline void preview_nv12_bilinear576(const uint8_t* src,uint8_t* dst){
#ifdef __ARM_NEON
 // Deinterleave the fixed 4x sampling phases. Sum all four source values
 // before rounding, exactly as the scalar path (including interleaved UV).
 for(int y=0;y<324;++y)for(int x=0;x<576;x+=8){
  const auto a=vld4_u8(src+(4*y+1)*2304+4*x),b=vld4_u8(src+(4*y+2)*2304+4*x);
  auto sum=vaddq_u16(vaddl_u8(a.val[1],a.val[2]),vaddl_u8(b.val[1],b.val[2]));
  vst1_u8(dst+y*576+x,vrshrn_n_u16(sum,2));
 }
 src+=2304*1296;dst+=576*324;
 for(int y=0;y<162;++y)for(int x=0;x<288;x+=4){
  const auto a=vld4_u16(reinterpret_cast<const uint16_t*>(src+(4*y+1)*2304+x*8));
  const auto b=vld4_u16(reinterpret_cast<const uint16_t*>(src+(4*y+2)*2304+x*8));
  auto sum=vaddq_u16(vaddl_u8(vreinterpret_u8_u16(a.val[1]),vreinterpret_u8_u16(a.val[2])),
                    vaddl_u8(vreinterpret_u8_u16(b.val[1]),vreinterpret_u8_u16(b.val[2])));
  vst1_u8(dst+y*576+x*2,vrshrn_n_u16(sum,2));
 }
#else
 for(int y=0;y<324;++y)for(int x=0;x<576;++x){
  const uint8_t* p=src+(4*y+1)*2304+4*x+1;
  dst[y*576+x]=(unsigned(p[0])+p[1]+p[2304]+p[2305]+2)/4;
 }
 src+=2304*1296;dst+=576*324;
 for(int y=0;y<162;++y)for(int x=0;x<288;++x)for(int c=0;c<2;++c){
  const uint8_t* p=src+(4*y+1)*2304+(4*x+1)*2+c;
  dst[y*576+x*2+c]=(unsigned(p[0])+p[2]+p[2304]+p[2306]+2)/4;
 }
#endif
}

}
