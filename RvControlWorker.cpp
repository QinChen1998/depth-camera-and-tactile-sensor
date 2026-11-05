// Native RV control only. Image reception/conversion remains in RvSessionController.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <wincred.h>
#include "RvControlProtocol.h"
#include "sensors/RvCameraView.h"
#include "tools/dtof_board/Options.h"
#include "tools/litesr_board/Contract.h"
#include <libssh2.h>
#include <openssl/evp.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QHostAddress>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#include "sensors/RvDeviceStore.h"
#include "sensors/RvTunerProtocol.h"
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>
using namespace rvcontrol;
namespace {
std::unique_ptr<RvDeviceStore> deviceStore;
RvDeviceProfile device;
bool inspectOnly = false;
bool readingSerial = false;
bool factoryLogin = false;
bool pinSelectedProfile = false;
bool automaticCameraStart = false;
QByteArray observedKey;
#ifdef RV_CONTROL_TRANSPORT_TEST
// Compiled only into the isolated formal localhost test image. Its manifest
// is marked and rejected by the release collector. No live endpoint override
// exists in the production worker.
QByteArray testKnown;
int testPort = 0;
#endif
QString sshHost() {
#ifdef RV_CONTROL_TRANSPORT_TEST
    if (testPort) return "127.0.0.1";
#endif
    return deviceStore || inspectOnly || readingSerial ? device.host : QString::fromLatin1(Host);
}
int sshPort() {
#ifdef RV_CONTROL_TRANSPORT_TEST
    if (testPort) return testPort;
#endif
    return deviceStore || inspectOnly || readingSerial ? device.sshPort : 22;
}
int sshTimeout() {
#ifdef RV_CONTROL_TRANSPORT_TEST
    if (testPort) return 1000;
#endif
    return 30000;
}
void require(bool ok, const QString &message, int code = 2) { if (!ok) throw Error(message, code); }
QByteArray readFile(const QString &path, qint64 limit = 1024 * 1024) {
    QFile f(path); require(f.open(QIODevice::ReadOnly) && f.size() <= limit, "Native RV runtime file missing or oversized");
    return f.readAll();
}
QString sha(const QString &path) {
    QFile f(path); require(f.open(QIODevice::ReadOnly), "Native RV runtime dependency missing");
    QCryptographicHash h(QCryptographicHash::Sha256); require(h.addData(&f), "Cannot hash RV runtime dependency");
    return QString::fromLatin1(h.result().toHex());
}
// Source file, destination within the private board staging directory, and executable bit.
constexpr struct BootstrapAsset { const char *name; const char *destination; bool executable; } bootstrapAssets[] = {
    {"M48_geometry.bin", "depth/M48_geometry.bin", false},
    {"bounded_log.awk", "depth/bounded_log.awk", false},
    {"camera_rtsp_config.awk", "depth/camera_rtsp_config.awk", false},
    {"depth_status", "depth/depth_status", true},
    {"librknnmrt.so", "depth/librknnmrt.so", false},
    {"no_route_monitor.so", "depth/no_route_monitor.so", false},
    {"depth-control.sh", "depth/control.sh", true},
    {"service.sh", "depth/service.sh", true},
    {"ensure-spi.sh", "depth/ensure-spi.sh", true},
    {"candidate-control.sh", "candidate/control.sh", true},
    {"session.sh", "candidate/session.sh", true},
    {"native_hd_gui", "candidate/native_hd_gui", true},
    {"tof_driver", "depth/tmf8829_ext", true},
    {"S99zdepth", "S99zdepth", true},
};
void resources(const QDir &root) {
    const auto info = QJsonDocument::fromJson(readFile(root.filePath("implementation.json"))).object();
    require(info["implementation"] == Implementation && info["executable"] == "RvControlWorker.exe"
        && info["executable_sha256"].toString() == sha(QCoreApplication::applicationFilePath()), "Native RV implementation fingerprint mismatch");
    const auto dlls = info["dependencies"].toObject(), files = info["payload_files"].toObject();
    require(dlls.contains("libssh2.dll") && dlls.contains("libcrypto-3-x64.dll") && files.contains("known_hosts"), "Native SSH closure missing");
    for (const auto *name : {"model.dctsm","litesr-fov1.bin","litesr-fov2.bin","litesr_gui.sh","dtof-switch.sh","rect-model.dctsm","rect-litesr-fov1.bin","rect-litesr-fov2.bin","dtof-switch-v1.sh","native16-model.dctsm","native16-litesr-fov1.bin","native16-litesr-fov2.bin","native-fov1.bin","native-fov2.bin"})
        require(files.contains(QString("litesr_board/")+name), "Protected LiteSR payload missing from the runtime manifest");

    require(files.contains("rv1103_bootstrap/install.sh"), "RV1103 setup script missing from the runtime manifest");
    for (const auto &asset : bootstrapAssets)
        require(files.contains(QString("rv1103_bootstrap/")+asset.name), "RV1103 setup asset missing from the runtime manifest");

    for (const auto &table : {dlls, files}) for (auto it = table.begin(); it != table.end(); ++it) {
        const auto name = it.key();
        require(!name.isEmpty() && !QDir::isAbsolutePath(name) && !name.contains(':') && !name.contains('\\')
            && !name.split('/').contains("..") && !name.contains("python", Qt::CaseInsensitive), "Unsafe RV runtime manifest");
        require(it.value().toString().size() == 64 && sha(root.filePath(name)) == it.value().toString(), "RV runtime dependency fingerprint mismatch");
    }
    require(QByteArray(libssh2_version(0)) == "1.11.1", "Unsupported libssh2 version");
}
struct SshLibrary {
    SshLibrary() { require(libssh2_init(0) == 0, "libssh2 initialization failed"); }
    ~SshLibrary() { libssh2_exit(); }
};
struct Session {
    LIBSSH2_SESSION *value = libssh2_session_init();
    ~Session() { if (value) { libssh2_session_disconnect(value, "Control complete"); libssh2_session_free(value); } }
};
bool pinnedKey(LIBSSH2_SESSION *session, const QByteArray &known, const QByteArray &key, int keyType) {
    // Onboarding pins the exact key explicitly confirmed in Device Manager.
    // It can only read identity; it cannot configure or trust a device.
    if (readingSerial || pinSelectedProfile)
        return keyType == LIBSSH2_HOSTKEY_TYPE_ED25519 && key == QByteArray::fromBase64(device.publicKey.toLatin1());
    if (deviceStore) {
        if(keyType!=LIBSSH2_HOSTKEY_TYPE_ED25519)return false;
        device=deviceStore->matchKey(device.host,device.sshPort,key);
        return true;
    }
    // Pin the existing Ed25519 line as well as the key returned by the SSH
    // handshake. A replaced/extended known_hosts cannot authorize a new key.
    QByteArray expected = QByteArray(Host) + " ssh-ed25519 " + Key;
#ifdef RV_CONTROL_TRANSPORT_TEST
    if (testPort) expected = testKnown.trimmed();
#endif
    const auto expectedKey = QByteArray::fromBase64(expected.split(' ').value(2));
    if (known.trimmed() != expected || keyType != LIBSSH2_HOSTKEY_TYPE_ED25519
        || key != expectedKey) return false;
    auto *hosts = libssh2_knownhost_init(session); if (!hosts) return false;
    const auto line = expected + '\n';
    const bool loaded = libssh2_knownhost_readline(hosts, line.constData(), size_t(line.size()), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0;
    libssh2_knownhost *matched = nullptr;
    const auto host = sshHost().toLatin1();
    const bool ok = loaded && libssh2_knownhost_checkp(hosts, host.constData(), sshPort(), key.constData(), size_t(key.size()),
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | LIBSSH2_KNOWNHOST_KEY_ED25519,
        &matched) == LIBSSH2_KNOWNHOST_CHECK_MATCH;
    libssh2_knownhost_free(hosts); return ok;
}
bool ed25519() {
    // Exercise the linked crypto backend; a version string is not proof that
    // this OpenSSL build supports Ed25519.
    auto *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY *key = nullptr; bool ok = context && EVP_PKEY_keygen_init(context) == 1 && EVP_PKEY_keygen(context, &key) == 1;
    auto *sign = EVP_MD_CTX_new(), *verify = EVP_MD_CTX_new();
    unsigned char signature[128]; size_t length = sizeof(signature); const unsigned char message[] = "RV offline backend check";
    ok = ok && sign && verify && EVP_DigestSignInit(sign, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestSign(sign, signature, &length, message, sizeof(message)) == 1
        && EVP_DigestVerifyInit(verify, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(verify, signature, length, message, sizeof(message)) == 1;
    EVP_MD_CTX_free(sign); EVP_MD_CTX_free(verify); EVP_PKEY_free(key); EVP_PKEY_CTX_free(context); return ok;
}
struct Credentials {
    QByteArray username, password;
    bool persist = false;
    ~Credentials() { if (!password.isEmpty()) SecureZeroMemory(password.data(), SIZE_T(password.size())); }
    void load() {
#ifdef RV_CONTROL_TRANSPORT_TEST
        if (testPort) { username = "offline-user"; password = "offline-password"; return; }
#endif
        if (factoryLogin) {
            username = "root";
            password = "luckfox";
            persist = true;
            return;
        }
        const auto targetName=(readingSerial
            ? QStringLiteral("DexterousHand/RV1103/new-device/")+device.fingerprint().mid(7)+"/"+device.username
            : deviceStore?device.credentialTarget():QStringLiteral("DexterousHand/RV1103/172.32.0.93")).toStdWString();
        const auto target=targetName.c_str();
        PCREDENTIALW entry = nullptr;
        bool found=!readingSerial&&CredReadW(target,CRED_TYPE_GENERIC,0,&entry);
        // Migrate only the approved original board's legacy IP-based credential.
        if(!found&&deviceStore&&device.serial=="REDACTED_DEVICE_SERIAL"&&device.publicKey==Key&&device.username=="root") {
            found=CredReadW(L"DexterousHand/RV1103/172.32.0.93",CRED_TYPE_GENERIC,0,&entry);persist=found;
        }
        if (found) {
            username = QString::fromWCharArray(entry->UserName).toUtf8();
            if (entry->CredentialBlobSize % sizeof(wchar_t) == 0)
                password = QString::fromWCharArray(reinterpret_cast<const wchar_t *>(entry->CredentialBlob),
                    int(entry->CredentialBlobSize / sizeof(wchar_t))).toUtf8();
            CredFree(entry);
            require(!deviceStore||username==device.username.toUtf8(),"Stored credential username does not match device profile");
        } else {
            require(!automaticCameraStart, "No saved login for automatic camera start. Click Start to sign in.");
            wchar_t user[CREDUI_MAX_USERNAME_LENGTH + 1] = L"root", pass[CREDUI_MAX_PASSWORD_LENGTH + 1] = {};
            if(deviceStore||readingSerial)device.username.toWCharArray(user),user[device.username.size()]=0;
            CREDUI_INFOW info{}; info.cbSize = sizeof(info); info.pszCaptionText = L"RV1103 pinned SSH";
            info.pszMessageText = L"Enter the existing board credentials. The Ed25519 host key has been verified. No device reset is performed.";
            BOOL save = FALSE;
            const auto result = CredUIPromptForCredentialsW(&info, target, nullptr, 0, user, DWORD(std::size(user)), pass,
                DWORD(std::size(pass)), &save, CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_DO_NOT_PERSIST | CREDUI_FLAGS_SHOW_SAVE_CHECK_BOX);
            username = QString::fromWCharArray(user).toUtf8(); password = QString::fromWCharArray(pass).toUtf8();
            SecureZeroMemory(pass, sizeof(pass)); SecureZeroMemory(user, sizeof(user));
            require(result == NO_ERROR, "RV authentication cancelled or credentials unavailable");
            require(!(deviceStore||readingSerial)||username==device.username.toUtf8(),"Login username differs; edit it in Device Manager first");
            persist = save;
        }
        require(!username.isEmpty() && !password.isEmpty() && username.size() <= 256 && password.size() <= 2560,
            "Missing or invalid Windows RV credential");
    }
    void authenticated() {
        if (!persist) return;
        const auto name = QString::fromUtf8(username).toStdWString();
        auto secret = QString::fromUtf8(password).toStdWString();
        CREDENTIALW record{}; record.Type = CRED_TYPE_GENERIC;
        const auto target=(deviceStore||readingSerial?device.credentialTarget():QStringLiteral("DexterousHand/RV1103/172.32.0.93")).toStdWString();
        record.TargetName = const_cast<LPWSTR>(target.c_str());
        record.UserName = const_cast<LPWSTR>(name.c_str()); record.Persist = CRED_PERSIST_LOCAL_MACHINE;
        record.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data()); record.CredentialBlobSize = DWORD(secret.size() * sizeof(wchar_t));
        const bool written = CredWriteW(&record, 0); SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
        require(written, "Cannot save Windows RV credential");
    }
};
class Ssh {
public:
    Ssh(const QByteArray &known) {
        require(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1"||sshHost()=="localhost"||QHostAddress(sshHost()).isLoopback(),
                "Offline test forbids external device connections");
        require(m_session.value != nullptr, "Cannot allocate SSH session");
        // The pinned LAN device uses the same direct route as HTTP and NV12.
        m_socket.setProxy(QNetworkProxy::NoProxy);
        m_clock.start(); m_socket.connectToHost(sshHost(), quint16(sshPort()));
        require(m_socket.waitForConnected(std::min(10000, sshTimeout())),
            QStringLiteral("RV SSH network connection failed: %1").arg(m_socket.errorString()), 4);
        m_descriptor = libssh2_socket_t(m_socket.socketDescriptor());
        libssh2_session_set_blocking(m_session.value, 0);
        require(libssh2_session_method_pref(m_session.value, LIBSSH2_METHOD_HOSTKEY, "ssh-ed25519") == 0,
            "Ed25519 host key negotiation unavailable");
        operation([&] { return libssh2_session_handshake(m_session.value, m_descriptor); }, "RV SSH handshake failed", 4);
        size_t size = 0; int type = 0; const char *key = libssh2_session_hostkey(m_session.value, &size, &type);
        if(inspectOnly) {
            require(key&&type==LIBSSH2_HOSTKEY_TYPE_ED25519,"Device does not provide an Ed25519 host key");
            observedKey=QByteArray(key,qsizetype(size));return; // No credentials or remote commands before trust.
        }
        require(key && pinnedKey(m_session.value, known, QByteArray(key, qsizetype(size)), type), "Pinned SSH host key rejected");
        auto credentials = std::make_unique<Credentials>(); credentials->load();
        // Credential dialogs are user time; authentication gets its own bound.
        m_clock.restart();
        operation([&] { return libssh2_userauth_password_ex(m_session.value, credentials->username.constData(),
            unsigned(credentials->username.size()), credentials->password.constData(), unsigned(credentials->password.size()), nullptr); },
            "RV SSH authentication failed");
        if(readingSerial)m_serialCredentials=std::move(credentials);
        else credentials->authenticated();
    }
    void rememberSerialLogin(const QString &serial) {
        require(readingSerial&&bool(m_serialCredentials)&&QRegularExpression("^[0-9a-f]{16}$").match(serial).hasMatch(),"Invalid onboarding identity");
        device.serial=serial;
        m_serialCredentials->authenticated();m_serialCredentials.reset();
    }
    QByteArray execute(const QString &command, int timeoutMs=0, const QByteArray &input={}) {
        m_timeout=timeoutMs>0?timeoutMs:sshTimeout();
        m_clock.restart(); LIBSSH2_CHANNEL *channel = nullptr;
        while (!(channel = libssh2_channel_open_session(m_session.value))) {
            require(libssh2_session_last_errno(m_session.value) == LIBSSH2_ERROR_EAGAIN, "RV SSH channel failed", 4); wait();
        }
        const auto cleanup = std::unique_ptr<LIBSSH2_CHANNEL, decltype(&libssh2_channel_free)>(channel, libssh2_channel_free);
        const auto bytes = command.toUtf8();
        operation([&] { return libssh2_channel_process_startup(channel, "exec", 4, bytes.constData(), unsigned(bytes.size())); }, "Board configure command failed", 4);
        for (qsizetype offset=0; offset<input.size();) {
            const auto sent=libssh2_channel_write(channel,input.constData()+offset,size_t(input.size()-offset));
            if (sent==LIBSSH2_ERROR_EAGAIN) { wait(); continue; }
            require(sent>0 && m_clock.elapsed()<m_timeout,"RV SSH upload failed or timed out",4);
            offset+=sent;
        }
        if (!input.isEmpty())
            operation([&] { return libssh2_channel_send_eof(channel); },"RV SSH upload completion failed",4);
        QByteArray output; qint64 errors = 0; char buffer[4096];
        while (true) {
            bool received = false;
            for (int stream = 0; stream < 2; ++stream) {
                const auto count = libssh2_channel_read_ex(channel, stream, buffer, sizeof(buffer));
                if (count > 0) {
                    received = true; if (!stream) output.append(buffer, qsizetype(count)); else errors += count;
                    require(output.size() <= 65536 && errors <= 65536, "Oversized board control response");
                } else require(count == 0 || count == LIBSSH2_ERROR_EAGAIN, "RV SSH response read failed", 4);
            }
            if (!received && libssh2_channel_eof(channel)) break;
            if (!received) wait();
            require(m_clock.elapsed() < m_timeout, "RV SSH command timed out", 4);
        }
        operation([&] { return libssh2_channel_close(channel); }, "RV SSH channel completion timed out", 4);
        const int code = libssh2_channel_get_exit_status(channel);
        // Never echo arbitrary remote stderr or authentication material.
        if(code!=0 && output.contains("DTOF_ERROR:")) {
            const auto diagnostic=output.mid(output.lastIndexOf("DTOF_ERROR:")+11).trimmed().left(512);
            throw Error(QString::fromUtf8(diagnostic));
        }
        require(code == 0, QStringLiteral("Board configure rejected (exit %1); lifecycle locks preserved").arg(code));
        return output;
    }
private:
    std::unique_ptr<Credentials> m_serialCredentials;
    void wait() {
        require(m_clock.elapsed() < m_timeout, "RV SSH operation timed out", 4);
        WSAPOLLFD fd{}; fd.fd = SOCKET(m_descriptor);
        const int direction = libssh2_session_block_directions(m_session.value);
        fd.events = (direction & LIBSSH2_SESSION_BLOCK_INBOUND ? POLLRDNORM : 0)
            | (direction & LIBSSH2_SESSION_BLOCK_OUTBOUND ? POLLWRNORM : 0);
        if (!fd.events) fd.events = POLLRDNORM;
        require(WSAPoll(&fd, 1, 100) >= 0 && !(fd.revents & (POLLERR | POLLHUP | POLLNVAL)), "RV SSH network disconnected", 4);
    }
    void operation(const std::function<int()> &call, const QString &message, int code = 2) {
        int result; while ((result = call()) == LIBSSH2_ERROR_EAGAIN) wait();
        require(result == 0, message, code);
    }
    QTcpSocket m_socket;
    Session m_session;
    QElapsedTimer m_clock;
    int m_timeout=sshTimeout();
    libssh2_socket_t m_descriptor = LIBSSH2_INVALID_SOCKET;
};
QJsonObject status(QNetworkAccessManager &network) {
    require(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1"||sshHost()=="localhost"||QHostAddress(sshHost()).isLoopback(),
            "Offline test forbids external device connections");
    QUrl endpoint;endpoint.setScheme("http");endpoint.setHost(sshHost());endpoint.setPort(8837);endpoint.setPath("/status");
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(3000);
    auto *reply = network.get(request); QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { reply->abort(); loop.quit(); });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] { if (reply->bytesAvailable() > 65536) reply->abort(); });
    timeout.start(3000); loop.exec();
    const bool ok = reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && reply->bytesAvailable() <= 65536;
    const auto bytes = reply->readAll(); delete reply;
    require(ok, "RV status HTTP failed or timed out", 4);
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "Invalid RV status JSON"); return doc.object();
}
// The previously approved old RV has the original coordinator. Its ordinary
// legacy/candidate operations preserve fault and ownership locks. Recovery
// arguments must not invoke an unsupported action or simulate a fault reset.
bool legacyCoordinatorProfile() {
    return deviceStore?device.legacyCoordinator:true;
}
Configuration coordinatorConfiguration(Configuration c,bool legacy) {
    if(legacy) {
        require(!c.recoverStreamFault,"Old RV has no automatic fault recovery; fault and ownership locks are preserved");
        c.recoverFault=false; // Explicit start uses the existing guarded configure.
    }
    return c;
}
QString coordinatorCandidateAction(const QString &flag,bool legacy) {
    if(legacy) {
        require(flag!="--recover-stream-fault","Old RV has no automatic fault recovery; fault and ownership locks are preserved");
        return "candidate";
    }
    return flag.isEmpty()?"candidate":flag=="--recover-fault"?"recover-candidate":"auto-candidate";
}
QByteArray withAutomaticTof(QByteArray session);
QJsonObject selfTest(const QByteArray &known) {
    QJsonArray checks;
    const auto check = [&](const char *name, bool ok) { checks.append(QJsonObject{{"name", name}, {"passed", ok}}); require(ok, QString::fromLatin1(name)); };
    Session session; auto key = QByteArray::fromBase64(Key);
    check("ed25519_crypto_sign_verify", ed25519());
    check("existing_pinned_key_accepted", pinnedKey(session.value, known, key, LIBSSH2_HOSTKEY_TYPE_ED25519));
    key[20] ^= 1; check("changed_host_key_rejected", !pinnedKey(session.value, known, key, LIBSSH2_HOSTKEY_TYPE_ED25519));
    check("unknown_or_added_host_rejected", !pinnedKey(session.value, known + "\nunknown ssh-ed25519 " + Key, QByteArray::fromBase64(Key), LIBSSH2_HOSTKEY_TYPE_ED25519));
    check("host_key_algorithm_downgrade_rejected", !pinnedKey(session.value, known, QByteArray::fromBase64(Key), LIBSSH2_HOSTKEY_TYPE_RSA));
    auto rejects = [](const std::function<void()> &call, int code = 2) { try { call(); return false; } catch (const Error &e) { return e.code == code; } };
    Configuration c;
    auto oldStart=c;oldStart.recoverFault=true;
    check("old_profile_manual_start_uses_guarded_ordinary_control",!coordinatorConfiguration(oldStart,true).recoverFault
        &&command(coordinatorConfiguration(oldStart,true)).contains("control.sh legacy ")
        &&!command(coordinatorConfiguration(oldStart,true)).contains("recover-legacy"));
    auto oldAuto=c;oldAuto.recoverStreamFault=true;
    check("old_profile_automatic_recovery_fails_closed",rejects([&]{coordinatorConfiguration(oldAuto,true);}));
    check("old_profile_candidate_manual_uses_guarded_candidate",coordinatorCandidateAction("--recover-fault",true)=="candidate");
    check("old_profile_candidate_auto_recovery_fails_closed",rejects([&]{coordinatorCandidateAction("--recover-stream-fault",true);}));
    check("new_profile_retains_manual_and_automatic_recovery",coordinatorConfiguration(oldStart,false).recoverFault
        &&coordinatorConfiguration(oldAuto,false).recoverStreamFault&&coordinatorCandidateAction("--recover-fault",false)=="recover-candidate");
    check("configuration_validated_before_io", rejects([&] { auto bad = c; bad.method = "none; reboot"; command(bad); }));
    check("ordinary_configure_never_recovers_fault", !command(c).contains("recover-legacy"));
    auto recovering = c; recovering.recoverFault = true;
    check("manual_recovery_uses_installed_coordinator_and_fails_closed_without_it",
          command(recovering).contains("control.sh recover-legacy 576 144 none none Native")
          && command(recovering).endsWith("else exit 2; fi"));
    recovering.recoverFault = false; recovering.recoverStreamFault = true;
    check("automatic_recovery_requires_stream_fault_classifier", command(recovering).contains("control.sh auto-legacy ") && command(recovering).endsWith("else exit 2; fi"));
    recovering.recoverFault = true;
    check("conflicting_recovery_modes_rejected_before_io", rejects([&]{ command(recovering); }));
    const QString boot = "11111111-2222-3333-4444-555555555555";
    const auto output = boot.toUtf8() + "\n{\"state\":\"running\",\"model\":\"Native\",\"generation\":\"9\"}";
    auto r = receipt(output, c); check("boot_and_service_receipt", r.generation == 9 && r.boot == boot);
    check("malformed_receipt_rejected", rejects([&] { receipt("not a boot\n{}", c); }));
    check("wrong_model_receipt_rejected", rejects([&] { auto f = c; f.model = "F_LNCA"; receipt(output, f); }));
    const auto startingOutput = boot.toUtf8() + "\n{\"state\":\"starting\",\"model\":\"Native\",\"generation\":\"9\"}";
    const auto startingReceipt = receipt(startingOutput, c);
    check("starting_receipt_binds_same_identity", startingReceipt.boot == r.boot && startingReceipt.model == r.model && startingReceipt.generation == r.generation);
    check("starting_receipt_wrong_model_rejected", rejects([&] { auto f = c; f.model = "F_constant"; receipt(startingOutput, f); }));
    check("fault_receipt_rejected", rejects([&] { receipt(boot.toUtf8() + "\n{\"state\":\"fault\",\"model\":\"Native\",\"generation\":\"9\"}", c); }));
    QJsonObject s{{"state", "running"}, {"model", "Native"}, {"generation", "9"}};
    QJsonObject t{{"model", "Native"}, {"generation", 9}, {"frame_id", 4}, {"updated_monotonic_ns", qint64(1000000000)},
        {"rgb_width", 576}, {"pixel_format", "NV12"}, {"work_mode", "rgb_tof"}, {"depth_present", false}, {"capture_only", true}, {"inference_location", "none"}};
    QJsonObject current{{"boot_id", boot}, {"service", s}, {"telemetry", t}, {"now_monotonic_ns", qint64(1000000001)}};
    Readiness ready(r, c); check("first_frame_is_only_a_floor", !ready.observe(current));
    Readiness startingReady(startingReceipt, c);
    auto startingStatus = current;
    auto startingService = s; startingService["state"] = "starting";
    startingStatus["service"] = startingService;
    check("starting_service_never_ready_even_with_frame", !startingReady.observe(startingStatus));
    check("first_running_frame_after_start_is_only_floor", !startingReady.observe(current));
    check("stalled_frame_not_ready", !ready.observe(current)); t["frame_id"] = 5; current["telemetry"] = t;
    check("fresh_advancing_frame_ready", ready.observe(current));
    check("starting_receipt_waits_for_fresh_advancing_running_frames", startingReady.observe(current));
    auto stale = current; stale["now_monotonic_ns"] = qint64(4000000000);
    check("three_second_boundary_rejected", !ready.observe(stale));
    auto changed = current; changed["boot_id"] = "new-boot";
    check("new_board_boot_requires_reconfigure", rejects([&] { ready.observe(changed); }, 4));
    changed = current; s["generation"] = "10"; changed["service"] = s;
    check("new_generation_requires_reconfigure", rejects([&] { ready.observe(changed); }, 4));
    changed = current; t["requested_method"] = "wrong"; changed["telemetry"] = t;
    check("wrong_complete_configuration_rejected", rejects([&] { ready.observe(changed); }));
    const QByteArray sessionScript="CANONICAL_RUN_ID=\"dtof_$2\" CANONICAL_WARNING_POLICY=observe-0x08 "
        "launch_tof \"$1\" ./tmf8829_ext -m -s 100 -g -d 11 -p 66 -t 0 -o 1 -r 6 -i 600 > \"$sensor_output\" 2>&1 &\n";
    const auto patched=withAutomaticTof(sessionScript);
    check("automatic_tof_preserves_saved_settings_and_is_idempotent",
        patched!=sessionScript && RvTuner::scriptSettings(patched,true)==RvTuner::scriptSettings(sessionScript,true)
        && withAutomaticTof(patched)==patched);
    bool ambiguousRejected=false;
    try { withAutomaticTof(sessionScript+sessionScript); } catch (const std::exception &) { ambiguousRejected=true; }
    check("automatic_tof_rejects_ambiguous_board_scripts",ambiguousRejected);
    return {{"ok", true}, {"checks", checks}, {"board_connected", false}, {"python_loaded", false}};
}
QString readDeviceSerial(Ssh &connection) {
    const QRegularExpression valid("^[0-9a-f]{16}$");
    // Both attempts are fixed, bounded, read-only commands. Keep verification
    // and onboarding on the same identity source; never substitute a MAC/UUID.
    try {
        const auto serial=QString::fromLatin1(connection.execute(QStringLiteral("sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo"))).trimmed().toLower();
        if(valid.match(serial).hasMatch())return serial;
    }catch(const Error &){}
    const auto cpuInfo=QString::fromLatin1(connection.execute(QStringLiteral("cat /proc/cpuinfo")));
    const QRegularExpression field("^Serial[\\t ]*:[\\t ]*([0-9a-fA-F]{16})[\\t ]*\\r?$",QRegularExpression::MultilineOption);
    auto matches=field.globalMatch(cpuInfo);
    require(matches.hasNext(),"Cannot read a 16-character CPU Serial from /proc/cpuinfo");
    const auto serial=matches.next().captured(1).toLower();
    require(!matches.hasNext(),"Device returned more than one CPU Serial");
    return serial;
}
void verifyDevice(Ssh &connection) {
    if(!deviceStore && !factoryLogin)return;
    require(device.ready(),"Device calibration or inference orientation is unconfirmed; open Device Manager");
    // Fixed read-only commands. Profile strings never become shell commands.
    const auto serial=readDeviceSerial(connection);
    require(serial==device.serial,"Authenticated RV serial differs from the trusted device profile");
    const auto geometry=connection.execute(QStringLiteral("sha256sum /root/r144_depth/M48_geometry.bin")).simplified().split(' ').value(0);
    require(geometry==device.geometrySha().toLatin1(),"Device GEO2 calibration differs from its profile; capture was not configured");
}
QJsonObject withDevice(QJsonObject result) {
    if(deviceStore) { result["trusted_device"]=device.json();result["device_registry_revision"]=deviceStore->revision();result["expected_fov"]=device.fov(); }
    return result;
}
#include "tools/dtof_tuner/Worker.inc"
// This hook lives in the existing candidate session so the coordinator still
// owns the same manager and producer PIDs, including on the legacy firmware.
QByteArray boardPayload(const QString &name)
{
    return readFile(QDir(QCoreApplication::applicationDirPath()).filePath("litesr_board/"+name), 4*1024*1024);
}
const QByteArray &automaticTofHook()
{
    static const auto hook=boardPayload("dtof-switch.sh");
    return hook;
}
const QByteArray DctsTofCall = R"SH(dcts_tof "${DCTS_DTOF_PRESET:-}" "$1" )SH";
const QString CandidateSession = "/root/dtof_candidates_20261009/session.sh";

QByteArray withAutomaticTof(QByteArray session)
{
    const auto &hook=automaticTofHook();
    // Recognize the exact previous hook, preserving saved producer arguments.
    const auto legacy=boardPayload("dtof-switch-v1.sh");
    auto previous=legacy;previous.replace("48:13|48:16)","48:13)");
    if(session.contains(previous))session.replace(previous,hook);
    if(session.contains(legacy))session.replace(legacy,hook);
    session.replace("13|14)","13|14|16|17)");
    session.replace("13|14|16)","13|14|16|17)");
    session.replace("[ \"$1\" = 13 ] || [ \"$1\" = 14 ];","[ \"$1\" = 13 ] || [ \"$1\" = 14 ] || [ \"$1\" = 16 ];");
    session.replace("[ \"$mode\" = 15 ];","[ \"$mode\" = 15 ] || [ \"$mode\" = 16 ];");
    session.replace("[ \"$mode\" = 16 ];","[ \"$mode\" = 16 ] || [ \"$mode\" = 17 ];");
    const auto existing=session.indexOf(hook);
    const auto call=session.indexOf(DctsTofCall);
    if(existing>=0 || call>=0) {
        require(existing>=0 && call>=existing+hook.size(),"Incomplete automatic dToF hook; review the board session script");
        auto original=session;
        original.remove(call,DctsTofCall.size());
        original.remove(existing,hook.size());
        require(!original.contains("dcts_tof") && withAutomaticTof(original)==session,
                "Automatic dToF hook differs from the supported version");
        return session;
    }
    require(!session.contains("dcts_tof"),"Unknown automatic dToF hook");
    RvTuner::scriptSettings(session,true);
    const QRegularExpression producer(R"RE(^(CANONICAL_RUN_ID="dtof_\$2" CANONICAL_WARNING_POLICY=observe-0x08 )(?=(?:launch_tof "\$1" )?\./tmf8829_ext -m\b))RE",
                                      QRegularExpression::MultilineOption);
    auto matches=producer.globalMatch(QString::fromLatin1(session));
    require(matches.hasNext(),"Unsupported board candidate session format");
    const auto line=matches.next();
    require(!matches.hasNext(),"Ambiguous board candidate producer");
    session.insert(line.capturedEnd(),DctsTofCall);
    session.insert(line.capturedStart(),hook);
    return session;
}

QString automaticTofState(Ssh &connection)
{
    try {
        const auto session=tunerRead(connection,CandidateSession);
        return withAutomaticTof(session)==session ? "ready" : "missing";
    } catch(const std::exception &) {
        return "unsupported";
    }
}

void installAutomaticTof(Ssh &connection)
{
    const auto before=tunerRead(connection,CandidateSession);
    const auto after=withAutomaticTof(before);
    if(before==after)return;
    const auto command=tunerPrelude()+QStringLiteral(R"SH(
locks
quiescent
receivers_gone
session="$candidate/session.sh"
[ -f "$session" ] && [ ! -L "$session" ] || fail 'Invalid board session file'
digest(){ sha256sum "$1" | cut -d' ' -f1; }
[ "$(digest "$session")" = '%1' ] || fail 'Board session changed before import'
backup="$session.dcts-before-%1"
umask 077
(
stage=$(mktemp -d "$candidate/.dcts-session.XXXXXX")
trap 'rm -f "$stage/session.sh"; rmdir "$stage"' EXIT
cp -p "$session" "$stage/session.sh"
cat > "$stage/session.sh"
sh -n "$stage/session.sh"
[ "$(digest "$stage/session.sh")" = '%2' ] || fail 'Uploaded board session checksum mismatch'
if [ ! -e "$backup" ]; then ln "$session" "$backup"; fi
[ -f "$backup" ] && [ ! -L "$backup" ] && [ "$(digest "$backup")" = '%1' ] || fail 'Board session backup differs'
mv -f "$stage/session.sh" "$session"
sync
)
echo ready
)SH").arg(tunerHash(before),tunerHash(after));
    require(connection.execute(command,30000,after).trimmed()=="ready"
            && tunerRead(connection,CandidateSession)==after,"Automatic dToF installation was not verified");
}

#include "tools/litesr_board/protected/Worker.inc"
#include "tools/rv_control/Bootstrap.inc"

int run(const QStringList &input) {
    auto args=input;
    QDir root(QCoreApplication::applicationDirPath()); resources(root); SshLibrary ssh;
    const auto known = readFile(root.filePath("known_hosts"), 4096);
    const int registryIndex=args.indexOf("--device-store");
    if(registryIndex>=0) {
        require(registryIndex+5<args.size()&&args[registryIndex+2]=="--device-revision"&&args[registryIndex+4]=="--device-id", "Incomplete device registry arguments");
        deviceStore=std::make_unique<RvDeviceStore>(args[registryIndex+1]);
        require(deviceStore->revision()==args[registryIndex+3],"Device registry changed before connection; retry from Device Manager");
        device=deviceStore->byId(args[registryIndex+5]);for(int i=0;i<6;++i)args.removeAt(registryIndex);
    }
    if(args.size()==4&&args[1]=="--inspect-host") {
        require(!deviceStore,"Inspection must not use saved credentials");
        bool ok=false;device.host=args[2];device.sshPort=args[3].toInt(&ok);
        require(ok&&device.sshPort>0&&device.sshPort<=65535&&RvDeviceStore::validHost(device.host),"Invalid inspection address");
        inspectOnly=true;Ssh connection(known);
        const auto key=QString::fromLatin1(observedKey.toBase64());
        const auto bytes=QJsonDocument(QJsonObject{{"public_key",key},{"fingerprint",RvDeviceStore::keyFingerprint(key)},{"authenticated",false}}).toJson(QJsonDocument::Compact);
        std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);return 0;
    }
    if(args.size()==5&&args[1]=="--verify-usb-enrollment") {
        require(!deviceStore&&qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1",
            "USB enrollment is unavailable in offline mode or with an existing registry");
        device=RvDeviceProfile::fromJson(QJsonDocument::fromJson(args[2].toUtf8()).object());
        require(device.id==device.serial&&device.host=="172.32.0.93"&&device.sshPort==22
            &&device.username=="root"&&device.ready()&&device.legacyCoordinator,"Invalid USB enrollment profile");
        factoryLogin=true;
        readingSerial=true;
        const auto boot=args[3];
        require(QRegularExpression("^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$").match(boot).hasMatch()
            && (args[4]=="v120" || args[4]=="installed"), "Invalid USB setup request");
        try {
            Ssh connection(known);
            require(readDeviceSerial(connection)==device.serial
                && connection.execute("cat /proc/sys/kernel/random/boot_id").trimmed()==boot.toLatin1(),
                "USB and SSH identify different board sessions");
            if(args[4]=="v120") installV120Board(connection,root,boot);
            verifyDevice(connection);
            auto modelState=nearModelState(connection);
            auto tofState=automaticTofState(connection);
            if ((modelState=="ready" || modelState=="missing") && (tofState=="ready" || tofState=="missing")
                && (modelState!="ready" || tofState!="ready")) {
                installProtectedLiteSr(connection);
                modelState=nearModelState(connection);
                tofState=automaticTofState(connection);
            }
            if(args[4]=="v120") require(modelState=="ready" && tofState=="ready", "New board setup did not pass verification");
            connection.rememberSerialLogin(device.serial);
            const auto receipt=QJsonDocument(QJsonObject{{"verified",true},{"litesr_model",modelState},{"automatic_tof",tofState}}).toJson(QJsonDocument::Compact);
            std::fwrite(receipt.constData(),1,size_t(receipt.size()),stdout);
            return 0;
        } catch (const std::exception &error) {
            const auto receipt=QJsonDocument(QJsonObject{{"setup_error",QString::fromUtf8(error.what())}}).toJson(QJsonDocument::Compact);
            std::fwrite(receipt.constData(),1,size_t(receipt.size()),stdout);
            return 2;
        }
    }
    if(args.size()==6&&args[1]=="--read-device-serial") {
        require(!deviceStore,"Serial discovery must not modify a saved device");
        bool ok=false;device.host=args[2];device.sshPort=args[3].toInt(&ok);
        device.username=args[4];device.publicKey=args[5];
        require(ok&&device.sshPort>0&&device.sshPort<=65535&&RvDeviceStore::validHost(device.host),"Invalid inspection address");
        require(QRegularExpression("^[A-Za-z0-9_.-]{1,64}$").match(device.username).hasMatch(),"Invalid login username");
        RvDeviceStore::keyFingerprint(device.publicKey);
        readingSerial=true;
#ifdef RV_CONTROL_TRANSPORT_TEST
        require(device.host=="127.0.0.1","Localhost-only serial fixture required");testPort=device.sshPort;
#endif
        Ssh connection(known);const auto serial=readDeviceSerial(connection);connection.rememberSerialLogin(serial);
        const auto bytes=QJsonDocument(QJsonObject{{"serial",serial},{"host",device.host},{"ssh_port",device.sshPort},
            {"username",device.username},{"public_key",device.publicKey},{"authenticated",true}}).toJson(QJsonDocument::Compact);
        std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);return 0;
    }
    if(args.size()==2&&args[1]=="--forget-device-credentials") {
        require(bool(deviceStore),"A selected device is required");
        const auto target=device.credentialTarget().toStdWString();
        require(CredDeleteW(target.c_str(),CRED_TYPE_GENERIC,0)||GetLastError()==ERROR_NOT_FOUND,"Cannot remove saved device credential");
        if(device.serial=="REDACTED_DEVICE_SERIAL"&&device.publicKey==Key&&device.username=="root")
            require(CredDeleteW(L"DexterousHand/RV1103/172.32.0.93",CRED_TYPE_GENERIC,0)||GetLastError()==ERROR_NOT_FOUND,"Cannot remove migrated credential");
        std::fputs("{\"removed\":true}",stdout);return 0;
    }
    if(args.size()==2&&args[1]=="--verify-device") {
        require(bool(deviceStore),"A selected device is required");pinSelectedProfile=true;
        Ssh connection(known);verifyDevice(connection);
        const auto bytes=QJsonDocument(withDevice({{"verified",true},{"litesr_model",nearModelState(connection)},{"automatic_tof",automaticTofState(connection)}})).toJson(QJsonDocument::Compact);std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);return 0;
    }
    if(args.size()==2&&args[1]=="--import-litesr-model") {
        require(bool(deviceStore)&&qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1",
            "LiteSR import requires a selected device and online mode");
        pinSelectedProfile=true;
        Ssh connection(known);verifyDevice(connection);
        installProtectedLiteSr(connection);
        const auto bytes=QJsonDocument(withDevice({{"litesr_model","ready"},{"automatic_tof","ready"}})).toJson(QJsonDocument::Compact);
        std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);return 0;
    }
#ifdef RV_CONTROL_TRANSPORT_TEST
    if(args.size()==3&&args[1]=="--tuner-save-fixture") {
        const auto request=QJsonDocument::fromJson(readFile(args[2])).object();
        const auto settings=RvTuner::Settings::fromJson(request["settings"].toObject());
        const auto script=tunerSaveCommand(request,settings,QByteArray::fromBase64(request["native"].toString().toLatin1()),QByteArray::fromBase64(request["litesr"].toString().toLatin1()),request["id"].toString()).toUtf8();
        std::fwrite(script.constData(),1,size_t(script.size()),stdout);return 0;
    }
    if(args.size()==4&&args[1]=="--ssh-device-test") {
        bool ok=false;testPort=args[2].toInt(&ok);require(ok&&testPort>0&&testPort<=65535,"Invalid localhost test port");
        deviceStore=std::make_unique<RvDeviceStore>(args[3]);device=deviceStore->active();
        require(device.host=="127.0.0.1"&&device.sshPort==testPort,"Localhost-only device fixture required");
        Ssh test(known);verifyDevice(test);require(test.execute("offline-test")=="offline-ok\n","Localhost device response mismatch");return 0;
    }
    if (args.size() == 4 && (args[1] == "--ssh-test" || args[1] == "--ssh-test-with-blocked-proxy")) {
        bool ok = false; testPort = args[2].toInt(&ok); require(ok && testPort > 0 && testPort <= 65535, "Invalid localhost test port");
        testKnown = readFile(args[3], 4096); require(testKnown.startsWith("127.0.0.1 ssh-ed25519 "), "Localhost-only SSH test profile required");
        if (args[1] == "--ssh-test-with-blocked-proxy")
            QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpCachingProxy, "127.0.0.1", 1));
        Ssh test(testKnown); const auto output = test.execute("offline-test");
        require(output == "offline-ok\n", "Localhost SSH response mismatch"); return 0;
    }
#endif
    if (args.size() == 2 && (args[1] == "--probe" || args[1] == "--self-test")) {
        const auto result = selfTest(known); auto report = result;
        report["implementation"] = Implementation; report["libssh2"] = libssh2_version(0); report["crypto_backend"] = "OpenSSL/Ed25519";
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Compact); std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); return 0;
    }
    if(args.size()==3&&args[1]=="--tuner") {
        require(bool(deviceStore),"dToF tuning requires a trusted device profile");
        QFile inputFile;require(inputFile.open(stdin,QIODevice::ReadOnly),"Cannot read tuner request");
        const auto bytes=inputFile.read(65537);require(bytes.size()<=65536,"Oversized tuner request");
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);require(error.error==QJsonParseError::NoError&&doc.isObject(),"Invalid tuner request");
        // Validate settings/capacity before even authenticating, let alone STOP.
        if(args[2]=="start"||args[2]=="switch"||args[2]=="persist")RvTuner::Settings::fromJson(doc.object()["settings"].toObject());
        Ssh connection(known);verifyDevice(connection);
        const auto result=QJsonDocument(withDevice(tunerOperation(connection,args[2],doc.object()))).toJson(QJsonDocument::Compact);
        std::fwrite(result.constData(),1,size_t(result.size()),stdout);return 0;
    }
    if(args.size()>=2&&args[1]=="--dtof") {
        require(args.size()>=5,"Incomplete candidate request");
        QString hostModel,recovery;
        bool automaticStart=false;
        for(int i=5;i<args.size();++i){
            if(args[i]=="--host-model"){
                require(hostModel.isEmpty()&&i+1<args.size(),"Invalid host model selection");hostModel=args[++i];
                require(RvCameraView::isUniDepthNative16(hostModel),"Unsupported host model");
            }
            else if(args[i]=="--automatic-start"){
                require(!automaticStart,"Duplicate automatic start option");
                automaticStart=true;
            }
            else {require(recovery.isEmpty()&&(args[i]=="--recover-fault"||args[i]=="--recover-stream-fault"),"Unknown candidate option");recovery=args[i];}
        }
        bool ok=false;const int mode=args[2].toInt(&ok);const auto token=args[3],csv=args[4];
        require(ok&&(mode==LiteSrBoardContract::Native16Depth||mode==LiteSrBoardContract::NearFullHd||mode==LiteSrBoardContract::RectFullHd||mode==LiteSrBoardContract::NativeHd)&&token.size()==32&&token.toLatin1().toHex().size()==64,"Invalid candidate selection");
        for(auto ch:token)require(QStringLiteral("0123456789abcdef").contains(ch),"Invalid candidate generation");
        if(LiteSrBoardContract::supports(mode))LiteSrBoardContract::validate(mode,token.toStdString(),csv.toStdString());
        const bool captureOnly=LiteSrBoardContract::isCaptureOnly(mode);
        require(captureOnly||hostModel.isEmpty(),"Host model requires capture-only mode");
        if(captureOnly&&hostModel.isEmpty())hostModel="unidepth_wide_576_fp16_tof16";
        require(!automaticStart||(bool(deviceStore)&&recovery.isEmpty()&&qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1"),
                "Automatic start requires an online trusted device");
        if(automaticStart){pinSelectedProfile=true;automaticCameraStart=true;}
        Ssh connection(known);
        verifyDevice(connection);
        const auto modelState=automaticStart||recovery=="--recover-fault"||LiteSrBoardContract::isNear(mode)
            ? nearModelState(connection) : QStringLiteral("ready");
        const auto tofState=automaticTofState(connection);
        if(modelState!="ready"||tofState!="ready"){
            const auto response=QJsonDocument(withDevice({{"state","board_files_unavailable"},{"generation",token},
                {"litesr_model",modelState},{"automatic_tof",tofState}})).toJson(QJsonDocument::Compact);
            std::fwrite(response.constData(),1,size_t(response.size()),stdout);
            return 0;
        }
        const auto session=tunerRead(connection,CandidateSession);
        require(withAutomaticTof(session)==session,
                "Automatic dToF support changed before capture could start.");
        const auto saved=RvTuner::scriptSettings(session,true);
        auto effective=saved;
        effective.mode=(captureOnly||LiteSrBoardContract::isNative16Depth(mode))?5:11;
        if(LiteSrBoardContract::isNative16Depth(mode))effective.periodMs=33;
        effective.objects=1;
        effective.signal=!captureOnly;
        effective.noise=false;
        require(LiteSrBoardContract::isNative16Depth(mode)?(effective.mode==5&&effective.signal&&effective.objects==1&&effective.periodMs==33):(captureOnly?effective.fp16Native16Compatible():effective.liteSrCompatible()),
                "Automatic dToF preset is incompatible with the requested imaging mode");
        if(captureOnly){
            QJsonParseError parse;const auto capabilities=QJsonDocument::fromJson(connection.execute("LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib /root/dtof_candidates_20261009/native_hd_gui --native-capabilities"),&parse).object();
            require(parse.error==QJsonParseError::NoError&&capabilities["native_capture_protocol"]==LiteSrBoardContract::NativeCaptureProtocolV2
                &&capabilities["m16"]==true&&capabilities["model_inference"]==false&&capabilities["photon_enabled"]==false
                &&capabilities["fov"]==(deviceStore?device.fov():1),"Native16 capture runtime unavailable or calibration differs; no capture command was issued.");
        }
        const auto action=coordinatorCandidateAction(recovery,legacyCoordinatorProfile());
        const auto output=connection.execute(QStringLiteral("DCTS_DTOF_PRESET=%1 /root/dtof_candidates_20261009/control.sh %2 %3 %4 %5")
                                             .arg(effective.size().width()).arg(action).arg(mode).arg(token,csv));
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(output,&error);auto r=doc.object();
        require(error.error==QJsonParseError::NoError&&doc.isObject()&&r["state"]=="running"&&r["protocol"]==(LiteSrBoardContract::supports(mode)?LiteSrBoardContract::Protocol:"DTF3")&&r["generation"]==token&&r["candidate_mode"].toInt(-1)==mode&&r["options"]==csv&&r["boot_id"].toString().size()==36,"Candidate receipt mismatch");
        if(LiteSrBoardContract::supports(mode))require(r["engine"]==LiteSrBoardContract::engine(mode)&&r["model_sha256"]==LiteSrBoardContract::modelSha(mode)&&(!LiteSrBoardContract::isNear(mode)||(r["checkpoint_sha256"]==LiteSrBoardContract::NearCheckpointSha&&r["training_minimum_m"]==0.01))&&r["width"]==LiteSrBoardContract::outputWidth(mode)&&r["height"]==LiteSrBoardContract::outputHeight(mode)&&r["fresh_ms"]==LiteSrBoardContract::FreshMs,"LiteSR model receipt mismatch");
        if(LiteSrBoardContract::isHighResolution(mode)||LiteSrBoardContract::isNative16Depth(mode))require(r["rgb_paused"]==true&&r["video_transport"]=="none","High-resolution RGB pause receipt mismatch");
        if(LiteSrBoardContract::isHd(mode))require(r["paired_depth_native_protocol"]==LiteSrBoardContract::PairedProtocol&&r["video_transport"]=="rtsp_h265"&&r["video_width"]==2304&&r["video_height"]==1296&&r["video_fps"]==30&&r["video_kbps"]==4096,"HD video receipt mismatch");
        if(LiteSrBoardContract::isCaptureOnly(mode))require(r["capture_only"]==true&&r["inference_location"]=="none"&&r["depth_present"]==false&&r["photon_enabled"]==false,"Capture-only receipt mismatch; update the board photon policy before host inference");
        if(captureOnly){
            require(r["native_capture_protocol"]==LiteSrBoardContract::NativeCaptureProtocolV2
                    &&r["native_columns"]==16&&r["native_rows"]==16,"Native capture grid differs from the automatic 16x16 preset");
            r["requested_host_model"]=hostModel;r["capture_settings"]=effective.json();r["saved_capture_settings"]=saved.json();
        }
        if(LiteSrBoardContract::isNative16Depth(mode)){
            require(r["native_columns"]==16&&r["native_rows"]==16&&r["video_fps"]==0&&r["paired_depth_native_protocol"]==LiteSrBoardContract::PairedProtocol,"Native16 depth/camera-off receipt mismatch");
            r["capture_settings"]=effective.json();r["saved_capture_settings"]=saved.json();
        }
        const auto response=QJsonDocument(withDevice(r)).toJson(QJsonDocument::Compact);std::fwrite(response.constData(),1,size_t(response.size()),stdout);return 0;
    }
    Configuration c; require(args.size() >= 2, "Preview width is required");
    bool number = false; c.width = args[1].toInt(&number); require(number, "Invalid preview width"); c.keepModel = true;
    for (int i = 2; i < args.size(); ++i) {
        const auto arg = args[i]; if (arg == "--keep-preview") { c.keepPreview = true; continue; }
        if (arg == "--recover-fault") { require(!c.recoverFault, "Duplicate recovery argument"); c.recoverFault = true; continue; }
        if (arg == "--recover-stream-fault") { require(!c.recoverStreamFault, "Duplicate stream recovery argument"); c.recoverStreamFault = true; continue; }
        require(i + 1 < args.size(), "Missing RV control argument"); const auto value = args[++i];
        if (arg == "--host") require(value == sshHost(), "No pinned control profile for this host");
        else if (arg == "--model") { c.model = value; c.keepModel = false; }
        else if (arg == "--depth-width") { c.depthWidth = value.toInt(&number); require(number, "Invalid depth width"); }
        else if (arg == "--location") c.location = value;
        else if (arg == "--method") c.method = value;
        else throw Error("Unknown RV control argument");
    }
    validate(c);
    Ssh connection(known);verifyDevice(connection);c=coordinatorConfiguration(c,legacyCoordinatorProfile());
    // Persistence accepts every tuner resolution; normal capture retains its
    // calibrated grid contract and must reject before any control mutation.
    const auto saved=RvTuner::scriptSettings(tunerRead(connection,"/root/r144_depth/service.sh"));
    require(saved.nativeCompatible(),"Saved dToF resolution is incompatible with shared Native capture: requires 48x32. Use dToF Parameter Tuning or explicitly apply and save compatible settings; saved values were not changed.");
    QNetworkAccessManager network; network.setProxy(QNetworkProxy::NoProxy);
    if (c.keepPreview||c.keepModel) {const auto service=status(network)["service"].toObject();if(c.keepPreview)c.width=service["rgb_width"].toInt();if(c.keepModel){c.model=service["model"].toString();c.keepModel=false;}validate(c);}
    const auto r = receipt(connection.execute(command(c)), c);
    // Bind the capture identity to this receipt before readiness checks.
    c.model = r.model; c.keepModel = false; validate(c); Readiness ready(r, c); QElapsedTimer deadline; deadline.start();
    while (deadline.elapsed() < 45000) {
        auto current = status(network);
        if (ready.observe(current)) { const auto bytes = QJsonDocument(withDevice(current)).toJson(QJsonDocument::Compact); std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); return 0; }
        QThread::msleep(250);
    }
    throw Error("Selected stream did not produce fresh advancing frames within timeout", 4);
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try { return run(app.arguments()); }
    catch (const Error &e) { std::fprintf(stderr, "RvControlWorker: %s\n", e.what()); return e.code; }
    catch (const std::exception &e) { std::fprintf(stderr,"RvControlWorker: %s\n",e.what());return 2; }
    catch (...) { std::fputs("RvControlWorker: native dependency or control failure\n", stderr); return 2; }
}
