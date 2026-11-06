#include "RvDtofBoardStream.h"
#include "RvCameraStream.h"
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>

struct RvDtofBoardStream::Decoder {
    std::atomic_bool cancelled{false};
    std::unique_ptr<const RvCameraStream::Calibration> rgbCalibration;
    QImage rgbScratch;
};

namespace {
QThreadPool *liveDecodePool(){
    // Capture must not queue behind diagnostic PNG/disk work in Qt's global
    // pool. Each stream still has only one pending packet and one decoder job.
    // Application ownership also keeps view/stream destruction non-blocking.
    static auto *pool=[](){
        auto *value=new QThreadPool(QCoreApplication::instance());
        value->setObjectName("rvLiveDecodePool");value->setMaxThreadCount(1);return value;
    }();
    return pool;
}
}

RvDtofBoardStream::RvDtofBoardStream(const QJsonObject& receipt,const RvDtofCandidates::Settings& s,
    QObject* parent,const QString& host,quint16 port,QThreadPool* pool)
    :QObject(parent),expected([&]{auto value=receipt;if(RvDtofCandidates::isLiteSrFull(value["candidate_mode"].toInt(-1))&&value["paired_depth_native_protocol"]==LiteSrBoardContract::PairedProtocol)value["protocol"]=LiteSrBoardContract::PairedProtocol;return value;}()),settings(s),decoder(std::make_shared<Decoder>()),
     decodePool(pool?pool:liveDecodePool()) {
    socket.setProxy(QNetworkProxy::NoProxy);socket.setReadBufferSize(LiteSrBoardContract::isHighResolution(expected["candidate_mode"].toInt(-1))?1600000:800000);
    poll.setInterval(LiteSrBoardContract::isNative16Depth(expected["candidate_mode"].toInt(-1))?8:20);timeout.setSingleShot(true);expiry.setSingleShot(true);progress.start();
    connect(&poll,&QTimer::timeout,this,[this]{ask();});
    connect(&socket,&QTcpSocket::connected,this,[this]{socket.setSocketOption(QAbstractSocket::LowDelayOption,1);
        if(RvDtofCandidates::isTinyNv12(expected["candidate_mode"].toInt(-1))){tinyClock.start();socket.write(expected["generation"].toString().toLatin1()+QByteArray(4,0));}
        else {poll.start();ask();}});
    connect(&socket,&QTcpSocket::readyRead,this,[this]{read();});
    connect(&socket,&QTcpSocket::errorOccurred,this,[this]{fail(tr("RV1103 candidate stream disconnected"));});
    connect(&timeout,&QTimer::timeout,this,[this]{fail(tr("RV1103 candidate stream timed out"));});
    connect(&expiry,&QTimer::timeout,this,[this]{if(expired){const auto callback=expired;callback();}});
    connect(&watcher,&QFutureWatcher<Decoded>::finished,this,[this]{finish();});
    timeout.start(3000);socket.connectToHost(host,RvDtofCandidates::isTinyNv12(expected["candidate_mode"].toInt(-1))&&port==8832?8846:port);
}
RvDtofBoardStream::~RvDtofBoardStream() {
    // A queued/running job retains its own data, never this QObject. Destruction
    // disconnects finished delivery without waiting on the GUI thread.
    decoder->cancelled.store(true);
    frame={};rawFrame={};photonFrame={};rgbFrame={};videoProgress={};error={};expired={};
    watcher.disconnect(this);socket.disconnect(this);socket.abort();
}
bool RvDtofBoardStream::setHostContour(bool enabled,const RvLiteSrContour::RepairSettings& repair){
    if(failed||repair.holeMode<0||repair.holeMode>2||(enabled&&!supportsHostContour()))return false;
    if(hostContour==enabled&&hostRepair==repair)return true;
    hostContour=enabled;hostRepair=repair;++refinementRevision;expiry.stop();return true;
}
void RvDtofBoardStream::fail(const QString& text) {
    if(failed)return;
    failed=true;decoder->cancelled.store(true);poll.stop();timeout.stop();expiry.stop();socket.abort();
    if(error){const auto callback=error;callback(text);}
}
void RvDtofBoardStream::ask() {
    if(failed||pending||socket.state()!=QAbstractSocket::ConnectedState)return;
    if(progress.elapsed()>10000){fail(tr("RV1103 candidate stream timed out"));return;}
    QByteArray bytes(32,0);qToLittleEndian<quint32>(usesPairedProtocol()?LiteSrBoardContract::PairedRequestMagic:RvDtofCandidates::isLiteSr(expected["candidate_mode"].toInt(-1))?LiteSrBoardContract::RequestMagic:0x33515444,bytes.data());
    qToLittleEndian<quint32>(++request,bytes.data()+4);
    qToLittleEndian<quint64>(identities.sequence,bytes.data()+8);
    qToLittleEndian<quint64>(identities.rawSequence,bytes.data()+16);
    qToLittleEndian<quint64>(identities.rgbSequence,bytes.data()+24);
    pending=true;roundtrip.restart();timeout.start(3000);socket.write(bytes);
}
void RvDtofBoardStream::read() {
    if(RvDtofCandidates::isTinyNv12(expected["candidate_mode"].toInt(-1))){readTiny();return;}
    if(failed)return;
    if(!pending||decoding||socket.bytesAvailable()+buffer.size()>(LiteSrBoardContract::isHighResolution(expected["candidate_mode"].toInt(-1))?1600000:800000)){fail(tr("Invalid candidate stream size"));return;}
    const bool lite=RvDtofCandidates::isLiteSr(expected["candidate_mode"].toInt(-1));
    const bool paired=usesPairedProtocol();const int headerSize=paired?36:lite?32:24;
    buffer+=socket.readAll();if(buffer.size()<headerSize)return;
    const auto u=[&](int off){return qFromLittleEndian<quint32>(buffer.constData()+off);};
    const auto jsonSize=u(4),depthSize=u(8),coverageSize=u(12),rawSize=u(16),rgbSize=u(20);
    const auto inputSize=lite?u(24):0,signalSize=lite?u(28):0;
    const auto sourceSize=paired?u(32):0;
    const auto grid=liteSrNativeGrid(expected);const auto nativeBytes=quint32(grid.width()*grid.height()*4);
    const quint32 maximumPixels=quint32(RvDtofCandidates::outputWidth(expected["candidate_mode"].toInt(-1))*RvDtofCandidates::outputHeight(expected["candidate_mode"].toInt(-1)));
    if(u(0)!=(paired?LiteSrBoardContract::PairedResponseMagic:lite?LiteSrBoardContract::ResponseMagic:0x33465444)||jsonSize==0||jsonSize>4096||depthSize>maximumPixels*4||coverageSize>maximumPixels*5
        ||depthSize+coverageSize>maximumPixels*5||!RvDtofCandidates::supports(expected["candidate_mode"].toInt(-1))
        ||!grid.isValid()||(rawSize&&rawSize!=nativeBytes)||(rgbSize&&rgbSize!=279936)||(inputSize&&inputSize!=nativeBytes)||(signalSize&&signalSize!=nativeBytes/2)||(sourceSize&&sourceSize!=nativeBytes)){fail(tr("Invalid candidate stream header"));return;}
    if(RvDtofCandidates::isLiteSrHd(expected["candidate_mode"].toInt(-1))&&rgbSize){fail(tr("HD mode cannot carry a second camera stream"));return;}
    const auto total=headerSize+jsonSize+depthSize+coverageSize+rawSize+rgbSize+inputSize+signalSize+sourceSize;
    if(buffer.size()<total)return;
    if(buffer.size()!=total){fail(tr("Unexpected candidate stream payload"));return;}
    decoding=true;
    // No next request until GUI delivery finishes: no packet/result backlog.
    auto packet=std::move(buffer);buffer.clear();
    watcher.setFuture(QtConcurrent::run(decodePool,[state=decoder,packet=std::move(packet),
        receipt=expected,options=settings,previous=identities,id=request,refine=hostContour,repair=hostRepair,revision=refinementRevision]{
        return decode(state,packet,receipt,options,previous,id,refine,repair,revision);
    }));
}
RvDtofBoardStream::Decoded RvDtofBoardStream::decode(const std::shared_ptr<Decoder>& decoder,
    const QByteArray& buffer,const QJsonObject& expected,const RvDtofCandidates::Settings& settings,
    Identities previous,quint32 request,bool hostContour,const RvLiteSrContour::RepairSettings& repair,quint64 revision) {
    Decoded out;out.request=request;out.identities=previous;out.refinementRevision=revision;
    if(decoder->cancelled.load())return out;
    const auto reject=[&](const QString& message){out.error=message;return out;};
    const auto u=[&](int off){return qFromLittleEndian<quint32>(buffer.constData()+off);};
    const auto jsonSize=u(4),depthSize=u(8),coverageSize=u(12),rawSize=u(16),rgbSize=u(20);
    const int mode=expected["candidate_mode"].toInt(-1);
    const int expectedFov=expected["expected_fov"].toInt(1);
    if(expectedFov!=1&&expectedFov!=2)return reject(tr("Unsupported device calibration"));
    const bool lite=RvDtofCandidates::isLiteSr(mode);
    const bool paired=expected["protocol"]==LiteSrBoardContract::PairedProtocol;const int headerSize=paired?36:lite?32:24;
    const auto inputSize=lite?u(24):0,signalSize=lite?u(28):0;
    const auto sourceSize=paired?u(32):0;
    const auto grid=liteSrNativeGrid(expected);if(!grid.isValid())return reject(tr("Candidate identity or parameters mismatch"));
    const auto nativeBytes=quint32(grid.width()*grid.height()*4);
    out.rawPresent=rawSize!=0;out.rgbPresent=rgbSize!=0;
    auto &rawSequence=out.identities.rawSequence,&rawCapture=out.identities.rawCapture;
    auto &rgbSequence=out.identities.rgbSequence,&rgbCapture=out.identities.rgbCapture;
    auto &sequence=out.identities.sequence,&capture=out.identities.capture;
    auto &rgbCalibration=decoder->rgbCalibration;auto &rgbScratch=decoder->rgbScratch;
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(buffer.mid(headerSize,jsonSize),&parse);const auto m=doc.object();
    const bool data=m["data"].toBool();
    if(RvDtofCandidates::isCaptureOnly(mode)&&(data||depthSize||coverageSize||sourceSize))return reject(tr("Capture-only stream contains unexpected model output"));
    if(RvDtofCandidates::isCaptureOnly(mode)&&(m["photon_data"]!=false||inputSize||signalSize))return reject(tr("Candidate identity or parameters mismatch"));
    if(paired&&(!RvDtofCandidates::isLiteSrFull(mode)||sourceSize!=(data?nativeBytes:0u)))return reject(tr("Invalid paired board inference native measurement"));
    if(LiteSrBoardContract::isDepthOnly(mode)&&(rgbSize||m["rgb_data"]!=false))return reject(tr("Candidate identity or parameters mismatch"));
    if(lite&&(!liteSrBoardIdentity(m)||(data&&(m["encoding"]!="raw"||depthSize!=quint32(RvDtofCandidates::outputWidth(mode)*RvDtofCandidates::outputHeight(mode)*4)||coverageSize!=quint32(RvDtofCandidates::outputWidth(mode)*RvDtofCandidates::outputHeight(mode))
        ||m["input_rotation_degrees"]!=0||m["intensity_white_counts"]!=512||m["quantization"]!="INT8"
        ||(m["positive_near_policy"]!="model_output_no_native_near_fill"&&m["positive_near_policy"]!="positive_native_near_bilinear_not_model_prediction")))
        ||!m["photon_data"].isBool()||(m["photon_data"].toBool()?(inputSize!=nativeBytes||signalSize!=nativeBytes/2):(inputSize||signalSize))))
        return reject(tr("Candidate identity or parameters mismatch"));
    if(expected.contains("native_capture_protocol")&&(m["native_capture_protocol"]!=expected["native_capture_protocol"]||m["native_columns"]!=grid.width()||m["native_rows"]!=grid.height()
        ||(rawSize&&(m["raw_columns"]!=grid.width()||m["raw_rows"]!=grid.height()))
        ||(inputSize&&(m["input_columns"]!=grid.width()||m["input_rows"]!=grid.height()))))return reject(tr("Candidate identity or parameters mismatch"));
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()||m["protocol"]!=expected["protocol"]||m["boot_id"]!=expected["boot_id"]||m["generation"]!=expected["generation"]||m["candidate_mode"]!=expected["candidate_mode"]||m["options"]!=expected["options"]||m["request"].toInteger()!=request||!m["data"].isBool()||(!data&&(depthSize||coverageSize))||(data&&m["encoding"]!="raw"&&m["encoding"]!="runs")||!m["raw_data"].isBool()||!m["rgb_data"].isBool()||m["raw_data"].toBool()!=bool(rawSize)||m["rgb_data"].toBool()!=bool(rgbSize)){return reject(tr("Candidate identity or parameters mismatch"));}
    if(RvDtofCandidates::isLiteSrHd(mode)){
        bool a=false,b=false,c=false;
        const auto encoded=m["video_encoded_sequence"].toString().toULongLong(&a),retrieved=m["video_retrieved_sequence"].toString().toULongLong(&b),observed=m["video_observed_ns"].toString().toULongLong(&c);
        const auto& last=previous;const bool same=encoded==last.videoEncoded&&retrieved==last.videoRetrieved&&observed==last.videoObserved;
        out.videoAge=m["video_age_ms"].toDouble(-1);
        if(!paired||rgbSize||!liteSrHdIdentity(m)||m["video_progress_source"]!="venc_info:chn0"||!a||!b||!c||!encoded||!retrieved||!observed||retrieved>encoded
            ||encoded>9007199254740991ULL||observed>9007199254740991ULL||!std::isfinite(out.videoAge)||out.videoAge<0||out.videoAge>=1000
            ||(!same&&(encoded<=last.videoEncoded||retrieved<=last.videoRetrieved||observed<=last.videoObserved)))
            return reject(tr("Invalid HD encoder progress or identity"));
        out.identities.videoEncoded=encoded;out.identities.videoRetrieved=retrieved;out.identities.videoObserved=observed;out.videoMeta=m;
    }
    auto &native=out.native;auto &camera=out.camera;auto &cameraMeta=out.cameraMeta;
    auto &rawAge=out.rawAge;auto &rgbAge=out.rgbAge;out.data=data;
    auto identity=[&](const char* seqKey,const char* stampKey,const char* ageKey,quint64& lastSeq,quint64& lastStamp,double& age){
        bool a=false,b=false;auto seq=m[seqKey].toString().toULongLong(&a),stamp=m[stampKey].toString().toULongLong(&b);age=m[ageKey].toDouble(-1);
        if(!a||!b||seq<=lastSeq||stamp<=lastStamp||seq>9007199254740991ULL||stamp>9007199254740991ULL||!std::isfinite(age)||age<0)return false;
        lastSeq=seq;lastStamp=stamp;return true;
    };
    if(rawSize){
        if(!identity("raw_sequence","raw_timestamp_ns","raw_age_ms",rawSequence,rawCapture,rawAge)||!m["tof_warning_flags"].isDouble()||(m["tof_warning_flags"].toDouble()!=0&&m["tof_warning_flags"].toDouble()!=8&&m["tof_warning_flags"].toDouble()!=16)){return reject(tr("Invalid candidate measurement identity"));}
        native.samples=buffer.mid(headerSize+jsonSize+depthSize+coverageSize,rawSize);native.metadata=m;auto& n=native.metadata;
        if(RvDtofCandidates::isCaptureOnly(mode)||LiteSrBoardContract::isNative16Depth(mode)){
            if(m["raw_fov_correction"]!=expectedFov)return reject(tr("Invalid candidate measurement identity"));
            n["dtof_fov_correction"]=m["raw_fov_correction"];
        }
        n["frame_id"]=qint64(rawSequence);n["tof_sequence"]=qint64(rawSequence);n["tof_timestamp_ns"]=qint64(rawCapture);n["tof_reused"]=false;n["work_mode"]="dtof_candidate";n["model"]=RvDtofCandidates::name(mode);
        n["dtof_width"]=grid.width();n["dtof_height"]=grid.height();n["dtof_bytes"]=int(nativeBytes);n["dtof_format"]="range_uq2_u16le_snr_u8_valid_u8";n["dtof_range_unit"]="0.25_mm";n["dtof_source"]="tmf8829_ipc_first_peak";n["dtof_coordinate"]="sensor_radial_native";n["tof_filter_raw_preserved"]=true;n["pairing_policy"]="independent_streams";
        if(!native.isValid()||(expected.contains("expected_fov") && (m.contains("dtof_fov_correction")||m.contains("tof_fov_correction")) && (m.contains("dtof_fov_correction")?m["dtof_fov_correction"]:m["tof_fov_correction"]).toInt(-1)!=expectedFov)){return reject(tr("Invalid candidate depth values"));}
    }
    if(rgbSize){
        try{
            if(!rgbCalibration)rgbCalibration=std::make_unique<const RvCameraStream::Calibration>(RvCameraStream::loadMap(576));
            if(!identity("rgb_sequence","rgb_timestamp_ns","rgb_age_ms",rgbSequence,rgbCapture,rgbAge)||m["rgb_width"]!=576||m["rgb_height"]!=324||m["pixel_format"]!="NV12"||m["color_range"]!="full"||m["color_matrix"]!="canonical-full-601"||m["resize"]!="bilinear4x4-round-half-up"||m["rectify_map_sha256"].toString().toLatin1()!=rgbCalibration->sha256())throw std::runtime_error("RGB contract");
            camera=RvCameraStream::process(buffer.mid(headerSize+jsonSize+depthSize+coverageSize+rawSize,rgbSize),*rgbCalibration,&rgbScratch);cameraMeta=m;cameraMeta["work_mode"]="dtof_candidate";cameraMeta["rgb_sequence"]=qint64(rgbSequence);cameraMeta["rgb_timestamp_ns"]=qint64(rgbCapture);cameraMeta["rectified"]=true;
        }catch(...){return reject(tr("Invalid candidate stream header"));}
    }
    if(decoder->cancelled.load())return out;
    auto &result=out.result;
    if(data){bool a=false,b=false;const auto seq=m["tof_sequence"].toString().toULongLong(&a),stamp=m["tof_timestamp_ns"].toString().toULongLong(&b);const double age=m["age_ms"].toDouble(-1);
        if(!a||!b||!seq||!stamp||seq<=sequence||stamp<=capture||!std::isfinite(age)||age<0||!std::isfinite(m["compute_ms"].toDouble(-1))||m["compute_ms"].toDouble(-1)<0){return reject(tr("Invalid candidate measurement identity"));}
        sequence=seq;capture=stamp;out.depthAge=age;
        result.depth=buffer.mid(headerSize+jsonSize,depthSize);result.coverage=buffer.mid(headerSize+jsonSize+depthSize,coverageSize);
        const int width=RvDtofCandidates::outputWidth(mode),height=RvDtofCandidates::outputHeight(mode),pixels=width*height;
        for(int i=0;i<pixels;++i){float v;std::memcpy(&v,result.depth.constData()+i*4,4);if(!std::isfinite(v)||v<0||v>1.4f||(LiteSrBoardContract::isNear(mode)&&v>0&&v<.01f)||((v>0)!=(result.coverage[i]!=0))){return reject(tr("Invalid candidate depth values"));}}
        auto& meta=result.metadata;meta=m;meta["width"]=width;meta["height"]=height;meta["model"]=RvDtofCandidates::name(mode);meta["work_mode"]="dtof_candidate";meta["frame_id"]=qint64(seq);meta["tof_sequence"]=qint64(seq);meta["tof_timestamp_ns"]=qint64(stamp);
        meta["depth_location"]="rv1103";meta["compute_location"]="rv1103";meta["rgb_used"]=false;meta["simulation"]=false;meta["tof_filter_raw_preserved"]=true;meta["depth_method"]=RvDtofCandidates::name(mode);meta["candidate_parameters"]=RvDtofCandidates::parameters(mode,settings);
        meta["depth_present"]=true;meta["depth_format"]="float32_le";meta["depth_unit"]="sensor_radial_m_float32";meta["depth_coordinate"]="sensor_radial_native";meta["depth_stride"]=width*4;meta["depth_bytes"]=result.depth.size();meta["wire_bytes"]=int(depthSize+coverageSize);meta["visual_only"]=false;meta["experimental"]=true;meta["accuracy_validated"]=false;meta["presentation_smoothing"]=0;meta["depth_usage"]=(mode==RvDtofCandidates::LiteSrContour)?"presentation":"current_frame_reconstruction";
    }
    if(lite&&m["photon_data"].toBool()){
        auto& lastSequence=out.identities.photonSequence;auto& lastStamp=out.identities.photonCapture;
        bool seqOk=false,stampOk=false;const auto seq=m["photon_sequence"].toString().toULongLong(&seqOk),stamp=m["photon_timestamp_ns"].toString().toULongLong(&stampOk);
        const bool repeated=seqOk&&stampOk&&seq==lastSequence&&stamp==lastStamp;
        double age=m["photon_age_ms"].toDouble(-1);
        if(repeated){if(!std::isfinite(age)||age<0)return reject(tr("Invalid candidate measurement identity"));}
        else if(!identity("photon_sequence","photon_timestamp_ns","photon_age_ms",lastSequence,lastStamp,age))return reject(tr("Invalid candidate measurement identity"));
        RvPhotonFrame light;light.bootId=m["boot_id"].toString();light.ageMs=age;light.resultFormat=m["input_signal_format"].toInt(-1);light.fovCorrection=m["input_fov_correction"].toInt(-1);
        light.tof.samples=buffer.mid(headerSize+jsonSize+depthSize+coverageSize+rawSize+rgbSize,inputSize);
        light.signalWords=buffer.mid(headerSize+jsonSize+depthSize+coverageSize+rawSize+rgbSize+inputSize,signalSize);
        auto& n=light.tof.metadata;n=m;n["frame_id"]=qint64(seq);n["tof_sequence"]=qint64(seq);n["tof_timestamp_ns"]=qint64(stamp);n["work_mode"]="dtof_candidate";n["model"]=RvDtofCandidates::name(mode);n["dtof_width"]=grid.width();n["dtof_height"]=grid.height();n["dtof_bytes"]=int(nativeBytes);n["dtof_format"]="range_uq2_u16le_snr_u8_valid_u8";n["dtof_range_unit"]="0.25_mm";n["dtof_source"]="tmf8829_ipc_first_peak";n["dtof_coordinate"]="sensor_radial_native";n["tof_reused"]=false;n["tof_warning_flags"]=m.value("input_warning_flags");n["tof_fov_correction"]=light.fovCorrection;
        const auto warning=m.value("input_warning_flags");
        if(!light.tof.isValid()||light.fovCorrection!=expectedFov||m["input_fov_correction"]!=expectedFov||m["input_signal_format"]!=light.resultFormat||light.resultFormat<0||(light.resultFormat&8)==0||(light.resultFormat&7)!=1||(light.resultFormat&0xc0)
            ||!warning.isDouble()||(warning.toDouble()!=0&&warning.toDouble()!=8&&warning.toDouble()!=16))return reject(tr("Invalid candidate measurement identity"));
        if(!repeated)out.photon=std::move(light);
    }
    if(paired&&data){
        if(LiteSrBoardContract::isNative16Depth(mode)&&
           (!m["photon_data"].toBool()||m["photon_sequence"]!=m["tof_sequence"]||m["photon_timestamp_ns"]!=m["tof_timestamp_ns"]
            ||m["input_warning_flags"]!=m["depth_native_warning_flags"]||m["input_columns"]!=16||m["input_rows"]!=16
            ||buffer.mid(headerSize+jsonSize+depthSize+coverageSize+rawSize+rgbSize,inputSize)!=buffer.right(sourceSize)))
            return reject(tr("Invalid paired board inference native measurement"));
        if(m["depth_native_sequence"]!=m["tof_sequence"]||m["depth_native_timestamp_ns"]!=m["tof_timestamp_ns"]
            ||m["depth_native_fov_correction"]!=expectedFov||(m["depth_native_warning_flags"]!=0&&m["depth_native_warning_flags"]!=8&&m["depth_native_warning_flags"]!=16))
            return reject(tr("Invalid paired board inference native measurement"));
        RvTofFrame source;source.samples=buffer.right(sourceSize);source.metadata=result.metadata;
        auto& n=source.metadata;n["dtof_width"]=grid.width();n["dtof_height"]=grid.height();n["dtof_bytes"]=int(nativeBytes);n["dtof_format"]="range_uq2_u16le_snr_u8_valid_u8";
        n["dtof_range_unit"]="0.25_mm";n["dtof_source"]="tmf8829_ipc_first_peak";n["dtof_coordinate"]="sensor_radial_native";n["tof_reused"]=false;n["tof_warning_flags"]=m.value("depth_native_warning_flags");n["tof_fov_correction"]=expectedFov;
        if(!source.isValid())return reject(tr("Invalid paired board inference native measurement"));
        if(hostContour&&!decoder->cancelled.load()){
            try{
                const auto refined=RvLiteSrContour::refineBoard(result.depth,source.samples,repair);
                result.depth=refined.depth;result.coverage=refined.coverage;
                auto& meta=result.metadata;meta["compute_location"]="rv1103+host_cpu";meta["depth_location"]="rv1103+host";meta["inference_location"]="rv1103";meta["postprocess_location"]="host_cpu";
                meta["host_contour_enabled"]=true;meta["host_contour_revision"]=qint64(revision);meta["host_contour_ms"]=refined.diagnostic["postprocess_ms"];
                meta["host_contour"]=refined.diagnostic;
                if(LiteSrBoardContract::isNear(mode)){auto diagnostic=refined.diagnostic;diagnostic["base_model"]=RvDtofCandidates::name(mode);meta["host_contour"]=diagnostic;}meta["depth_method"]=LiteSrBoardContract::isHighResolution(mode)?"litesr_board_512_host_contour_v1":"litesr_board_full_host_contour_repair_v2";meta["depth_usage"]="presentation";
            }catch(...){return reject(tr("Host contour processing failed"));}
        }
    }
    return out;
}
void RvDtofBoardStream::finish() {
    if(failed||!decoding)return;
    Decoded out;
    try{out=watcher.result();}catch(...){fail(tr("Invalid candidate stream header"));return;}
    if(out.request!=request){fail(tr("Candidate identity or parameters mismatch"));return;}
    if(!out.error.isEmpty()){fail(out.error);return;}
    const double videoAge=out.videoAge+roundtrip.nsecsElapsed()/1e6;
    if(!out.videoMeta.isEmpty()&&videoAge>=1000){
        // Valid when sent, expired while queued/in transit: discard the whole
        // response without publishing data or advancing accepted identities.
        // Poll again; existing encoder/transport watchdogs still detect a real
        // stall. One late host packet must not request a board fault recovery.
        decoding=false;pending=false;timeout.stop();return;
    }
    identities=out.identities;
    QPointer<RvDtofBoardStream> guard(this);
    // Every delivery includes transport, pool wait, decoding and earlier callback
    // time. A callback may stop/delete the stream or process nested GUI events.
    const auto alive=[&]{return guard&&!guard->failed;};
    if(!out.videoMeta.isEmpty()){
        out.videoMeta["video_age_ms"]=videoAge;
        if(videoProgress){const auto callback=videoProgress;callback(out.videoMeta);}
    }
    if(!alive())return;
    if(out.rawPresent){
        const double age=out.rawAge+roundtrip.nsecsElapsed()/1e6;
        out.native.metadata["raw_age_ms"]=age;
        if(age<220){progress.restart();if(rawFrame){const auto callback=rawFrame;callback(out.native);}}
    }
    if(!alive())return;
    if(out.rgbPresent){
        const double age=out.rgbAge+roundtrip.nsecsElapsed()/1e6;
        out.cameraMeta["rgb_age_ms"]=age;
        if(age<500){progress.restart();if(rgbFrame){const auto callback=rgbFrame;callback(out.camera,out.cameraMeta);}}
    }
    if(!alive())return;
    if(!out.photon.signalWords.isEmpty()){
        out.photon.ageMs+=roundtrip.nsecsElapsed()/1e6;
        if(out.photon.isValid()){progress.restart();if(photonFrame){const auto callback=photonFrame;callback(out.photon);}}
    }
    if(!alive())return;
    if(out.data&&out.refinementRevision==refinementRevision){
        const double age=out.depthAge+roundtrip.nsecsElapsed()/1e6;
        out.result.metadata["candidate_age_ms"]=age;
        out.result.metadata["board_compute_ms"]=out.result.metadata.value("compute_ms");
        const int freshMs=RvDtofCandidates::freshnessMs(expected["candidate_mode"].toInt(-1));
        out.result.fresh=age<freshMs;
        if(out.result.fresh){
            progress.restart();expiry.start(std::max(1,int(freshMs-age)));
            if(frame){const auto callback=frame;callback(out.result);}
        }
    }
    if(!alive())return;
    decoding=false;pending=false;timeout.stop();
}

// TNY6 is an independent push stream. The token handshake binds this socket to
// the already verified control receipt; native measurements travel in each frame.
void RvDtofBoardStream::readTiny(){
    if(failed)return;
    constexpr int packetBytes=TinyNv12Contract::HeaderBytes+TinyNv12Contract::PayloadBytes;
    if(socket.bytesAvailable()+buffer.size()>packetBytes*8){fail(tr("Invalid candidate stream size"));return;}
    buffer+=socket.readAll();
    if(!tinyBoardClock){
        if(buffer.size()<8)return;
        tinyBoardClock=qFromLittleEndian<quint64>(buffer.constData());buffer.remove(0,8);
        if(!tinyBoardClock||!tinyClock.isValid()){fail(tr("Candidate identity or parameters mismatch"));return;}
    }
    while(buffer.size()>=packetBytes){
        auto packet=buffer.left(packetBytes);buffer.remove(0,packetBytes);
        RvTinyNv12::Frame out;
        try{out=RvTinyNv12::decode(packet,expected,tinyIdentity,tinyBoardClock+quint64(tinyClock.nsecsElapsed()));}
        catch(const std::exception& e){fail(QString::fromUtf8(e.what()));return;}
        tinyIdentity=out.identity;progress.restart();timeout.start(3000);
        QPointer<RvDtofBoardStream> guard(this);
        if(out.rawAge<220&&rawFrame){const auto callback=rawFrame;callback(out.native);}
        if(!guard||failed)return;
        if(out.photon.isValid()&&photonFrame){const auto callback=photonFrame;callback(out.photon);}
        if(!guard||failed)return;
        if(out.rgbAge<500&&rgbFrame){const auto callback=rgbFrame;callback(out.camera,out.cameraMetadata);}
        if(!guard||failed)return;
        if(out.result.fresh){expiry.start(std::max(1,int(1000-out.rawAge)));if(frame){const auto callback=frame;callback(out.result);}}
        if(!guard||failed)return;
    }
}
