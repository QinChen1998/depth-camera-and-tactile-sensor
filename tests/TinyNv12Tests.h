#pragma once
#include "../sensors/RvTinyNv12Protocol.h"
#include "../sensors/RvDtofBoardStream.h"
#include "../sensors/RvDepthView.h"
#include <QtNetwork/QTcpServer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <functional>
#include <limits>

inline void checkTinyNv12(const std::function<void(const QString&,bool)>& check){
    const auto receipt=QJsonDocument::fromJson(R"JSON({"state":"running","protocol":"TNY6","engine":"tiny-nv12-128x80-holes-v2","candidate_mode":18,"options":"1,1","model_sha256":"034bb260a7751c24ec133ec01ea6343f9ecf9102684162e0efa55aaef7056434","checkpoint_sha256":"f3fb6e51ba0a455faf756aada1da2aa8677e11ff729e429dcb976d114b10509f","width":80,"height":128,"fresh_ms":1000,"port":8846,"native_columns":16,"native_rows":16,"calibration":"rv20261008-f1","camera_mount_rotation":180,"input_rotation_degrees":0,"video_transport":"nv12_paired","video_width":80,"video_height":128,"video_fps":30,"crop_raw_xywh":[560,0,1248,1296],"depth_unit":"camera-Z_m_float32","postprocess":"enclosed_small_slope_v2","pair_limit_ms":50,"rgb_used":true,"rgb_paused":false,"inference_location":"rv1103","generation":"0123456789abcdef0123456789abcdef","boot_id":"12345678-1234-1234-1234-123456789abc"})JSON").object();
    QByteArray packet(TinyNv12Contract::HeaderBytes+TinyNv12Contract::PayloadBytes,0);
    auto p=packet.data();auto u32=[&](int at,quint32 n){qToLittleEndian(n,p+at);};
    auto u64=[&](int at,quint64 n){qToLittleEndian(n,p+at);};
    auto f64=[&](int at,double n){quint64 bits;std::memcpy(&bits,&n,8);u64(at,bits);};
    u32(0,0x32594e54);u32(4,6);u32(8,67315);u32(16,1);u32(20,10);u32(24,8);u32(44,80|(128<<16));
    u64(48,1000000000);u64(56,1000000000);u64(64,1001000000);u64(72,1010000000);u64(80,1011000000);f64(112,4.);
    const float invalid=std::numeric_limits<float>::quiet_NaN();
    for(int n=0;n<10240;++n){float z=n?2.5f:invalid;quint32 bits;std::memcpy(&bits,&z,4);u32(264+4*n,bits);}
    auto*t=p+264+40960+15360;qToLittleEndian<quint32>(0x39464d54,t);qToLittleEndian<quint16>(5,t+4);qToLittleEndian<quint16>(44,t+6);
    qToLittleEndian<quint32>(10995,t+8);qToLittleEndian<quint32>(10,t+12);qToLittleEndian<quint64>(1000000000,t+16);
    qToLittleEndian<quint16>(16,t+28);qToLittleEndian<quint16>(16,t+30);t[32]=1;t[34]=1;t[44]=1;t[6380]=25;
    qToLittleEndian<quint16>(2400,t+3308);t[7916]=1;t[7918]=1;t[7920]=1;t[10994]=1;
    for(int n=0;n<15360;++n)p[264+40960+n]=char(128);
    const auto f=RvTinyNv12::decode(packet,receipt,{},1020000000);
    check("TINY_CAMERA_Z_RANGE_AND_MASK",f.result.depth.size()==40960&&f.result.coverage.size()==10240&&f.result.coverage[0]==0&&f.result.coverage[1]!=0&&RvTinyNv12::f32(f.result.depth.constData()+4)==2.5f);
    check("TINY_NATIVE_UNMODIFIED",f.native.isValid()&&f.native.rangeUq2(0)==2400&&f.native.snr(0)==25&&f.native.samples[3]==1&&f.photon.isValid());
    check("TINY_INDEPENDENT_IDENTITY",f.result.metadata["model"]==TinyNv12Contract::Scheme&&f.result.metadata["depth_unit"]=="camera-Z_m_float32"&&f.result.metadata["rgb_used"]==true&&f.result.metadata["host_contour_enabled"]==false);
    check("TINY_CAMERA_SHAPE",f.camera.size()==QSize(80,128));
    auto reject=[&](const QByteArray& b,QJsonObject r=QJsonObject{},RvTinyNv12::Identity old=RvTinyNv12::Identity{}){try{RvTinyNv12::decode(b,r.isEmpty()?receipt:r,old,1020000000);return false;}catch(...){return true;}};
    check("TINY_REPEATED_DEPTH_REJECTED",reject(packet,{},f.identity));
    check("TINY_TRUNCATED_PACKET_REJECTED",reject(packet.left(packet.size()-1)));
    auto changed=packet;changed[4]=5;check("TINY_OTHER_WIRE_REJECTED",reject(changed));
    changed=packet;changed[264+40960+15360+7916]=2;check("TINY_WRONG_FOV_REJECTED",reject(changed));
    changed=packet;qToLittleEndian<quint32>(11,changed.data()+264+40960+15360+12);check("TINY_MISMATCHED_NATIVE_REJECTED",reject(changed));
    changed=packet;qToLittleEndian<quint64>(1100000000,changed.data()+56);check("TINY_UNPAIRED_RGB_REJECTED",reject(changed));
    auto wrong=receipt;wrong["model_sha256"]=LiteSrBoardContract::RectModelSha;check("TINY_OTHER_MODEL_REJECTED",reject(packet,wrong));
    wrong=receipt;wrong["camera_mount_rotation"]=0;check("TINY_WRONG_ORIENTATION_REJECTED",reject(packet,wrong));
    check("TINY_STALE_NOT_PUBLISHED",!RvTinyNv12::decode(packet,receipt,{},2100000000).result.fresh);
    RvDepthView canvas;canvas.setRadialMode();canvas.setBoardCameraZ(true);canvas.setDisplayRange(.01,3);canvas.setBlackOutsideRange(false);
    check("TINY_CAMERA_Z_VIEWPORT",canvas.setDepth(f.result.depth,f.result.metadata)&&canvas.setCoverage(f.result.coverage)&&canvas.presentationSize()==QSizeF(1248,1296));
    check("TINY_VALID_DEPTH_RENDERS_FULL_BRIGHTNESS",canvas.colorAt(1,0)==DepthColorScale::color(2.5f,.01,3.));
    check("TINY_INVALID_DEPTH_STAYS_BLACK",canvas.colorAt(0,0)==qRgb(0,0,0)&&canvas.depthAt(0,0)==0);
    bool aspectAndSamples=true;canvas.resize(500,400);
    for(int degrees:{0,90,180,270}){
        canvas.setDisplayRotationDegrees(degrees);canvas.grab();
        const auto rect=canvas.property("presentationRect").toRectF();
        const double expected=(degrees==90||degrees==270)?1296./1248:1248./1296;
        aspectAndSamples&=rect.height()>0&&std::abs(rect.width()/rect.height()-expected)<1e-6
            &&canvas.presentationImage().size()==QSize(80,128)&&canvas.depthAt(1,0)==2.5f;
    }
    check("TINY_CROP_ASPECT_ROTATION_PRESERVES_SAMPLES",aspectAndSamples);canvas.setDisplayRotationDegrees(0);
    canvas.setBoardCameraZ(false);canvas.setNativeGridSize(QSize(48,32));
    check("TINY_RETURN_TO_RADIAL_VIEWPORT",canvas.presentationSize()==QSizeF(32,48));
    QTcpServer server;check("TINY_LOCAL_TRANSPORT_LISTEN",server.listen(QHostAddress::LocalHost,0));
    RvDtofBoardStream stream(receipt,{},nullptr,"127.0.0.1",server.serverPort());
    int frames=0,raw=0;QString error;QByteArray hello;QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);
    stream.frame=[&](const RvDtofCandidates::Result&r){if(r.fresh)++frames;loop.quit();};stream.rawFrame=[&](const RvTofFrame&r){if(r.isValid())++raw;};stream.error=[&](const QString&e){error=e;loop.quit();};
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    QObject::connect(&server,&QTcpServer::newConnection,&loop,[&]{auto*s=server.nextPendingConnection();QObject::connect(s,&QTcpSocket::readyRead,&loop,[&,s]{hello+=s->readAll();if(hello.size()!=36)return;
        QByteArray clock(8,0);qToLittleEndian<quint64>(1020000000,clock.data());s->write(clock.left(3));
        QTimer::singleShot(5,&loop,[&,s,clock]{s->write(clock.mid(3)+packet.left(100));});
        QTimer::singleShot(10,&loop,[&,s]{s->write(packet.mid(100,777));});
        QTimer::singleShot(15,&loop,[&,s]{s->write(packet.mid(877));});
    });});
    deadline.start(2500);loop.exec();
    check("TINY_FRAGMENTED_HANDSHAKE_AND_FRAME",frames==1&&raw==1&&error.isEmpty()&&hello.left(32)==receipt["generation"].toString().toLatin1());
}
