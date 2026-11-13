#pragma once
#include "../sdk/DepthSdkExports.h"
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSize>
#include <QtCore/QVector>
#include <QtCore/QMetaType>
#include <stdexcept>

// DTN1 is a separate complete-measurement protocol, never CAN8/PHC1/LSG.
namespace RvTuner {
struct DCTS_DEPTH_INTERNAL Settings {
    int mode=11,iterations=1200,snr=4,periodMs=66,shortIterations=100,objects=1;
    bool signal=false,noise=false;
    void validate() const;
    QSize size() const;
    bool dual() const;
    bool nativeCompatible() const;
    bool hostDepthCompatible() const;
    bool fp16Native16Compatible() const;
    QJsonObject json() const;
    QString arguments() const;
    static Settings fromJson(const QJsonObject &);
    bool operator==(const Settings &b) const {return json()==b.json();}
    bool operator!=(const Settings &b) const {return !(*this==b);}
};
DCTS_DEPTH_INTERNAL Settings scriptSettings(const QByteArray &script,bool liteSr=false);
DCTS_DEPTH_INTERNAL QByteArray patchScript(const QByteArray &script,const Settings &,bool liteSr=false);
struct Sample {quint16 range=0;quint8 snr=0,valid=0;quint16 signal=0,noise=0;};
struct DCTS_DEPTH_INTERNAL Frame {
    Settings settings;
    quint32 sequence=0,warnings=0,dropped=0,sensorTick=0;
    quint64 timestamp=0;
    int temperature=0,fov=0,resultFormat=0;
    QString token;
    QVector<Sample> samples;
    const Sample &sample(int target,int index) const;
    QJsonObject statistics(int target) const;
};
class DCTS_DEPTH_INTERNAL Decoder {
public:
    // DTN1 carries sensor FOV correction, not the calibrated CAN8/GEO2 F index.
    // -1 binds that metadata from the first complete measurement of this token.
    Decoder(const QString &token,const Settings &settings,int fov=-1);
    QVector<Frame> feed(const QByteArray &);
    bool partial() const {return !m_buffer.isEmpty();}
private:
    QString m_token;Settings m_settings;int m_fov;
    QByteArray m_buffer;quint32 m_sequence=0;quint64 m_stamp=0;
};
DCTS_DEPTH_INTERNAL int displayIndex(int x,int y,const QSize &native,bool aligned,int rotation);
}
Q_DECLARE_METATYPE(RvTuner::Frame)
