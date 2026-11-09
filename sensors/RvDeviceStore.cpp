#include "RvDeviceStore.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <stdexcept>

namespace {
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
QByteArray read(const QString &path) {
    if (!QFileInfo::exists(path)) return {};
    QFile file(path); require(file.open(QIODevice::ReadOnly) && file.size() <= 1024*1024, "Cannot read device registry (missing permissions or oversized file)");
    return file.readAll();
}
QJsonObject document(const QByteArray &bytes) {
    require(bytes.size() <= 1024*1024, "Device registry exceeds size limit");
    QJsonParseError error; const auto doc=QJsonDocument::fromJson(bytes,&error);
    require(error.error==QJsonParseError::NoError && doc.isObject(), "Invalid device registry JSON");
    const auto obj=doc.object();
    require(obj["schema"]==1 && obj["kind"]=="rv1103-devices" && obj["devices"].isArray(), "Unsupported device registry format");
    for (const auto &key:obj.keys()) require(QStringList{"schema","kind","devices","active"}.contains(key), "Unexpected device registry field; credentials must not be imported");
    return obj;
}
void validateList(const QVector<RvDeviceProfile> &profiles) {
    require(profiles.size()<=256, "At most 256 devices may be saved");
    QSet<QString> ids, keys, serials;
    for(const auto &p:profiles) {
        p.validate();
        require(!ids.contains(p.id)&&!keys.contains(p.publicKey)&&!serials.contains(p.serial), "Duplicate device ID, public key or serial; edit the existing device explicitly");
        ids.insert(p.id);keys.insert(p.publicKey);serials.insert(p.serial);
    }
}
QJsonObject payload(const QVector<RvDeviceProfile> &profiles,const QString &active) {
    QJsonArray list;for(const auto &p:profiles)list.append(p.json());
    return {{"schema",1},{"kind","rv1103-devices"},{"active",active},{"devices",list}};
}
}
QJsonObject RvDeviceProfile::json() const {
    QJsonObject result{{"id",id},{"name",name},{"host",host},{"ssh_port",sshPort},{"serial",serial},
        {"public_key",publicKey},{"username",username},{"calibration",calibration},
        {"input_rotation",inputRotation},{"camera_mount_rotation",cameraMountRotation},{"legacy_coordinator",legacyCoordinator},{"last_connected",lastConnected}};
    if(temporaryFovCalibration)result["temporary_fov_calibration"]=true;
    return result;
}
RvDeviceProfile RvDeviceProfile::fromJson(const QJsonObject &o) {
    QStringList fields={"id","name","host","ssh_port","serial","public_key","username","calibration","input_rotation","legacy_coordinator","last_connected"};
    const bool hasMount=o.contains("camera_mount_rotation");
    const bool hasTemporary=o.contains("temporary_fov_calibration");
    require(o.size()==fields.size()+int(hasMount)+int(hasTemporary), "Incomplete device profile");
    for(const auto &key:fields)require(o.contains(key), "Incomplete device profile");
    if(hasMount)fields.append("camera_mount_rotation");
    if(hasTemporary)fields.append("temporary_fov_calibration");
    for(const auto &key:o.keys())require(fields.contains(key), "Unknown device field; passwords/private keys are not permitted");
    for(const auto &key:QStringList{"id","name","host","serial","public_key","username","calibration","last_connected"})require(o[key].isString(), "Device field must be text");
    require(o["ssh_port"].isDouble()&&o["input_rotation"].isDouble()&&o["legacy_coordinator"].isBool(), "Invalid device field types");
    RvDeviceProfile p;p.id=o["id"].toString();p.name=o["name"].toString();p.host=o["host"].toString();p.sshPort=o["ssh_port"].toInt(-1);
    require(!hasTemporary||o["temporary_fov_calibration"].isBool(),"Temporary FOV calibration must be boolean");
    p.temporaryFovCalibration=o["temporary_fov_calibration"].toBool();
    p.serial=o["serial"].toString();p.publicKey=o["public_key"].toString();p.username=o["username"].toString();p.calibration=o["calibration"].toString();
    p.cameraMountRotation=hasMount?o["camera_mount_rotation"].toInt(-1):
        (p.serial=="REDACTED_DEVICE_SERIAL"&&p.publicKey==""?180:0);
    require(!hasMount||(o["camera_mount_rotation"].isDouble()&&o["camera_mount_rotation"].toDouble()==p.cameraMountRotation),"Camera mounting angle must be an integer");
    p.inputRotation=o["input_rotation"].toInt(-2);p.legacyCoordinator=o["legacy_coordinator"].toBool();p.lastConnected=o["last_connected"].toString();
    require(o["ssh_port"].toDouble()==p.sshPort&&o["input_rotation"].toDouble()==p.inputRotation,"Integer device fields required");p.validate();p.calibration=QLatin1String(RvCalibration::Id);p.temporaryFovCalibration=false;return p;
}
void RvDeviceProfile::validate() const {
    require(QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(id).hasMatch(), "Invalid device ID");
    require(!name.trimmed().isEmpty()&&name.size()<=80&&!name.contains(QChar::Null), "Device name must contain 1 to 80 characters");
    require(RvDeviceStore::validHost(host)&&sshPort>0&&sshPort<=65535, "Invalid device address or SSH port");
    require(QRegularExpression("^[0-9a-f]{16}$").match(serial).hasMatch(), "RV serial must be 16 lowercase hexadecimal characters");
    require(QRegularExpression("^[A-Za-z0-9_.-]{1,64}$").match(username).hasMatch(), "Invalid login username");
    RvDeviceStore::keyFingerprint(publicKey);
    require(calibration==QLatin1String(RvCalibration::Id)||calibration=="unconfigured"||calibration=="rv20261008-f1"||calibration=="rv20261008-f2", "Unsupported calibration profile");
    require(!temporaryFovCalibration||fov()>0,"Select F1/F2 before enabling temporary calibration");
    require(cameraMountRotation==0||cameraMountRotation==180, "Camera mounting angle must be 0 or 180 degrees");
    require(inputRotation==-1||inputRotation==0||inputRotation==180, "Inference orientation must be verified as 0 or 180 degrees");
    require(lastConnected.isEmpty()||(lastConnected.size()<=32&&QDateTime::fromString(lastConnected,Qt::ISODate).isValid()), "Invalid last connection time");
}
QString RvDeviceProfile::fingerprint() const { return RvDeviceStore::keyFingerprint(publicKey); }
QString RvDeviceProfile::credentialTarget() const { return "DexterousHand/RV1103/"+serial+"/"+fingerprint().mid(7)+"/"+username; }
int RvDeviceProfile::fov() const { return calibration==QLatin1String(RvCalibration::Id)?RvCalibration::Fov:calibration=="rv20261008-f1"?1:calibration=="rv20261008-f2"?2:0; }
QString RvDeviceProfile::geometrySha() const { return fov()==RvCalibration::Fov?QLatin1String(RvCalibration::GeometrySha):fov()==1?"83b49ace957a4f70982380c6f22a7c39236764ff7311232d03724e85273324db":fov()==2?"54802b37740d0075d8a9da95a0dcb5fde27b2f1e20249312a9f2325280c51729":QString(); }
QString RvDeviceProfile::streamUrl(bool hd) const { QUrl u;u.setScheme(hd?"rtsp":"nv12");u.setHost(host);if(hd)u.setPath("/live/0");else u.setPort(8831);return u.toString(); }
QString RvDeviceStore::keyFingerprint(const QString &key) {
    const auto raw=QByteArray::fromBase64(key.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
    require(raw.size()==51&&raw.left(19)==QByteArray::fromHex("0000000b7373682d6564323535313900000020")&&raw.toBase64()==key.toLatin1(), "A complete ssh-ed25519 public key is required");
    return "SHA256:"+QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
}
bool RvDeviceStore::validHost(const QString &host) {
    if(host.isEmpty()||host.size()>253||host!=host.trimmed()||host.contains(QRegularExpression("[\\s/@?#%]")))return false;
    QUrl u;u.setScheme("ssh");u.setHost(host);return u.isValid()&&!u.host().isEmpty()&&u.host().compare(host,Qt::CaseInsensitive)==0;
}
QString RvDeviceStore::defaultPath() {
    const auto configured=qEnvironmentVariable("DCTS_DEVICE_STORE_PATH");
    return configured.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/RV1103/devices.json":configured;
}
RvDeviceStore::RvDeviceStore(QString path):m_path(std::move(path)) { reload(); }
QVector<RvDeviceProfile> RvDeviceStore::parseImport(const QByteArray &bytes) {
    QVector<RvDeviceProfile> result;for(const auto &value:document(bytes)["devices"].toArray()) {require(value.isObject(),"Invalid device entry");result.append(RvDeviceProfile::fromJson(value.toObject()));}
    validateList(result);return result;
}
QByteArray RvDeviceStore::exportProfiles(const QVector<RvDeviceProfile> &profiles) {validateList(profiles);return QJsonDocument(payload(profiles,{})).toJson();}
QVector<RvDeviceProfile> RvDeviceStore::mergeProfiles(const QVector<RvDeviceProfile> &existing,const QVector<RvDeviceProfile> &incoming) {
    validateList(existing);validateList(incoming);auto result=existing;
    for(const auto &p:incoming) {
        bool same=false;
        for(const auto &current:existing) {
            if(current.serial==p.serial&&current.publicKey==p.publicKey)same=true;
            else require(current.id!=p.id&&current.serial!=p.serial&&current.publicKey!=p.publicKey,"Import conflicts with a saved device identity; rebind it explicitly");
        }
        if(!same)result.append(p); // Keep local aliases, endpoints and settings for an already trusted identity.
    }
    validateList(result);return result;
}
void RvDeviceStore::reload() {
    const auto bytes=read(m_path);
    if(!QFileInfo::exists(m_path)){m_profiles.clear();m_active.clear();m_disk.clear();return;}
    auto profiles=parseImport(bytes);const auto o=document(bytes);require(o["active"].isString(),"Missing active device");
    const auto active=o["active"].toString();bool found=active.isEmpty();for(const auto &p:profiles)found|=p.id==active;
    require(found,"Selected device is absent from registry");m_profiles=profiles;m_active=active;m_disk=bytes;
}
void RvDeviceStore::save(const QVector<RvDeviceProfile> &profiles,const QString &activeId) {
    validateList(profiles);bool found=activeId.isEmpty();for(const auto &p:profiles)found|=p.id==activeId;require(found,"Selected device is absent from registry");
    require(QDir().mkpath(QFileInfo(m_path).absolutePath()),"Cannot create device registry directory");
    QLockFile lock(m_path+".lock");require(lock.tryLock(0),"Device registry is busy; reload and try again");
    require(read(m_path)==m_disk,"Device registry changed in another window; reload before saving");
    const auto bytes=QJsonDocument(payload(profiles,activeId)).toJson();QSaveFile file(m_path);
    require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot atomically save device registry");
    m_disk=bytes;m_profiles=profiles;m_active=activeId;
}
RvDeviceProfile RvDeviceStore::byId(const QString &id) const {for(const auto &p:m_profiles)if(p.id==id)return p;throw std::runtime_error("Select a trusted device in Settings > Device Manager");}
RvDeviceProfile RvDeviceStore::active() const {return byId(m_active);}
RvDeviceProfile RvDeviceStore::matchKey(const QString &host,int port,const QByteArray &key) const {
    for(const auto &p:m_profiles)if(p.host.compare(host,Qt::CaseInsensitive)==0&&p.sshPort==port&&QByteArray::fromBase64(p.publicKey.toLatin1())==key)return p;
    throw std::runtime_error("Unknown or changed SSH host key; open Settings > Device Manager to verify and add the device");
}
QString RvDeviceStore::revision() const { return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(payload(m_profiles,m_active)).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()); }
