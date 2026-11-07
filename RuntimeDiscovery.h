#pragma once
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>

// Sensor-only runtime discovery. All default paths belong to this application.
namespace RuntimeDiscovery {
struct NativeWorkerResult {
    QString executable, error;
    bool ok() const { return !executable.isEmpty(); }
};
QString applicationRoot();
QString rvAssistRoot();
QString rvAssistModelRoot(const QString &model);
QStringList vlcDirectories();
NativeWorkerResult rvControlForLaunch();
QProcessEnvironment nativeWorkerEnvironment(const QString &executable);
}
