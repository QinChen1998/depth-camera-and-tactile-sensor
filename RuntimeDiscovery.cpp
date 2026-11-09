#include "RuntimeDiscovery.h"
#include "sensors/RvCameraView.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace RuntimeDiscovery {
namespace {
QString absolute(const QString &path) {
    return path.trimmed().isEmpty()?QString():QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QDir::fromNativeSeparators(path)));
}
QJsonObject runtimeOverrides() {
    QFile file(QDir(QCoreApplication::applicationDirPath()).filePath("runtime-paths.json"));
    if(!file.open(QIODevice::ReadOnly)||file.size()>65536)return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QString overrideValue(const QString &name) {
    if(qEnvironmentVariableIsSet(name.toLatin1().constData()))return absolute(qEnvironmentVariable(name.toLatin1().constData()));
    return absolute(runtimeOverrides().value(name).toString());
}
QString configError() {
    QFile file(QDir(QCoreApplication::applicationDirPath()).filePath("runtime-paths.json"));
    if(!file.exists())return {};
    if(!file.open(QIODevice::ReadOnly)||file.size()>65536)return "Cannot read runtime-paths.json";
    QJsonParseError error;const auto document=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())return "Invalid runtime-paths.json";
    const auto object=document.object();for(auto it=object.begin();it!=object.end();++it)if(!it.value().isString())return "Runtime path must be a string: "+it.key();
    return {};
}
QString nativeExecutable(const char *name,const QString &root,const QString &directory,const QString &file) {
    if(qEnvironmentVariableIsSet(name))return absolute(qEnvironmentVariable(name));
    if(runtimeOverrides().contains(QLatin1String(name)))return absolute(runtimeOverrides()[QLatin1String(name)].toString());
#ifdef QT_DEBUG
    const QString configuration="Debug";
#else
    const QString configuration="Release";
#endif
    return QDir(root).filePath(directory+'/'+configuration+"/runtime/"+file);
}
}
QString applicationRoot() {
    QDir directory(QCoreApplication::applicationDirPath());
    for(int i=0;i<4;++i){if(directory.exists("Depth Camera and Tactile Sensor.vcxproj")||directory.exists("DCTS_APP.json"))return directory.absolutePath();directory.cdUp();}
    return QCoreApplication::applicationDirPath();
}
QString rvAssistRoot(){return rvAssistModelRoot(QLatin1String(RvCameraView::UniDepthVgaNative16));}
QString rvCompatibleRoot(){
    const auto path=overrideValue("RV_ASSIST_COMPAT_ROOT");
    return path.isEmpty()?QDir(applicationRoot()).filePath("UniDepthRuntimeCompatible"):path;
}
QString rvAssistModelRoot(const QString &model){
#if defined(DCTS_TENSORRT_RTX)
    if(RvCameraView::isUniDepthVga(model)){
        const auto path=overrideValue("RV_ASSIST_RGB672_VGA_FP16_ROOT");
        return path.isEmpty()?QDir(applicationRoot()).filePath("UniDepthRuntimeRgb672VgaFP16RTX"):path;
    }
#endif
    return {};
}
QStringList vlcDirectories() {
    const auto configured = overrideValue("RTSP_VLC_DIR");
    if (!configured.isEmpty()) return {configured};
    QStringList directories{QDir(applicationRoot()).filePath("vlc")};
    for (const auto *key : {"HKEY_CURRENT_USER\\Software\\VideoLAN\\VLC", "HKEY_LOCAL_MACHINE\\Software\\VideoLAN\\VLC"}) {
        QSettings registry(QString::fromLatin1(key), QSettings::NativeFormat);
        directories.append(registry.value("InstallDir").toString());
    }
    directories.append({qEnvironmentVariable("ProgramFiles") + "/VideoLAN/VLC",
        qEnvironmentVariable("ProgramFiles(x86)") + "/VideoLAN/VLC",
        QStringLiteral("F:/Software/VideoLAN/VLC"), QStringLiteral("D:/Software/VLC")});
    directories.removeAll(QString()); directories.removeDuplicates();
    return directories;
}
QProcessEnvironment nativeWorkerEnvironment(const QString &executable) {
    auto env = QProcessEnvironment::systemEnvironment();
    // The standalone runtime owns its DLL closure. Do not inherit Conda/MinGW
    // loader paths or any Python interpreter configuration.
    env.insert("PATH", QDir::toNativeSeparators(QFileInfo(executable).absolutePath())
        + ';' + env.value("SystemRoot", "C:/Windows") + "\\System32");
    for (const auto *key : {"PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "VIRTUAL_ENV", "CONDA_PREFIX",
                            "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"})
        env.remove(QString::fromLatin1(key));
    env.insert("QT_PLUGIN_PATH", QDir::toNativeSeparators(QFileInfo(executable).absolutePath() + "/plugins"));
    return env;
}
NativeWorkerResult rvControlForLaunch() {
    NativeWorkerResult result;
    result.error = configError(); if (!result.error.isEmpty()) return result;
    const auto executable = nativeExecutable("RV_CONTROL_CPP", applicationRoot(),
        "x64/rv_control", "RvControlWorker.exe");
    const QDir directory(QFileInfo(executable).absolutePath());
    QFile f(directory.filePath("implementation.json"));
    if (executable.isEmpty() || !QFileInfo(executable).isFile() || !f.open(QIODevice::ReadOnly) || f.size() > 1048576) {
        result.error = QStringLiteral("Native RV worker or implementation manifest is missing; no Python fallback."); return result;
    }
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(f.readAll(), &error); const auto info = doc.object();
    const auto matches = [](const QString &file, const QString &expected) {
        QFile source(file); if (expected.size() != 64 || !source.open(QIODevice::ReadOnly)) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256); return hash.addData(&source) && hash.result().toHex() == expected.toLatin1();
    };
    bool ok = error.error == QJsonParseError::NoError && doc.isObject() && info["implementation"] == "rv-control-cpp-v1"
        && !info["transport_test_fixture"].toBool()
        && info["executable"] == "RvControlWorker.exe" && info["host_key_algorithm"] == "ssh-ed25519"
        && matches(executable, info["executable_sha256"].toString());
    const auto dlls = info["dependencies"].toObject(), files = info["payload_files"].toObject();
    ok = ok && dlls.contains("libssh2.dll") && dlls.contains("libcrypto-3-x64.dll") && files.contains("known_hosts");
    for (const auto &table : {dlls, files}) for (auto it = table.begin(); it != table.end(); ++it) {
        const auto name = it.key(), canonical = QFileInfo(directory.filePath(name)).canonicalFilePath();
        ok = ok && !name.isEmpty() && !QDir::isAbsolutePath(name) && !name.contains(':') && !name.contains('\\')
            && !name.split('/').contains("..") && !name.contains("python", Qt::CaseInsensitive)
            && canonical.startsWith(directory.canonicalPath() + '/', Qt::CaseInsensitive)
            && matches(directory.filePath(name), it.value().toString());
    }
    if (!ok) { result.error = QStringLiteral("Native RV dependency fingerprint mismatch; reinstall RV. No Python fallback."); return result; }
    result.executable = executable; result.error.clear(); return result;
}

}
