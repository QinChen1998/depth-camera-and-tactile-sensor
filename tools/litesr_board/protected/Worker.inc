// Included by RvControlWorker.cpp inside its private namespace.
const QString BoardCandidateDir = "/root/dtof_candidates_20261009";

struct ProtectedLiteSrPayload {
    QByteArray executable, model, launcher, rectExecutable, rectModel, control, native16Executable, native16Model, driver, nativeCapture;
    QString executableSha, modelSha, launcherSha, rectExecutableSha, rectModelSha, controlSha, native16ExecutableSha, native16ModelSha, driverSha, nativeCaptureSha;

    explicit ProtectedLiteSrPayload(int fov)
    {
        require(fov==1||fov==2,"Protected LiteSR requires a supported device calibration");
        executable=boardPayload(QString("litesr-fov%1.bin").arg(fov));
        model=boardPayload("model.dctsm");
        launcher=boardPayload("litesr_gui.sh");
        executableSha=tunerHash(executable);
        modelSha=tunerHash(model);
        launcher.replace("@BINARY_SHA@",executableSha.toLatin1());
        launcher.replace("@MODEL_SHA@",modelSha.toLatin1());
        rectExecutable=boardPayload(QString("rect-litesr-fov%1.bin").arg(fov));
        rectModel=boardPayload("rect-model.dctsm");
        rectExecutableSha=tunerHash(rectExecutable);rectModelSha=tunerHash(rectModel);
        launcher.replace("@RECT_BINARY_SHA@",rectExecutableSha.toLatin1());
        launcher.replace("@RECT_MODEL_SHA@",rectModelSha.toLatin1());
        native16Executable=boardPayload(QString("native16-litesr-fov%1.bin").arg(fov));
        native16Model=boardPayload("native16-model.dctsm");
        native16ExecutableSha=tunerHash(native16Executable);native16ModelSha=tunerHash(native16Model);
        launcher.replace("@NATIVE16_BINARY_SHA@",native16ExecutableSha.toLatin1());
        launcher.replace("@NATIVE16_MODEL_SHA@",native16ModelSha.toLatin1());
        driver=readFile(QDir(QCoreApplication::applicationDirPath()).filePath("rv1103_bootstrap/tof_driver"),4*1024*1024);
        nativeCapture=boardPayload(QString("native-fov%1.bin").arg(fov));
        driverSha=tunerHash(driver);nativeCaptureSha=tunerHash(nativeCapture);
        control=readFile(QDir(QCoreApplication::applicationDirPath()).filePath("rv1103_bootstrap/candidate-control.sh"));
        controlSha=tunerHash(control);
        launcherSha=tunerHash(launcher);
    }

    QString stateScript() const
    {
        return QStringLiteral(R"SH(
dir=/root/dtof_candidates_20261009
bundle="$dir/dcts-litesr-protected-v3-info10"
rect_bundle="$dir/dcts-litesr-384x256-v2-info10"
native16_bundle="$dir/dcts-litesr-128x128-v1"
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
valid_bundle(){
    [ -d "$1" ] && [ ! -L "$1" ] &&
    [ -x "$1/litesr_near_gui" ] && matches "$1/litesr_near_gui" '%1' &&
    matches "$1/model.dctsm" '%2'
}
valid_rect(){
    [ -d "$1" ] && [ ! -L "$1" ] &&
    [ -x "$1/litesr_near_gui" ] && matches "$1/litesr_near_gui" '%4' &&
    matches "$1/model.dctsm" '%5'
}
valid_native16(){
    [ -d "$1" ] && [ ! -L "$1" ] &&
    [ -x "$1/litesr_near_gui" ] && matches "$1/litesr_near_gui" '@NATIVE16_EXE@' &&
    matches "$1/model.dctsm" '@NATIVE16_MODEL@'
}
status_ready(){
    matches "$driver" '@DRIVER@' && matches "$native_capture" '@NATIVE_CAPTURE@' &&
    { [ ! -e "$factory_driver" ] || matches "$factory_driver" '@DRIVER@'; }
}
model_state(){
    if ! supported; then echo runtime_missing
    elif matches "$launcher" '%3'; then
        if [ -x "$launcher" ] && valid_bundle "$bundle" && valid_rect "$rect_bundle" && valid_native16 "$native16_bundle" && status_ready && matches "$control" '%6'; then echo ready; else echo mismatch; fi
    elif [ -L "$launcher" ] || { [ -e "$launcher" ] && [ ! -f "$launcher" ]; }; then echo mismatch
    elif [ -f "$launcher" ] && grep -q '^# DCTS protected LiteSR' "$launcher"; then echo mismatch
    else echo missing
    fi
}
)SH").arg(executableSha,modelSha,launcherSha,rectExecutableSha,rectModelSha,controlSha)
            .replace("@NATIVE16_EXE@",native16ExecutableSha).replace("@NATIVE16_MODEL@",native16ModelSha)
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
    connection.execute("set -eu\numask 077\n"+stageVariable+"mkdir \"$stage\" \"$stage/payload\" \"$stage/rect\" \"$stage/native16\"");
    try {
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/payload/litesr_near_gui\"",30000,payload.executable);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/payload/model.dctsm\"",30000,payload.model);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/litesr_gui.sh\"",30000,payload.launcher);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/rect/litesr_near_gui\"",30000,payload.rectExecutable);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/rect/model.dctsm\"",30000,payload.rectModel);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/control.sh\"",30000,payload.control);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/native16/litesr_near_gui\"",30000,payload.native16Executable);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/native16/model.dctsm\"",30000,payload.native16Model);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tmf8829_ext\"",30000,payload.driver);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/tof_driver\"",30000,payload.driver);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/native_hd_gui\"",30000,payload.nativeCapture);
        const auto currentControl=tunerRead(connection,BoardCandidateDir+"/control.sh");
        auto priorControl=payload.control;priorControl.replace("13|14|16|17)","13|14)");
        auto previousControl=payload.control;previousControl.replace("13|14|16|17)","13|14|16)");
        require(currentControl==payload.control||currentControl==priorControl||currentControl==previousControl,"Unrecognized candidate control script; preserve it for review");
        installAutomaticTof(connection);
        const auto activate=tunerPrelude()+payload.stateScript()+stageVariable+QStringLiteral(R"SH(
locks
quiescent
receivers_gone
supported || fail 'The on-board inference runtime is incomplete.'
known_driver(){ matches "$1" '@DRIVER@' || matches "$1" '290bf1a751fbaa3105a2f9caf5e6aeac867131cd2dbab1ee16e40394163f7521'; }
known_driver "$driver" || fail 'Unrecognized SPI driver; preserve it for review'
[ ! -e "$factory_driver" ] || known_driver "$factory_driver" || fail 'Unrecognized delivery driver on board'
matches "$native_capture" '@NATIVE_CAPTURE@' || matches "$native_capture" '667a20e511ef65e633054484ee3946af22a4416e0c48716eed0cd12ce52cb44a' || fail 'Unrecognized native capture program'
chmod 700 "$stage/native16/litesr_near_gui" "$stage/tmf8829_ext" "$stage/tof_driver" "$stage/native_hd_gui"
chmod 600 "$stage/native16/model.dctsm"
valid_native16 "$stage/native16" || fail 'Uploaded Native16 LiteSR checksum mismatch'
matches "$stage/tmf8829_ext" '@DRIVER@' && matches "$stage/tof_driver" '@DRIVER@' && matches "$stage/native_hd_gui" '@NATIVE_CAPTURE@' || fail 'Uploaded sensor runtime checksum mismatch'
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/native16/litesr_near_gui" check-protected "$stage/native16/model.dctsm" > "$stage/native16-check.log" 2>&1 || fail 'The board failed to initialize Native16 LiteSR'
grep -qx DCTS_PROTECTED_LITESR_READY "$stage/native16-check.log" || fail 'Native16 initialization receipt missing'
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/native_hd_gui" --native-capabilities > "$stage/native-check.log" || fail 'Native capture initialization failed'
chmod 700 "$stage/payload/litesr_near_gui" "$stage/litesr_gui.sh"
chmod 600 "$stage/payload/model.dctsm"
chmod 700 "$stage/rect/litesr_near_gui" "$stage/control.sh"
chmod 600 "$stage/rect/model.dctsm"
valid_rect "$stage/rect" || fail 'Uploaded rectangular LiteSR payload checksum mismatch'
valid_bundle "$stage/payload" || fail 'Uploaded protected LiteSR payload checksum mismatch'
matches "$stage/litesr_gui.sh" '%1' || fail 'Uploaded protected LiteSR launcher checksum mismatch'
sh -n "$stage/litesr_gui.sh"
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/payload/litesr_near_gui"     check-protected "$stage/payload/model.dctsm" > "$stage/check.log" 2>&1 ||
    fail 'The board failed to initialize the protected LiteSR model'
grep -qx DCTS_PROTECTED_LITESR_READY "$stage/check.log" || fail 'Protected LiteSR initialization receipt missing'
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/rect/litesr_near_gui" check-protected "$stage/rect/model.dctsm" > "$stage/rect-check.log" 2>&1 ||
    fail 'The board failed to initialize rectangular LiteSR'
grep -qx DCTS_PROTECTED_LITESR_READY "$stage/rect-check.log" || fail 'Rectangular initialization receipt missing'
matches "$stage/control.sh" '%2' && sh -n "$stage/control.sh" || fail 'Candidate control upload differs'
matches "$control" '%3' || fail 'Candidate control changed before activation'
if [ -e "$native16_bundle" ] || [ -L "$native16_bundle" ]; then
    valid_native16 "$native16_bundle" || fail 'Existing Native16 payload differs'
else
    mv "$stage/native16" "$native16_bundle"
fi
if [ -e "$rect_bundle" ] || [ -L "$rect_bundle" ]; then
    valid_rect "$rect_bundle" || fail 'Existing rectangular payload differs'
else
    mv "$stage/rect" "$rect_bundle"
fi
if [ -e "$bundle" ] || [ -L "$bundle" ]; then
    valid_bundle "$bundle" || fail 'Existing protected LiteSR payload differs'
else
    mv "$stage/payload" "$bundle"
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
                "rm -f \"$stage/native16/litesr_near_gui\" \"$stage/native16/model.dctsm\" \"$stage/native16-check.log\" \"$stage/native-check.log\" \"$stage/tmf8829_ext\" \"$stage/tof_driver\" \"$stage/native_hd_gui\" \"$stage/payload/litesr_near_gui\" \"$stage/payload/model.dctsm\" \"$stage/litesr_gui.sh\" \"$stage/check.log\" \"$stage/rect-check.log\" \"$stage/control.sh\" \"$stage/rect/litesr_near_gui\" \"$stage/rect/model.dctsm\";  "
                "[ ! -d \"$stage/payload\" ] || rmdir \"$stage/payload\"; [ ! -d \"$stage/rect\" ] || rmdir \"$stage/rect\"; [ ! -d \"$stage/native16\" ] || rmdir \"$stage/native16\"; rmdir \"$stage\"");
        } catch(...) {}
        throw;
    }
    connection.execute("set -eu\n"+stageVariable+
        "rm -f \"$stage/native16/litesr_near_gui\" \"$stage/native16/model.dctsm\" \"$stage/native16-check.log\" \"$stage/native-check.log\" \"$stage/tmf8829_ext\" \"$stage/tof_driver\" \"$stage/native_hd_gui\" \"$stage/payload/litesr_near_gui\" \"$stage/payload/model.dctsm\" \"$stage/check.log\" \"$stage/rect-check.log\" \"$stage/control.sh\" \"$stage/rect/litesr_near_gui\" \"$stage/rect/model.dctsm\";  "
        "[ ! -d \"$stage/payload\" ] || rmdir \"$stage/payload\"; [ ! -d \"$stage/rect\" ] || rmdir \"$stage/rect\"; [ ! -d \"$stage/native16\" ] || rmdir \"$stage/native16\"; rmdir \"$stage\"");
}
