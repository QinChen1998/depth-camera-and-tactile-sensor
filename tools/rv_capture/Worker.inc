// Included inside the native SSH worker's private namespace.
const QString BoardCandidateDir="/root/dtof_candidates_20261009";
bool nativeCaptureCapabilities(const QJsonObject& capabilities,int fov){
    return capabilities["native_capture_protocol"]==RvCaptureContract::NativeCaptureProtocolV2
        &&capabilities["native_hole_fill_protocol"]==RvCaptureContract::NativeHoleFillProtocol
        &&capabilities["fov"]==fov&&capabilities["m16"]==true&&capabilities["m48"]==true
        &&capabilities["model_inference"]==false&&capabilities["photon_enabled"]==false;
}
struct NativeCapturePayload {
    QByteArray launcher,control,driver,nativeCapture;
    QString launcherSha,controlSha,driverSha,nativeCaptureSha;
    explicit NativeCapturePayload(int fov){
        require(fov==RvCalibration::Fov,"Native capture requires a supported device calibration");
        launcher=boardPayload("native_gui.sh");
        nativeCapture=boardPayload(QString("native-fov%1.bin").arg(fov));
        const QDir runtime(QCoreApplication::applicationDirPath());
        driver=readFile(runtime.filePath("rv1103_bootstrap/tof_driver"),4*1024*1024);
        control=readFile(runtime.filePath("rv1103_bootstrap/candidate-control.sh"));
        launcherSha=tunerHash(launcher);controlSha=tunerHash(control);
        driverSha=tunerHash(driver);nativeCaptureSha=tunerHash(nativeCapture);
    }
    QString stateScript() const{
        return QStringLiteral(R"SH(
dir=/root/dtof_candidates_20261009
driver=/root/r144_depth/tmf8829_ext
factory_driver=/root/rv1103_mw_spi/bin/tof_driver
matches(){ [ -f "$1" ] && [ ! -L "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ]; }
capture_state(){
 if [ ! -d "$dir" ] || [ ! -f "$dir/native_gui.sh" ]; then echo missing
 elif matches "$dir/native_gui.sh" '@LAUNCHER@' && [ -x "$dir/native_gui.sh" ] &&
      matches "$dir/control.sh" '@CONTROL@' && [ -x "$dir/control.sh" ] &&
      matches "$driver" '@DRIVER@' && [ -x "$driver" ] &&
      matches "$dir/native_hd_gui" '@NATIVE@' && [ -x "$dir/native_hd_gui" ] &&
      { [ ! -e "$factory_driver" ] || matches "$factory_driver" '@DRIVER@'; } &&
      [ -r "$dir/session.sh" ] && [ -s /root/r144_depth/camera_rtsp_config.awk ] &&
      [ -s /root/r144_depth/no_route_monitor.so ] && [ -x /oem/usr/bin/rkipc ] && [ -s /userdata/rkipc.ini ]; then echo ready
 else echo mismatch
 fi
}
)SH").replace("@LAUNCHER@",launcherSha).replace("@CONTROL@",controlSha)
            .replace("@DRIVER@",driverSha).replace("@NATIVE@",nativeCaptureSha);
    }
};
QString captureRuntimeState(Ssh& connection){
    const auto state=connection.execute(NativeCapturePayload(device.fov()).stateScript()+"\ncapture_state").trimmed();
    require(state=="ready"||state=="missing"||state=="mismatch","Invalid native capture runtime status");
    return QString::fromLatin1(state);
}

QString candidateRecoveryCapability(Ssh &connection){
    QJsonParseError parse;
    const auto data=connection.execute(BoardCandidateDir+"/control.sh capabilities");
    const auto document=QJsonDocument::fromJson(data,&parse);
    const auto protocol=document.object()["candidate_stream_recovery"].toString();
    require(parse.error==QJsonParseError::NoError&&protocol==QLatin1String(RvCaptureContract::CandidateRecoveryProtocol),
            "Board capture coordinator does not report safe stream recovery support. Update it through USB one-click connection.");
    return protocol;
}

QJsonArray boardPreparationSelfTest(){
    QJsonArray checks;
    const auto check=[&](const QString& name,bool ok){require(ok,name);checks.append(QJsonObject{{"name",name},{"passed",true}});};
    const QDir runtime(QCoreApplication::applicationDirPath());
    const auto fresh=readFile(runtime.filePath("rv1103_bootstrap/session.sh"));
    const auto prepared=withAutomaticTof(fresh);
    check("capture_preserves_saved_sensor_arguments",RvTuner::scriptSettings(fresh,true)==RvTuner::scriptSettings(prepared,true));
    check("capture_preset_insertion_idempotent",withAutomaticTof(prepared)==prepared);
    check("capture_supports_native_modes",prepared.contains("in 14|24|25)")&&!prepared.contains("|16")&&!prepared.contains("|22"));
    auto unknown=prepared;unknown.replace("-o 1 -t 0","-o 2 -t 0");
    bool refused=false;try{withAutomaticTof(unknown);}catch(const std::exception&){refused=true;}
    check("modified_host_preset_refused",refused);
    for(int fov:{6}){
        const NativeCapturePayload payload(fov);const auto state=payload.stateScript();
        check(QString("capture_f%1_hashes_resolve").arg(fov),state.contains(payload.nativeCaptureSha)&&state.contains(payload.launcherSha)
            &&state.contains(payload.controlSha)&&state.contains(payload.driverSha)&&!state.contains(QRegularExpression("@[A-Z_]+@")));
        check(QString("capture_f%1_has_no_model_dependency").arg(fov),!state.contains("dctsm")&&!state.contains("rknn")&&!payload.launcher.contains("16|22"));
    }
    const auto installer=readFile(runtime.filePath("rv1103_bootstrap/install.sh"));
    check("fresh_import_checks_native_capabilities",installer.contains("--native-capabilities"));
    check("fresh_import_requires_hole_fill_protocol",installer.contains(RvCaptureContract::NativeHoleFillProtocol));
    check("fresh_import_retains_rollback",installer.contains("committed=0")&&installer.contains("if [ \"$committed\" = 0 ]; then")&&installer.contains("installed_candidate=1"));
    return checks;
}
