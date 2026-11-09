#pragma once
#include "RvCalibration.h"
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

// User-managed trust is separate from the immutable worker payload manifest.
// A session takes a value snapshot; editing the store cannot change an in-flight command.
struct RvDeviceProfile {
    QString id, name, host, serial, publicKey, username = QStringLiteral("root");
    QString calibration=QLatin1String(RvCalibration::Id), lastConnected;
    int sshPort = 22, inputRotation = -1, cameraMountRotation = 0;
    bool legacyCoordinator = true;
    bool temporaryFovCalibration = false;
    QJsonObject json() const;
    static RvDeviceProfile fromJson(const QJsonObject &object);
    void validate() const;
    QString fingerprint() const;
    QString credentialTarget() const;
    int fov() const;
    QString geometrySha() const;
    QString streamUrl(bool hd = false) const;
    bool ready() const { return fov() > 0 && inputRotation >= 0; }
};

class RvDeviceStore {
public:
    explicit RvDeviceStore(QString path = defaultPath());
    static QString defaultPath();
    static QString keyFingerprint(const QString &key);
    static bool validHost(const QString &host);
    static QVector<RvDeviceProfile> parseImport(const QByteArray &bytes);
    static QByteArray exportProfiles(const QVector<RvDeviceProfile> &profiles);
    static QVector<RvDeviceProfile> mergeProfiles(const QVector<RvDeviceProfile> &existing,const QVector<RvDeviceProfile> &incoming);
    void reload();
    void save(const QVector<RvDeviceProfile> &profiles, const QString &activeId);
    QVector<RvDeviceProfile> profiles() const { return m_profiles; }
    RvDeviceProfile active() const;
    RvDeviceProfile byId(const QString &id) const;
    RvDeviceProfile matchKey(const QString &host, int port, const QByteArray &key) const;
    QString activeId() const { return m_active; }
    QString path() const { return m_path; }
    QString revision() const;
private:
    QString m_path, m_active;
    QVector<RvDeviceProfile> m_profiles;
    QByteArray m_disk;
};
