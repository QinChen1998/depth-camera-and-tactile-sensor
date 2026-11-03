#pragma once
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>

// Sensor-only runtime discovery. All default paths belong to this application.
namespace RuntimeDiscovery {
struct PythonResult {
    QString executable, error;
    QStringList diagnostics;
    bool ok() const { return !executable.isEmpty(); }
};
QString applicationRoot();
bool componentAvailable(const QString &id);
QString rvAssistRoot();
QString rvAssistModelRoot(const QString &model);
QString rvLiteSrRoot();
QString rvLiteSrNearRoot();
QStringList vlcDirectories();
PythonResult rvControlForLaunch();
QProcessEnvironment handPoseNativeEnvironment(const QString &executable);
}
