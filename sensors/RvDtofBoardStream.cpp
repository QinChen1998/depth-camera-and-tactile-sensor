#include "RvDtofBoardStream.h"
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>

struct RvDtofBoardStream::Decoder {std::atomic_bool cancelled{false};};
namespace {
QThreadPool *liveDecodePool(){
    static auto *pool=[](){auto *value=new QThreadPool(QCoreApplication::instance());
        value->setObjectName("rvLiveDecodePool");value->setMaxThreadCount(1);return value;}();return pool;
}
}
RvDtofBoardStream::RvDtofBoardStream(const QJsonObject& receipt,const RvDtofCandidates::Settings&,
    QObject* parent,const QString& host,quint16 port,QThreadPool* pool)
    :QObject(parent),expected([&]{auto value=receipt;value["protocol"]=RvCaptureContract::PairedProtocol;return value;}()),
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
    decoder->cancelled.store(true);rawFrame={};videoProgress={};error={};
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
    qToLittleEndian<quint32>(++request,bytes.data()+4);qToLittleEndian<quint64>(identities.rawSequence,bytes.data()+16);
    pending=true;roundtrip.restart();timeout.start(3000);socket.write(bytes);
}
void RvDtofBoardStream::read(){
    if(failed)return;
    if(!pending||decoding||socket.bytesAvailable()+buffer.size()>16384){fail(tr("Invalid candidate stream size"));return;}
    buffer+=socket.readAll();if(buffer.size()<36)return;
    const auto u=[&](int off){return qFromLittleEndian<quint32>(buffer.constData()+off);};
    const auto jsonSize=u(4),rawSize=u(16);
    if(u(0)!=RvCaptureContract::PairedResponseMagic||jsonSize==0||jsonSize>4096
        ||u(8)||u(12)||u(20)||u(24)||u(28)||u(32)||(rawSize&&rawSize!=quint32(nativeCaptureGrid(expected).width()*nativeCaptureGrid(expected).height()*4))
        ||!nativeCaptureGrid(expected).isValid()){fail(tr("Invalid candidate stream header"));return;}
    const auto total=36+jsonSize+rawSize;if(buffer.size()<total)return;
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
    const auto jsonSize=u(4),rawSize=u(16);QJsonParseError parse;
    const auto doc=QJsonDocument::fromJson(buffer.mid(36,jsonSize),&parse);const auto m=doc.object();
    const int expectedFov=expected["expected_fov"].toInt(6);
    const auto grid=nativeCaptureGrid(expected);
    if(expectedFov!=1&&expectedFov!=2&&expectedFov!=6)return reject(tr("Unsupported device calibration"));
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()||!nativeCaptureIdentity(m)||!nativeVideoIdentity(m)
        ||m["protocol"]!=expected["protocol"]||m["boot_id"]!=expected["boot_id"]||m["generation"]!=expected["generation"]
        ||m["candidate_mode"]!=expected["candidate_mode"]||m["options"]!=expected["options"]||m["request"].toInteger()!=request
        ||m["data"]!=false||m["photon_data"]!=false||m["rgb_data"]!=false||!m["raw_data"].isBool()
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
        auto &native=out.native;native.samples=buffer.mid(36+jsonSize,rawSize);native.metadata=m;auto& n=native.metadata;
        n["dtof_fov_correction"]=m["raw_fov_correction"];n["frame_id"]=qint64(seq);n["tof_sequence"]=qint64(seq);n["tof_timestamp_ns"]=qint64(stamp);
        n["tof_reused"]=false;n["work_mode"]="dtof_candidate";n["model"]="native_hd_capture";
        n["dtof_width"]=grid.width();n["dtof_height"]=grid.height();n["dtof_bytes"]=grid.width()*grid.height()*4;n["dtof_format"]="range_uq2_u16le_snr_u8_valid_u8";
        n["dtof_range_unit"]="0.25_mm";n["dtof_source"]="tmf8829_ipc_first_peak";n["dtof_coordinate"]="sensor_radial_native";
        n["tof_filter_raw_preserved"]=true;n["pairing_policy"]="independent_streams";
        if(!native.isValid()||(m.contains("dtof_fov_correction")&&m["dtof_fov_correction"]!=expectedFov)
            ||(m.contains("tof_fov_correction")&&m["tof_fov_correction"]!=expectedFov))return reject(tr("Invalid candidate depth values"));
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
        if(age<220){progress.restart();if(rawFrame){const auto callback=rawFrame;callback(out.native);}}}
    if(!alive())return;decoding=false;pending=false;timeout.stop();
}
