// Included by RvControlWorker.cpp inside its private namespace.
const QString BoardCandidateDir = "/root/dtof_candidates_20261009";

struct ProtectedLiteSrPayload {
    QByteArray executable, model, launcher;
    QString executableSha, modelSha, launcherSha;

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
        launcherSha=tunerHash(launcher);
    }

    QString stateScript() const
    {
        return QStringLiteral(R"SH(
dir=/root/dtof_candidates_20261009
bundle="$dir/dcts-litesr-protected-v2"
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
model_state(){
    if ! supported; then echo runtime_missing
    elif matches "$launcher" '%3'; then
        if [ -x "$launcher" ] && valid_bundle "$bundle"; then echo ready; else echo mismatch; fi
    elif [ -L "$launcher" ] || { [ -e "$launcher" ] && [ ! -f "$launcher" ]; }; then echo mismatch
    elif [ -f "$launcher" ] && grep -q '^# DCTS protected LiteSR' "$launcher"; then echo mismatch
    else echo missing
    fi
}
)SH").arg(executableSha,modelSha,launcherSha);
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
    connection.execute("set -eu\numask 077\n"+stageVariable+"mkdir \"$stage\" \"$stage/payload\"");
    try {
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/payload/litesr_near_gui\"",30000,payload.executable);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/payload/model.dctsm\"",30000,payload.model);
        connection.execute("set -eu\n"+stageVariable+"cat > \"$stage/litesr_gui.sh\"",30000,payload.launcher);
        installAutomaticTof(connection);
        const auto activate=tunerPrelude()+payload.stateScript()+stageVariable+QStringLiteral(R"SH(
locks
quiescent
receivers_gone
supported || fail 'The on-board inference runtime is incomplete.'
chmod 700 "$stage/payload/litesr_near_gui" "$stage/litesr_gui.sh"
chmod 600 "$stage/payload/model.dctsm"
valid_bundle "$stage/payload" || fail 'Uploaded protected LiteSR payload checksum mismatch'
matches "$stage/litesr_gui.sh" '%1' || fail 'Uploaded protected LiteSR launcher checksum mismatch'
sh -n "$stage/litesr_gui.sh"
LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib "$stage/payload/litesr_near_gui"     check-protected "$stage/payload/model.dctsm" > "$stage/check.log" 2>&1 ||
    fail 'The board failed to initialize the protected LiteSR model'
grep -qx DCTS_PROTECTED_LITESR_READY "$stage/check.log" || fail 'Protected LiteSR initialization receipt missing'
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
mv -f "$stage/litesr_gui.sh" "$launcher"
[ "$(model_state)" = ready ] || fail 'Protected LiteSR activation could not be verified'
sync
echo ready
)SH").arg(payload.launcherSha);
        require(connection.execute(activate,30000).trimmed()=="ready","Protected LiteSR installation was not verified");
    } catch(...) {
        try {
            connection.execute("set -eu\n"+stageVariable+
                "rm -f \"$stage/payload/litesr_near_gui\" \"$stage/payload/model.dctsm\" \"$stage/litesr_gui.sh\" \"$stage/check.log\"; "
                "[ ! -d \"$stage/payload\" ] || rmdir \"$stage/payload\"; rmdir \"$stage\"");
        } catch(...) {}
        throw;
    }
    connection.execute("set -eu\n"+stageVariable+
        "rm -f \"$stage/payload/litesr_near_gui\" \"$stage/payload/model.dctsm\" \"$stage/check.log\"; "
        "[ ! -d \"$stage/payload\" ] || rmdir \"$stage/payload\"; rmdir \"$stage\"");
}
