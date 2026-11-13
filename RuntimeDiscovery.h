#pragma once
#include "sdk/DepthSdkExports.h"
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>

class QSettings;

// Sensor-only runtime discovery. All default paths belong to this application.
namespace RuntimeDiscovery {
struct NativeWorkerResult {
    QString executable, error;
    bool ok() const { return !executable.isEmpty(); }
};
DCTS_DEPTH_INTERNAL QString applicationRoot();
DCTS_DEPTH_INTERNAL QString runtimeDirectory();
QString inferenceWorker();
DCTS_DEPTH_INTERNAL QString rvAssistRoot();
DCTS_DEPTH_INTERNAL QString rvCompatibleRoot();
DCTS_DEPTH_INTERNAL QString rvAssistModelRoot(const QString &model);
DCTS_DEPTH_INTERNAL QStringList vlcDirectories();
DCTS_DEPTH_INTERNAL NativeWorkerResult rvControlForLaunch();
DCTS_DEPTH_INTERNAL QProcessEnvironment nativeWorkerEnvironment(const QString &executable);
void setSdkPaths(const QString &runtimeDirectory, const QString &settingsFile);
DCTS_DEPTH_INTERNAL QSettings depthSettings();
}
