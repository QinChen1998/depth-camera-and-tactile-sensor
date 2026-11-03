#pragma once
#include <QtCore/QThread>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QtEndian>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QNetworkProxy>
#include <QtGui/QImage>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <QtCore/QJsonArray>
#include <QtCore/QWaitCondition>
#include <atomic>
#include "DepthPost.h"
#include "RvTofFrame.h"
#include <cstring>
#include <memory>

// CAN4 transport/processing worker. No control commands, capture ownership or GUI access.
// One bounded mailbox: the GUI polls it; slow painting never queues images.
class RvCameraStream : public QThread {
public:
 struct Snapshot { QImage image; QByteArray depth,baseDepth; RvTofFrame tof; QString status; QJsonObject metadata; quint64 sequence=0; int reconnects=0; quint64 received=0,dropped=0; qint64 receivedNs=0; double processingMs=0; };
 QString host; quint16 port=8831;
 Snapshot snapshot() { QMutexLocker lock(&mutex);auto s=latest;if(clock.isValid()&&s.receivedNs)s.metadata["host_receive_to_poll_ms"]=(clock.nsecsElapsed()-s.receivedNs)/1e6;return s; }
 ~RvCameraStream() override { requestInterruption(); wait(); }
 static void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
 // Immutable calibration shared by the receiver and processing worker. Validate
 // coordinates and hash once on load, while still checking every packet's SHA.
 class Calibration {
 public:
  explicit Calibration(const QByteArray& map) {
   require(map.size()>=16&&map.left(8)==QByteArray("NVMAP1\0\0",8),"Map header");
   w=qFromLittleEndian<quint32>(map.constData()+8);h=qFromLittleEndian<quint32>(map.constData()+12);
   require((w==144&&h==81)||(w==576&&h==324)||(w==2304&&h==1296),"Supported map dimensions");
   const int n=w*h;require(map.size()==16+n*6,"Map size");pixels.resize(n);
   for(int p=0;p<n;++p){const auto idx=qFromLittleEndian<quint32>(map.constData()+16+p*4);const auto frac=qFromLittleEndian<quint16>(map.constData()+16+n*4+p*2);
    require(idx<quint32(n-w-1)&&idx%w<w-1&&frac<1024,"Map coordinate");
    const int fx=frac%32,fy=frac/32;
    pixels[p]={idx*3,{quint16((32-fx)*(32-fy)),quint16(fx*(32-fy)),quint16((32-fx)*fy),quint16(fx*fy)}};
   }
   sha=QCryptographicHash::hash(map,QCryptographicHash::Sha256).toHex();
  }
  int width() const {return w;}
  int height() const {return h;}
  const QByteArray& sha256() const {return sha;}
 private:
  friend class RvCameraStream;
  struct Pixel {quint32 offset;quint16 weights[4];};
  int w=0,h=0;QByteArray sha;QVector<Pixel> pixels;
 };
 // Normalize physical mounting before any calibration-map sampling. ToF and
 // inferred depth already use the calibrated reference frame and stay native.
 static QImage correctCameraMounting(const QImage& image,int degrees) {
  require(degrees==0||degrees==180,"Camera mounting angle must be 0 or 180 degrees");
  return degrees==180?image.mirrored(true,true):image;
 }
 static QImage process(const QByteArray& nv,const QByteArray& map) {return process(nv,Calibration(map));}
 // Decoded H.265 remains in the original camera geometry. Shrink at the same
 // half-pixel centres as the Native 4x preview, then rectify exactly once.
 // This does not rotate the model input or change any ToF coordinates.
 static QImage prepareHd(const QImage& decoded,const Calibration& calibration) {
  require(decoded.size()==QSize(2304,1296)&&calibration.width()==576,"HD model input dimensions");
  const auto source=decoded.convertToFormat(QImage::Format_RGB888);
  QImage small(576,324,QImage::Format_RGB888);
  for(int y=0;y<324;++y) {
   const auto* a=source.constScanLine(4*y+1);const auto* b=source.constScanLine(4*y+2);
   auto* row=small.scanLine(y);
   for(int x=0;x<576;++x)for(int c=0;c<3;++c) {
    const int i=(4*x+1)*3+c;
    row[x*3+c]=(unsigned(a[i])+a[i+3]+b[i]+b[i+3]+2)/4;
   }
  }
  return rectify(small,calibration);
 }
 static QImage process(const QByteArray& nv,const Calibration& calibration,QImage* scratch=nullptr) {
  const int w=calibration.w,h=calibration.h,ch=(h+1)/2*2;
  require(nv.size()==w*ch*3/2,"NV12/map size");
  const auto* src=reinterpret_cast<const unsigned char*>(nv.constData());
  QImage local;QImage& raw=scratch?*scratch:local;
  if(raw.size()!=QSize(w,h)||raw.format()!=QImage::Format_RGB888)raw=QImage(w,h,QImage::Format_RGB888);
  auto clip=[](int v){return static_cast<unsigned char>(std::clamp(v,0,255));};
  for(int y=0;y<h;++y){auto* row=raw.scanLine(y);for(int x=0;x<w;++x){int yy=src[y*w+x],p=w*ch+(y/2)*w+(x/2)*2,u=int(src[p])-128,v=int(src[p+1])-128;
   row[x*3]=clip(yy+((22987*v+8192)>>14));row[x*3+1]=clip(yy+((-11698*v-5636*u+8192)>>14));row[x*3+2]=clip(yy+((29049*u+8192)>>14));}}
  return rectify(raw,calibration);
 }
 static QImage rectify(const QImage& raw,const QByteArray& map){
  return rectify(raw,Calibration(map));
 }
 static QImage rectify(const QImage& raw,const Calibration& calibration){
  const int w=calibration.w,h=calibration.h,n=w*h;
  require(raw.size()==QSize(w,h)&&raw.format()==QImage::Format_RGB888&&raw.bytesPerLine()==w*3,"RGB/map layout");
  QImage out(w,h,QImage::Format_RGB888);auto* dst=out.bits();const auto* src=raw.constBits();
  const int offsets[4]={0,3,w*3,(w+1)*3};
  for(int p=0;p<n;++p){const auto& pixel=calibration.pixels[p];
   for(int c=0;c<3;++c){unsigned sum=0;for(int k=0;k<4;++k)sum+=pixel.weights[k]*src[pixel.offset+offsets[k]+c];dst[p*3+c]=(sum+512)>>10;}}
  return out;
 }
 static QJsonObject validate(const QByteArray& header,const QByteArray& json,const QByteArray& map) {
  return validate(header,json,Calibration(map));
 }
 static QJsonObject validate(const QByteArray& header,const QByteArray& json,const Calibration& calibration,bool nativeEnvelope=false) {
  require(wireWidth(header)==calibration.width(),"Calibration dimensions");
  require(header.size()==16,"Header size");auto u=[&](int i){return qFromLittleEndian<quint32>(header.constData()+i*4);};
  if(u(0)==0x384e4143) {
   wireWidth(header);
   require(json.size()==u(1),"CAN8 JSON length");
   QJsonParseError error;auto doc=QJsonDocument::fromJson(json,&error);require(error.error==QJsonParseError::NoError&&doc.isObject(),"CAN8 metadata JSON");
   auto m=doc.object();const int fusion=m["fusion_protocol"].toInt();
   require(m["protocol"].toInt()==8&&(fusion==4||fusion==5||fusion==7)
       &&((m["dtof_bytes"]==6144&&m["dtof_width"]==48&&m["dtof_height"]==32)
          ||(m["dtof_bytes"]==256&&m["dtof_width"]==8&&m["dtof_height"]==8))
       &&m["dtof_format"]=="range_uq2_u16le_snr_u8_valid_u8"&&m["dtof_coordinate"]=="sensor_radial_native"
       &&m["dtof_range_unit"]=="0.25_mm"&&m["dtof_source"]=="tmf8829_ipc_first_peak","CAN8 raw contract");
   QByteArray legacy=header; qToLittleEndian<quint32>(quint32(fusion==4?0x344e4143:fusion==5?0x354e4143:0x374e4143),legacy.data());
   const int rawBytes=m["dtof_bytes"].toInt();require(u(3)>=quint32(rawBytes),"CAN8 payload length");qToLittleEndian<quint32>(u(3)-rawBytes,legacy.data()+12);
   auto old=m;old["protocol"]=fusion;auto oldJson=QJsonDocument(old).toJson(QJsonDocument::Compact);qToLittleEndian<quint32>(oldJson.size(),legacy.data()+4);
   validate(legacy,oldJson,calibration,true);
   require(m["work_mode"]=="rgb_tof"&&m["tof_reused"]==false,"CAN8 live capture");
   require(RvTofFrame{QByteArray(rawBytes,0),m}.isValid(),"CAN8 sensor identity");
   return m;
  }
  const int w=wireWidth(header),h=w*9/16;const bool cameraOnly=u(0)==0x364e4143,post=u(0)==0x374e4143;
  require(json.size()==u(1),"JSON length");
  QJsonParseError error;auto doc=QJsonDocument::fromJson(json,&error);require(error.error==QJsonParseError::NoError && doc.isObject(),"Invalid metadata JSON");auto m=doc.object();
  require(m["protocol"].toInt()==(post?7:(cameraOnly?6:(w==576?4:5))) && (cameraOnly || post || (m["width"].toInt()==144 && m["height"].toInt()==81)) && m["rgb_width"].toInt()==w && m["rgb_height"].toInt()==h,"Frame dimensions/version");
  if(post){validatePost(m,u(3));require(m["coded_height"].toInt()==(h+1)/2*2,"Post NV12 coded height");}
  require(m["replay"].isBool() && !m["replay"].toBool() && m["rectified"].isBool() && !m["rectified"].toBool(),"Live/distorted contract");
  require(m["pixel_format"]=="NV12" && m["color_range"]=="full" && m["color_matrix"]=="canonical-full-601" && (m["resize"]==(w==576?"area4x4-round-half-up":(w==144?"area16x16-round-half-up":"identity")) || (w==144 && m["resize"]=="bilinear16x16-round-half-up") || (w==576 && m["resize"]=="bilinear4x4-round-half-up")) && m["y_stride"].toInt()==w && m["uv_stride"].toInt()==w,"Pixel format contract");
  require(w==576 || m["coded_height"].toInt()==(h+1)/2*2,"Coded NV12 height");
  if(cameraOnly){
   if(m["pixel_format"]=="NV12")require(u(2)==4478976,"Raw camera length");
   require(m["work_mode"]=="camera_only" && m["depth_present"].isBool() && !m["depth_present"].toBool(),"Camera-only mode");
   for(const auto* k:{"tof_timestamp_ns","tof_sequence","pair_delta_ms","depth_unit","tof_reused"})require(!m.contains(k),"Camera-only fusion fields");
   require(m["rgb_sequence"].isDouble() && m["rgb_timestamp_ns"].isDouble(),"Camera identity");
  }else require(m["depth_unit"]=="camera-Z_m_float32" && m["pair_delta_ms"].isDouble() && std::isfinite(m["pair_delta_ms"].toDouble()) && std::abs(m["pair_delta_ms"].toDouble())<=200,"Depth/pairing contract");
  require(m["model"]=="Native","Retired board model");
  if(!cameraOnly)require(nativeEnvelope&&!post&&m["work_mode"]=="rgb_tof"
      &&m["depth_present"]==false&&m["capture_only"]==true&&m["inference_location"]=="none"
      &&m["depth_source"]=="none","Model-free capture contract");
  require(m["frame_id"].isDouble() && m["frame_id"].toDouble()>0 && m["generation"].isDouble(),"Missing identity");
  require(m["rectify_map_sha256"].toString().toLatin1()==calibration.sha256(),"Calibration map mismatch");return m;
 }
 static int wireWidth(const QByteArray& header) {
  require(header.size()==16,"Header size");auto u=[&](int i){return qFromLittleEndian<quint32>(header.constData()+4*i);};
  require(u(1)>0&&u(1)<=4096,"Packet limits");
  if(u(0)==0x384e4143){require(u(3)==46656+6144||u(3)==186624+6144||u(3)==393216+6144||u(3)==46656+256||u(3)==186624+256||u(3)==393216+256,"CAN8 payload limit");if(u(2)==17712)return 144;if(u(2)==279936)return 576;throw std::runtime_error("CAN8 camera size");}
  if(u(0)==0x364e4143){require(u(2)==4478976&&u(3)==0,"Camera-only lengths");return 2304;}
  if(u(0)==0x374e4143){require(u(3)==46656||u(3)==186624||u(3)==393216,"Post depth limit");if(u(2)==17712)return 144;if(u(2)==279936)return 576;throw std::runtime_error("Post image size");}
  require(u(3)==46656,"Depth length");
  if(u(0)==0x344e4143&&u(2)==279936)return 576;
  if(u(0)==0x354e4143&&u(2)==17712)return 144;
  if(u(0)==0x354e4143&&u(2)==4478976)return 2304;
  throw std::runtime_error("Unsupported CAN4/5 dimensions or pixel format");
 }
 static void validatePost(const QJsonObject& m,int bytes){
  int w=m["width"].toInt(),h=m["height"].toInt(),tw=m["target_width"].toInt(),th=m["target_height"].toInt();bool board=m["requested_location"]=="rv1103";auto method=m["requested_method"].toString();
  require((tw==288&&th==162)||(tw==384&&th==256)||(tw==576&&th==324),"Post target");
  require(board||m["requested_location"]=="host","Post location");
  require(method=="bilinear_valid"||(!board&&method=="fast_guided_guarded"&&m["rgb_width"].toInt()==576),"Post method/guide");require(!board||tw!=576,"Host-only size");
  require(w==(board?tw:144)&&h==(board?th:81)&&bytes==w*h*4,"Post transmitted size");
  require(m["model"]=="B0"&&m["work_mode"]=="fusion"&&m["depth_present"]==true&&m["base_width"].toInt()==144&&m["base_height"].toInt()==81&&m["depth_stride"].toInt()==w*4&&m["depth_format"]=="float32_le"&&m["pixel_center"]=="half_pixel"&&m["depth_location"]==(board?"rv1103":"none")&&m["depth_method"]==(board?method:QString("none"))&&m["tof_reused"]==false,"Post contract");
  auto base=m["base_K"].toArray(),k=m["depth_K"].toArray();require(base==QJsonArray{143.5,143.5,71.5,40.0}&&k.size()==4,"Base K");double sx=w/144.,sy=h/81.;double expected[]={sx*base[0].toDouble(),sy*base[1].toDouble(),sx*(base[2].toDouble()+.5)-.5,sy*(base[3].toDouble()+.5)-.5};for(int i=0;i<4;++i)require(k[i].isDouble()&&std::isfinite(k[i].toDouble())&&std::abs(k[i].toDouble()-expected[i])<1e-5,"Output K");
 }
 static QByteArray loadMap(int width,bool wide=false){
  require(!wide||width==576,"Wide camera supports the HD 576 preview only");
  QFile f(wide?QString(":/camera/nv12_rectify_wide.bin"):width==576?QString(":/camera/nv12_rectify.bin"):QString(":/camera/nv12_rectify_%1.bin").arg(width));
  require(f.open(QIODevice::ReadOnly),"Missing calibration map");return f.readAll();
 }
 static const Calibration &hdCalibration(bool wide=false){
  if(wide){static const Calibration value(loadMap(576,true));return value;}
  static const Calibration value(loadMap(576));return value;
 }
protected:
 void run() override {
  clock.start();stopProcessing=false;
  auto* processor=QThread::create([this]{consume();});processor->start();
  struct Join {RvCameraStream* p;QThread* t;~Join(){p->stopProcessing=true;p->ready.wakeAll();t->wait();delete t;}} join{this,processor};
  std::shared_ptr<const Calibration> calibration;
  while(!isInterruptionRequested()){
   try{QTcpSocket socket;socket.setProxy(QNetworkProxy::NoProxy);socket.setReadBufferSize(16+4096+4478976);socket.connectToHost(host,port);QElapsedTimer connectClock;connectClock.start();
    while(socket.state()!=QAbstractSocket::ConnectedState){if(isInterruptionRequested())return;require(connectClock.elapsed()<3000,"Connection timeout");if(!socket.waitForConnected(3000) && socket.state()==QAbstractSocket::UnconnectedState)throw std::runtime_error(socket.errorString().toStdString());}
    // Fixed capability token on the same connection. Old senders ignore it;
    // upgraded senders enable native ToF without changing configuration.
    const QByteArray capability("RVDTOF8\n",8);
    require(socket.write(capability)==capability.size(),"Native dToF capability write");
    if(socket.bytesToWrite())require(socket.waitForBytesWritten(1000),"Native dToF capability timeout");
    auto exact=[&](int n){QByteArray data;data.reserve(n);QElapsedTimer clock;clock.start();while(data.size()<n){if(isInterruptionRequested())throw std::runtime_error("Stopped");if(socket.bytesAvailable()){data+=socket.read(n-data.size());continue;}require(socket.state()==QAbstractSocket::ConnectedState,"Disconnected");require(clock.elapsed()<3000,"Frame timeout");socket.waitForReadyRead(100);}return data;};
    double previous=-1,generation=-1,rgbSequence=-1;
    while(!isInterruptionRequested()){
     auto header=exact(16);auto u=[&](int i){return qFromLittleEndian<quint32>(header.constData()+i*4);};
     const int width=wireWidth(header);if(!calibration||width!=calibration->width())calibration=std::make_shared<const Calibration>(loadMap(width));
     auto m=validate(header,exact(u(1)),*calibration);double id=m["frame_id"].toDouble(),gen=m["generation"].toDouble();require(previous<0 || (gen==generation && id>previous),"Frame identity/progress");previous=id;generation=gen;
     if(m["protocol"].toInt()==6){double seq=m["rgb_sequence"].toDouble();require(rgbSequence<0||seq>rgbSequence,"Camera sequence progress");rgbSequence=seq;}
     auto nv=exact(u(2));const bool raw=m["protocol"].toInt()==8;const int rawBytes=raw?m["dtof_bytes"].toInt():0;auto depth=exact(u(3)-rawBytes);RvTofFrame tof;tof.metadata=m;
     if(raw){tof.samples=exact(rawBytes);require(tof.isValid(),"Invalid native dToF frame");}
     // CAN8 retains its fixed placeholder layout; it must never contain predictions.
     for(char value:depth)require(value==0,"Capture depth placeholder must be zero");
     depth.clear();
     QMutexLocker lock(&mutex);if(pending.valid)++dropped;pending={std::move(nv),std::move(depth),calibration,m,std::move(tof),epoch,clock.nsecsElapsed(),true};++received;ready.wakeOne();
    }
   }catch(const std::exception& e){if(isInterruptionRequested())return;publishError(QString::fromUtf8(e.what()));}
   for(int i=0;i<10 && !isInterruptionRequested();++i)msleep(100);
  }
 }
private:
 struct Input {QByteArray nv,depth;std::shared_ptr<const Calibration> calibration;QJsonObject m;RvTofFrame tof;quint64 epoch=0;qint64 receivedNs=0;bool valid=false;};
public:
 static void guide(const QImage& image,int w,int h,std::vector<float>& out){
  out.resize(w*h);for(int y=0;y<h;++y)for(int x=0;x<w;++x){double px=std::clamp((x+.5)*image.width()/w-.5,0.,double(image.width()-1)),py=std::clamp((y+.5)*image.height()/h-.5,0.,double(image.height()-1));int x0=int(px),y0=int(py),x1=std::min(x0+1,image.width()-1),y1=std::min(y0+1,image.height()-1);float fx=float(px-x0),fy=float(py-y0);
   auto Y=[&](int u,int v){const auto* p=image.constScanLine(v)+u*3;return (.299f*p[0]+.587f*p[1]+.114f*p[2])/255.f;};out[y*w+x]=(1-fy)*((1-fx)*Y(x0,y0)+fx*Y(x1,y0))+fy*((1-fx)*Y(x0,y1)+fx*Y(x1,y1));}
 }
private:
 void consume(){
  depthpost::Bilinear post;depthpost::Guided guided;std::vector<float> base(11664),out,low,high;QImage rgbScratch;
  while(!stopProcessing){Input input;{QMutexLocker lock(&mutex);if(!pending.valid){ready.wait(&mutex,100);continue;}input=std::move(pending);pending=Input{};}
   try{QElapsedTimer timer;timer.start();auto image=process(input.nv,*input.calibration,&rgbScratch);auto m=input.m;double postMs=0;const QByteArray baseDepth=m["width"].toInt()==144&&m["height"].toInt()==81?input.depth:QByteArray();
    if((m["protocol"].toInt()==7||(m["protocol"].toInt()==8&&m["fusion_protocol"].toInt()==7))&&m["requested_location"]=="host"){
     QElapsedTimer pt;pt.start();int w=m["target_width"].toInt(),h=m["target_height"].toInt();post.configure(w,h);out.resize(w*h);
     for(int i=0;i<11664;++i){quint32 bits=qFromLittleEndian<quint32>(input.depth.constData()+i*4);std::memcpy(&base[i],&bits,4);}
     if(m["requested_method"]=="fast_guided_guarded"){require(image.width()==576,"576 guide unavailable");guide(image,144,81,low);guide(image,w,h,high);guided.fit(base.data(),low.data());post.run(base.data(),out.data(),low.data(),high.data(),guided.meanA.data(),guided.meanB.data());m["guide_source"]="same_packet_rectified_576";}else post.run(base.data(),out.data());
     input.depth.resize(w*h*4);for(int i=0;i<w*h;++i){quint32 bits;std::memcpy(&bits,&out[i],4);qToLittleEndian(bits,input.depth.data()+i*4);}
     m["transmitted_width"]=144;m["transmitted_height"]=81;m["width"]=w;m["height"]=h;m["depth_stride"]=w*4;m["depth_location"]="host";m["depth_method"]=m["requested_method"];auto k=m["base_K"].toArray();double sx=w/144.,sy=h/81.;m["depth_K"]=QJsonArray{sx*k[0].toDouble(),sy*k[1].toDouble(),sx*(k[2].toDouble()+.5)-.5,sy*(k[3].toDouble()+.5)-.5};postMs=pt.nsecsElapsed()/1e6;
    }
    m["host_post_ms"]=postMs;m["host_processing_ms"]=timer.nsecsElapsed()/1e6;
    QMutexLocker lock(&mutex);if(input.epoch!=epoch)continue;latest.image=std::move(image);latest.depth=std::move(input.depth);latest.baseDepth=baseDepth;latest.tof=std::move(input.tof);latest.tof.metadata=m;latest.metadata=m;latest.processingMs=timer.nsecsElapsed()/1e6;m["host_rx_count"]=double(received);m["host_rx_elapsed_ms"]=clock.nsecsElapsed()/1e6;latest.metadata=m;latest.received=received;latest.dropped=dropped;latest.receivedNs=input.receivedNs;latest.status="NV12 / host color + rectification";++latest.sequence;
   }catch(const std::exception& e){publishError(QString::fromUtf8(e.what()),input.epoch);}
  }
 }
 void publishError(const QString& s){QMutexLocker lock(&mutex);publishErrorLocked(s);}
 void publishError(const QString& s,quint64 expectedEpoch){
  QMutexLocker lock(&mutex);if(expectedEpoch!=epoch)return;publishErrorLocked(s);
 }
 // Caller holds mutex: clear one complete mailbox state and revoke old work.
 void publishErrorLocked(const QString& s){++epoch;pending=Input{};latest.image=QImage();latest.depth.clear();latest.baseDepth.clear();latest.tof={};latest.metadata={};latest.receivedNs=0;latest.processingMs=0;latest.status=s;latest.received=received;latest.dropped=dropped;++latest.reconnects;++latest.sequence;}
 QMutex mutex;QWaitCondition ready;Snapshot latest;Input pending;std::atomic<bool> stopProcessing{false};quint64 received=0,dropped=0,epoch=0;QElapsedTimer clock;
};
