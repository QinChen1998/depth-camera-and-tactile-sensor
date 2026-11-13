#pragma once
#include "RvDtofCandidates.h"
#include "RvCameraView.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtCore/QFutureWatcher>
#include <QtCore/QThreadPool>
#include <QtCore/QtEndian>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpSocket>
#include <memory>
#include <functional>
#include <cmath>

inline QString nativeCaptureOptions(const RvDtofCandidates::Settings&){return QStringLiteral("1,1");}
inline bool nativeVideoIdentity(const QJsonObject& m){
    return m["video_transport"]=="rtsp_h265"&&m["video_width"]==2304&&m["video_height"]==1296
        &&m["video_fps"]==30&&m["video_kbps"]==4096;
}
inline QSize nativeCaptureGrid(const QJsonObject& m){
    const int mode=m["candidate_mode"].toInt(-1);
    if(!RvCaptureContract::supports(mode)||m["native_capture_protocol"]!=RvCaptureContract::NativeCaptureProtocolV2)return {};
    const QSize grid(RvCaptureContract::columns(mode),RvCaptureContract::rows(mode));
    return m["native_columns"]==grid.width()&&m["native_rows"]==grid.height()?grid:QSize();
}
inline bool dtofBoardReceipt(const QJsonObject& m,int mode,const QString& token,const QString& options){
    return RvDtofCandidates::supports(mode)&&RvCaptureContract::boardImagingIdentity(m)&&nativeVideoIdentity(m)&&nativeCaptureGrid(m).isValid()
        &&m["paired_depth_native_protocol"]==RvCaptureContract::PairedProtocol
        &&m["protocol"]==RvCaptureContract::Protocol&&m["state"]=="running"&&m["candidate_mode"]==mode
        &&m["generation"]==token&&m["options"]==options&&options=="1,1"
        &&QRegularExpression("^[0-9a-f]{32}$").match(token).hasMatch()
        &&QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(m["boot_id"].toString()).hasMatch();
}

// Native measurements, optional hole-fill estimates and encoder telemetry.
// One asynchronous decode is pending at a time.
class DCTS_DEPTH_INTERNAL RvDtofBoardStream final:public QObject {
    Q_DECLARE_TR_FUNCTIONS(RvDtofBoardStream)
    friend struct RvDtofBoardStreamTestAccess;
    struct Identities {
        quint64 rawSequence=0,rawCapture=0,videoEncoded=0,videoRetrieved=0,videoObserved=0;
    };
    struct Decoded {
        RvTofFrame native;QByteArray holeFillRanges;QJsonObject videoMeta;Identities identities;QString error;
        double rawAge=0,videoAge=0;quint32 request=0;bool rawPresent=false;
    };
    struct Decoder;
    QTcpSocket socket;QTimer poll,timeout;QElapsedTimer roundtrip,progress;QByteArray buffer;
    const QJsonObject expected;const bool requestHoleFill;Identities identities;quint32 request=0;
    bool pending=false,decoding=false,failed=false;
    std::shared_ptr<Decoder> decoder;QFutureWatcher<Decoded> watcher;QThreadPool *decodePool;
    void fail(const QString&);void ask();void read();void finish();
    static Decoded decode(const std::shared_ptr<Decoder>&,const QByteArray&,const QJsonObject&,Identities,quint32);
public:
    enum class Output { RawToF, BoardDepth };
    std::function<void(const RvTofFrame&,const QByteArray& holeFillRanges)> rawFrame;
    std::function<void(const QJsonObject&)> videoProgress;
    std::function<void(const QString&)> error;
    RvDtofBoardStream(const QJsonObject&,Output,QObject* parent=nullptr,
        const QString& host="172.32.0.93",quint16 port=8832,QThreadPool* pool=nullptr);
    ~RvDtofBoardStream() override;
};
