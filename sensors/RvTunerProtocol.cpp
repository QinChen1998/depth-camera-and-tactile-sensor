#include "RvTunerProtocol.h"
#include <QtCore/QRegularExpression>
#include <QtCore/QtEndian>
#include <algorithm>
#include <cmath>

namespace RvTuner {
namespace {
void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
QString commandLine(const QByteArray &script) {
    const QString text=QString::fromUtf8(script);
    const QRegularExpression re("^[^\\r\\n#]*tmf8829_(?:ext|tuner)[ \\t]+-m\\b[^\\r\\n]*",QRegularExpression::MultilineOption);
    auto matches=re.globalMatch(text);require(matches.hasNext(),"Missing sensor command; unknown service format");
    const auto line=matches.next().captured();require(!matches.hasNext(),"Ambiguous sensor command");return line;
}
QString expandLegacy(QString line,const QByteArray &script,bool liteSr) {
    if(!liteSr)return line;
    // Only the exact, shipped LiteSR defaults may be interpreted as variables.
    // After an explicit save this single command contains literal sensor flags.
    if(line.contains("$distance")||line.contains("$rate")||line.contains("$signal")) {
        require(script.contains("signal=-g; rate=4; distance=11;"),"Unknown LiteSR sensor variables; refuse to guess parameters");
        line.replace("\"$distance\"","11");line.replace("\"$rate\"","4");line.replace("$signal","-g");
    }
    return line;
}
QRegularExpression flag(const QString &name){return QRegularExpression("(?<!\\S)"+name+"[ \\t]+([0-9]+)(?=[ \\t]|$)");}
int numeric(const QString &line,const QString &name,int fallback=-1){
    auto matches=flag(name).globalMatch(line);if(!matches.hasNext()){require(fallback>=0,"Missing numeric sensor argument");return fallback;}
    bool ok=false;const int n=matches.next().captured(1).toInt(&ok);require(ok&&!matches.hasNext(),"Invalid or repeated sensor argument");return n;
}
bool toggle(const QString &line,const QString &name){auto m=QRegularExpression("(?<!\\S)"+name+"(?=[ \\t]|$)").globalMatch(line);if(!m.hasNext())return false;m.next();require(!m.hasNext(),"Repeated sensor switch");return true;}
}
void Settings::validate() const {
    require(mode>=0&&mode<=13&&iterations>=100&&iterations<=10000&&snr>=4&&snr<=255&&periodMs>=20&&periodMs<=2000&&shortIterations>=1&&shortIterations<=10000&&objects>=1&&objects<=4,"dToF parameter is outside the supported range");
    require(!(mode==13&&objects==4),"48x32 dual mode with four targets exceeds sensor result capacity");
    require(!(mode>=11&&objects==4&&signal&&noise),"48x32 four targets with both Signal and Noise exceeds sensor result capacity");
}
QSize Settings::size() const {validate();return mode<=4?QSize(8,8):mode<=7?QSize(16,16):mode<=10?QSize(32,32):QSize(48,32);}
bool Settings::dual() const{return mode==3||mode==4||mode==7||mode==10||mode==13;}
bool Settings::nativeCompatible() const{return size()==QSize(48,32);}
bool Settings::liteSrCompatible() const{return (mode==11||mode==12)&&objects==1&&signal&&!noise;}
// UniDepth retains its calibrated M48 ordinary-mode policy.
bool Settings::hostDepthCompatible() const{return mode==11&&objects==1&&!noise;}
bool Settings::depthorCompatible() const{return ((mode>=0&&mode<=2)||mode==11)&&objects==1&&!noise;}
bool Settings::fp16Native16Compatible() const{return (mode==5||mode==6)&&objects==1&&!noise;}
QJsonObject Settings::json() const{return {{"mode",mode},{"iterations",iterations},{"snr",snr},{"period_ms",periodMs},{"short_iterations",shortIterations},{"objects",objects},{"signal",signal},{"noise",noise}};}
Settings Settings::fromJson(const QJsonObject &j){
    require(j.size()==8,"Incomplete dToF settings");Settings s;
    const auto number=[&](const char *key){const auto v=j[key];require(v.isDouble()&&std::isfinite(v.toDouble())&&std::floor(v.toDouble())==v.toDouble()&&v.toDouble()>=0&&v.toDouble()<=10000,"Non-integer dToF setting");return v.toInt();};
    s.mode=number("mode");s.iterations=number("iterations");s.snr=number("snr");s.periodMs=number("period_ms");s.shortIterations=number("short_iterations");s.objects=number("objects");
    require(j["signal"].isBool()&&j["noise"].isBool(),"Non-boolean dToF switch");s.signal=j["signal"].toBool();s.noise=j["noise"].toBool();s.validate();return s;
}
QString Settings::arguments() const {validate();return QString("%1 %2 %3 %4 %5 %6 %7 %8").arg(mode).arg(iterations).arg(shortIterations).arg(snr).arg(periodMs).arg(objects).arg(int(signal)).arg(int(noise));}
Settings scriptSettings(const QByteArray &script,bool liteSr){
    const auto line=expandLegacy(commandLine(script),script,liteSr);Settings s;
    s.mode=numeric(line,"-d");s.iterations=numeric(line,"-i");s.snr=numeric(line,"-r");s.periodMs=numeric(line,"-p");
    s.shortIterations=numeric(line,"-s",100);s.objects=numeric(line,"-o",1);s.signal=toggle(line,"-g");s.noise=toggle(line,"-n");s.validate();return s;
}
QByteArray patchScript(const QByteArray &script,const Settings &s,bool liteSr){
    s.validate();
    scriptSettings(script,liteSr);const auto old=commandLine(script);auto line=expandLegacy(old,script,liteSr);
    const QList<QPair<QString,int>> values={{"-d",s.mode},{"-i",s.iterations},{"-r",s.snr},{"-p",s.periodMs},{"-s",s.shortIterations},{"-o",s.objects}};
    QString additions;
    for(const auto &v:values){if(flag(v.first).match(line).hasMatch())line.replace(flag(v.first),v.first+" "+QString::number(v.second));else additions+=" "+v.first+" "+QString::number(v.second);}
    for(const auto &v:QList<QPair<QString,bool>>{{"-g",s.signal},{"-n",s.noise}}){
        const bool had=toggle(line,v.first);if(v.second&&!had)additions+=" "+v.first;
        if(!v.second&&had)line.remove(QRegularExpression("(?<!\\S)"+v.first+"(?=[ \\t]|$)"));
    }
    if(!additions.isEmpty()){const auto anchor=QRegularExpression("tmf8829_ext[ \\t]+-m\\b").match(line);require(anchor.hasMatch(),"Unknown sensor patch anchor");line.insert(anchor.capturedEnd(),additions);}
    auto result=script;const auto pos=result.indexOf(old.toUtf8());require(pos>=0,"Cannot preserve sensor script bytes");result.replace(pos,old.toUtf8().size(),line.toUtf8());
    require(scriptSettings(result,liteSr)==s,"Sensor patch readback mismatch");return result;
}
const Sample &Frame::sample(int target,int index) const {const int count=settings.size().width()*settings.size().height();require(target>=1&&target<=settings.objects&&index>=0&&index<count&&samples.size()==count*settings.objects,"Invalid target or incomplete measurement");return samples[(target-1)*count+index];}
QJsonObject Frame::statistics(int target) const {
    int count=settings.size().width()*settings.size().height(),valid=0,lo=255,hi=0;double sum=0;
    for(int i=0;i<count;++i){const auto &s=sample(target,i);if(s.valid&&s.range&&s.snr>=settings.snr){++valid;lo=std::min(lo,int(s.snr));hi=std::max(hi,int(s.snr));sum+=s.snr;}}
    return {{"valid",valid},{"total",count},{"coverage",double(valid)/count},{"snr_min",valid?lo:0},{"snr_max",hi},{"snr_mean",valid?sum/valid:0},{"temperature_c",temperature},{"warnings",int(warnings)},{"dropped",double(dropped)},{"target",target},{"sequence",double(sequence)},{"timestamp_ns",QString::number(timestamp)},{"fov_correction",fov},{"result_format",resultFormat}};
}
Decoder::Decoder(const QString &token,const Settings &s,int fov):m_token(token),m_settings(s),m_fov(fov){s.validate();require(QRegularExpression("^[a-f0-9]{32}$").match(token).hasMatch()&&fov>=-1&&fov<=15,"Invalid DTN1 session identity");}
QVector<Frame> Decoder::feed(const QByteArray &bytes){
    require(bytes.size()+m_buffer.size()<=512*1024,"DTN1 receive buffer exceeded");m_buffer+=bytes;QVector<Frame> frames;
    while(m_buffer.size()>=72){
        const char *p=m_buffer.constData();const auto u8=[&](int n){return quint8(p[n]);};
        const auto u16=[&](int n){return qFromLittleEndian<quint16>(p+n);};const auto u32=[&](int n){return qFromLittleEndian<quint32>(p+n);};
        Settings s;s.mode=u8(44);s.objects=u8(45);s.signal=u8(46)&8;s.noise=u8(46)&16;s.iterations=u16(60);s.shortIterations=u16(62);s.snr=u16(64);s.periodMs=u16(66);s.validate();
        const auto dim=s.size();const int count=dim.width()*dim.height()*s.objects,packet=72+count*8;
        require(m_buffer.startsWith("DTN1")&&u16(4)==1&&u16(6)==72&&u32(8)==quint32(packet)&&s==m_settings&&u16(40)==dim.height()&&u16(42)==dim.width(),"DTN1 header, settings or size mismatch");
        require(QByteArray(p+24,16).toHex()==m_token.toLatin1(),"DTN1 session token mismatch");
        require(u8(47)<=15&&(m_fov<0||u8(47)==m_fov),"DTN1 FOV correction changed or is outside its 4-bit range");
        // Bit 6 is the vendor's final subframe index. DTN1 already contains
        // the complete assembled measurement; it is not an extra channel.
        require(((u8(46)&7)?(u8(46)&7):1)==s.objects&&!(u8(46)&0xa0),"DTN1 result layout mismatch");
        const auto warnings=u32(52);require((warnings==0||warnings==8||warnings==16)&&u8(49)==(1|warnings)&&((u8(50)|u8(51))&~1u)==warnings&&(u8(50)&u8(51)&1)==1,"DTN1 sensor fault/status mismatch");
        if(m_buffer.size()<packet)break;
        Frame f;f.settings=s;f.token=m_token;f.sequence=u32(12);f.timestamp=qFromLittleEndian<quint64>(p+16);f.fov=u8(47);f.resultFormat=u8(46);f.temperature=qint8(p[48]);f.warnings=warnings;f.dropped=u32(56);f.sensorTick=u32(68);
        require(f.sequence>m_sequence&&f.timestamp>m_stamp,"DTN1 measurement did not advance");
        f.samples.reserve(count);for(int i=0;i<count;++i){int n=72+i*8;Sample v{u16(n),u8(n+2),u8(n+3),u16(n+4),u16(n+6)};require(v.valid<=1,"DTN1 invalid slot flag");require(s.signal||v.signal==0,"DTN1 unexpected Signal bytes");require(s.noise||v.noise==0,"DTN1 unexpected Noise bytes");f.samples.append(v);}
        m_fov=f.fov;m_sequence=f.sequence;m_stamp=f.timestamp;frames.append(f);m_buffer.remove(0,packet);
    }
    return frames;
}
int displayIndex(int x,int y,const QSize &s,bool aligned,int rotation){const int w=aligned?s.height():s.width(),h=aligned?s.width():s.height();if(x<0||x>=w||y<0||y>=h)return -1;if(rotation==180){x=w-1-x;y=h-1-y;}return aligned?(s.height()-1-x)*s.width()+s.width()-1-y:y*s.width()+x;}
}
