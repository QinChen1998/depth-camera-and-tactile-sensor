#include "SensorMainWindow.h"
#include "TactileDockWidget.h"
#include "LanguageManager.h"
#include "RuntimeDiscovery.h"
#include "UiStyle.h"
#include "sensors/RvAssistDepth.h"
#include "sensors/RvDepthDiagnostic.h"
#include "sensors/RvDeviceStore.h"
#include "sensors/RvUsbEnrollment.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstring>
#include <memory>

int runPortSmokeTests(SensorMainWindow &,LanguageManager &,const QString &);
int runTactile3DTests(const QString &);
int runTactileRenderSmoke(const QString &);
int runTactileSerialTests(const QString &);
int runTactileVisualizationTests(const QString &);
int runRvAssistRecoveryTests(const QString &);
int runRvTunerTests(const QString &);
int runLiveValidation(SensorMainWindow &,const QString &,int);

namespace {
QString configurationDirectory()
{
#ifdef DCTS_INSTALLER_BUILD
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + "/DepthCameraAndTactileSensor/config";
#else
    return RuntimeDiscovery::applicationRoot() + "/config";
#endif
}

void configureSettings(const QString &directory,bool seedDefaults,bool importOriginal=false)
{
    QDir().mkpath(directory);
    QCoreApplication::setOrganizationName("DepthCameraAndTactileSensor");
    QCoreApplication::setApplicationName("DepthCameraAndTactileSensor");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory+"/preferences");
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,directory+"/preferences");
    qputenv("DCTS_DEVICE_STORE_PATH",QFile::encodeName(directory+"/devices.json"));
    QSettings current;
    if(!current.value("application/imported",false).toBool()){
        const auto allowed=[](const QString &key){
            return key.startsWith("camera/")||key.startsWith("depth/")||key.startsWith("tactile/")
                ||key.startsWith("rendering/")||key.startsWith("imageDisplay/")||key.startsWith("rvDepth/")
                ||key.startsWith("dtof/")||key.startsWith("rvDevices/")||key=="ui/language";
        };
        if(importOriginal){
            QSettings native(QSettings::NativeFormat,QSettings::UserScope,"DexterousHandDemo","DexterousHandVisualization");
            for(const auto &key:native.allKeys())if(allowed(key))current.setValue(key,native.value(key));
            QSettings previous(directory+"/original-display-settings.ini",QSettings::IniFormat);
            for(const auto &key:previous.allKeys())if(allowed(key))current.setValue(key,previous.value(key));
        }
        if(seedDefaults){
            const QSettings defaults(":/config/first-run-settings.ini",QSettings::IniFormat);
            for(const auto &key:defaults.allKeys())
                if(!current.contains(key))current.setValue(key,defaults.value(key));
        }
#ifdef DCTS_INSTALLER_BUILD
        current.setValue("ui/language","zh_CN");
#endif
        current.setValue("depth/uniDepthExperiment","H");
        current.setValue("depth/hostModel","unidepth_wide_576_fp16_tof16");
        current.setValue("application/imported",true);current.sync();
    }
}
}

int main(int argc,char **argv)
{
    qputenv("RV_UNIDEPTH_EXPERIMENT","H");
    if(argc>1&&std::strcmp(argv[1],"--rv-assist-worker")==0){
        QCoreApplication app(argc,argv);const auto args=app.arguments();
        if(args.size()!=5||!RvAssistDepth::supportsHostModel(args[4]))return 3;
        return runRvAssistWorker(args[2],args[3],args[4]);
    }
    if(argc>1&&std::strcmp(argv[1],"--rv-depth-diagnostic-replay")==0){
        QCoreApplication app(argc,argv);const auto args=app.arguments();if(args.size()<4)return 3;
        return RvDepthDiagnostic::replay(args[2],args[3],args.value(4));
    }
    if(argc>1&&std::strcmp(argv[1],"--import-settings")==0){
        QCoreApplication app(argc,argv);configureSettings(configurationDirectory(),true,true);return 0;
    }
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);UiStyle::install(app);
    app.setApplicationDisplayName(QStringLiteral("触想工作台"));
    app.setApplicationVersion(QStringLiteral("1.0"));
    app.setWindowIcon(QIcon(QStringLiteral(":/branding/robot-hand.ico")));
    const auto arguments=app.arguments();const QString flag=arguments.value(1),report=arguments.value(2);
    const bool live=flag=="--live-validation";
    if(live&&(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||qEnvironmentVariable("DCTS_LIVE_SERIAL").isEmpty()
        ||qEnvironmentVariable("DCTS_ALLOW_SAVED_TOF_SWITCH")!="1"||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"))return 3;
    const bool testing=!flag.isEmpty();std::unique_ptr<QTemporaryDir> temporary;
    if(testing){
#ifdef DCTS_INSTALLER_BUILD
        const auto parent=QDir::tempPath()+"/DepthCameraAndTactileSensor";
#else
        const auto parent=RuntimeDiscovery::applicationRoot()+"/x64/validation";
#endif
        QDir().mkpath(parent);
        temporary=std::make_unique<QTemporaryDir>(parent+"/settings-XXXXXX");
        if(!temporary->isValid())return 3;
    }
    if(live){
        const auto source=configurationDirectory();
        const auto preferences="/preferences/DepthCameraAndTactileSensor/DepthCameraAndTactileSensor.ini";
        QDir().mkpath(temporary->path()+"/preferences/DepthCameraAndTactileSensor");
        if(!QFile::copy(source+"/devices.json",temporary->path()+"/devices.json")
            ||!QFile::copy(source+preferences,temporary->path()+preferences))return 3;
        RvDeviceStore store(temporary->path()+"/devices.json");QString selected;
        for(const auto &device:store.profiles())if(device.serial==qEnvironmentVariable("DCTS_LIVE_SERIAL"))selected=device.id;
        if(selected.isEmpty())return 3;store.save(store.profiles(),selected);
    }
    configureSettings(testing?temporary->path():configurationDirectory(),
        !testing||flag=="--port-ui-smoke-test");
    LanguageManager language;
    if(flag=="--tactile-3d-test")return runTactile3DTests(report);
    if(flag=="--tactile-render-test")return runTactileRenderSmoke(report);
    if(flag=="--tactile-serial-test")return runTactileSerialTests(report);
    if(flag=="--tactile-visualization-test"||flag=="--tactile-ui-smoke-test")return runTactileVisualizationTests(report);
    if(flag=="--rv-assist-recovery-test")return runRvAssistRecoveryTests(report);
    if(flag=="--rv-tuner-test")return runRvTunerTests(report);
    if(flag=="--rv-usb-enrollment-test"){
        const auto checks=checkRvUsbEnrollment();
        QFile output(report);
        if(!output.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
        bool passed=true;
        for(auto it=checks.constBegin();it!=checks.constEnd();++it){
            const bool ok=it.value().toBool();
            output.write(QByteArray(ok?"PASS ":"FAIL ")+it.key().toUtf8()+"\n");
            passed &= ok;
        }
        return passed?0:1;
    }
    if(testing&&flag!="--port-ui-smoke-test"&&!live)return 3;
    SensorMainWindow window(&language);window.show();
    if(live)return runLiveValidation(window,report,300);
    if(flag=="--port-ui-smoke-test"){
        const int result=runPortSmokeTests(window,language,report);
        QSettings user;
        user.setValue("tactile/display/arrowGain",7);
        user.setValue("imageDisplay/rotationDegrees",90);
        user.sync();
        configureSettings(temporary->path(),true);
        QSettings reopened;
        const bool preserved=reopened.value("tactile/display/arrowGain").toInt()==7
            &&reopened.value("imageDisplay/rotationDegrees").toInt()==90;
        QFile output(report);
        if(output.open(QIODevice::Append|QIODevice::Text))
            output.write(preserved?"FIRST_RUN_DEFAULTS_DO_NOT_OVERWRITE_USER PASS\n":"FIRST_RUN_DEFAULTS_DO_NOT_OVERWRITE_USER FAIL\n");
        return result?result:(preserved?0:1);
    }
    if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1")window.tactile()->display()->connectDefaultSerial();
    watchRvUsbDevices(window.session(), &window);
    window.monitorCameraConnection();
    app.setQuitOnLastWindowClosed(true);return app.exec();
}
