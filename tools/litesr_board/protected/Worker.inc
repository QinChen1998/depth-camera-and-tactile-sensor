// Included by RvControlWorker.cpp inside its private namespace.
const QString BoardCandidateDir = "/root/dtof_candidates_20261009";

struct ProtectedLiteSrPayload {
    QByteArray tinyExecutable,tinyModel,tinyCamera;
    QString tinyExecutableSha,tinyModelSha,tinyCameraSha;
    QByteArray launcher, rectExecutable, rectModel, control, driver, nativeCapture;
    QString launcherSha, rectExecutableSha, rectModelSha, controlSha, driverSha, nativeCaptureSha;

    explicit ProtectedLiteSrPayload(int fov)
    {
        require(fov==1||fov==2,"Protected LiteSR requires a supported device calibration");
        launcher=boardPayload("litesr_gui.sh");
        rectExecutable=boardPayload(QString("rect-litesr-fov%1.bin").arg(fov));
        rectModel=boardPayload("rect-model.dctsm");
        rectExecutableSha=tunerHash(rectExecutable);rectModelSha=tunerHash(rectModel);
        launcher.replace("@RECT_BINARY_SHA@",rectExecutableSha.toLatin1());
        launcher.replace("@RECT_MODEL_SHA@",rectModelSha.toLatin1());
        driver=readFile(QDir(QCoreApplication::applicationDirPath()).filePath("rv1103_bootstrap/tof_driver"),4*1024*1024);
        nativeCapture=boardPayload(QString("native-fov%1.bin").arg(fov));
        driverSha=tunerHash(driver);nativeCaptureSha=tunerHash(nativeCapture);
        control=readFile(QDir(QCoreApplication::applicationDirPath()).filePath("rv1103_bootstrap/candidate-control.sh"));
        controlSha=tunerHash(control);
        tinyExecutable=boardPayload("tiny-engine.bin");tinyModel=boardPayload("tiny-model.dctsm");tinyCamera=boardPayload("tiny-camera.bin");
        tinyExecutableSha=tunerHash(tinyExecutable);tinyModelSha=tunerHash(tinyModel);tinyCameraSha=tunerHash(tinyCamera);
        launcher.replace("@TINY_BINARY_SHA@",tinyExecutableSha.toLatin1());launcher.replace("@TINY_MODEL_SHA@",tinyModelSha.toLatin1());launcher.replace("@TINY_CAMERA_SHA@",tinyCameraSha.toLatin1());
        launcherSha=tunerHash(launcher);
    }

    QString stateScript() const
    {
        return QStringLiteral(R"SH(
dir=/root/dtof_candidates_20261009
rect_bundle="$dir/dcts-litesr-384x256-v3-depth-only"
tiny_bundle="$dir/dcts-tiny-nv12-128x80-v1"
driver=/root/r144_depth/tmf8829_ext
factory_driver=/root/rv1103_mw_spi/bin/tof_driver
native_capture="$dir/native_hd_gui"
control="$dir/control.sh"
launcher="$dir/litesr_gui.sh"
digest(){ sha256sum "$1" | cut -d' ' -f1; }
matches(){ [ -f "$1" ] && [ ! -L "$1" ] && [ "$(digest "$1")" = "$2" ]; }
supported(){
    [ -x "$dir/control.sh" ] && [ -r "$dir/session.sh" ] &&
    [ -r /root/r144_depth/librknnmrt.so ] && [ -r /usr/lib/libcrypto.so.1.1 ] &&
    [ -s /root/r144_depth/camera_rtsp_config.awk ] && [ -s /root/r144_depth/no_route_monitor.so ] &&
    [ -s /oem/usr/bin/rkipc ] && [ -s /userdata/rkipc.ini ]
}
valid_rect(){
    [ -d "$1" ] && [ ! -L "$1" ] &&
    [ -x "$1/litesr_near_gui" ] && matches "$1/litesr_near_gui" '@RECT_EXE@' &&
    matches "$1/model.dctsm" '@RECT_MODEL@'
}
valid_tiny(){
    [ -d "$1" ] && [ ! -L "$1" ] && [ -x "$1/tiny_nv12" ] && [ -x "$1/tiny_nv12_camera" ] &&
    matches "$1/tiny_nv12" '@TINY_EXE@' && matches "$1/model.dctsm" '@TINY_MODEL@' && matches "$1/tiny_nv12_camera" '@TINY_CAMERA@'
}
status_ready(){
    matches "$driver" '@DRIVER@' && matches "$native_capture" '@NATIVE_CAPTURE@' &&
    { [ ! -e "$factory_driver" ] || matches "$factory_driver" '@DRIVER@'; }
}
model_state(){
    if ! supported; then echo runtime_missing
    elif matches "$launcher" '@LAUNCHER@'; then
        if [ -x "$launcher" ] && valid_rect "$rect_bundle" && valid_tiny "$tiny_bundle" && status_ready && matches "$control" '@CONTROL@'; then echo ready; else echo mismatch; fi
    elif [ -L "$launcher" ] || { [ -e "$launcher" ] && [ ! -f "$launcher" ]; }; then echo mismatch
    elif [ -f "$launcher" ] && grep -q '^# DCTS protected LiteSR' "$launcher"; then echo mismatch
    else echo missing
    fi
}
)SH").replace("@RECT_EXE@",rectExecutableSha).replace("@RECT_MODEL@",rectModelSha)
            .replace("@LAUNCHER@",launcherSha).replace("@CONTROL@",controlSha)
            .replace("@TINY_EXE@",tinyExecutableSha).replace("@TINY_MODEL@",tinyModelSha).replace("@TINY_CAMERA@",tinyCameraSha)
            .replace("@DRIVER@",driverSha).replace("@NATIVE_CAPTURE@",nativeCaptureSha);
    }
};

QString nearModelState(Ssh &connection)
{
    const int fov=device.fov()==2 ? 2 : 1;
    const auto state=connection.execute(ProtectedLiteSrPayload(fov).stateScript()+"\nmodel_state").trimmed();
    require(state=="ready"||state=="missing"||state=="mismatch"||state=="runtime_missing","Invalid protected LiteSR status");
    return QString::fromLatin1(state);
}

void installProtectedLiteSr(Ssh &connection)
{
    const ProtectedLiteSrPayload payload(device.fov());
    const auto state=connection.execute(payload.stateScript()+"\nmodel_state").trimmed();
    require(state!="runtime_missing","The on-board inference runtime is incomplete.");
    if(state=="ready") {
        installAutomaticTof(connection);
        return;
    }

    const auto stage=BoardCandidateDir+"/.dcts-protected-"+QUuid::createUuid().toString(QUuid::Id128);
    const auto stageVariable="\nstage="+shellQuote(stage)+"\n";
    connection.execute("set -eu\numask 077\n"+stageVariable+"mkdir \"$stage\" \"$stage/rect\" \"$stage/tiny\"");
    try {
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/litesr_gui.sh\"",30000,payload.launcher);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/rect/litesr_near_gui\"",30000,payload.rectExecutable);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/rect/model.dctsm\"",30000,payload.rectModel);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/control.sh\"",30000,payload.control);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tmf8829_ext\"",30000,payload.driver);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tof_driver\"",30000,payload.driver);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/native_hd_gui\"",30000,payload.nativeCapture);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tiny/tiny_nv12\"",30000,payload.tinyExecutable);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tiny/model.dctsm\"",30000,payload.tinyModel);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tiny/tiny_nv12_camera\"",30000,payload.tinyCamera);
        const auto currentControl=tunerRead(connection,BoardCandidateDir+"/control.sh");
        const QStringList knownControls{"3eae39c9873a9e0f570223989a6159caa9b3fe2d92d470e817bd8845244a376a", "c33a00cbed0f3d0b51608a7dfd5ac3212edea99cf83881e8a64edc5c5063cd21", "8f5b9aed04e921463fcd0be1c0c6d1de49b9deafe5bec8d823224640804d34ad", "446a044d71bcf20c5a1aeda478c51458cbf4a982f8e170a59bb0c4ef87c4312c"};
        require(currentControl==payload.control||knownControls.contains(tunerHash(currentControl)),"Unrecognized candidate control script; preserve it for review");
        installAutomaticTof(connection);
        const auto activate=tunerPrelude()+payload.stateScript()+stageVariable+QStringLiteral(R"SH(
locks
quiescent
receivers_gone
supported || fail 'The on-board inference runtime is incomplete.'
known_driver(){ matches "$1" '@DRIVER@' || matches "$1" '290bf1a751fbaa3105a2f9caf5e6aeac867131cd2dbab1ee16e40394163f7521'; }
known_driver "$driver" || fail 'Unrecognized SPI driver; preserve it for review'
[ ! -e "$factory_driver" ] || known_driver "$factory_driver" || fail 'Unrecognized delivery driver on board'
matches "$native_capture" '@NATIVE_CAPTURE@' || matches "$native_capture" '5c1794dd1f1621aecb9d7185e275b8f61b14c69a5ebbc7a34a9020507c1ba8c4' || matches "$native_capture" '5eb12aac52e9386b6eafca53895999217b6325ee2381c35a09033a89507e9c4c' || matches "$native_capture" '667a20e511ef65e633054484ee3946af22a4416e0c48716eed0cd12ce52cb44a' || fail 'Unrecognized native capture program'
chmod 700 "$stage/tiny/tiny_nv12" "$stage/tiny/tiny_nv12_camera"
chmod 600 "$stage/tiny/model.dctsm"
valid_tiny "$stage/tiny" || fail 'Uploaded Tiny NV12 checksum mismatch'
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/tiny/tiny_nv12" check-protected "$stage/tiny/model.dctsm" > "$stage/tiny-check.log" 2>&1 || fail 'Tiny NV12 authenticated initialization failed'
grep -qx DCTS_PROTECTED_TINY_NV12_READY "$stage/tiny-check.log" || fail 'Tiny NV12 initialization receipt missing'
chmod 700 "$stage/tmf8829_ext" "$stage/tof_driver" "$stage/native_hd_gui"
matches "$stage/tmf8829_ext" '@DRIVER@' && matches "$stage/tof_driver" '@DRIVER@' && matches "$stage/native_hd_gui" '@NATIVE_CAPTURE@' || fail 'Uploaded sensor runtime checksum mismatch'
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/native_hd_gui" --native-capabilities > "$stage/native-check.log" || fail 'Native capture initialization failed'
chmod 700 "$stage/litesr_gui.sh" "$stage/rect/litesr_near_gui" "$stage/control.sh"
chmod 600 "$stage/rect/model.dctsm"
valid_rect "$stage/rect" || fail 'Uploaded rectangular LiteSR payload checksum mismatch'
matches "$stage/litesr_gui.sh" '%1' && sh -n "$stage/litesr_gui.sh" || fail 'Uploaded launcher differs'
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/rect/litesr_near_gui" check-protected "$stage/rect/model.dctsm" > "$stage/rect-check.log" 2>&1 ||
    fail 'The board failed to initialize rectangular LiteSR'
grep -qx DCTS_PROTECTED_LITESR_READY "$stage/rect-check.log" || fail 'Rectangular initialization receipt missing'
matches "$stage/control.sh" '%2' && sh -n "$stage/control.sh" || fail 'Candidate control upload differs'
matches "$control" '%3' || fail 'Candidate control changed before activation'
if [ -e "$tiny_bundle" ] || [ -L "$tiny_bundle" ]; then
    valid_tiny "$tiny_bundle" || fail 'Existing Tiny NV12 payload differs'
else
    mv "$stage/tiny" "$tiny_bundle"
fi
if [ -e "$rect_bundle" ] || [ -L "$rect_bundle" ]; then
    valid_rect "$rect_bundle" || fail 'Existing rectangular payload differs'
else
    mv "$stage/rect" "$rect_bundle"
fi
[ ! -L "$launcher" ] || fail 'Invalid existing LiteSR launcher'
if [ -e "$launcher" ] && ! matches "$launcher" '%1'; then
    [ -f "$launcher" ] || fail 'Invalid existing LiteSR launcher'
    previous=$(digest "$launcher")
    backup="$launcher.dcts-before-$previous"
    if [ ! -e "$backup" ]; then ln "$launcher" "$backup"; fi
    matches "$backup" "$previous" || fail 'Existing LiteSR launcher backup differs'
fi
if ! matches "$control" '%2'; then
    backup="$control.dcts-before-%3"
    if [ ! -e "$backup" ]; then ln "$control" "$backup"; fi
    matches "$backup" '%3' || fail 'Candidate control backup differs'
    mv -f "$stage/control.sh" "$control"
fi
install_status_file(){
    destination=$1; source=$2; expected=$3
    if matches "$destination" "$expected"; then return; fi
    previous=$(digest "$destination")
    backup="$destination.dcts-before-$previous"
    if [ ! -e "$backup" ]; then ln "$destination" "$backup"; fi
    matches "$backup" "$previous" || fail 'Sensor runtime backup differs'
    mv -f "$source" "$destination"
    matches "$destination" "$expected" || fail 'Sensor runtime activation differs'
}
install_status_file "$driver" "$stage/tmf8829_ext" '@DRIVER@'
if [ -e "$factory_driver" ]; then install_status_file "$factory_driver" "$stage/tof_driver" '@DRIVER@'; fi
install_status_file "$native_capture" "$stage/native_hd_gui" '@NATIVE_CAPTURE@'
mv -f "$stage/litesr_gui.sh" "$launcher"
[ "$(model_state)" = ready ] || fail 'Protected LiteSR activation could not be verified'
sync
echo ready
)SH").arg(payload.launcherSha,payload.controlSha,tunerHash(currentControl))
            .replace("@DRIVER@",payload.driverSha).replace("@NATIVE_CAPTURE@",payload.nativeCaptureSha);
        require(connection.execute(activate,30000).trimmed()=="ready","Protected LiteSR installation was not verified");
    } catch(...) {
        try {
            connection.execute("set -eu\n"+stageVariable+
                "rm -f \"$stage/tiny/tiny_nv12\" \"$stage/tiny/model.dctsm\" \"$stage/tiny/tiny_nv12_camera\" \"$stage/tiny-check.log\" \"$stage/native-check.log\" \"$stage/tmf8829_ext\" \"$stage/tof_driver\" \"$stage/native_hd_gui\" \"$stage/litesr_gui.sh\" \"$stage/rect-check.log\" \"$stage/control.sh\" \"$stage/rect/litesr_near_gui\" \"$stage/rect/model.dctsm\";  "
                "[ ! -d \"$stage/rect\" ] || rmdir \"$stage/rect\"; [ ! -d \"$stage/tiny\" ] || rmdir \"$stage/tiny\"; rmdir \"$stage\"");
        } catch(...) {}
        throw;
    }
    connection.execute("set -eu\n"+stageVariable+
        "rm -f \"$stage/tiny/tiny_nv12\" \"$stage/tiny/model.dctsm\" \"$stage/tiny/tiny_nv12_camera\" \"$stage/tiny-check.log\" \"$stage/native-check.log\" \"$stage/tmf8829_ext\" \"$stage/tof_driver\" \"$stage/native_hd_gui\" \"$stage/rect-check.log\" \"$stage/control.sh\" \"$stage/rect/litesr_near_gui\" \"$stage/rect/model.dctsm\";  "
        "[ ! -d \"$stage/rect\" ] || rmdir \"$stage/rect\"; [ ! -d \"$stage/tiny\" ] || rmdir \"$stage/tiny\"; rmdir \"$stage\"");
}

// Exercise the exact resource closure used by both fresh USB setup and upgrade.
// No network, process startup, camera access or device writes occur here.
QJsonArray boardPreparationSelfTest()
{
    QJsonArray checks;
    const auto check=[&](const QString &name,bool ok){require(ok,name);checks.append(QJsonObject{{"name",name},{"passed",true}});};
    const QDir runtime(QCoreApplication::applicationDirPath());
    const auto fresh=readFile(runtime.filePath("rv1103_bootstrap/session.sh"));
    const auto prepared=withAutomaticTof(fresh);
    check("fresh_board_preserves_saved_sensor_arguments",RvTuner::scriptSettings(fresh,true)==RvTuner::scriptSettings(prepared,true));
    check("fresh_board_preparation_idempotent",withAutomaticTof(prepared)==prepared);
    check("fresh_board_camera_only_in_host_mode",prepared.contains("if [ \"$1\" = 14 ]; then")&&!prepared.contains("if [ \"$1\" = 13 ]"));
    check("fresh_board_tiny_camera_lifecycle_present",prepared.count("# DCTS Tiny NV12 camera owner")==1);
    auto previous=prepared;previous.replace(automaticTofHook(),boardPayload("dtof-switch-v3.sh"));
    previous.replace("14|16|18)","13|14|16|17|18)");
    previous.replace("if [ \"$1\" = 14 ]; then","if [ \"$1\" = 13 ] || [ \"$1\" = 14 ] || [ \"$1\" = 16 ]; then");
    check("previous_three_model_session_upgrades_exactly",withAutomaticTof(previous)==prepared);
    for(int fov:{1,2}){
        const ProtectedLiteSrPayload payload(fov);const auto state=payload.stateScript();
        check(QString("preparation_f%1_hashes_resolve").arg(fov),state.contains(payload.rectExecutableSha)&&state.contains(payload.rectModelSha)&&state.contains(payload.launcherSha)&&state.contains(payload.controlSha)&&state.contains(payload.tinyExecutableSha)&&!state.contains(QRegularExpression("@[A-Z_]+@|%[1-9]")));
        check(QString("preparation_f%1_only_current_models").arg(fov),!state.contains("dcts-litesr-protected-v3")&&!state.contains("dcts-litesr-128x128")&&!payload.launcher.contains(" 13)")&&!payload.launcher.contains(" 17)")&&payload.launcher.contains(" 16)")&&payload.launcher.contains(" 18)"));
        check(QString("preparation_f%1_encrypted_model_closure").arg(fov),payload.rectModel.size()>96&&payload.tinyModel.size()>96&&!payload.rectExecutable.isEmpty()&&!payload.nativeCapture.isEmpty()&&!payload.tinyCamera.isEmpty());
    }
    const auto installer=readFile(runtime.filePath("rv1103_bootstrap/install.sh"));
    check("fresh_import_checks_both_models_before_activation",installer.contains("dcts-tiny-nv12-128x80-v1/tiny_nv12\" check-protected")&&installer.contains("dcts-litesr-384x256-v3-depth-only/litesr_near_gui\" check-protected")&&!installer.contains("dcts-litesr-128x128")&&!installer.contains("dcts-litesr-protected-v3"));
    check("fresh_import_retains_rollback",installer.contains("committed=0")&&installer.contains("if [ \"$committed\" = 0 ]; then")&&installer.contains("installed_candidate=1"));
    return checks;
}
