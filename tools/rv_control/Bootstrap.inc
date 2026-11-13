
int prepareFactoryCamera(Ssh &connection, const QDir &runtime)
{
    const auto script=readFile(runtime.filePath("rv1103_bootstrap/camera-takeover.sh"));
    const auto result=connection.execute(tunerPrelude()+"\n"+QString::fromUtf8(script)
        +"\nlocks;health;quiescent;receivers_gone\ndcts_release_camera_helpers\nprintf '%s\\n' \"$DCTS_CAMERA_HELPERS_RELEASED\"\n",20000).trimmed();
    bool ok=false;const int count=result.toInt(&ok);
    require(ok&&count>=0&&count<=32,"Invalid factory camera takeover receipt");
    return count;
}

// Read-only engineering inspection also uses the pinned tuner runtime. Provision
// its complete closure while idle; never replace an unknown existing directory.
void ensureTunerRuntime(Ssh &connection, const QDir &runtime)
{
    const QString destination="/root/dtof_tuner";
    const QStringList names{"session.sh","tmf8829_tuner","bounded_log.awk","LICENSE-MIT.TXT","provenance.json"};
    QList<QByteArray> contents;QString checks;
    for(const auto &name:names){
        const auto data=readFile(runtime.filePath("dtof_tuner/"+name));
        require(!data.isEmpty(),"Empty bundled dToF inspection asset");contents.append(data);
        // Existing installations may retain their original license/provenance
        // records. Execution always requires the exact three runtime assets.
        if(name!="LICENSE-MIT.TXT"&&name!="provenance.json")
            checks+=QString("[ -f \"$dir/%1\" ] && [ ! -L \"$dir/%1\" ] && [ \"$(sha256sum \"$dir/%1\" | cut -d' ' -f1)\" = %2 ] &&\n")
                .arg(name,tunerHash(data));
    }
    const auto state=connection.execute("dir="+shellQuote(destination)+"\n"
        "if [ -d \"$dir\" ] && [ ! -L \"$dir\" ] &&\n"+checks+"true; then echo ready\n"
        "elif [ ! -e \"$dir\" ] && [ ! -L \"$dir\" ]; then echo missing\nelse echo mismatch; fi").trimmed();
    if(state=="ready")return;
    require(state=="missing","The existing dToF inspection runtime differs; preserve it for review");
    connection.execute(tunerPrelude()+"\nhealth;quiescent;receivers_gone\n");
    const auto stage="/root/.dcts-tuner-"+QUuid::createUuid().toString(QUuid::Id128);
    const auto quoted=shellQuote(stage);
    connection.execute("set -eu; umask 077; mkdir "+quoted);
    try{
        for(qsizetype i=0;i<names.size();++i){
            const auto file=shellQuote(stage+"/"+names[i]);
            const auto digest=connection.execute("set -eu; cat > "+file+"; chmod "
                +(names[i]=="session.sh"||names[i]=="tmf8829_tuner"?"700 ":"600 ")+file+"; sha256sum "+file,30000,contents[i]);
            require(digest.simplified().split(' ').value(0)==tunerHash(contents[i]).toLatin1(),
                    "Uploaded dToF inspection runtime checksum mismatch");
        }
        connection.execute(tunerPrelude()+"\nlocks;health;quiescent;receivers_gone\n"
            "[ ! -e "+shellQuote(destination)+" ] && [ ! -L "+shellQuote(destination)+" ] || fail 'dToF runtime appeared during setup'\n"
            "sh -n "+shellQuote(stage+"/session.sh")+"\nmv "+quoted+" "+shellQuote(destination)+"\nsync\n");
    }catch(...){
        try{connection.execute("rm -rf "+quoted);}catch(...){}
        throw;
    }
}

struct UsbRuntimePlan {
    struct File { QString relative; QByteArray bytes; bool executable; };
    QList<File> files;
    QString checks;
};

// Inspection and installation must compare the same complete payload, including
// scripts patched with the board's saved sensor settings. This only reads files.
template<class Connection>
UsbRuntimePlan usbRuntimePlan(Connection &connection, const QDir &runtime)
{
    require(device.fov()==RvCalibration::Fov,"USB setup requires the current calibration");
    UsbRuntimePlan plan;
    auto &files=plan.files;
    const auto readExisting=[&](const QString &path){
        return connection.execute(QString("if [ -f %1 ] && [ ! -L %1 ]; then cat %1; fi").arg(shellQuote(path)));
    };
    const auto preservedScript=[&](QByteArray script,const QString &path,bool candidate){
        const auto previous=readExisting(path);
        if(!previous.isEmpty()) {
            RvTuner::Settings settings;
            try {
                settings=RvTuner::scriptSettings(previous,candidate);
            } catch(const std::exception &) { return script; }
            if(!(settings==RvTuner::scriptSettings(script,candidate)))script=RvTuner::patchScript(script,settings,candidate);
        }
        return script;
    };
    const NativeCapturePayload capture(device.fov());
    for(const auto &asset:bootstrapAssets) {
        auto bytes=readFile(runtime.filePath(QString("rv1103_bootstrap/")+asset.name));
        const QString relative=QString::fromLatin1(asset.destination);
        if(relative=="depth/M48_geometry.bin") {
            require(tunerHash(bytes)==device.geometrySha(),"The matching board calibration is unavailable; no files were replaced");
        } else if(relative=="candidate/session.sh")bytes=preservedScript(withAutomaticTof(bytes),CandidateSession,true);
        else if(relative=="depth/service.sh")bytes=preservedScript(bytes,"/root/r144_depth/service.sh",false);
        files.append({relative,bytes,asset.executable});
    }
    for(const auto *name:{"calibration.json","intrinsics.json","extrinsics.json","projection.bin","M48_geometry.bin","manifest.json","host_contract.json"})
        files.append({QString("depth/calibration_current/")+name,readFile(runtime.filePath(QString("rv1103_bootstrap/calibration/")+name)),false});
    files.append({"depth/spi_native16.enabled",device.serial.toLatin1()+"\n",false});
    files.append({"candidate/native_hd_gui",capture.nativeCapture,true});
    files.append({"candidate/native_gui.sh",capture.launcher,true});
    for(const auto &name:{"session.sh","tmf8829_tuner","bounded_log.awk","LICENSE-MIT.TXT","provenance.json"})
        files.append({QString("tuner/")+name,readFile(runtime.filePath(QString("dtof_tuner/")+name)),QString(name)=="session.sh"||QString(name)=="tmf8829_tuner"});
    const auto boardPath=[](const QString &relative){
        if(relative=="S99zdepth")return QString("/etc/init.d/S99zdepth");
        if(relative.startsWith("depth/"))return QString("/root/r144_depth/")+relative.mid(6);
        if(relative.startsWith("candidate/"))return BoardCandidateDir+"/"+relative.mid(10);
        return QString("/root/dtof_tuner/")+relative.mid(6);
    };
    auto &checks=plan.checks;
    checks="set -eu\nready=1\ncheck(){ [ -f \"$1\" ] && [ ! -L \"$1\" ] && [ \"$(sha256sum \"$1\" | cut -d' ' -f1)\" = \"$2\" ] && { [ \"$3\" = 0 ] || [ -x \"$1\" ]; } || ready=0; }\n";
    for(const auto &file:files)checks+=QString("check %1 %2 %3\n").arg(shellQuote(boardPath(file.relative)),tunerHash(file.bytes),file.executable?"1":"0");
    checks+=QString("if [ -e /root/rv1103_mw_spi/bin/tof_driver ] || [ -L /root/rv1103_mw_spi/bin/tof_driver ]; then check /root/rv1103_mw_spi/bin/tof_driver %1 1; fi\n").arg(capture.driverSha);
    checks+="for path in /root/r144_depth /root/dtof_candidates_20261009 /root/dtof_tuner /root/rv1103_mw_spi /root/rv1103_mw_spi/bin; do [ ! -L \"$path\" ] || ready=0; done\n"
        "grep -q '^[[:space:]]*\\[ ! -f /root/r144_depth/spi_native16.enabled \\] || return 0$' /oem/usr/bin/RkLunch.sh || ready=0\nprintf '%s\\n' \"$ready\"\n";
    return plan;
}

template<class Connection>
bool usbRuntimeMatches(Connection &connection, const QDir &runtime)
{
    const auto state=connection.execute(usbRuntimePlan(connection,runtime).checks).trimmed();
    require(state=="0"||state=="1","Invalid complete USB runtime inspection receipt");
    return state=="1";
}

bool matchesNativeCaptureCapabilities(const QByteArray &receipt, int fov)
{
    const auto capabilities = QJsonDocument::fromJson(receipt).object();
    return capabilities["native_capture_protocol"] == RvCaptureContract::NativeCaptureProtocolV2
        && capabilities["fov"] == fov && capabilities["m8"] == false
        && capabilities["m16"] == true && capabilities["m48"] == true
        && capabilities["model_inference"] == false && capabilities["photon_enabled"] == false;
}

bool ensureUsbRuntime(Ssh &connection, const QDir &runtime, const QString &boot)
{
    const auto plan=usbRuntimePlan(connection,runtime);
    const auto &files=plan.files;
    const auto &checks=plan.checks;
    const auto state=connection.execute(checks).trimmed();
    require(state=="0"||state=="1","Invalid complete USB runtime inspection receipt");
    if(state=="1")return false;
    const auto stage="/root/.dcts-setup-"+QUuid::createUuid().toString(QUuid::Id128);
    const auto quoted=shellQuote(stage);
    const auto mode=connection.execute("if [ -e /root/r144_depth ] || [ -L /root/r144_depth ] || [ -e /root/dtof_candidates_20261009 ] || [ -L /root/dtof_candidates_20261009 ] || [ -e /root/dtof_tuner ] || [ -e /etc/init.d/S99zdepth ] || [ -e /oem/usr/bin/RkLunch.sh.dcts-before-bootstrap ] || [ -e /run/r144_depth ] || [ -e /run/dtof_candidates ]; then echo repair; else echo fresh; fi").trimmed();
    require(mode=="repair"||mode=="fresh","Invalid board installation state");
    const auto command=[&](const QString &action) {
        return QString("sh -s -- %1 %2 %3 %4 %5 %6")
            .arg(action,shellQuote(device.serial),shellQuote(boot),quoted,QString::fromLatin1(mode)).arg(device.fov());
    };
    auto setup=readFile(runtime.filePath("rv1103_bootstrap/install.sh"));
    setup.replace("@SPI_DRIVER_SHA@",sha(runtime.filePath("rv1103_bootstrap/tof_driver")).toLatin1());
    require(connection.execute(command("preflight"),10000,setup).trimmed()=="ready",
            "The RV1103 board setup preflight failed");
    const auto upload=[&](const QString &name, const QByteArray &data, bool executable) {
        const auto file=shellQuote(stage+"/"+name);
        require(!data.isEmpty(),"Empty RV1103 setup asset");
        const auto digest=connection.execute(QString("set -eu; umask 077; cat > %1; chmod %2 %1; sha256sum %1")
            .arg(file,executable?"700":"600"),30000,data);
        require(digest.simplified().split(' ').value(0)==tunerHash(data).toLatin1(),
                "Uploaded RV1103 setup asset checksum mismatch");
    };
    connection.execute("set -eu; umask 077; mkdir "+quoted+"; cd "+quoted
        +"; mkdir depth candidate tuner; mkdir depth/calibration_current");
    QString phase;
    try {
        QByteArray manifest;
        for(const auto &file:files) {
            phase="upload "+file.relative;
            upload(file.relative,file.bytes,file.executable);
            manifest+=tunerHash(file.bytes).toLatin1()+"  "+file.relative.toLatin1()+"\n";
        }
        upload("files.sha256",manifest,false);
        phase="staged capture capability check";
        const auto capabilities = connection.execute(
            "LD_LIBRARY_PATH=" + shellQuote(stage + "/depth:/oem/usr/lib:/usr/lib") + " "
            + shellQuote(stage + "/candidate/native_hd_gui") + " --native-capabilities"
            " || { echo 'DTOF_ERROR: The Native16 capture program could not start on this firmware.'; exit 2; }");
        require(matchesNativeCaptureCapabilities(capabilities, device.fov()),
                "The native capture program did not report the required F6, Native16 and Native48 capabilities.");
        // Bind activation to the exact receipt already parsed on the host.
        setup.replace("@NATIVE_CAPABILITIES_SHA@", tunerHash(capabilities).toLatin1());
        phase="board preflight, capture check and activation";
        require(connection.execute(command("activate"), 75000, setup) == capabilities,
                "The activated native capture capability receipt differs from the validated receipt.");
    } catch (const std::exception &error) {
        // Keep staging and rollback evidence on failure, including interrupted
        // SSH uploads. Never delete the last copy of an earlier deployment.
        throw Error(QString("RV1103 setup (%1): %2").arg(phase,QString::fromUtf8(error.what())));
    }
    require(connection.execute(checks).trimmed()=="1","USB runtime readback differs after installation");
    connection.execute("rm -rf "+quoted);
    return true;
}

QJsonArray usbRuntimeInspectionSelfTest()
{
    // A closed fake SSH transport exercises the production payload builder and
    // inspection parser. No socket, credentials or device command is opened.
    struct Fixture {
        QByteArray geometry,session,service,state="1";
        QString checks;
        QByteArray execute(const QString &command) {
            if(command.startsWith("set -eu\nready=1\n")) { checks=command;return state; }
            if(command.contains("sha256sum"))return tunerHash(geometry).toLatin1()+"  geometry\n";
            if(command.contains("M48_geometry.bin"))return geometry;
            if(command.contains(CandidateSession))return session;
            if(command.contains("/root/r144_depth/service.sh"))return service;
            throw std::runtime_error("Unexpected command in offline USB inspection");
        }
    } fixture;
    const QDir runtime(QCoreApplication::applicationDirPath());
    const auto savedDevice=device;
    device=RvDeviceProfile();device.calibration=QLatin1String(RvCalibration::Id);
    device.serial="1234567890abcdef";
    fixture.geometry=readFile(runtime.filePath("rv1103_bootstrap/M48_geometry.bin"));
    fixture.session=readFile(runtime.filePath("rv1103_bootstrap/session.sh"));
    fixture.service=readFile(runtime.filePath("rv1103_bootstrap/service.sh"));
    auto settings=RvTuner::scriptSettings(fixture.session,true);
    settings.periodMs=91;
    fixture.session=RvTuner::patchScript(fixture.session,settings,true);
    QJsonArray checks;
    const auto check=[&](const QString &name,bool ok){require(ok,name);checks.append(QJsonObject{{"name",name},{"passed",true}});};
    const QJsonObject baseCapabilities{
        {"native_capture_protocol", RvCaptureContract::NativeCaptureProtocolV2}, {"fov", 6},
        {"model_inference", false}, {"photon_enabled", false}, {"m8", false}, {"m16", true}, {"m48", true}};
    auto extended = baseCapabilities;
    extended["native_hole_fill_protocol"] = "native48-hole-fill-v1";
    check("usb_native_capabilities_accept_base_and_extended_receipts",
          matchesNativeCaptureCapabilities(QJsonDocument(baseCapabilities).toJson(QJsonDocument::Compact), 6)
          && matchesNativeCaptureCapabilities(QJsonDocument(extended).toJson(), 6));
    const auto changedCapabilityRejected = [&](const QString &key, const QJsonValue &value) {
        auto changed = extended;
        changed[key] = value;
        return !matchesNativeCaptureCapabilities(QJsonDocument(changed).toJson(), 6);
    };
    check("usb_native_capabilities_reject_malformed_and_wrong_required_fields",
          !matchesNativeCaptureCapabilities("{\"fov\":6", 6)
          && changedCapabilityRejected("native_capture_protocol", "unknown")
          && changedCapabilityRejected("fov", 2)
          && changedCapabilityRejected("m8", true)
          && changedCapabilityRejected("m16", false)
          && changedCapabilityRejected("m48", QStringLiteral("true"))
          && changedCapabilityRejected("model_inference", true)
          && changedCapabilityRejected("photon_enabled", QJsonValue(QJsonValue::Undefined)));
    const auto plan=usbRuntimePlan(fixture,runtime);
    check("usb_complete_runtime_match_accepted",usbRuntimeMatches(fixture,runtime));
    fixture.state="0";
    check("usb_complete_runtime_mismatch_requires_setup",!usbRuntimeMatches(fixture,runtime));
    bool rejected=false;fixture.state="ready";
    try { usbRuntimeMatches(fixture,runtime); } catch(const std::exception &) { rejected=true; }
    check("usb_incomplete_inspection_receipt_rejected",rejected);
    bool hashes=true,preserved=false;
    QStringList paths;
    for(const auto &file:plan.files) {
        hashes &= !file.bytes.isEmpty() && plan.checks.contains(tunerHash(file.bytes));
        paths.append(file.relative);
        if(file.relative=="candidate/session.sh")preserved=RvTuner::scriptSettings(file.bytes,true)==settings;
    }
    const auto total=paths.size();paths.removeDuplicates();
    check("usb_inspection_checks_every_installation_asset",hashes&&total==paths.size()&&total>=20);
    for(const auto &path:QStringList{"S99zdepth","depth/service.sh","depth/M48_geometry.bin",
            "candidate/session.sh","candidate/native_hd_gui","candidate/native_gui.sh",
            "tuner/session.sh","tuner/tmf8829_tuner","tuner/provenance.json"})
        check("usb_inspection_covers_"+path,paths.contains(path));
    check("usb_inspection_preserves_saved_sensor_settings",preserved);
    check("usb_inspection_checks_factory_startup_and_symlinks",plan.checks.contains("RkLunch.sh")&&plan.checks.contains("[ ! -L"));
    check("usb_inspection_issues_no_installation_commands",!fixture.checks.contains("mkdir ")
        &&!fixture.checks.contains("chmod ")&&!fixture.checks.contains("mv ")&&!fixture.checks.contains("rm "));
    Credentials credentials;credentials.factoryUsbLogin();
    check("usb_missing_saved_login_uses_factory_without_prompt",credentials.username=="root"&&credentials.persist&&!credentials.password.isEmpty());
    device.username="other";rejected=false;
    try { Credentials unsupported;unsupported.factoryUsbLogin(); } catch(const std::exception &) { rejected=true; }
    check("usb_nonfactory_login_without_saved_secret_fails_without_prompt",rejected);
    device=savedDevice;
    return checks;
}

