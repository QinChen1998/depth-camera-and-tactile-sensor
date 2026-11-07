#pragma once
#include "RvDtofCandidates.h"
#include "RvPhotonFrame.h"
#include "RvLiteSrContour.h"
#include "RvCameraView.h"
#include "RvTinyNv12Protocol.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtCore/QFutureWatcher>
#include <QtCore/QThreadPool>
#include <memory>
#include <functional>
#include <QtCore/QtEndian>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpSocket>
#include <cmath>
#include <cstring>

inline QString liteSrBoardOptions(const RvDtofCandidates::Settings& s){
    const auto csv=QString::number(s.liteSrHoles)+","+(s.liteSrOutliers?"1":"0");
    LiteSrBoardContract::Options checked(csv.toStdString());return csv;
}
inline bool liteSrBoardIdentity(const QJsonObject& m){
    const int mode=m["candidate_mode"].toInt(-1);
    return RvDtofCandidates::supports(mode)&&m["engine"]==LiteSrBoardContract::engine(mode)&&m["model_sha256"]==LiteSrBoardContract::modelSha(mode)
        &&(!LiteSrBoardContract::isCaptureOnly(mode)||(m["capture_only"]==true&&m["inference_location"]=="none"&&m["depth_present"]==false&&m["photon_enabled"]==false))
        &&(!LiteSrBoardContract::isNear(mode)||(m["checkpoint_sha256"]==LiteSrBoardContract::NearCheckpointSha&&m["training_minimum_m"]==0.01))
        &&(!LiteSrBoardContract::isDepthOnly(mode)||(m["rgb_paused"]==true&&m["video_transport"]=="none"&&m["video_fps"]==0))
        &&(!LiteSrBoardContract::isNative16Depth(mode)||(m["video_fps"]==0&&m["native_columns"]==16&&m["native_rows"]==16))
        &&m["width"]==LiteSrBoardContract::outputWidth(mode)&&m["height"]==LiteSrBoardContract::outputHeight(mode)&&m["fresh_ms"]==LiteSrBoardContract::FreshMs;
}
inline bool liteSrHdIdentity(const QJsonObject& m){
    return m["video_transport"]=="rtsp_h265"&&m["video_width"]==2304&&m["video_height"]==1296
        &&m["video_fps"]==30&&m["video_kbps"]==4096;
}
inline QSize liteSrNativeGrid(const QJsonObject& receipt){
    if(RvDtofCandidates::isTinyNv12(receipt["candidate_mode"].toInt(-1)))return receipt["native_columns"]==16&&receipt["native_rows"]==16?QSize(16,16):QSize();
    if(LiteSrBoardContract::isNative16Depth(receipt["candidate_mode"].toInt(-1)))return receipt["native_columns"]==16&&receipt["native_rows"]==16?QSize(16,16):QSize();
    if(RvDtofCandidates::isCaptureOnly(receipt["candidate_mode"].toInt(-1))&&receipt.contains("requested_host_model")
       &&!RvCameraView::isHostDepth(receipt["requested_host_model"].toString()))return {};
    if(!receipt.contains("native_capture_protocol"))return RvCameraView::isUniDepthNative16(receipt["requested_host_model"].toString())?QSize():QSize(48,32);
    if(!RvDtofCandidates::isCaptureOnly(receipt["candidate_mode"].toInt(-1))
       ||(receipt["native_capture_protocol"]!=LiteSrBoardContract::NativeCaptureProtocol&&receipt["native_capture_protocol"]!=LiteSrBoardContract::NativeCaptureProtocolV2))return {};
    if(RvCameraView::isUniDepthNative16(receipt["requested_host_model"].toString()))return receipt["native_capture_protocol"]==LiteSrBoardContract::NativeCaptureProtocolV2&&receipt["native_columns"]==16&&receipt["native_rows"]==16?QSize(16,16):QSize();
    if(receipt["native_columns"]==48&&receipt["native_rows"]==32)return QSize(48,32);
    return {};
}
inline bool dtofBoardReceipt(const QJsonObject& m,int mode,const QString& token,const QString& options){
    if(RvDtofCandidates::isTinyNv12(mode))return tinyNv12Receipt(m,token,options);
    return RvDtofCandidates::supports(mode)&&(!RvDtofCandidates::isLiteSr(mode)||liteSrBoardIdentity(m))
        &&liteSrNativeGrid(m).isValid()
        &&(!LiteSrBoardContract::isDepthOnly(mode)||m["paired_depth_native_protocol"]==LiteSrBoardContract::PairedProtocol)
        &&(!RvDtofCandidates::isLiteSrHd(mode)||(liteSrHdIdentity(m)&&m["paired_depth_native_protocol"]==LiteSrBoardContract::PairedProtocol))
        &&m["protocol"]==(RvDtofCandidates::isLiteSr(mode)?LiteSrBoardContract::Protocol:"DTF3")&&m["state"]=="running"&&m["candidate_mode"].toInt(-1)==mode&&m["generation"]==token&&m["options"]==options
        &&QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(m["boot_id"].toString()).hasMatch();
}

// One request and one decode at a time. Transport and callbacks stay on the
// owning GUI thread; the worker owns only packet data and calibration scratch.
class RvDtofBoardStream final:public QObject {
    Q_DECLARE_TR_FUNCTIONS(RvDtofBoardStream)
    friend struct RvDtofBoardStreamTestAccess;
    struct Identities {
        quint64 rawSequence=0,rawCapture=0,rgbSequence=0,rgbCapture=0,sequence=0,capture=0,photonSequence=0,photonCapture=0;
        quint64 videoEncoded=0,videoRetrieved=0,videoObserved=0;
    };
    struct Decoded {
        RvTofFrame native;
        RvPhotonFrame photon;
        QImage camera;
        QJsonObject cameraMeta,videoMeta;
        RvDtofCandidates::Result result;
        Identities identities;
        QString error;
        double rawAge=0,rgbAge=0,depthAge=0,videoAge=0;
        quint32 request=0;
        quint64 refinementRevision=0;
        bool rawPresent=false,rgbPresent=false,data=false;
    };
    struct Decoder;
    QTcpSocket socket;
    QTimer poll,timeout,expiry;
    QElapsedTimer roundtrip,progress;
    QByteArray buffer;
    const QJsonObject expected;
    const RvDtofCandidates::Settings settings;
    Identities identities;
    quint32 request=0;
    bool pending=false,decoding=false,failed=false;
    bool hostContour=false;
    RvLiteSrContour::RepairSettings hostRepair;
    quint64 refinementRevision=0;
    std::shared_ptr<Decoder> decoder;
    QFutureWatcher<Decoded> watcher;
    QThreadPool *decodePool;
    void fail(const QString&);
    void ask();
    void read();
    void readTiny();
    QElapsedTimer tinyClock;
    quint64 tinyBoardClock=0;
    RvTinyNv12::Identity tinyIdentity;
    void finish();
    bool usesPairedProtocol() const {return expected["protocol"]==LiteSrBoardContract::PairedProtocol;}
    static Decoded decode(const std::shared_ptr<Decoder>&,const QByteArray&,const QJsonObject&,
        const RvDtofCandidates::Settings&,Identities,quint32,bool,const RvLiteSrContour::RepairSettings&,quint64);
public:
    std::function<void(const RvDtofCandidates::Result&)> frame;
    std::function<void(const RvTofFrame&)> rawFrame;
    std::function<void(const RvPhotonFrame&)> photonFrame;
    std::function<void(const QImage&,const QJsonObject&)> rgbFrame;
    std::function<void(const QJsonObject&)> videoProgress;
    std::function<void(const QString&)> error;
    std::function<void()> expired;
    // An injected pool must outlive its jobs. Production uses an isolated,
    // application-lifetime capture pool, independent of snapshot/disk jobs.
    RvDtofBoardStream(const QJsonObject& receipt,const RvDtofCandidates::Settings& s,QObject* parent=nullptr,
        const QString& host="172.32.0.93",quint16 port=8832,QThreadPool* pool=nullptr);
    ~RvDtofBoardStream() override;
    bool supportsHostContour() const {return !LiteSrBoardContract::isNative16Depth(expected["candidate_mode"].toInt(-1))&&!RvDtofCandidates::isCaptureOnly(expected["candidate_mode"].toInt(-1))&&RvDtofCandidates::isLiteSrFull(expected["candidate_mode"].toInt(-1))&&usesPairedProtocol();}
    bool setHostContour(bool enabled,const RvLiteSrContour::RepairSettings& settings);
};
