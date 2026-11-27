#include "SensorMainWindow.h"
#include "AppEdition.h"
#include "TactileDockWidget.h"
#include "LanguageManager.h"
#include "RuntimeDiscovery.h"
#include "UiStyle.h"
#include "sensors/RvAssistDepth.h"
#include "sensors/DepthSdkClient.h"
#include "sensors/RvDepthDiagnostic.h"
#include "sensors/RvDeviceStore.h"
#include "sensors/RvSessionController.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
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
int runImagingSwitchValidation(SensorMainWindow&,const QString&);
int runPointCloudLiveValidation(SensorMainWindow &, const QString &);
int runMeasurementLiveValidation(SensorMainWindow &, const QString &);
int runHostAssistLiveValidation(SensorMainWindow &,const QString &);
int runHostThermalLiveValidation(SensorMainWindow &,const QString &);
int runUsbEnrollmentLiveValidation(SensorMainWindow &,const QString &);

namespace {
QString configurationDirectory()
{
    if (AppEdition::Customer)
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + "/DepthCameraAndTactileSensorCustomer/config";
#ifdef DCTS_INSTALLER_BUILD
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + "/DepthCameraAndTactileSensor/config";
#else
    return RuntimeDiscovery::applicationRoot() + "/config";
#endif
}

int writeEditionInfo(const QString &path)
{
    const auto &schemes = AppEdition::imagingSchemes();
    CxwlDepthVersionInfo sdk{};
    sdk.size = sizeof(sdk);
    if (CxwlDepth_AbiVersion() != CXWL_DEPTH_ABI_VERSION || CxwlDepth_GetVersionInfo(&sdk) != CXWL_DEPTH_OK)
        return 3;
    const QJsonObject info{
        {"edition", QLatin1String(AppEdition::Name)},
        {"version", AppEdition::version()},
        {"sdk_version", QString::fromUtf8(sdk.version_utf8)}, {"sdk_abi", int(sdk.abi_version)},
        {"imaging_schemes", QJsonArray::fromStringList(schemes)},
        {"default_imaging_scheme", schemes.constFirst()},
        {"measurement_capture", AppEdition::measurementCaptureEnabled()}
    };
    const auto bytes = QJsonDocument(info).toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit() ? 0 : 3;
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
            if(!current.contains("depth/imagingScheme"))
                current.setValue("depth/imagingScheme", AppEdition::imagingSchemes().constFirst());
            for(const auto &key:defaults.allKeys())
                if(!current.contains(key))current.setValue(key,defaults.value(key));
        }
#ifdef DCTS_INSTALLER_BUILD
        current.setValue("ui/language","zh_CN");
#endif
        current.setValue("application/imported",true);current.sync();
    }
}
}

int main(int argc,char **argv)
{
    if (AppEdition::imagingSchemes().isEmpty()) {
        qWarning("The application edition has no imaging schemes.");
        return 3;
    }
    if (argc > 1 && std::strcmp(argv[1], "--edition-info") == 0) {
        QCoreApplication app(argc, argv);
        const auto args = app.arguments();
        return args.size() == 3 ? writeEditionInfo(args[2]) : 3;
    }
    if(argc>1&&std::strcmp(argv[1],"--rv-assist-path-test")==0){
        QCoreApplication app(argc,argv);
        const auto args=app.arguments();
        if(AppEdition::Customer||args.size()!=3||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1")return 3;
        extern int runRvAssistDepthTests(const QString &reportPath);
        return runRvAssistDepthTests(args[2]);
    }
    if(argc>1&&std::strcmp(argv[1],"--rv-depth-diagnostic-replay")==0){
        QCoreApplication app(argc,argv);const auto args=app.arguments();
        if(AppEdition::Customer||args.size()<4)return 3;
        return RvDepthDiagnostic::replay(args[2],args[3],args.value(4));
    }
    if(argc>1&&std::strcmp(argv[1],"--import-settings")==0){
        QCoreApplication app(argc,argv);configureSettings(configurationDirectory(),true,true);return 0;
    }
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);UiStyle::install(app);
    app.setApplicationDisplayName(QStringLiteral("触想工作台"));
    app.setApplicationVersion(AppEdition::version());
    app.setWindowIcon(QIcon(QStringLiteral(":/branding/cx.ico")));
    const auto arguments=app.arguments();const QString flag=arguments.value(1),report=arguments.value(2);
    const bool hostAssistLive=flag=="--host-assist-live-validation";
    const bool hostThermalLive=flag=="--host-thermal-live-validation";
    const bool usbEnrollmentLive=flag=="--usb-enrollment-live-validation";
    const bool imagingSwitchLive=flag=="--imaging-switch-validation";
    const bool pointCloudLive=flag=="--point-cloud-live-validation";
    const bool measurementLive=flag=="--measurement-live-validation";
    if ((AppEdition::Customer && (hostAssistLive || hostThermalLive))
        || (measurementLive && !AppEdition::measurementCaptureEnabled())) return 3;
    const bool live=usbEnrollmentLive||hostAssistLive||hostThermalLive||imagingSwitchLive||pointCloudLive||measurementLive||flag=="--live-validation";
    if(live&&(qEnvironmentVariable("DCTS_LIVE_ENABLE")!="1"||qEnvironmentVariable("DCTS_LIVE_SERIAL").isEmpty()
        ||(!usbEnrollmentLive&&!hostAssistLive&&!hostThermalLive&&qEnvironmentVariable("DCTS_ALLOW_SAVED_TOF_SWITCH")!="1")||qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1")){
        qWarning("Live validation requires explicit enablement, a device identity, and the applicable mode-switch authorization.");
        return 3;
    }
    const bool testing=!flag.isEmpty();std::unique_ptr<QTemporaryDir> temporary;
    if(testing){
#ifdef DCTS_INSTALLER_BUILD
        const auto parent=QDir::tempPath()+"/DepthCameraAndTactileSensor";
#else
        const auto parent=RuntimeDiscovery::applicationRoot()+"/x64/validation";
#endif
        QDir().mkpath(parent);
        temporary=std::make_unique<QTemporaryDir>(parent+"/settings-XXXXXX");
        if(!temporary->isValid()){
            qWarning() << "Cannot create isolated test settings:" << temporary->errorString();
            return 3;
        }
    }
    if(live){
        qputenv("CXWL_DEPTH_USB_SERIAL", qgetenv("DCTS_LIVE_SERIAL"));
        // Explicitly gated live tests may read an existing development profile
        // while keeping installed user settings and credentials untouched.
        const auto source=qEnvironmentVariable("DCTS_LIVE_CONFIG_SOURCE",configurationDirectory());
        const auto preferences="/preferences/DepthCameraAndTactileSensor/DepthCameraAndTactileSensor.ini";
        QDir().mkpath(temporary->path()+"/preferences/DepthCameraAndTactileSensor");
        const bool freshUsb=usbEnrollmentLive&&qEnvironmentVariable("DCTS_USB_TEST_NEW")=="1";
        if((!freshUsb&&!QFile::copy(source+"/devices.json",temporary->path()+"/devices.json"))
            ||(QFile::exists(source+preferences)&&!QFile::copy(source+preferences,temporary->path()+preferences))){
            qWarning("Cannot copy the existing configuration into isolated live-test settings.");
            return 3;
        }
        if(!usbEnrollmentLive){
            RvDeviceStore store(temporary->path()+"/devices.json");QString selected;
            for(const auto &device:store.profiles())if(device.serial==qEnvironmentVariable("DCTS_LIVE_SERIAL"))selected=device.id;
            if(selected.isEmpty()){
                qWarning("The requested device was not found in the registered live-test profiles.");
                return 3;
            }
            store.save(store.profiles(),selected);
        }
    }
    configureSettings(testing?temporary->path():configurationDirectory(),
        !testing||flag=="--port-ui-smoke-test");
    if(usbEnrollmentLive)QSettings().setValue("depth/imagingScheme",AppEdition::imagingSchemes().constFirst());
    if(imagingSwitchLive&&!AppEdition::Customer&&qEnvironmentVariable("DCTS_LIVE_NATIVE_SENSOR")=="1")
        QSettings().setValue("depth/imagingScheme",QLatin1String(RvDtofCandidates::SensorDepthPresets[0].id));
    LanguageManager language;
    if(flag=="--tactile-3d-test")return runTactile3DTests(report);
    if(flag=="--tactile-render-test")return runTactileRenderSmoke(report);
    if(flag=="--tactile-serial-test")return runTactileSerialTests(report);
    if(flag=="--tactile-visualization-test"||flag=="--tactile-ui-smoke-test")return runTactileVisualizationTests(report);
    if(flag=="--rv-assist-recovery-test")return runRvAssistRecoveryTests(report);
    if(flag=="--rv-tuner-test")return runRvTunerTests(report);
    if(flag=="--rv-usb-enrollment-test"){
        if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")!="1")return 3;
        CxwlDepthVersionInfo version{};version.size=sizeof(version);
        CxwlDepthCapabilities caps{};caps.size=sizeof(caps);
        const bool currentAbi=CxwlDepth_AbiVersion()==3&&CxwlDepth_GetVersionInfo(&version)==CXWL_DEPTH_OK
            &&QByteArray(version.version_utf8)=="3.1.0"&&version.abi_version==3;
        const bool quality=currentAbi&&CxwlDepth_GetCapabilities(&caps)==CXWL_DEPTH_OK&&caps.mode_count==6
            &&caps.depth_width==48&&caps.depth_height==32&&caps.video_width==2304&&caps.video_height==1296
            &&caps.has_snr&&caps.has_valid_mask&&caps.max_sessions==1;
        RvDeviceProfile fixture;
        fixture.id=QStringLiteral("direct-test");
        fixture.name=QStringLiteral("USB direct test");
        fixture.serial=QStringLiteral("0123456789abcdef");
        fixture.host=QStringLiteral("172.32.0.93");
        fixture.serviceInstance=QString(32, QLatin1Char('a'));
        const auto fixtureData=fixture.json();
        const auto loaded=RvDeviceProfile::fromJson(fixtureData);
        bool rejected=false;
        try {
            auto invalid=fixtureData;
            invalid["service_instance"]="wrong";
            RvDeviceProfile::fromJson(invalid);
        } catch(const std::exception &) {
            rejected=true;
        }
        DepthSdkClient sdk(RvDeviceStore::defaultPath());
        bool enumerated=false;
        const bool empty=sdk.savedDevices(&enumerated).isEmpty()&&enumerated;
        const QJsonObject checks{{"SDK_3_1_0_ABI_3",currentAbi},{"SDK_SIX_MODES_AND_QUALITY",quality},
            {"SDK_DIRECT_PROFILE_ROUND_TRIP",loaded.json()==fixtureData&&loaded.fingerprint().startsWith("SHA256:")},
            {"SDK_MALFORMED_DIRECT_PROFILE_REJECTED",rejected},
            {"SDK_EMPTY_DEVICE_STORE",sdk.available()&&empty},
            {"SDK_OFFLINE_USB_CONNECTION_DENIED",sdk.connectViaUsb()==CXWL_DEPTH_UNAVAILABLE&&sdk.userStopped()}};
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
    if(usbEnrollmentLive)return runUsbEnrollmentLiveValidation(window,report);
    if(hostAssistLive)return runHostAssistLiveValidation(window,report);
    if(hostThermalLive)return runHostThermalLiveValidation(window,report);
    if(imagingSwitchLive)return runImagingSwitchValidation(window,report);
    if(pointCloudLive)return runPointCloudLiveValidation(window,report);
    if(measurementLive)return runMeasurementLiveValidation(window,report);
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
    app.setQuitOnLastWindowClosed(true);return app.exec();
}
