#include "RvDtofBoardStream.h"
#include "../tools/litesr_board/Contour.h"
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>

struct RvDtofBoardStream::Decoder {std::atomic_bool cancelled{false};};
namespace {
using namespace RvCaptureContract;
constexpr int HeaderBytes=36,MaximumJsonBytes=4096,NativePacketLimit=16384;
constexpr int LiteSrPacketLimit=HeaderBytes+MaximumJsonBytes+DepthBytes+2*Native48Bytes;

RvTofFrame nativeMeasurement(const QByteArray& bytes,const QJsonObject& packet,const QSize& grid,
                             quint64 sequence,quint64 timestamp,int fov){
    RvTofFrame frame{bytes,packet};auto& m=frame.metadata;
    m["dtof_fov_correction"]=fov;m["frame_id"]=qint64(sequence);m["tof_sequence"]=qint64(sequence);m["tof_timestamp_ns"]=qint64(timestamp);
    m["tof_reused"]=false;m["work_mode"]="dtof_candidate";
    const int mode=packet["candidate_mode"].toInt();
    m["model"]=isLiteSr(mode)?RvDtofCandidates::name(mode):QStringLiteral("native_hd_capture");
    m["dtof_width"]=grid.width();m["dtof_height"]=grid.height();m["dtof_bytes"]=bytes.size();
    m["dtof_format"]="range_uq2_u16le_snr_u8_valid_u8";m["dtof_range_unit"]="0.25_mm";
    m["dtof_source"]="tmf8829_ipc_first_peak";m["dtof_coordinate"]="sensor_radial_native";
    m["tof_filter_raw_preserved"]=true;m["pairing_policy"]="independent_streams";
    return frame;
}

QThreadPool *liveDecodePool(){
    static auto *pool=[](){auto *value=new QThreadPool(QCoreApplication::instance());
        value->setObjectName("rvLiveDecodePool");value->setMaxThreadCount(1);return value;}();return pool;
}
}
RvDtofBoardStream::RvDtofBoardStream(const QJsonObject& receipt,Output output,
    QObject* parent,const QString& host,quint16 port,QThreadPool* pool)
    :QObject(parent),expected([&]{auto value=receipt;value["protocol"]=RvCaptureContract::PairedProtocol;return value;}()),
     liteSr(isLiteSr(expected["candidate_mode"].toInt(-1))),
     requestHoleFill(output==Output::BoardDepth&&RvCaptureContract::supportsHoleFill(expected["candidate_mode"].toInt(-1))
        &&expected["native_hole_fill_protocol"]==RvCaptureContract::NativeHoleFillProtocol),
     decoder(std::make_shared<Decoder>()),decodePool(pool?pool:liveDecodePool()){
    socket.setProxy(QNetworkProxy::NoProxy);socket.setReadBufferSize(8192);
    poll.setInterval(20);timeout.setSingleShot(true);progress.start();
    connect(&poll,&QTimer::timeout,this,[this]{ask();});
    connect(&socket,&QTcpSocket::connected,this,[this]{socket.setSocketOption(QAbstractSocket::LowDelayOption,1);poll.start();ask();});
    connect(&socket,&QTcpSocket::readyRead,this,[this]{read();});
    connect(&socket,&QTcpSocket::errorOccurred,this,[this]{fail(tr("RV1103 candidate stream disconnected"));});
    connect(&timeout,&QTimer::timeout,this,[this]{fail(tr("RV1103 candidate stream timed out"));});
    connect(&watcher,&QFutureWatcher<Decoded>::finished,this,[this]{finish();});
    timeout.start(3000);socket.connectToHost(host,port);
}
RvDtofBoardStream::~RvDtofBoardStream(){
    decoder->cancelled.store(true);rawFrame={};depthFrame={};videoProgress={};error={};
    watcher.disconnect(this);socket.disconnect(this);socket.abort();
}
void RvDtofBoardStream::fail(const QString& text){
    if(failed)return;failed=true;decoder->cancelled.store(true);poll.stop();timeout.stop();socket.abort();
    if(error){const auto callback=error;callback(text);}
}
void RvDtofBoardStream::ask(){
    if(failed||pending||socket.state()!=QAbstractSocket::ConnectedState)return;
    if(progress.elapsed()>10000){fail(tr("RV1103 candidate stream timed out"));return;}
    QByteArray bytes(32,0);qToLittleEndian<quint32>(RvCaptureContract::PairedRequestMagic,bytes.data());
    qToLittleEndian<quint32>(++request,bytes.data()+4);
    qToLittleEndian<quint64>(identities.depthSequence,bytes.data()+8);qToLittleEndian<quint64>(identities.rawSequence,bytes.data()+16);
    if(requestHoleFill)qToLittleEndian(RvCaptureContract::HoleFillRequest,bytes.data()+RvCaptureContract::HoleFillRequestOffset);
    pending=true;roundtrip.restart();timeout.start(3000);socket.write(bytes);
}
void RvDtofBoardStream::read(){
    if(failed)return;
    if(!pending||decoding||socket.bytesAvailable()+buffer.size()>(liteSr?LiteSrPacketLimit:NativePacketLimit)){fail(tr("Invalid candidate stream size"));return;}
    buffer+=socket.readAll();if(buffer.size()<HeaderBytes)return;
    const auto u=[&](int off){return qFromLittleEndian<quint32>(buffer.constData()+off);};
    const auto jsonSize=u(4),depthSize=u(8),rawSize=u(16),sourceSize=u(32);
    const auto grid=nativeCaptureGrid(expected);
    const bool validDepth=liteSr?(depthSize==0||depthSize==DepthBytes)&&sourceSize==(depthSize?Native48Bytes:0)
        :!depthSize&&(!sourceSize||(requestHoleFill&&rawSize&&sourceSize==HoleFillBytes));
    if(u(0)!=PairedResponseMagic||jsonSize==0||jsonSize>MaximumJsonBytes||!validDepth
        ||u(12)||u(20)||u(24)||u(28)||(rawSize&&rawSize!=quint32(grid.width()*grid.height()*4))||!grid.isValid()){
        fail(tr("Invalid candidate stream header"));return;
    }
    const auto total=HeaderBytes+jsonSize+depthSize+rawSize+sourceSize;if(buffer.size()<total)return;
    if(buffer.size()!=total){fail(tr("Unexpected candidate stream payload"));return;}
    decoding=true;auto packet=std::move(buffer);buffer.clear();
    watcher.setFuture(QtConcurrent::run(decodePool,[state=decoder,packet=std::move(packet),receipt=expected,previous=identities,id=request]{
        return decode(state,packet,receipt,previous,id);
    }));
}
RvDtofBoardStream::Decoded RvDtofBoardStream::decode(const std::shared_ptr<Decoder>& decoder,
    const QByteArray& buffer,const QJsonObject& expected,Identities previous,quint32 request){
    Decoded out;out.request=request;out.identities=previous;if(decoder->cancelled.load())return out;
    const auto reject=[&](const QString& message){out.error=message;return out;};
    const auto u=[&](int off){return qFromLittleEndian<quint32>(buffer.constData()+off);};
    const auto jsonSize=u(4),depthSize=u(8),rawSize=u(16),sourceSize=u(32);QJsonParseError parse;
    const auto doc=QJsonDocument::fromJson(buffer.mid(HeaderBytes,jsonSize),&parse);const auto m=doc.object();
    const bool liteSr=isLiteSr(expected["candidate_mode"].toInt(-1));
    const int expectedFov=expected["expected_fov"].toInt(6);
    const auto grid=nativeCaptureGrid(expected);
    if(expectedFov!=1&&expectedFov!=2&&expectedFov!=6)return reject(tr("Unsupported device calibration"));
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()||!boardImagingIdentity(m)||!nativeVideoIdentity(m)
        ||m["protocol"]!=expected["protocol"]||m["boot_id"]!=expected["boot_id"]||m["generation"]!=expected["generation"]
        ||m["candidate_mode"]!=expected["candidate_mode"]||m["options"]!=expected["options"]||m["request"].toInteger()!=request
        ||m["data"]!=bool(depthSize)||m["photon_data"]!=false||m["rgb_data"]!=false||!m["raw_data"].isBool()
        ||m["raw_data"].toBool()!=bool(rawSize)||m["native_capture_protocol"]!=RvCaptureContract::NativeCaptureProtocolV2
        ||m["native_columns"]!=grid.width()||m["native_rows"]!=grid.height())
        return reject(tr("Candidate identity or parameters mismatch"));
    bool a=false,b=false,c=false;
    const auto encoded=m["video_encoded_sequence"].toString().toULongLong(&a),retrieved=m["video_retrieved_sequence"].toString().toULongLong(&b),observed=m["video_observed_ns"].toString().toULongLong(&c);
    const auto& last=previous;const bool same=encoded==last.videoEncoded&&retrieved==last.videoRetrieved&&observed==last.videoObserved;
    out.videoAge=m["video_age_ms"].toDouble(-1);
    if(m["video_progress_source"]!="venc_info:chn0"||!a||!b||!c||!encoded||!retrieved||!observed||retrieved>encoded
        ||encoded>9007199254740991ULL||observed>9007199254740991ULL||!std::isfinite(out.videoAge)||out.videoAge<0||out.videoAge>=1000
        ||(!same&&(encoded<=last.videoEncoded||retrieved<=last.videoRetrieved||observed<=last.videoObserved)))
        return reject(tr("Invalid HD encoder progress or identity"));
    out.identities.videoEncoded=encoded;out.identities.videoRetrieved=retrieved;out.identities.videoObserved=observed;out.videoMeta=m;
    out.rawPresent=rawSize!=0;
    if(rawSize){
        bool seqOk=false,stampOk=false;const auto seq=m["raw_sequence"].toString().toULongLong(&seqOk),stamp=m["raw_timestamp_ns"].toString().toULongLong(&stampOk);
        out.rawAge=m["raw_age_ms"].toDouble(-1);
        if(!seqOk||!stampOk||seq<=last.rawSequence||stamp<=last.rawCapture||seq>9007199254740991ULL||stamp>9007199254740991ULL
            ||!std::isfinite(out.rawAge)||out.rawAge<0||m["raw_fov_correction"]!=expectedFov||m["raw_columns"]!=grid.width()||m["raw_rows"]!=grid.height()
            ||(m["tof_warning_flags"]!=0&&m["tof_warning_flags"]!=8&&m["tof_warning_flags"]!=16))
            return reject(tr("Invalid candidate measurement identity"));
        out.identities.rawSequence=seq;out.identities.rawCapture=stamp;
        out.native=nativeMeasurement(buffer.mid(HeaderBytes+jsonSize+depthSize,rawSize),m,grid,seq,stamp,expectedFov);
        const auto& native=out.native;
        if(!native.isValid()||(m.contains("dtof_fov_correction")&&m["dtof_fov_correction"]!=expectedFov)
            ||(m.contains("tof_fov_correction")&&m["tof_fov_correction"]!=expectedFov))return reject(tr("Invalid candidate depth values"));
        if(!liteSr&&sourceSize){
            if(m["native_hole_fill_protocol"]!=RvCaptureContract::NativeHoleFillProtocol||m["tof_warning_flags"]!=0)
                return reject(tr("Invalid candidate depth values"));
            out.holeFillRanges=buffer.mid(HeaderBytes+jsonSize+rawSize,sourceSize);
            for(int i=0;i<native.sampleCount();++i){
                if(qFromLittleEndian<quint16>(out.holeFillRanges.constData()+i*2)&&native.valid(i)&&native.rangeUq2(i))
                    return reject(tr("Invalid candidate depth values"));
            }
        }
    }
    if(depthSize){
        bool seqOk=false,stampOk=false;
        const auto seq=m["tof_sequence"].toString().toULongLong(&seqOk),stamp=m["tof_timestamp_ns"].toString().toULongLong(&stampOk);
        const int warnings=m["depth_native_warning_flags"].toInt(-1);
        out.depthAge=m["age_ms"].toDouble(-1);
        const double npu=m["npu_ms"].toDouble(-1),compute=m["compute_ms"].toDouble(-1);
        if(!liteSr||expectedFov!=6||!seqOk||!stampOk||seq<=last.depthSequence||stamp<=last.depthCapture
            ||seq>9007199254740991ULL||stamp>9007199254740991ULL
            ||m["depth_native_sequence"].toString()!=m["tof_sequence"].toString()
            ||m["depth_native_timestamp_ns"].toString()!=m["tof_timestamp_ns"].toString()
            ||m["depth_native_fov_correction"]!=expectedFov||(warnings!=0&&warnings!=8&&warnings!=16)
            ||!std::isfinite(out.depthAge)||out.depthAge<0||!std::isfinite(npu)||!std::isfinite(compute)||npu<0||compute<npu)
            return reject(tr("Invalid LiteSR frame identity or timing"));
        const auto source=nativeMeasurement(buffer.mid(HeaderBytes+jsonSize+depthSize+rawSize,sourceSize),m,grid,seq,stamp,expectedFov);
        if(!source.isValid())return reject(tr("Invalid LiteSR support grid"));
        if(rawSize&&seq==out.identities.rawSequence&&
            (stamp!=out.identities.rawCapture||m["tof_warning_flags"]!=warnings||source.samples!=out.native.samples))
            return reject(tr("Invalid paired LiteSR measurement"));

        std::vector<float> values(Width*Height);
        const char* pixels=buffer.constData()+HeaderBytes+jsonSize;
        for(int i=0;i<Width*Height;++i){
            const auto bits=qFromLittleEndian<quint32>(pixels+4*i);std::memcpy(&values[i],&bits,4);
            const float z=values[i];
            if(!std::isfinite(z)||(z!=0&&(z<LiteSrMinimumM||z>LiteSrMaximumM)))return reject(tr("Invalid LiteSR depth values"));
        }
        if(decoder->cancelled.load())return out;
        const auto refined=LiteSrBoardContour::refineFast(values.data(),reinterpret_cast<const uint8_t*>(source.samples.constData()),0,{1,true});
        out.depth.resize(DepthBytes);
        out.coverage=QByteArray(reinterpret_cast<const char*>(refined.coverage.data()),Width*Height);
        for(int i=0;i<Width*Height;++i){
            quint32 bits;std::memcpy(&bits,&refined.depth[i],4);qToLittleEndian(bits,out.depth.data()+4*i);
        }
        out.identities.depthSequence=seq;out.identities.depthCapture=stamp;
        out.depthMeta=source.metadata;auto& metadata=out.depthMeta;
        metadata["depth_unit"]="sensor_radial_m_float32";metadata["depth_location"]="board";
        metadata["pairing_policy"]="same_native_measurement";metadata["native_order"]=true;metadata["tof_warning_flags"]=warnings;
        metadata["depth_method"]="litesr_board_full_host_contour_repair_v2";
        metadata["host_contour"]=QJsonObject{{"location","host_cpu_background"},{"postprocess_ms",refined.totalMs},
            {"filled_native_cells",refined.repairs.filledCells},{"outlier_native_cells",refined.repairs.outlierCells},
            {"coverage_semantics","display_opacity_only"}};
    }
    return out;
}
void RvDtofBoardStream::finish(){
    if(failed||!decoding)return;Decoded out;
    try{out=watcher.result();}catch(...){fail(tr("Invalid candidate stream header"));return;}
    if(out.request!=request){fail(tr("Candidate identity or parameters mismatch"));return;}
    if(!out.error.isEmpty()){fail(out.error);return;}
    const double videoAge=out.videoAge+roundtrip.nsecsElapsed()/1e6;
    // Drop the whole late packet without advancing accepted identities.
    if(videoAge>=1000){decoding=false;pending=false;timeout.stop();return;}
    identities=out.identities;QPointer<RvDtofBoardStream> guard(this);
    const auto alive=[&]{return guard&&!guard->failed;};
    out.videoMeta["video_age_ms"]=videoAge;
    if(videoProgress){const auto callback=videoProgress;callback(out.videoMeta);}
    if(!alive())return;
    if(out.rawPresent){const double age=out.rawAge+roundtrip.nsecsElapsed()/1e6;out.native.metadata["raw_age_ms"]=age;
        if(age<220){if(!liteSr)progress.restart();if(rawFrame){const auto callback=rawFrame;callback(out.native,out.holeFillRanges);}}}
    if(!alive())return;
    if(!out.depth.isEmpty()){
        const double age=out.depthAge+roundtrip.nsecsElapsed()/1e6;
        if(age<FreshMs){
            progress.restart();out.depthMeta["age_ms"]=age;
            if(depthFrame){const auto callback=depthFrame;callback(out.depth,out.coverage,out.depthMeta);}
        }
    }
    if(!alive())return;decoding=false;pending=false;timeout.stop();
}
