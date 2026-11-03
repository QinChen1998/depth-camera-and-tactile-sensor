// Protected LiteSR near-depth runtime for RV1103.
#include "Preprocess.h"
#include "Contour.h"
#include "Contract.h"
#include "NativePacket.h"
#include "SpiNative48.h"
#include <rknn_api.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <sstream>
#include <netinet/tcp.h>
#include "../dtof_board/Camera.h"
#include "HdVideo.h"

namespace {
void require(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void check(int code,const char* stage){require(code==RKNN_SUCC,std::string(stage)+" code="+std::to_string(code));}
double now(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
double percentile(std::vector<double> values,double q){if(values.empty())return 0;std::sort(values.begin(),values.end());return values[size_t((values.size()-1)*q)];}
volatile std::sig_atomic_t quit=0;
void onSignal(int){quit=1;DtofRgb::running=0;}
void memory(const char* stage){std::ifstream f("/proc/meminfo");std::string key,unit;long value;std::cout<<"{\"memory_stage\":\""<<stage<<"\"";while(f>>key>>value>>unit)if(key=="MemAvailable:"||key=="MemFree:"||key=="CmaAllocated:"||key=="CmaReleased:")std::cout<<",\""<<key.substr(0,key.size()-1)<<"_kib\":"<<value;std::cout<<"}"<<std::endl;}
size_t elementBytes(rknn_tensor_type type){switch(type){case RKNN_TENSOR_INT8:case RKNN_TENSOR_UINT8:return 1;case RKNN_TENSOR_FLOAT32:return 4;default:throw std::runtime_error("Unsupported tensor dtype");}}
std::array<unsigned,4> shape(const rknn_tensor_attr& a){
 require(a.n_dims==4,"Only rank-four tensors supported");
 if(a.fmt==RKNN_TENSOR_NHWC)return {{a.dims[0],a.dims[3],a.dims[1],a.dims[2]}};
 require(a.fmt==RKNN_TENSOR_NCHW,"Unsupported packed layout");return {{a.dims[0],a.dims[1],a.dims[2],a.dims[3]}};
}
size_t offset(const rknn_tensor_attr& a,int y,int x){
 const auto s=shape(a);require(s[0]==1&&s[1]==1,"Only scalar D/I channels supported");
 const size_t stride=a.w_stride?a.w_stride:s[3];require(stride>=s[3],"Invalid width stride");
 const size_t index=size_t(y)*stride+x;require((index+1)*elementBytes(a.type)<=a.size_with_stride,"Tensor allocation bounds");return index;
}
void printAttr(const rknn_tensor_attr& a,const char* role){
 std::cout<<"{\"tensor\":\""<<role<<"\",\"index\":"<<a.index<<",\"name\":\""<<a.name<<"\",\"dims\":[";
 for(unsigned j=0;j<a.n_dims;++j)std::cout<<(j?",":"")<<a.dims[j];
 std::cout<<"],\"layout\":"<<a.fmt<<",\"type\":"<<a.type<<",\"zero_point\":"<<a.zp<<",\"scale\":"<<a.scale<<",\"w_stride\":"<<a.w_stride<<",\"bytes\":"<<a.size_with_stride<<"}"<<std::endl;
}
#include "ProtectedModel.inc"
class Model{
 rknn_context context=0;
 std::array<rknn_tensor_attr,2> inputs{},outputs{};
 std::array<rknn_tensor_mem*,2> inputMemory{{nullptr,nullptr}},outputMemory{{nullptr,nullptr}};
 std::array<size_t,2> inputStride{},outputStride{};
 template<class Sink> void decode(unsigned i,const Sink& sink){
  const auto& a=outputs[i];
  const auto visit=[&](const auto* data){
   for(int y=0;y<256;++y)for(int x=0;x<256;++x){
    const auto raw=data[size_t(y)*outputStride[i]+x];float value;
    if constexpr(std::is_same_v<std::decay_t<decltype(raw)>,float>)value=raw;
    else value=(int(raw)-a.zp)*a.scale;
    require(std::isfinite(value),"Nonfinite model output");sink(y*256+x,value);
   }
  };
  if(a.type==RKNN_TENSOR_INT8)visit(static_cast<const int8_t*>(outputMemory[i]->virt_addr));
  else if(a.type==RKNN_TENSOR_UINT8)visit(static_cast<const uint8_t*>(outputMemory[i]->virt_addr));
  else visit(static_cast<const float*>(outputMemory[i]->virt_addr));
 }
public:
 using Residual=std::array<std::array<float,65536>,2>;
 explicit Model(const std::string& path){
  try{
   initProtectedModel(context, path);
   rknn_sdk_version version{};check(rknn_query(context,RKNN_QUERY_SDK_VERSION,&version,sizeof(version)),"query version");
   std::cout<<"{\"runtime\":\""<<version.api_version<<"\",\"driver\":\""<<version.drv_version<<"\"}"<<std::endl;
   rknn_mem_size memory{};int memoryCode=rknn_query(context,RKNN_QUERY_MEM_SIZE,&memory,sizeof(memory));
   std::cout<<"{\"memory_query_code\":"<<memoryCode<<",\"weights_bytes\":"<<memory.total_weight_size<<",\"internal_bytes\":"<<memory.total_internal_size<<"}"<<std::endl;
   rknn_input_output_num count{};check(rknn_query(context,RKNN_QUERY_IN_OUT_NUM,&count,sizeof(count)),"query count");
   require(count.n_input==2&&count.n_output==2,"Frozen full model requires two inputs and two outputs");
   for(unsigned i=0;i<2;++i){
    rknn_tensor_attr logical{};logical.index=i;check(rknn_query(context,RKNN_QUERY_INPUT_ATTR,&logical,sizeof(logical)),"query logical input");
    require(std::string(logical.name)==(i?"intensity_work":"depth_work"),"Input name/order mismatch");
    auto& a=inputs[i];a.index=i;check(rknn_query(context,RKNN_QUERY_NATIVE_INPUT_ATTR,&a,sizeof(a)),"query native input");
    printAttr(a,"input");require(shape(a)==std::array<unsigned,4>{{1,1,64,64}},"Input shape mismatch");
    require(a.type==RKNN_TENSOR_INT8||a.type==RKNN_TENSOR_UINT8,"Input is not INT8");
    require(a.qnt_type==RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC&&std::isfinite(a.scale)&&a.scale>0&&a.size_with_stride,"Input affine quantization invalid");
    // Attributes are immutable after binding. Validate the last scalar once;
    // each frame then uses the verified row stride without per-pixel queries.
    offset(a,63,63);inputStride[i]=a.w_stride?a.w_stride:64;
    inputMemory[i]=rknn_create_mem(context,a.size_with_stride);require(inputMemory[i]&&inputMemory[i]->virt_addr,"Input allocation failed");
    a.pass_through=1;check(rknn_set_io_mem(context,inputMemory[i],&a),"bind input");
    auto& b=outputs[i];b.index=i;check(rknn_query(context,RKNN_QUERY_OUTPUT_ATTR,&b,sizeof(b)),"query output");
    printAttr(b,"output");require(shape(b)==std::array<unsigned,4>{{1,1,256,256}},"Output shape mismatch");
    require(b.size_with_stride&&b.qnt_type==RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC&&std::isfinite(b.scale)&&b.scale>0,"Output affine quantization invalid");
    offset(b,255,255);outputStride[i]=b.w_stride?b.w_stride:256;
    elementBytes(b.type);outputMemory[i]=rknn_create_mem(context,b.size_with_stride);require(outputMemory[i]&&outputMemory[i]->virt_addr,"Output allocation failed");
    check(rknn_set_io_mem(context,outputMemory[i],&b),"bind output");
   }
  }catch(...){clear();throw;}
 }
 Model(const Model&)=delete;Model& operator=(const Model&)=delete;
 void clear(){for(auto& p:inputMemory)if(p){rknn_destroy_mem(context,p);p=nullptr;}for(auto& p:outputMemory)if(p){rknn_destroy_mem(context,p);p=nullptr;}if(context){rknn_destroy(context);context=0;}}
 ~Model(){clear();}
 double infer(const float* d,const float* in,double& packMs,double& decodeMs,Residual* residual=nullptr){
  double begin=now();const float* values[]={d,in};
  for(int i=0;i<2;++i){const auto& a=inputs[i];std::memset(inputMemory[i]->virt_addr,0,a.size_with_stride);
   for(int y=0;y<64;++y)for(int x=0;x<64;++x){const float value=values[i][y*64+x];require(std::isfinite(value),"Nonfinite model input");
    const long q=std::lround(value/a.scale)+a.zp;const auto at=size_t(y)*inputStride[i]+x;
    if(a.type==RKNN_TENSOR_INT8)static_cast<int8_t*>(inputMemory[i]->virt_addr)[at]=int8_t(std::clamp(q,-128L,127L));
    else static_cast<uint8_t*>(inputMemory[i]->virt_addr)[at]=uint8_t(std::clamp(q,0L,255L));
   }
  }
  packMs=now()-begin;begin=now();check(rknn_run(context,nullptr),"rknn_run");const double runMs=now()-begin;begin=now();
  if(residual)for(unsigned i=0;i<2;++i)decode(i,[&](int at,float value){(*residual)[i][at]=value;});
  decodeMs=now()-begin;return runMs;
 }
 int publishDepth(const LiteSrBoard::DepthPrepared& p,int rotation,float* out,bool allowPositiveBelowTrainingRange=true){
  require(rotation==0||rotation==180,"Invalid output orientation");int valid=0;
  decode(0,[&](int i,float value){const float z=LiteSrBoard::publishValue(p,i,value,allowPositiveBelowTrainingRange);out[rotation?65535-i:i]=z;valid+=z>0;});
  return valid;
 }
};
#include "NativeInput.inc"
#include "Gui.inc"
}
int main(int argc, char** argv) {
    signal(SIGTERM, onSignal);
    signal(SIGINT, onSignal);
    signal(SIGPIPE, SIG_IGN);
    try {
        const std::string action = argc > 1 ? argv[1] : "";
        if (argc == 3 && action == "check-protected") {
            Model model(argv[2]);
            std::cout << "DCTS_PROTECTED_LITESR_READY" << std::endl;
            return 0;
        }
        if (argc == 5 && action == "validate") {
            require(std::stoi(argv[2]) == LiteSrBoardContract::NearFullHd, "Unsupported protected LiteSR mode");
            LiteSrBoardContract::validate(LiteSrBoardContract::NearFullHd, argv[3], argv[4]);
            return 0;
        }
        if (argc == 6 && action == "gui") {
            require(std::stoi(argv[3]) == LiteSrBoardContract::NearFullHd, "Unsupported protected LiteSR mode");
            return gui(argv[2], LiteSrBoardContract::NearFullHd, argv[4], argv[5]);
        }
        throw std::runtime_error("Expected protected LiteSR validate, check-protected or gui");
    } catch (const std::exception& error) {
        std::cerr << "LITESR_ERROR " << error.what() << std::endl;
        return 2;
    }
}
