#pragma once
#include <QtCore/QThread>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QLibrary>
#include <QtGui/QImage>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <map>
#include <memory>
#include <functional>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// The existing H.265 route, decoded with the same versioned FFmpeg DLLs as Qt.
// PRFT is the sender's RTCP clock reference for THIS packet, never arrival time.
// One latest image; no recording, synthetic exposure clock or unbounded queue.
class RvHdVideoStream final : public QThread {
public:
 struct Snapshot {QImage image;QString error;quint64 sequence=0;qint64 receivedNs=0,ptsUs=-1,producerRealtimeUs=0;double decodeMs=0;};
 explicit RvHdVideoStream(QString source):url(std::move(source)){}
 ~RvHdVideoStream() override {requestInterruption();wait();}
 Snapshot snapshot() const {QMutexLocker lock(&mutex);return latest;}
 // Install before start. Notifications carry no image and must not block.
 bool setFrameAvailable(std::function<void()> notify){if(isRunning())return false;frameAvailable=std::move(notify);return true;}
 static qint64 nowNs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
private:
 struct Api {
  QLibrary format{"avformat-61"},codec{"avcodec-61"},util{"avutil-59"},scale{"swscale-8"};
#define RV_AV_DECLARE(name) decltype(&::name) name=nullptr
  RV_AV_DECLARE(avformat_version);RV_AV_DECLARE(avcodec_version);RV_AV_DECLARE(avutil_version);
  RV_AV_DECLARE(avformat_network_init);RV_AV_DECLARE(avformat_alloc_context);RV_AV_DECLARE(avformat_open_input);
  RV_AV_DECLARE(avformat_find_stream_info);RV_AV_DECLARE(avformat_close_input);RV_AV_DECLARE(av_read_frame);
  RV_AV_DECLARE(avcodec_find_decoder);RV_AV_DECLARE(avcodec_alloc_context3);RV_AV_DECLARE(avcodec_parameters_to_context);
  RV_AV_DECLARE(avcodec_open2);RV_AV_DECLARE(avcodec_send_packet);RV_AV_DECLARE(avcodec_receive_frame);RV_AV_DECLARE(avcodec_free_context);
  RV_AV_DECLARE(av_packet_alloc);RV_AV_DECLARE(av_packet_free);RV_AV_DECLARE(av_packet_unref);RV_AV_DECLARE(av_packet_get_side_data);
  RV_AV_DECLARE(av_frame_alloc);RV_AV_DECLARE(av_frame_free);RV_AV_DECLARE(av_frame_unref);
  RV_AV_DECLARE(av_dict_set);RV_AV_DECLARE(av_dict_free);RV_AV_DECLARE(av_rescale_q);
  RV_AV_DECLARE(sws_getCachedContext);RV_AV_DECLARE(sws_getCoefficients);RV_AV_DECLARE(sws_setColorspaceDetails);RV_AV_DECLARE(sws_scale);RV_AV_DECLARE(sws_freeContext);
#undef RV_AV_DECLARE
  template<class T>static void resolve(QLibrary &lib,T &p,const char *name){p=reinterpret_cast<T>(lib.resolve(name));if(!p)throw std::runtime_error("Missing versioned FFmpeg function");}
  Api(){
   if(!format.load()||!codec.load()||!util.load()||!scale.load())throw std::runtime_error("Existing Qt FFmpeg DLLs unavailable");
#define RV_AV_LOAD(lib,name) resolve(lib,name,#name)
   RV_AV_LOAD(format,avformat_version);RV_AV_LOAD(codec,avcodec_version);RV_AV_LOAD(util,avutil_version);
   RV_AV_LOAD(format,avformat_network_init);RV_AV_LOAD(format,avformat_alloc_context);RV_AV_LOAD(format,avformat_open_input);
   RV_AV_LOAD(format,avformat_find_stream_info);RV_AV_LOAD(format,avformat_close_input);RV_AV_LOAD(format,av_read_frame);
   RV_AV_LOAD(codec,avcodec_find_decoder);RV_AV_LOAD(codec,avcodec_alloc_context3);RV_AV_LOAD(codec,avcodec_parameters_to_context);
   RV_AV_LOAD(codec,avcodec_open2);RV_AV_LOAD(codec,avcodec_send_packet);RV_AV_LOAD(codec,avcodec_receive_frame);RV_AV_LOAD(codec,avcodec_free_context);
   RV_AV_LOAD(codec,av_packet_alloc);RV_AV_LOAD(codec,av_packet_free);RV_AV_LOAD(codec,av_packet_unref);RV_AV_LOAD(codec,av_packet_get_side_data);
   RV_AV_LOAD(util,av_frame_alloc);RV_AV_LOAD(util,av_frame_free);RV_AV_LOAD(util,av_frame_unref);
   RV_AV_LOAD(util,av_dict_set);RV_AV_LOAD(util,av_dict_free);RV_AV_LOAD(util,av_rescale_q);
   RV_AV_LOAD(scale,sws_getCachedContext);RV_AV_LOAD(scale,sws_getCoefficients);RV_AV_LOAD(scale,sws_setColorspaceDetails);RV_AV_LOAD(scale,sws_scale);RV_AV_LOAD(scale,sws_freeContext);
#undef RV_AV_LOAD
   if(avformat_version()!=LIBAVFORMAT_VERSION_INT||avcodec_version()!=LIBAVCODEC_VERSION_INT||avutil_version()!=LIBAVUTIL_VERSION_INT)
    throw std::runtime_error("FFmpeg DLL/header version mismatch");
  }
 };
 QString url;mutable QMutex mutex;Snapshot latest;std::atomic<qint64> deadline{0};std::function<void()> frameAvailable;
 static void require(bool good,const char *why){if(!good)throw std::runtime_error(why);}
 void run() override {
  std::unique_ptr<Api> a;AVFormatContext *format=nullptr;AVCodecContext *codec=nullptr;AVFrame *frame=nullptr;AVPacket *packet=nullptr;SwsContext *convert=nullptr;
  try{
   a=std::make_unique<Api>();a->avformat_network_init();format=a->avformat_alloc_context();require(format,"FFmpeg input allocation");
   format->interrupt_callback={[](void *p){const auto *s=static_cast<RvHdVideoStream*>(p);return int(s->isInterruptionRequested()||nowNs()>s->deadline.load());},this};
   deadline=nowNs()+12000000000LL;
   AVDictionary *options=nullptr;
   for(const auto &item:{std::pair<const char*,const char*>{"rtsp_transport","tcp"},{"fflags","nobuffer"},{"flags","low_delay"},{"probesize","32"},{"analyzeduration","0"},{"timeout","5000000"},{"reorder_queue_size","0"}})
    a->av_dict_set(&options,item.first,item.second,0);
   const auto address=url.toUtf8();const int opened=a->avformat_open_input(&format,address.constData(),nullptr,&options);a->av_dict_free(&options);
   require(opened>=0,"Cannot open managed H.265 stream");
   require(a->avformat_find_stream_info(format,nullptr)>=0,"H.265 stream parameters unavailable");
   int video=-1;for(unsigned i=0;i<format->nb_streams;++i)if(format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){require(video<0,"Multiple video streams");video=int(i);}
   require(video>=0&&format->streams[video]->codecpar->codec_id==AV_CODEC_ID_HEVC,"Managed source must be H.265");
   const auto *decoder=a->avcodec_find_decoder(AV_CODEC_ID_HEVC);require(decoder,"H.265 decoder unavailable");codec=a->avcodec_alloc_context3(decoder);require(codec,"Decoder allocation");
   require(a->avcodec_parameters_to_context(codec,format->streams[video]->codecpar)>=0,"Decoder parameters");
   codec->thread_count=1;codec->flags|=AV_CODEC_FLAG_LOW_DELAY;
   require(a->avcodec_open2(codec,decoder,nullptr)>=0,"Cannot open H.265 decoder");
   frame=a->av_frame_alloc();packet=a->av_packet_alloc();require(frame&&packet,"Frame allocation");
   std::map<qint64,qint64> referenceTimes;
   auto receive=[&]{
    while(!isInterruptionRequested()){
     const int rc=a->avcodec_receive_frame(codec,frame);if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return;require(rc>=0,"H.265 decoding failed");
     require(frame->width==2304&&frame->height==1296,"Managed H.265 dimensions changed");
     const qint64 started=nowNs();QImage image(2304,1296,QImage::Format_RGB888);
     auto pixelFormat=AVPixelFormat(frame->format);bool fullRange=frame->color_range==AVCOL_RANGE_JPEG;
     // The deprecated YUVJ aliases have the same planes as YUV. Normalize the
     // alias and set full range explicitly so the cached context is reused.
     if(pixelFormat==AV_PIX_FMT_YUVJ420P){pixelFormat=AV_PIX_FMT_YUV420P;fullRange=true;}
     if(pixelFormat==AV_PIX_FMT_YUVJ422P){pixelFormat=AV_PIX_FMT_YUV422P;fullRange=true;}
     if(pixelFormat==AV_PIX_FMT_YUVJ444P){pixelFormat=AV_PIX_FMT_YUV444P;fullRange=true;}
     convert=a->sws_getCachedContext(convert,frame->width,frame->height,pixelFormat,2304,1296,AV_PIX_FMT_RGB24,SWS_BILINEAR,nullptr,nullptr,nullptr);require(convert,"H.265 color conversion");
     const int matrix=frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:frame->colorspace==AVCOL_SPC_BT2020_NCL?SWS_CS_BT2020:SWS_CS_ITU601;
     const auto *coefficients=a->sws_getCoefficients(matrix);
     require(a->sws_setColorspaceDetails(convert,coefficients,fullRange,coefficients,1,0,1<<16,1<<16)>=0,"H.265 color contract");
     uint8_t *destination[]={image.bits(),nullptr,nullptr,nullptr};int lines[]={int(image.bytesPerLine()),0,0,0};
     require(a->sws_scale(convert,frame->data,frame->linesize,0,1296,destination,lines)==1296,"Incomplete H.265 image");
     const qint64 stamp=frame->best_effort_timestamp;
     const auto ref=referenceTimes.find(stamp);const qint64 wall=ref==referenceTimes.end()?0:ref->second;
     {QMutexLocker lock(&mutex);latest.image=std::move(image);latest.error.clear();++latest.sequence;latest.receivedNs=nowNs();
      latest.ptsUs=stamp==AV_NOPTS_VALUE?-1:a->av_rescale_q(stamp,format->streams[video]->time_base,AVRational{1,1000000});latest.producerRealtimeUs=wall;latest.decodeMs=(nowNs()-started)/1e6;}
     if(ref!=referenceTimes.end())referenceTimes.erase(ref);
     a->av_frame_unref(frame);
     if(frameAvailable)frameAvailable();
    }
   };
   while(!isInterruptionRequested()){
    deadline=nowNs()+6000000000LL;const int rc=a->av_read_frame(format,packet);if(rc<0){if(isInterruptionRequested())break;throw std::runtime_error("Managed H.265 transport disconnected");}
    if(packet->stream_index==video){
     size_t size=0;const auto *bytes=a->av_packet_get_side_data(packet,AV_PKT_DATA_PRFT,&size);
     if(bytes&&size==sizeof(AVProducerReferenceTime)&&packet->pts!=AV_NOPTS_VALUE){AVProducerReferenceTime ref{};memcpy(&ref,bytes,sizeof(ref));if(ref.wallclock>0)referenceTimes[packet->pts]=ref.wallclock;}
     while(referenceTimes.size()>64)referenceTimes.erase(referenceTimes.begin());
     int sent=a->avcodec_send_packet(codec,packet);if(sent==AVERROR(EAGAIN)){receive();sent=a->avcodec_send_packet(codec,packet);}require(sent>=0,"H.265 packet rejected");receive();
    }
    a->av_packet_unref(packet);
   }
  }catch(const std::exception &e){if(!isInterruptionRequested()){
    {QMutexLocker lock(&mutex);latest.image={};latest.error=QString::fromUtf8(e.what());++latest.sequence;}
    if(frameAvailable)frameAvailable();}}
  if(a){if(convert)a->sws_freeContext(convert);if(packet)a->av_packet_free(&packet);if(frame)a->av_frame_free(&frame);if(codec)a->avcodec_free_context(&codec);if(format)a->avformat_close_input(&format);}
 }
};
