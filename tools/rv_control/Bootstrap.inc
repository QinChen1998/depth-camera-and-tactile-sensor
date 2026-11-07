
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

bool ensureUsbRuntime(Ssh &connection, const QDir &runtime, const QString &boot)
{
    require(device.fov()==1||device.fov()==2,"USB setup requires a supported calibration");
    const auto geometry=connection.execute("if [ -f /root/r144_depth/M48_geometry.bin ] && [ ! -L /root/r144_depth/M48_geometry.bin ]; then sha256sum /root/r144_depth/M48_geometry.bin; fi").simplified().split(' ').value(0);
    require(geometry!=QByteArray("83b49ace957a4f70982380c6f22a7c39236764ff7311232d03724e85273324db")
            &&geometry!=QByteArray("54802b37740d0075d8a9da95a0dcb5fde27b2f1e20249312a9f2325280c51729")
            ||geometry==device.geometrySha().toLatin1(),"USB calibration conflicts with the saved device profile");
    struct File { QString relative; QByteArray bytes; bool executable; };
    QList<File> files;
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
    const ProtectedLiteSrPayload liteSr(device.fov());
    for(const auto &asset:bootstrapAssets) {
        auto bytes=readFile(runtime.filePath(QString("rv1103_bootstrap/")+asset.name));
        const QString relative=QString::fromLatin1(asset.destination);
        if(relative=="depth/M48_geometry.bin") {
            if(device.fov()==2)bytes=readExisting("/root/r144_depth/M48_geometry.bin");
            require(tunerHash(bytes)==device.geometrySha(),"The matching board calibration is unavailable; no files were replaced");
        } else if(relative=="candidate/session.sh")bytes=preservedScript(withAutomaticTof(bytes),CandidateSession,true);
        else if(relative=="depth/service.sh")bytes=preservedScript(bytes,"/root/r144_depth/service.sh",false);
        else if(relative=="candidate/native_hd_gui")bytes=liteSr.nativeCapture;
        files.append({relative,bytes,asset.executable});
    }
    files.append({"depth/spi_native16.enabled",device.serial.toLatin1()+"\n",false});
    files.append({"candidate/litesr_gui.sh",liteSr.launcher,true});
    files.append({"candidate/dcts-litesr-384x256-v3-depth-only/litesr_near_gui",liteSr.rectExecutable,true});
    files.append({"candidate/dcts-litesr-384x256-v3-depth-only/model.dctsm",liteSr.rectModel,false});
    files.append({"candidate/dcts-tiny-nv12-128x80-v1/tiny_nv12",liteSr.tinyExecutable,true});
    files.append({"candidate/dcts-tiny-nv12-128x80-v1/model.dctsm",liteSr.tinyModel,false});
    files.append({"candidate/dcts-tiny-nv12-128x80-v1/tiny_nv12_camera",liteSr.tinyCamera,true});
    for(const auto &name:{"session.sh","tmf8829_tuner","bounded_log.awk","LICENSE-MIT.TXT","provenance.json"})
        files.append({QString("tuner/")+name,readFile(runtime.filePath(QString("dtof_tuner/")+name)),QString(name)=="session.sh"||QString(name)=="tmf8829_tuner"});
    const auto boardPath=[](const QString &relative){
        if(relative=="S99zdepth")return QString("/etc/init.d/S99zdepth");
        if(relative.startsWith("depth/"))return QString("/root/r144_depth/")+relative.mid(6);
        if(relative.startsWith("candidate/"))return BoardCandidateDir+"/"+relative.mid(10);
        return QString("/root/dtof_tuner/")+relative.mid(6);
    };
    QString checks="set -eu\nready=1\ncheck(){ [ -f \"$1\" ] && [ ! -L \"$1\" ] && [ \"$(sha256sum \"$1\" | cut -d' ' -f1)\" = \"$2\" ] && { [ \"$3\" = 0 ] || [ -x \"$1\" ]; } || ready=0; }\n";
    for(const auto &file:files)checks+=QString("check %1 %2 %3\n").arg(shellQuote(boardPath(file.relative)),tunerHash(file.bytes),file.executable?"1":"0");
    checks+=QString("if [ -e /root/rv1103_mw_spi/bin/tof_driver ] || [ -L /root/rv1103_mw_spi/bin/tof_driver ]; then check /root/rv1103_mw_spi/bin/tof_driver %1 1; fi\n").arg(liteSr.driverSha);
    checks+="for path in /root/r144_depth /root/dtof_candidates_20261009 /root/dtof_tuner /root/rv1103_mw_spi /root/rv1103_mw_spi/bin; do [ ! -L \"$path\" ] || ready=0; done\n"
        "grep -q '^[[:space:]]*\\[ ! -f /root/r144_depth/spi_native16.enabled \\] || return 0$' /oem/usr/bin/RkLunch.sh || ready=0\nprintf '%s\\n' \"$ready\"\n";
    if(connection.execute(checks).trimmed()=="1")return false;
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
        +"; mkdir depth candidate tuner candidate/dcts-litesr-384x256-v3-depth-only candidate/dcts-tiny-nv12-128x80-v1");
    QString phase;
    try {
        QByteArray manifest;
        for(const auto &file:files) {
            phase="upload "+file.relative;
            upload(file.relative,file.bytes,file.executable);
            manifest+=tunerHash(file.bytes).toLatin1()+"  "+file.relative.toLatin1()+"\n";
        }
        upload("files.sha256",manifest,false);
        phase="board preflight, model check and activation";
        const auto capabilities=QJsonDocument::fromJson(connection.execute(command("activate"),75000,setup)).object();
        require(capabilities["native_capture_protocol"]==LiteSrBoardContract::NativeCaptureProtocolV2
            && capabilities["fov"]==device.fov() && capabilities["m16"]==true
            && capabilities["model_inference"]==false && capabilities["photon_enabled"]==false,
            "Installed board Native16 capabilities differ from the device profile");
    } catch (const std::exception &error) {
        // Keep staging and rollback evidence on failure, including interrupted
        // SSH uploads. Never delete the last copy of an earlier deployment.
        throw Error(QString("RV1103 setup (%1): %2").arg(phase,QString::fromUtf8(error.what())));
    }
    require(connection.execute(checks).trimmed()=="1","USB runtime readback differs after installation");
    connection.execute("rm -rf "+quoted);
    return true;
}

