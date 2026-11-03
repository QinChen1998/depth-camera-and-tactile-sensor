// LSG1 shared-session endpoint. All depth inference and repair run on RV1103.
// Fixed-size latest mailboxes; no image/depth recording and no history queues.
int gui(const std::string& modelPath,int mode,const std::string& token,const std::string& csv){
 using namespace LiteSrBoardContract;
 validate(mode,token,csv);const int Pixels=LiteSrBoard::Output*LiteSrBoard::Output;require((isCaptureOnly(mode)||LiteSrBoard::NearModel==isNear(mode))&&LiteSrBoard::HighResolution==isHighResolution(mode),"LiteSR binary/model family mismatch");
#ifdef LITESR_CAPTURE_ONLY
 require(isCaptureOnly(mode),"Capture-only binary refuses model modes");
#endif
 const Options options(csv);const bool hd=isHd(mode);
 std::ifstream bootFile("/proc/sys/kernel/random/boot_id");std::string boot;bootFile>>boot;require(boot.size()==36,"Boot identity");
 const std::string nearIdentity=isNear(mode)?std::string(",\"checkpoint_sha256\":\"")+NearCheckpointSha+"\"":"";
 std::string identity="\"protocol\":\"LSG1\",\"boot_id\":\""+boot+"\",\"generation\":\""+token+"\",\"candidate_mode\":"+std::to_string(mode)+",\"options\":\""+csv+"\",\"engine\":\""+engine(mode)+"\",\"model_sha256\":\""+modelSha(mode)+"\",\"width\":"+std::to_string(outputSide(mode))+",\"height\":"+std::to_string(outputSide(mode))+",\"fresh_ms\":1000"+nearIdentity+",\"training_minimum_m\":"+(isNear(mode)?"0.01":"0.1")+(hd?",\"video_transport\":\"rtsp_h265\",\"video_width\":2304,\"video_height\":1296,\"video_fps\":30,\"video_kbps\":4096":"")+(isCaptureOnly(mode)?",\"capture_only\":true,\"inference_location\":\"none\",\"depth_present\":false":"");
 if(isHighResolution(mode))identity+=",\"rgb_paused\":true,\"video_transport\":\"none\"";
 if(isCaptureOnly(mode))identity+=",\"photon_enabled\":false";
 int sensor=socket(AF_UNIX,SOCK_SEQPACKET,0);require(sensor>=0,"Native socket");
 timeval bound{2,0};setsockopt(sensor,SOL_SOCKET,SO_RCVTIMEO,&bound,sizeof(bound));
 sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,"/run/extrinsic_capture/tof.sock");
 if(connect(sensor,reinterpret_cast<sockaddr*>(&address),sizeof(address))){close(sensor);throw std::runtime_error("Native IPC connection");}
 int listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0){close(sensor);throw std::runtime_error("Listener socket");}
 int yes=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
 sockaddr_in endpoint{};endpoint.sin_family=AF_INET;endpoint.sin_port=htons(8832);endpoint.sin_addr.s_addr=INADDR_ANY;
 if(bind(listener,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))||listen(listener,1)){close(sensor);close(listener);throw std::runtime_error("LiteSR stream bind");}
 struct Output{std::vector<float> depth;std::vector<uint8_t> coverage;Native source;uint64_t seq=0,stamp=0;double cost=0,npu=0,contour=0;int near=0,filled=0,outliers=0;};
 struct LightOutput{Native native;std::array<uint16_t,1536> signal{};int format=0;};
 struct CameraOutput{std::vector<uint8_t> nv12;uint64_t seq=0,stamp=0;};
 std::mutex mutex;std::condition_variable wake;Native pending,latestRaw;bool hasPending=false;
 std::shared_ptr<const Output> latest;std::shared_ptr<const CameraOutput> latestCamera;
 std::shared_ptr<const LightOutput> latestLight;
 std::atomic<bool> stop{false},cameraReady{false},modelReady{false},nativeReady{false};std::string failure;
 uint64_t videoEncoded=0,videoRetrieved=0,videoObserved=0;
 auto failed=[&](const std::exception& e){std::lock_guard<std::mutex> lock(mutex);if(!stop&&!quit)failure=e.what();stop=true;wake.notify_all();};
 std::thread cameraReader([&]{try{
  if(isHighResolution(mode)){cameraReady=true;return;}
  if(hd){
   LiteSrHdVideo::Observer video(token);
   while(!stop&&!quit){video.poll();if(video.progress.ready){std::lock_guard<std::mutex> lock(mutex);videoEncoded=video.progress.previous.encoded;videoRetrieved=video.progress.previous.retrieved;videoObserved=video.progress.observed_ns;cameraReady=true;}usleep(250000);}
   return;
  }
  DtofRgb::Camera camera(2);double converted=0,lastFrame=now();cameraReady=true;
  while(!stop&&!quit){v4l2_buffer b{};v4l2_plane plane{};
   if(!camera.acquire_latest(b,plane)){require(now()-lastFrame<3000,"RGB capture stalled");continue;}
   lastFrame=now();if(lastFrame-converted<180){camera.release(b);continue;}
   CameraOutput out;out.seq=b.sequence;out.stamp=uint64_t(b.timestamp.tv_sec)*1000000000+b.timestamp.tv_usec*1000;out.nv12.resize(279936);
   auto& buffer=camera.buffers[b.index];dma_buf_sync sync{DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
   try{DtofRgb::call(buffer.dma,DMA_BUF_IOCTL_SYNC,&sync);DtofRgb::preview_nv12_bilinear576(static_cast<const uint8_t*>(buffer.ptr),out.nv12.data());sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;DtofRgb::call(buffer.dma,DMA_BUF_IOCTL_SYNC,&sync);}
   catch(...){sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;ioctl(buffer.dma,DMA_BUF_IOCTL_SYNC,&sync);camera.release(b);throw;}
   camera.release(b);converted=lastFrame;std::lock_guard<std::mutex> lock(mutex);latestCamera=std::make_shared<const CameraOutput>(std::move(out));
  }
 }catch(const std::exception& e){failed(e);}});
 std::thread reader([&]{try{uint64_t seq=0,stamp=0;uint16_t columns=0,rows=0;
  while(!stop&&!quit){auto in=readNative(sensor,isCaptureOnly(mode));if(in.sequence<=seq||in.stamp<=stamp)continue;
   require(!columns||(columns==in.columns&&rows==in.rows),"Native sensor grid changed during capture");columns=in.columns;rows=in.rows;seq=in.sequence;stamp=in.stamp;
   std::lock_guard<std::mutex> lock(mutex);latestRaw=in;nativeReady=true;
   if(!isCaptureOnly(mode)){pending=in;hasPending=true;wake.notify_one();}}
 }catch(const std::exception& e){failed(e);}});
 std::thread compute([&]{try{
  std::unique_ptr<Model> model;std::unique_ptr<LiteSrBoard::DepthPrepared> p;
  std::unique_ptr<std::array<float,Pixels>> depth;
  if(!isCaptureOnly(mode)){model=std::make_unique<Model>(modelPath);p=std::make_unique<LiteSrBoard::DepthPrepared>();depth=std::make_unique<std::array<float,Pixels>>();}
  modelReady=true;
  // Host models only need native range/SNR. Do not open PHQ1 or wait for D/I.
  if(isCaptureOnly(mode))return;
  while(!stop&&!quit){Native in;{std::unique_lock<std::mutex> lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return hasPending||stop||quit;});if(!hasPending)continue;in=pending;hasPending=false;}
   const double age=now()-in.stamp/1e6;if(age<0||age>=500)continue;
   const double begin=now();std::array<uint16_t,1536> words{};int format=0;if(!photon(in,boot,words,&format))continue;
   // Publish the measured D/I before the slower contour stage. Its identity is
   // independent of the last completed depth, with the original 500 ms age cap.
   {auto light=std::make_shared<LightOutput>();light->native=in;light->signal=words;light->format=format;std::lock_guard<std::mutex> lock(mutex);latestLight=std::move(light);}
   LiteSrBoard::prepareDepth(in.bytes.data(),words.data(),0,512,*p);double pack=0,decode=0;
   Output out;out.npu=model->infer(p->depthWork.data(),p->intensityWork.data(),pack,decode);
   model->publishDepth(*p,0,depth->data());
   // Keep the model publication; unsupported native near cells stay invalid.
   if(mode==Contour){const double start=now();auto refined=LiteSrBoardContour::refineFast(depth->data(),in.bytes.data(),0,{options.holes,options.outliers});
    out.depth=std::move(refined.depth);out.coverage=std::move(refined.coverage);
    out.filled=refined.repairs.filledCells;out.outliers=refined.repairs.outlierCells;out.contour=now()-start;
   }else{out.depth.assign(depth->begin(),depth->end());out.coverage.resize(Pixels);for(int i=0;i<Pixels;++i)out.coverage[i]=out.depth[i]>0?255:0;}
   out.source=in;out.cost=now()-begin;out.seq=in.sequence;out.stamp=in.stamp;
   if(now()-in.stamp/1e6>=FreshMs)continue;
   std::lock_guard<std::mutex> lock(mutex);latest=std::make_shared<const Output>(std::move(out));
  }
 }catch(const std::exception& e){failed(e);}});
 int client=-1,result=0;
 auto sendBytes=[&](const void* buffer,size_t size){const char* ptr=static_cast<const char*>(buffer);const double start=now();
  while(size&&!stop&&!quit){if(now()-start>1500)return false;pollfd event{client,POLLOUT,0};const int ready=poll(&event,1,25);if(ready<0&&errno==EINTR)continue;if(ready<0)return false;if(!ready)continue;
   const auto n=send(client,ptr,size,MSG_NOSIGNAL|MSG_DONTWAIT);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<=0)return false;ptr+=n;size-=size_t(n);}
  return size==0;};
 try{
  double started=now();while((!cameraReady||!modelReady||!nativeReady)&&!stop&&!quit&&now()-started<10000)usleep(10000);
  require(cameraReady&&modelReady&&nativeReady&&!stop&&!quit,"LiteSR startup failed");
  if(isCaptureOnly(mode)){
   std::lock_guard<std::mutex> lock(mutex);
   // Preserve the existing wire identity for M48/M8 clients. Only an actual
   // native16 capture opts into the extended grid protocol.
   const auto nativeProtocol=latestRaw.columns==16?NativeCaptureProtocolV2:NativeCaptureProtocol;
   identity+=",\"native_capture_protocol\":\""+std::string(nativeProtocol)+"\",\"native_columns\":"+std::to_string(latestRaw.columns)+",\"native_rows\":"+std::to_string(latestRaw.rows);
  }
  {std::ofstream ready("/run/dtof_candidates/ready.json.tmp");ready<<"{\"state\":\"running\",\"paired_depth_native_protocol\":\"LSG2\","<<identity<<"}\n";require(bool(ready),"Ready receipt write");}
  require(rename("/run/dtof_candidates/ready.json.tmp","/run/dtof_candidates/ready.json")==0,"Ready receipt publish");
  pollfd event{listener,POLLIN,0};started=now();
  while(!stop&&!quit&&now()-started<15000){if(poll(&event,1,100)>0){client=accept(listener,nullptr,nullptr);break;}}
  if(client>=0){int size=65536;setsockopt(client,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size));setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));uint64_t photonSent=0;
   while(!stop&&!quit){std::array<uint8_t,32> request{};size_t received=0;const double requestStart=now();
    while(received<request.size()&&!stop&&!quit&&now()-requestStart<3000){
     pollfd incoming{client,POLLIN,0};const int ready=poll(&incoming,1,25);if(ready<0&&errno==EINTR)continue;if(ready<0)break;if(!ready)continue;
     const auto count=recv(client,request.data()+received,request.size()-received,MSG_DONTWAIT);
     if(count<0&&(errno==EINTR||errno==EAGAIN))continue;if(count<=0)break;received+=size_t(count);
    }
    if(received!=request.size())break;
    const bool paired=u32(request.data())==PairedRequestMagic;
    require((u32(request.data())==RequestMagic||(paired&&isFull(mode)))&&u32(request.data()+4)>0,"LiteSR stream request");std::shared_ptr<const Output> output;Native raw;std::shared_ptr<const CameraOutput> rgb;std::shared_ptr<const LightOutput> light;uint64_t encoded=0,retrieved=0,observed=0;
    {std::lock_guard<std::mutex> lock(mutex);if(latest&&latest->seq>u64(request.data()+8))output=latest;if(latestRaw.sequence>u64(request.data()+16))raw=latestRaw;if(latestCamera&&latestCamera->seq>u64(request.data()+24))rgb=latestCamera;if(latestLight&&latestLight->native.sequence>photonSent)light=latestLight;encoded=videoEncoded;retrieved=videoRetrieved;observed=videoObserved;}
    const Output emptyOutput;const CameraOutput emptyCamera;const auto& out=output?*output:emptyOutput;const auto& camera=rgb?*rgb:emptyCamera;
    const double age=now()-out.stamp/1e6,rawAge=now()-raw.stamp/1e6,rgbAge=now()-camera.stamp/1e6;
    const bool fresh=out.seq&&age>=0&&age<FreshMs,rawFresh=raw.sequence&&rawAge>=0&&rawAge<220,rgbFresh=camera.seq&&rgbAge>=0&&rgbAge<500;
    const double lightAge=light?now()-light->native.stamp/1e6:-1;const bool lightFresh=light&&lightAge>=0&&lightAge<500;
    auto wireIdentity=identity;if(paired)wireIdentity.replace(wireIdentity.find("LSG1"),4,"LSG2");
    std::ostringstream meta;meta.precision(17);meta<<"{"<<wireIdentity<<",\"request\":"<<u32(request.data()+4)<<",\"data\":"<<(fresh?"true":"false");
    if(isCaptureOnly(mode)){
     timespec real{},mono{};require(clock_gettime(CLOCK_REALTIME,&real)==0&&clock_gettime(CLOCK_MONOTONIC,&mono)==0,"Clock sample");
     meta<<",\"server_realtime_ns\":\""<<(uint64_t(real.tv_sec)*1000000000ULL+real.tv_nsec)
         <<"\",\"server_monotonic_ns\":\""<<(uint64_t(mono.tv_sec)*1000000000ULL+mono.tv_nsec)<<"\"";
    }
    if(hd){require(encoded&&retrieved&&observed,"HD encoder has no real progress");meta<<",\"video_encoded_sequence\":\""<<encoded<<"\",\"video_retrieved_sequence\":\""<<retrieved<<"\",\"video_observed_ns\":\""<<observed<<"\",\"video_age_ms\":"<<(now()-observed/1e6)<<",\"video_progress_source\":\"venc_info:chn0\"";}
    if(fresh)meta<<",\"tof_sequence\":\""<<out.seq<<"\",\"tof_timestamp_ns\":\""<<out.stamp<<"\",\"age_ms\":"<<age<<",\"compute_ms\":"<<out.cost<<",\"npu_ms\":"<<out.npu<<",\"contour_ms\":"<<out.contour<<",\"encoding\":\"raw\",\"maximum_m\":1.4,\"minimum_exclusive_m\":0,\"positive_near_policy\":\"model_output_no_native_near_fill\",\"near_native_pixels\":"<<out.near<<",\"filled_native_cells\":"<<out.filled<<",\"outlier_native_cells\":"<<out.outliers<<",\"input_rotation_degrees\":0,\"intensity_white_counts\":512,\"quantization\":\"INT8\"";
    meta<<",\"photon_data\":"<<(lightFresh?"true":"false");
    if(lightFresh)meta<<",\"photon_sequence\":\""<<light->native.sequence<<"\",\"photon_timestamp_ns\":\""<<light->native.stamp<<"\",\"photon_age_ms\":"<<lightAge<<",\"input_warning_flags\":"<<light->native.warnings<<",\"input_signal_format\":"<<light->format<<",\"input_fov_correction\":"<<unsigned(light->native.fov);
    if(lightFresh&&isCaptureOnly(mode))meta<<",\"input_columns\":"<<light->native.columns<<",\"input_rows\":"<<light->native.rows;
    meta<<",\"raw_data\":"<<(rawFresh?"true":"false")<<",\"rgb_data\":"<<(rgbFresh?"true":"false");
    if(rawFresh)meta<<",\"raw_sequence\":\""<<raw.sequence<<"\",\"raw_timestamp_ns\":\""<<raw.stamp<<"\",\"raw_age_ms\":"<<rawAge<<",\"tof_warning_flags\":"<<raw.warnings;
    if(rawFresh&&isCaptureOnly(mode))meta<<",\"raw_fov_correction\":"<<unsigned(raw.fov)<<",\"raw_columns\":"<<raw.columns<<",\"raw_rows\":"<<raw.rows;
    if(rgbFresh)meta<<",\"rgb_sequence\":\""<<camera.seq<<"\",\"rgb_timestamp_ns\":\""<<camera.stamp<<"\",\"rgb_age_ms\":"<<rgbAge<<",\"rgb_width\":576,\"rgb_height\":324,\"pixel_format\":\"NV12\",\"color_range\":\"full\",\"color_matrix\":\"canonical-full-601\",\"resize\":\"bilinear4x4-round-half-up\",\"rectify_map_sha256\":\"00de89f4e6d95f09758b0d8274c2694168a27b5f601bdfeda667b0b179126b5d\"";
    if(paired&&fresh)meta<<",\"depth_native_sequence\":\""<<out.source.sequence<<"\",\"depth_native_timestamp_ns\":\""<<out.source.stamp<<"\",\"depth_native_warning_flags\":"<<out.source.warnings<<",\"depth_native_fov_correction\":"<<unsigned(out.source.fov);
    meta<<"}";const auto json=meta.str();const uint32_t rawBytes=rawFresh?uint32_t(raw.byteCount()):0,inputBytes=lightFresh?uint32_t(light->native.byteCount()):0;
    const uint32_t header[]={paired?PairedResponseMagic:ResponseMagic,uint32_t(json.size()),fresh?Pixels*4u:0,fresh?uint32_t(Pixels):0,rawBytes,rgbFresh?279936u:0,inputBytes,inputBytes/2,paired&&fresh?6144u:0};
    if(!sendBytes(header,paired?36:32)||!sendBytes(json.data(),json.size())||(fresh&&(!sendBytes(out.depth.data(),out.depth.size()*4)||!sendBytes(out.coverage.data(),out.coverage.size())))||(rawFresh&&!sendBytes(raw.bytes.data(),rawBytes))||(rgbFresh&&!sendBytes(camera.nv12.data(),camera.nv12.size()))||(lightFresh&&(!sendBytes(light->native.bytes.data(),inputBytes)||!sendBytes(light->signal.data(),inputBytes/2)))||(paired&&fresh&&!sendBytes(out.source.bytes.data(),out.source.bytes.size())))break;
    if(lightFresh)photonSent=light->native.sequence;
   }
  }
 }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;result=2;}
 stop=true;wake.notify_all();shutdown(sensor,SHUT_RDWR);reader.join();compute.join();cameraReader.join();close(sensor);if(client>=0)close(client);close(listener);
 if(!failure.empty()){std::cerr<<failure<<std::endl;result=2;}return result;
}
