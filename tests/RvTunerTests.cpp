#include "../sensors/RvTunerWidget.h"
#include "../sensors/RvSessionController.h"
#include "../sensors/RvAssistDepth.h"
#include "../tools/rv_photon/Wire.h"
#include "../LanguageManager.h"
#include "../RuntimeDiscovery.h"
#include "RvDeviceFixture.h"
#include "../sensors/RvDtofBoardStream.h"
#include "../sensors/RvDepthView.h"
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSettings>
#include <QtCore/QtEndian>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSplitterHandle>
#include <QtGui/QMouseEvent>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <functional>
#include <cstring>

namespace {
const QString Token(32,'a'),Boot="11111111-2222-3333-4444-555555555555";
QByteArray packet(const RvTuner::Settings &s,quint32 sequence=1){
 const auto dim=s.size();const int count=dim.width()*dim.height()*s.objects;QByteArray b(72+count*8,0);char *p=b.data();std::memcpy(p,"DTN1",4);
 qToLittleEndian<quint16>(1,p+4);qToLittleEndian<quint16>(72,p+6);qToLittleEndian<quint32>(b.size(),p+8);qToLittleEndian<quint32>(sequence,p+12);qToLittleEndian<quint64>(1000000000ULL*sequence,p+16);std::memcpy(p+24,QByteArray::fromHex(Token.toLatin1()).constData(),16);
 qToLittleEndian<quint16>(dim.height(),p+40);qToLittleEndian<quint16>(dim.width(),p+42);p[44]=char(s.mode);p[45]=char(s.objects);p[46]=char(s.objects|(s.signal?8:0)|(s.noise?16:0));p[47]=1;p[48]=25;p[49]=p[50]=p[51]=1;
 qToLittleEndian<quint16>(s.iterations,p+60);qToLittleEndian<quint16>(s.shortIterations,p+62);qToLittleEndian<quint16>(s.snr,p+64);qToLittleEndian<quint16>(s.periodMs,p+66);
 for(int i=0;i<count;++i){const int n=72+i*8;qToLittleEndian<quint16>(1000+i%100,p+n);p[n+2]=40;p[n+3]=1;if(s.signal)qToLittleEndian<quint16>(0x8002+i%3,p+n+4);if(s.noise)qToLittleEndian<quint16>(15,p+n+6);}return b;
}
bool rejects(const std::function<void()> &fn){try{fn();return false;}catch(const std::exception &){return true;}}
}
// In-process loopback server exercises the production asynchronous decoder,
// fragmentation, shape validation and exact same-frame D/I pairing.
enum class StreamOutcome { InfrastructureFailure, Accepted, Rejected };
static StreamOutcome rectStreamCase(int warning,int defect){
 QJsonObject receipt{{"state","running"},{"protocol","LSG1"},{"paired_depth_native_protocol","LSG2"},{"boot_id",Boot},{"generation",Token},
   {"candidate_mode",16},{"options","1,1"},{"engine",LiteSrBoardContract::RectEngine},{"model_sha256",LiteSrBoardContract::RectModelSha},
   {"checkpoint_sha256",LiteSrBoardContract::NearCheckpointSha},{"training_minimum_m",.01},{"width",384},{"height",256},{"fresh_ms",1000},
   {"rgb_paused",true},{"video_transport","none"},{"video_fps",0},{"native_columns",48},{"native_rows",32},{"expected_fov",1}};
 if(!dtofBoardReceipt(receipt,16,Token,"1,1"))return StreamOutcome::InfrastructureFailure;
 QJsonObject m=receipt;m["protocol"]="LSG2";
 for(const auto *key:{"tof_sequence","raw_sequence","photon_sequence","depth_native_sequence"})m[key]="1";
 for(const auto *key:{"tof_timestamp_ns","raw_timestamp_ns","photon_timestamp_ns","depth_native_timestamp_ns"})m[key]="1000000000";
 for(const auto *key:{"age_ms","raw_age_ms","photon_age_ms"})m[key]=5.;
 for(const auto *key:{"tof_warning_flags","input_warning_flags","depth_native_warning_flags"})m[key]=warning;
 m["request"]=1;m["data"]=true;m["raw_data"]=true;m["rgb_data"]=false;m["photon_data"]=true;
 m["compute_ms"]=10.;m["encoding"]="raw";m["input_rotation_degrees"]=0;m["intensity_white_counts"]=512;m["quantization"]="INT8";
 m["positive_near_policy"]="model_output_no_native_near_fill";m["raw_fov_correction"]=1;m["input_fov_correction"]=1;m["depth_native_fov_correction"]=1;
 m["input_signal_format"]=9;m["input_columns"]=48;m["input_rows"]=32;
 QByteArray native(6144,0),signal(3072,0),depth(393216,0),coverage(98304,char(255));const float value=.3f;
 for(int i=0;i<1536;++i){qToLittleEndian<quint16>(1200,native.data()+i*4);native[i*4+2]=20;native[i*4+3]=1;qToLittleEndian<quint16>(100,signal.data()+2*i);}
 for(int i=0;i<98304;++i)std::memcpy(depth.data()+4*i,&value,4);
 auto source=native;
 if(defect==1)m["depth_native_sequence"]="2";
 if(defect==2)m["depth_native_fov_correction"]=2;
 if(defect==3)m["video_transport"]="rtsp_h265";
 if(defect==4)m["video_fps"]=30;
 if(defect==5)m["input_warning_flags"]=24;
 if(defect==6)source=QByteArray(1024,0);
 if(defect==7){m["rgb_data"]=true;signal.clear();}
 const auto input=defect==7?QByteArray():native;
 const auto json=QJsonDocument(m).toJson(QJsonDocument::Compact);
 QByteArray packet(36,0);quint32 sizes[]={LiteSrBoardContract::PairedResponseMagic,quint32(json.size()),393216,98304,6144,0,quint32(input.size()),quint32(signal.size()),quint32(source.size())};
 for(int i=0;i<9;++i)qToLittleEndian(sizes[i],packet.data()+4*i);
 packet+=json+depth+coverage+native+input+signal+source;
 QTcpServer server;if(!server.listen(QHostAddress::LocalHost))return StreamOutcome::InfrastructureFailure;
 QObject scope;QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);
 auto outcome=StreamOutcome::InfrastructureFailure;
 bool requested=false;int raw=0,photons=0,position=0;QTcpSocket *peer=nullptr;std::function<void()> send;
 QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
 send=[&]{if(!peer||position>=packet.size())return;const auto part=packet.mid(position,position<36?1:8191);position+=part.size();peer->write(part);QTimer::singleShot(0,&scope,send);};
 QObject::connect(&server,&QTcpServer::newConnection,&scope,[&]{peer=server.nextPendingConnection();QObject::connect(peer,&QTcpSocket::readyRead,&scope,[&]{
    if(peer->bytesAvailable()<32)return;const auto request=peer->read(32);if(qFromLittleEndian<quint32>(request.constData())!=LiteSrBoardContract::PairedRequestMagic){loop.quit();return;}requested=true;if(position==0)send();
 });});
 RvDtofCandidates::Settings settings;RvDtofBoardStream stream(receipt,settings,nullptr,"127.0.0.1",server.serverPort());
 stream.rawFrame=[&](const RvTofFrame &f){raw+=f.isValid()&&f.columns()==48&&f.rows()==32;};
 stream.photonFrame=[&](const RvPhotonFrame &f){photons+=f.tof.isValid()&&f.tof.columns()==48;};
 stream.frame=[&](const RvDtofCandidates::Result &r){
    if(requested&&r.fresh&&r.depth==depth&&r.coverage==coverage&&raw==1&&photons==1)outcome=StreamOutcome::Accepted;
    loop.quit();
 };
 stream.error=[&](const QString &reason){
    // A blocked loopback connection or a timeout must fail negative cases too.
    if(requested&&position>=36&&!reason.isEmpty()
       &&reason!=RvDtofBoardStream::tr("RV1103 candidate stream disconnected")
       &&reason!=RvDtofBoardStream::tr("RV1103 candidate stream timed out"))outcome=StreamOutcome::Rejected;
    loop.quit();
 };
 deadline.start(4000);loop.exec();return outcome;
}

int runRvTunerTests(const QString &path){
 QFile report(path);if(!report.open(QIODevice::WriteOnly|QIODevice::Text))return 3;QTextStream out(&report);bool passed=true;int checks=0;
 const auto check=[&](const QString &name,bool ok){++checks;out<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;passed&=ok;};
 for(int mode:{9,10,11,12,13,15,17})check(QString("RETIRED_BOARD_MODE_%1_REJECTED").arg(mode),!RvDtofCandidates::supports(mode));
 {
    QJsonObject identity{{"candidate_mode",16},{"engine",LiteSrBoardContract::RectEngine},{"model_sha256",LiteSrBoardContract::RectModelSha},{"checkpoint_sha256",LiteSrBoardContract::NearCheckpointSha},{"training_minimum_m",.01},{"width",384},{"height",256},{"fresh_ms",1000},{"rgb_paused",true},{"video_transport","none"},{"video_fps",0}};
    check("RECT_IDENTITY_ACCEPTED",liteSrBoardIdentity(identity));
    identity["width"]=256;check("RECT_SQUARE_REJECTED",!liteSrBoardIdentity(identity));identity["width"]=384;
    identity["model_sha256"]=QString(64,'0');check("RECT_LEGACY_HASH_REJECTED",!liteSrBoardIdentity(identity));
    QByteArray depth(384*256*4,0),native(48*32*4,0);float value=.35f;quint32 bits;std::memcpy(&bits,&value,4);
    for(int i=0;i<384*256;++i)qToLittleEndian(bits,depth.data()+4*i);
    for(int i=0;i<48*32;++i){qToLittleEndian<quint16>(1400,native.data()+4*i);native[4*i+2]=20;native[4*i+3]=1;}
    auto result=RvLiteSrContour::refineBoard(depth,native,{0,false});
    check("RECT_CONTOUR_COMPLETE",result.depth.size()==depth.size()&&result.coverage.size()==384*256&&result.diagnostic["algorithm"]=="litesr_board_contour_384x256_v1");
    check("RECT_PARTIAL_REJECTED",rejects([&]{RvLiteSrContour::refineBoard(depth.chopped(4),native,{0,false});}));
    check("RETIRED_512_CONTOUR_REJECTED",rejects([&]{RvLiteSrContour::refineBoard(QByteArray(512*512*4,0),native,{0,false});}));
    RvDepthView view;view.setRadialMode();view.setNativeGridSize({48,32});
    const bool accepted=view.setDepth(result.depth,{{"width",384},{"height",256},{"depth_unit","sensor_radial_m_float32"},{"frame_id",1},{"generation","rect-test"}});
    check("RECT_METRIC_CORNERS_AND_FOV",accepted&&std::abs(view.depthAt(0,0)-value)<1e-6f&&std::abs(view.depthAt(383,255)-value)<1e-6f&&view.depthAt(384,255)==0&&view.presentationSize()==QSizeF(32,48));
 }
 for(int warning:{0,8,16,24,32,64,128})check(QString("RECT_DEPTH_ONLY_FRAGMENTED_LSG2_WARNING_%1").arg(warning),rectStreamCase(warning,0)==(warning==0||warning==8||warning==16?StreamOutcome::Accepted:StreamOutcome::Rejected));
 for(int defect=1;defect<=7;++defect)check(QString("RECT_DEPTH_ONLY_PAIR_CAMERA_SHAPE_REJECT_%1").arg(defect),rectStreamCase(0,defect)==StreamOutcome::Rejected);
 {
    RvDepthView view;view.setRadialMode();view.setNativeGridSize({16,16});QByteArray depth(128*128*4,0);const float v=.35f;
    for(int i=0;i<128*128;++i)std::memcpy(depth.data()+4*i,&v,4);
    check("NATIVE16_DISPLAY_GRID",view.setDepth(depth,{{"width",128},{"height",128},{"depth_unit","sensor_radial_m_float32"},{"frame_id",1},{"generation","native16-test"}})&&view.presentationSize()==QSizeF(16,16)&&std::abs(view.depthAt(127,127)-v)<1e-6);
 }
 QTemporaryDir temporary;if(!temporary.isValid())return 3;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,temporary.path());
 using namespace RvTuner;Settings s;s.signal=true;
 for(int mode=0;mode<14;++mode){s.mode=mode;check(QString("MODE_%1_MAPPING").arg(mode),s.size()==(mode<=4?QSize(8,8):mode<=7?QSize(16,16):mode<=10?QSize(32,32):QSize(48,32)));check(QString("MODE_%1_PACKET").arg(mode),Decoder(Token,s,1).feed(packet(s)).size()==1);}
 s.mode=13;s.objects=4;check("DUAL_48_FOUR_TARGET_CAPACITY",rejects([&]{s.validate();}));s.mode=11;s.noise=true;check("SIGNAL_NOISE_48_FOUR_TARGET_CAPACITY",rejects([&]{s.validate();}));s.objects=3;s.noise=false;s.mode=0;
 auto bytes=packet(s);Decoder fragment(Token,s,1);int complete=0;for(int i=0;i<bytes.size();++i)complete+=fragment.feed(bytes.mid(i,1)).size();check("EVERY_BYTE_FRAGMENTED_ONE_COMPLETE_FRAME",complete==1&&!fragment.partial());
 for(int offset:{0,4,6,8,24,40,42,44,45,47,49,50,52,60,62,64,66,75}){auto broken=bytes;broken[offset]^=0x40;check(QString("BAD_HEADER_OR_SLOT_%1").arg(offset),rejects([&]{Decoder(Token,s,1).feed(broken);}));}
 check("LAYOUT_TARGET_MISMATCH_REJECTED",rejects([&]{auto b=bytes;b[46]^=1;Decoder(Token,s).feed(b);}));
 {auto b=bytes;b[46]|=0x40;const auto f=Decoder(Token,s).feed(b).first();check("VENDOR_FINAL_SUBFRAME_BIT_PRESERVED_COMPLETE_DTN1",f.samples.size()==s.size().width()*s.size().height()*s.objects&&f.resultFormat==quint8(b[46]));}
 auto zeroSequence=bytes;qToLittleEndian<quint32>(0,zeroSequence.data()+12);check("ZERO_SEQUENCE_REJECTED",rejects([&]{Decoder(Token,s,1).feed(zeroSequence);}));
 Decoder monotonic(Token,s,1);monotonic.feed(bytes);check("REPEATED_SEQUENCE_REJECTED",rejects([&]{monotonic.feed(bytes);}));
 check("WRONG_FOV_REJECTED",rejects([&]{Decoder(Token,s,2).feed(bytes);}));check("WRONG_TOKEN_REJECTED",rejects([&]{Decoder(QString(32,'b'),s,1).feed(bytes);}));
 for(int fov=0;fov<16;++fov){auto b=bytes;b[47]=char(fov);check(QString("DTN1_RAW_FOV_%1_INDEPENDENT_FROM_GEO2").arg(fov),Decoder(Token,s).feed(b).first().statistics(1)["fov_correction"].toInt()==fov);}
 check("DTN1_RAW_FOV_RANGE",rejects([&]{auto b=bytes;b[47]=16;Decoder(Token,s).feed(b);}));
 check("DTN1_RAW_FOV_STABLE_WITHIN_TOKEN",rejects([&]{Decoder d(Token,s);d.feed(bytes);auto b=packet(s,2);b[47]=0;d.feed(b);}));
 check("CAN8_IS_NOT_DTN1",rejects([&]{auto b=bytes;b.replace(0,4,"CAN8");Decoder(Token,s,1).feed(b);}));
 auto joined=bytes+packet(s,2);check("COALESCED_PACKETS",Decoder(Token,s,1).feed(joined).size()==2);
 auto partial=Decoder(Token,s,1);check("TRUNCATED_NEVER_PUBLISHED",partial.feed(bytes.chopped(1)).isEmpty()&&partial.partial());
 check("OVERSIZED_BUFFER_REJECTED",rejects([&]{Decoder(Token,s,1).feed(QByteArray(512*1024+1,0));}));
 auto warning=bytes;warning[49]=warning[50]=9;qToLittleEndian<quint32>(8,warning.data()+52);check("OBSERVE_08_PRESERVED",Decoder(Token,s,1).feed(warning).first().warnings==8);
 {auto b=bytes;b[49]=b[50]=17;qToLittleEndian<quint32>(16,b.data()+52);check("INFO_10_PRESERVED",Decoder(Token,s,1).feed(b).first().warnings==16);
 b[49]=b[50]=25;qToLittleEndian<quint32>(24,b.data()+52);check("MIXED_08_10_REJECTED",rejects([&]{Decoder(Token,s,1).feed(b);}));}
 s.signal=false;const auto noSignal=Decoder(Token,s,1).feed(packet(s)).first();check("MISSING_SIGNAL_IS_CHANNEL_ABSENCE",!noSignal.settings.signal);auto forged=packet(s);forged[76]=1;check("MISSING_SIGNAL_NOT_ZERO_MEASUREMENT",rejects([&]{Decoder(Token,s,1).feed(forged);}));
 check("CHIP_ENCODING_SHARED_WITH_PHOTON",rv_photon_count(0x8002)==512&&rv_photon_count(0x7fff)==32767&&rv_photon_count(0xffff)==8388352);
 s.signal=true;auto frame=Decoder(Token,s,1).feed(packet(s)).first();check("TARGET_MAJOR_SAME_FRAME",frame.sample(2,0).range==1064&&frame.sample(1,0).range==1000);check("INVALID_TARGET_REJECTED",rejects([&]{frame.sample(4,0);}));
 check("DISPLAY_DIRECTION_NO_RAW_REORDER",displayIndex(0,0,QSize(48,32),true,0)==1535&&displayIndex(0,0,QSize(48,32),false,180)==1535&&displayIndex(0,0,QSize(48,32),true,180)==0);
 const QByteArray native="#!/bin/sh\n# preserve MODEL and calibration\nCANONICAL_RUN_ID=\"$generation\" ./tmf8829_ext -m -g -b 0 -d 11 -p 66 -t 0 -o 1 -r 7 -i 1500 --socket /run/extrinsic_capture/tof.sock &\necho unchanged\n";
 const QByteArray lite="#!/bin/sh\nsignal=-g; rate=4; distance=11;engine=unchanged\nCANONICAL_RUN_ID=\"dtof_$2\" launch_tof \"$1\" ./tmf8829_ext -m $signal -b 0 -d \"$distance\" -p 66 -t 0 -o 1 -r \"$rate\" -i 1200 --socket /run/extrinsic_capture/tof.sock &\necho unchanged\n";
 check("REAL_SETTINGS_NOT_SNR_DEFAULT",scriptSettings(native).snr==7&&scriptSettings(native).iterations==1500);
 Settings save=scriptSettings(native);save.mode=13;save.objects=3;save.shortIterations=200;save.noise=true;const auto changedNative=patchScript(native,save),changedLite=patchScript(lite,save,true);
 check("BOTH_ACTUAL_SCRIPT_READBACK",scriptSettings(changedNative)==save&&scriptSettings(changedLite,true)==save);
 check("NON_SENSOR_BYTES_PRESERVED",changedNative.startsWith("#!/bin/sh\n# preserve MODEL and calibration\n")&&changedNative.endsWith("echo unchanged\n")&&changedLite.contains("signal=-g; rate=4; distance=11;engine=unchanged"));
 check("RESTART_READS_SAVED_NOT_DEFAULT",scriptSettings(patchScript(changedLite,save,true),true)==save);
 check("MODEL_COMPATIBILITY_FAIL_CLOSED",!save.liteSrCompatible());
 {
    Settings compatible;compatible.signal=true;compatible.snr=6;
    check("ORDINARY_SUPPORTED_BY_LITESR_AND_HOST_DEPTH",compatible.liteSrCompatible()&&compatible.hostDepthCompatible());
    auto noSignal=compatible;noSignal.signal=false;
    check("HOST_DEPTH_DOES_NOT_REQUIRE_SIGNAL",noSignal.hostDepthCompatible()&&!noSignal.liteSrCompatible());
    compatible.mode=12;
    check("HIGH_ACCURACY_SUPPORTED_BY_BOARD_LITESR_ONLY",compatible.liteSrCompatible()&&!compatible.hostDepthCompatible());
    const auto savedHighNative=patchScript(native,compatible),savedHighLite=patchScript(lite,compatible,true);
    check("HIGH_ACCURACY_SAVE_READBACK_KEEPS_MODE_AND_SNR",scriptSettings(savedHighNative)==compatible&&scriptSettings(savedHighLite,true)==compatible&&scriptSettings(savedHighLite,true).snr==6);
    for(int mode=0;mode<14;++mode){auto candidate=compatible;candidate.mode=mode;
        check(QString("HOST_DEPTH_SENSOR_MODE_%1_SCOPE").arg(mode),candidate.hostDepthCompatible()==(mode==11));
        check(QString("FP16_NATIVE16_SENSOR_MODE_%1_SCOPE").arg(mode),candidate.fp16Native16Compatible()==(mode==5||mode==6));
        candidate.signal=false;
        candidate.objects=2;
        check(QString("FP16_NATIVE16_SENSOR_MODE_%1_SINGLE_TARGET").arg(mode),!candidate.fp16Native16Compatible());
        candidate.objects=1;candidate.noise=true;
        check(QString("FP16_NATIVE16_SENSOR_MODE_%1_NOISE_REJECTED").arg(mode),!candidate.fp16Native16Compatible());}
    for(int mode:{0,1,11}){auto candidate=compatible;candidate.mode=mode;candidate.objects=2;
        check(QString("HOST_DEPTH_%1_SINGLE_TARGET").arg(mode),!candidate.hostDepthCompatible());
        candidate.objects=1;candidate.signal=false;
        check(QString("HOST_DEPTH_%1_WITHOUT_SIGNAL_SCOPE").arg(mode),candidate.hostDepthCompatible()==(mode==11));candidate.noise=true;
        check(QString("HOST_DEPTH_%1_NOISE_REJECTED").arg(mode),!candidate.hostDepthCompatible());}
    for(int unsupported:{0,2,5,6,8,9,13}){auto candidate=compatible;candidate.mode=unsupported;
        check(QString("LITESR_REJECTS_SMALL_GRID_OR_DUAL_%1").arg(unsupported),!candidate.liteSrCompatible()&&!candidate.hostDepthCompatible());}
    for(int mode:{11,12}){
        auto candidate=compatible;candidate.mode=mode;candidate.objects=2;check(QString("LITESR_%1_REJECTS_MULTIPLE_TARGETS").arg(mode),!candidate.liteSrCompatible());
        candidate.objects=1;candidate.signal=false;check(QString("LITESR_%1_REQUIRES_SIGNAL").arg(mode),!candidate.liteSrCompatible());
        candidate.signal=true;candidate.noise=true;check(QString("LITESR_%1_REJECTS_NOISE").arg(mode),!candidate.liteSrCompatible());
    }
 }
 for(int mode=0;mode<14;++mode){
    save.mode=mode;const auto permanentNative=patchScript(native,save),permanentLite=patchScript(lite,save,true);
    check(QString("PERMANENT_MODE_%1_BOTH_SCRIPTS_READBACK").arg(mode),scriptSettings(permanentNative)==save&&scriptSettings(permanentLite,true)==save);
    check(QString("PERMANENT_MODE_%1_REOPEN_IDEMPOTENT").arg(mode),patchScript(permanentNative,save)==permanentNative&&patchScript(permanentLite,save,true)==permanentLite);
    check(QString("PERMANENT_MODE_%1_SHARED_GRID_GATE").arg(mode),save.nativeCompatible()==(mode>=11)&&(mode>=11||!save.liteSrCompatible()));
 }
 save.mode=13;save.objects=4;check("PERMANENT_DUAL_CAPACITY_STILL_REJECTED",rejects([&]{patchScript(native,save);}));
 save.mode=11;check("PERMANENT_SIGNAL_NOISE_CAPACITY_STILL_REJECTED",rejects([&]{patchScript(lite,save,true);}));
 check("UNKNOWN_LITESR_VARIABLES_REJECTED",rejects([&]{auto b=lite;b.replace("rate=4","rate=6");scriptSettings(b,true);}));
 check("DUPLICATE_SENSOR_COMMAND_REJECTED",rejects([&]{scriptSettings(native+native);}));
 check("JSON_NONINTEGER_REJECTED",rejects([&]{auto j=s.json();j["snr"]=4.1;Settings::fromJson(j);}));
 check("JSON_NONBOOLEAN_REJECTED",rejects([&]{auto j=s.json();j["signal"]=1;Settings::fromJson(j);}));
 const auto devicePath=temporary.filePath("devices.json");
 const auto fixture=offlineRvDevice();
 RvDeviceStore(devicePath).save({fixture},fixture.id);
 RvSessionController session(nullptr,devicePath);auto *control=session.tuner();RvTunerWidget widget(control);widget.resize(1000,900);widget.setAttribute(Qt::WA_DontShowOnScreen);widget.show();QApplication::processEvents();
 check("OPEN_INERT",control->state()==RvTunerController::Idle&&!control->pending()&&session.userStopped()&&!control->exclusive());
 {
    const auto initial=widget.settings().json();
    auto *splitter=widget.m_splitter;
    auto *parameters=widget.findChild<QScrollArea*>("tunerParametersScroll");
    auto *measurements=widget.findChild<QScrollArea*>("tunerMeasurementsScroll");
    check("VERTICAL_SPLITTER_HAS_TWO_NONCOLLAPSIBLE_SCROLL_PANES",splitter&&splitter->orientation()==Qt::Vertical&&splitter->count()==2
        &&parameters&&measurements&&splitter->widget(0)==parameters&&splitter->widget(1)==measurements&&!splitter->childrenCollapsible());
    const auto drag=[&](int delta){
        auto *handle=splitter->handle(1);const QPointF local(handle->rect().center()),global(handle->mapToGlobal(local.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(handle,&press);
        const QPointF destination=global+QPointF(0,delta);
        QMouseEvent move(QEvent::MouseMove,QPointF(handle->mapFromGlobal(destination.toPoint())),destination,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(handle,&move);
        QMouseEvent release(QEvent::MouseButtonRelease,QPointF(handle->mapFromGlobal(destination.toPoint())),destination,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(handle,&release);QApplication::processEvents();
    };
    splitter->setSizes({220,660});QApplication::processEvents();const auto before=splitter->sizes();drag(180);const auto expanded=splitter->sizes();
    check("DRAG_EXPANDS_SETTINGS_BEYOND_OLD_280_PIXEL_CAP",expanded[0]>280&&expanded[0]>before[0]&&expanded[1]<before[1]);
    drag(-130);const auto reduced=splitter->sizes();
    check("DRAG_BACK_RETURNS_HEIGHT_TO_MEASUREMENTS",reduced[0]<expanded[0]&&reduced[1]>expanded[1]);
    drag(-2000);const bool settingsAccessible=splitter->sizes()[0]>=parameters->minimumHeight();drag(2000);
    check("EXTREME_DRAGS_KEEP_BOTH_PANES_ACCESSIBLE",settingsAccessible&&splitter->sizes()[1]>=measurements->minimumHeight());
    widget.resize(420,640);QApplication::processEvents();splitter->setSizes({100,520});QApplication::processEvents();parameters->verticalScrollBar()->setValue(parameters->verticalScrollBar()->maximum());QApplication::processEvents();
    const auto intersects=[](QWidget *item,QScrollArea *area){return QRect(item->mapTo(area->viewport(),QPoint()),item->size()).intersects(area->viewport()->rect());};
    check("COMPACT_SETTINGS_SCROLL_REACHES_LAST_SAVED_ROW",parameters->verticalScrollBar()->maximum()>0&&intersects(widget.m_saved,parameters));
    splitter->setSizes({440,180});QApplication::processEvents();measurements->verticalScrollBar()->setValue(measurements->verticalScrollBar()->maximum());QApplication::processEvents();
    check("COMPACT_MEASUREMENTS_SCROLL_REACHES_COMPARISON_TABLE",measurements->verticalScrollBar()->maximum()>0&&intersects(widget.m_results,measurements));
    check("NARROW_PANES_KEEP_HORIZONTAL_ACCESS",parameters->horizontalScrollBar()->maximum()>0&&measurements->horizontalScrollBar()->maximum()>0);
    check("RESIZING_ONLY_CHANGES_PRESENTATION",widget.settings().json()==initial&&control->state()==RvTunerController::Idle&&!control->pending()&&session.userStopped()&&!control->exclusive());
    widget.resize(1000,900);splitter->setSizes({390,490});parameters->verticalScrollBar()->setValue(0);measurements->verticalScrollBar()->setValue(0);measurements->horizontalScrollBar()->setValue(0);QApplication::processEvents();
 }
 widget.acceptFrame(frame);auto *distance=widget.findChild<QWidget*>("tunerDistanceCanvas"),*signal=widget.findChild<QWidget*>("tunerSignalCanvas");
 widget.m_target->setCurrentIndex(1);check("DUAL_PLOTS_SAME_FRAME_AND_TARGET",distance->property("measurement")==signal->property("measurement")&&distance->property("target").toInt()==2&&signal->property("target").toInt()==2);
 widget.acceptFrame(noSignal);check("NO_SIGNAL_CLEAR_RIGHT_ONLY",distance->property("hasFrame").toBool()&&!signal->property("hasFrame").toBool());
 widget.acceptFrame(frame);widget.record();check("COMPARISON_RECORD_IDENTITY",widget.m_records.size()==1&&widget.m_records[0].toObject()["sequence"].toInt()==1&&widget.m_results->rowCount()==1);
 struct Call{QString action;QJsonObject request;quint64 operation;};QVector<Call> calls;
 control->m_testTransport=[&](const QString &a,const QJsonObject &r,quint64 o){calls.append({a,r,o});};
 const auto reply=[&](const QJsonObject &r,const QString &error=QString()){const auto c=calls.last();control->completed(c.operation,c.action,r,error);};
 const QJsonObject inspection{{"settings",scriptSettings(native).json()},{"snapshot",QString(64,'c')},{"boot_id",Boot}};
 s.mode=11;s.objects=1;s.signal=true;
 for(int mode:{-1,-2,11,12}){
    session.m_dtofMode=mode<0?-1:mode;session.m_userStopped=mode==-1;session.m_captureOwner=mode==-1?QJsonObject():QJsonObject{{"owner_boot",Boot},{"owner_generation",Token}};const auto scheme=session.dtofCandidateMode();
    control->start(s);if(calls.isEmpty()){out<<"START_PREREQUISITE FAIL "<<control->detail()<<"\nAPP_DIR "<<QCoreApplication::applicationDirPath()<<"\nAPP_ROOT "<<RuntimeDiscovery::applicationRoot()<<"\nRV_OVERRIDE "<<qEnvironmentVariable("RV_CONTROL_CPP")<<" is_set="<<qEnvironmentVariableIsSet("RV_CONTROL_CPP")<<Qt::endl;return 1;}check(QString("PREFLIGHT_%1_BEFORE_STOP").arg(mode),calls.last().action=="inspect"&&!control->exclusive()&&session.userStopped()==(mode==-1));reply(inspection);
    check(QString("HANDOVER_%1_REVOKES_SHARED").arg(mode),calls.last().action=="start"&&control->exclusive()&&session.userStopped()&&!session.m_reconnectTimer.isActive());
    const int count=calls.size();session.start();session.reconnectNow();session.retryConnection();session.selectImagingScheme("host_assisted",{});check(QString("MUTEX_%1_CANNOT_BYPASS").arg(mode),calls.size()==count&&session.userStopped()&&session.dtofCandidateMode()==scheme);
    const auto start=calls.last();reply({{"token",start.request["token"]},{"settings",s.json()},{"snapshot",QString(64,'c')}});
    check(QString("RUNNING_%1").arg(mode),control->state()==RvTunerController::Running);
    control->stop();check("STOP_ASYNC_HOLDS_MUTEX",calls.last().action=="stop"&&control->exclusive());reply({{"state","stopped"}});
    check(QString("STOP_%1_NO_AUTORESTORE").arg(mode),!control->exclusive()&&session.userStopped()&&session.dtofCandidateMode()==scheme&&control->appliedSettings().isEmpty());
 }
 session.m_dtofMode=-1;session.m_userStopped=false;auto invalid=s;invalid.mode=13;invalid.objects=4;const int count=calls.size();control->start(invalid);check("INVALID_BEFORE_ANY_STOP_OR_IO",calls.size()==count&&!session.userStopped());
 control->start(s);const auto stale=calls.last();reply({},"External owner");check("EXTERNAL_OWNER_PREFLIGHT_KEEPS_SHARED",!session.userStopped()&&!control->exclusive());
 control->start(s);control->stop();reply(inspection);check("CLOSE_DURING_PREFLIGHT_NO_TAKEOVER",!control->exclusive()&&!session.userStopped()&&control->state()==RvTunerController::Idle);
 control->start(s);++session.m_presentationEpoch;reply(inspection);check("SHARED_CHANGE_DURING_PREFLIGHT_NO_TAKEOVER",!control->exclusive()&&!session.userStopped()&&control->state()==RvTunerController::Fault);
 control->start(s);reply(inspection);const auto starting=calls.last();control->stop();check("STOP_WAITS_FOR_START_TRANSACTION",calls.last().operation==starting.operation);reply({{"token",starting.request["token"]},{"settings",s.json()},{"snapshot",QString(64,'c')}});check("STOP_AFTER_START_RECEIPT",calls.last().action=="stop");reply({{"state","stopped"}});
 control->completed(stale.operation,stale.action,inspection,{});check("OLD_CALLBACK_CANNOT_REVIVE",session.userStopped()&&!control->exclusive());
 control->start(s);reply(inspection);reply({},"Startup failed");check("FAILED_START_STILL_CLEANS_OWN_TOKEN",calls.last().action=="stop"&&control->exclusive());reply({{"state","stopped"}});check("FAILED_START_REASON_PRESERVED",control->state()==RvTunerController::Fault&&control->detail().contains("Startup failed"));
 control->start(s);reply(inspection);auto c=calls.last();reply({{"token",c.request["token"]},{"settings",s.json()},{"snapshot",QString(64,'c')}});
 auto edited=s;edited.snr=20;const auto oldSaved=control->savedSettings();control->apply(edited);c=calls.last();check("APPLY_NEW_TOKEN_AND_NO_PERSIST",c.action=="switch"&&c.request["old_token"]!=c.request["token"]&&control->savedSettings()==oldSaved);reply({{"token",c.request["token"]},{"settings",edited.json()},{"snapshot",QString(64,'c')}});
 check("TEMPORARY_DOES_NOT_UPDATE_SAVED",control->appliedSettings()==edited.json()&&control->savedSettings()==oldSaved);
 control->persist();check("EXPLICIT_PERSIST_ONLY_APPLIED",calls.last().action=="persist"&&calls.last().request["settings"]==edited.json());reply({{"settings",edited.json()},{"snapshot",QString(64,'d')},{"litesr_compatible",true},{"backup","mock"}});check("SAVE_RECEIPT_UPDATES_SNAPSHOT",control->savedSettings()==edited.json()&&control->inspection()["snapshot"]==QString(64,'d'));
 control->persist();reply({},"Configuration drift");check("FAILED_SAVE_NOT_CLAIMED_SAVED",control->savedSettings().isEmpty()&&calls.last().action=="stop");reply({},"STOP could not be confirmed");check("FAILED_STOP_MUTEX_RETAINED",control->exclusive()&&control->state()==RvTunerController::Fault);session.start();check("FAULT_CANNOT_RECONNECT",session.userStopped());control->stop();reply({{"state","stopped"}});
 control->start(s);reply(inspection);c=calls.last();reply({{"token",c.request["token"]},{"settings",s.json()},{"snapshot",QString(64,'c')}});control->fail("Disconnected");check("DISCONNECT_CLEARS_AND_STOPS_NO_RETRY",calls.last().action=="stop"&&!control->m_heartbeat.isActive()&&widget.m_frame.samples.isEmpty());reply({{"state","stopped"}});
 for(int mode:{0,1,5,8,11,12}){
    auto grid=s;grid.mode=mode;auto current=inspection;current["settings"]=grid.json();
    RvTunerWidget reopened(control);control->read();reply(current);
    check(QString("SAVED_GRID_%1_REOPEN_READ").arg(mode),control->state()==RvTunerController::Idle&&control->savedSettings()==grid.json()&&reopened.settings()==grid);
    control->start(grid);reply(current);c=calls.last();reply({{"token",c.request["token"]},{"settings",grid.json()},{"snapshot",QString(64,'c')}});
    widget.setControls(grid);widget.refresh();check(QString("SAVE_GRID_%1_BUTTON_ENABLED").arg(mode),widget.m_buttons["Persist"]->isEnabled());
    auto draft=grid;draft.snr+=1;widget.setControls(draft);widget.refresh();check(QString("SAVE_GRID_%1_PENDING_MUST_APPLY").arg(mode),!widget.m_buttons["Persist"]->isEnabled());
    widget.setControls(grid);widget.refresh();widget.m_buttons["Persist"]->click();
    check(QString("SAVE_GRID_%1_REACHES_TRANSACTION").arg(mode),calls.last().action=="persist"&&calls.last().request["settings"]==grid.json());
    const int pendingCount=calls.size();control->persist();check(QString("SAVE_GRID_%1_NO_DUPLICATE_TRANSACTION").arg(mode),calls.size()==pendingCount);
    reply({{"settings",grid.json()},{"snapshot",QString(64,'d')},{"litesr_compatible",grid.liteSrCompatible()},{"backup","mock"}});
    check(QString("SAVE_GRID_%1_RECEIPT_AND_COMPATIBILITY").arg(mode),control->savedSettings()==grid.json()&&control->state()==RvTunerController::Running&&(grid.fp16Native16Compatible()?control->detail().contains(QString::fromUtf8("640×480"))&&control->detail().contains(QString::fromUtf8("16×16")):grid.liteSrCompatible()||control->detail().contains(QString::fromUtf8("48×32"))));
    control->stop();reply({{"state","stopped"}});
 }
 LanguageManager language;language.setLanguage(LanguageManager::Language::SimplifiedChinese,false);QApplication::processEvents();check("EMBEDDED_CHINESE_TUNER_TRANSLATION",widget.m_buttons["Persist"]->text()==QString::fromUtf8("长期保存"));
 check("EMBEDDED_CHINESE_ALL_RESOLUTIONS_SAVE",widget.m_buttons["Persist"]->toolTip().contains(QString::fromUtf8("所有受支持分辨率")));
 widget.acceptFrame(frame);widget.m_status->setText(QString::fromUtf8("离线合成 DTN1 验证（SIMULATED），未连接设备"));QApplication::processEvents();
 widget.grab().save(QFileInfo(path).absolutePath()+"/tuner_offline.png");
 out<<"CHECKS "<<checks<<"\nOFFLINE_SYNTHETIC_ONLY; no board connection, deployment, capture or permanent write\n";return passed?0:1;
}

// Explicit opt-in diagnostic, deliberately absent from every automatic suite.
// Uses the production controller, native worker, decoder and both Qt canvases.
// No deployment, persistence, scheme changes, image or measurement-array files.
int runRvTunerLiveTests(const QString &path){
 QTemporaryDir temporary(QFileInfo(path).absolutePath()+"/tuner_live_XXXXXX");if(!temporary.isValid())return 3;
 QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,temporary.path());
 RvSessionController session;auto *control=session.tuner();RvTunerWidget widget(control);widget.resize(1000,900);widget.setAttribute(Qt::WA_DontShowOnScreen);widget.show();
 QElapsedTimer total,receiving;total.start();int frames=0;bool paired=true;QString failure;QJsonObject last,before;double fps=0;quint32 firstSequence=0,lastSequence=0;
 QObject::connect(control,&RvTunerController::frameReady,&widget,[&](const RvTuner::Frame &frame){
  if(!frames){receiving.start();firstSequence=frame.sequence;}++frames;lastSequence=frame.sequence;
  const auto *distance=widget.findChild<QWidget*>("tunerDistanceCanvas"),*signal=widget.findChild<QWidget*>("tunerSignalCanvas");
  paired&=distance->property("hasFrame").toBool()&&signal->property("hasFrame").toBool()&&distance->property("measurement")==signal->property("measurement")&&distance->property("target")==signal->property("target");
  last=frame.statistics(1);int nonzero=0;for(const auto &sample:frame.samples)nonzero+=sample.signal!=0;last["signal_nonzero_samples"]=nonzero;fps=control->fps();
 });
 const auto pump=[&]{QApplication::processEvents(QEventLoop::AllEvents,20);QThread::msleep(5);};
 control->read();while(control->pending()&&total.elapsed()<10000)pump();
 before=control->inspection();
 if(control->pending()||control->state()!=RvTunerController::Idle||before["owner"]!="idle")failure="Read-only preflight did not confirm an idle healthy device: "+control->detail();
 else if(!before["settings"].toObject()["signal"].toBool())failure="Signal is disabled; this diagnostic never changes saved or experiment parameters";
 else {
  control->start(RvTuner::Settings::fromJson(before["settings"].toObject()));
  while(total.elapsed()<30000){pump();if(control->state()==RvTunerController::Fault){failure=control->detail();break;}if(frames&&receiving.elapsed()>=8000)break;}
  if(frames<2&&failure.isEmpty())failure="Fewer than two complete measurements";
 }
 const qint64 receiveMs=frames?receiving.elapsed():0;
 control->stop();while(control->shutdownPending()&&total.elapsed()<55000)pump();
 const bool stopped=!control->shutdownPending(); // Fault text may remain after acknowledged cleanup.
 if(!stopped&&failure.isEmpty())failure="Normal STOP was not confirmed: "+control->detail();
 if(stopped){control->read();while(control->pending()&&total.elapsed()<60000)pump();}
 const auto after=control->inspection();const bool unchanged=!control->pending()&&after["owner"]=="idle"&&after["snapshot"]==before["snapshot"]&&after["settings"]==before["settings"]&&after["litesr_settings"]==before["litesr_settings"];
 const bool passed=failure.isEmpty()&&paired&&frames>=2&&stopped&&unchanged;
 const QJsonObject result{{"ok",passed},{"failure",failure},{"frames",frames},{"first_sequence",int(firstSequence)},{"last_sequence",int(lastSequence)},{"paired_canvases",paired},{"actual_fps",fps},{"receive_ms",receiveMs},{"elapsed_ms",total.elapsed()},{"normal_stop",stopped},{"configuration_unchanged",unchanged},{"statistics",last},{"settings",before["settings"]},{"boot_id",before["boot_id"]},{"inspection_before",before},{"inspection_after",after},{"arrays_recorded",false},{"deployed",false},{"persisted",false}};
 QSaveFile output(path);const auto bytes=QJsonDocument(result).toJson();if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())return 3;return passed?0:1;
}
