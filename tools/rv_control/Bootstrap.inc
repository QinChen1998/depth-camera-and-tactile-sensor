
void installV120Board(Ssh &connection, const QDir &runtime, const QString &boot)
{
    require(device.fov()==1, "V1.2.0 USB setup requires the matching F1 camera assembly");
    const auto stage="/root/.dcts-v120-"+QUuid::createUuid().toString(QUuid::Id128);
    const auto quoted=shellQuote(stage);
    const auto command=[&](const QString &action) {
        return QString("sh -s -- %1 %2 %3 %4")
            .arg(action,shellQuote(device.serial),shellQuote(boot),quoted);
    };
    const auto setup=readFile(runtime.filePath("rv1103_bootstrap/install.sh"));
    require(connection.execute(command("preflight"),10000,setup).trimmed()=="ready",
            "The V1.2.0 board setup preflight failed");
    const ProtectedLiteSrPayload liteSr(device.fov());
    const auto upload=[&](const QString &name, const QByteArray &data, bool executable) {
        const auto file=shellQuote(stage+"/"+name);
        require(!data.isEmpty(),"Empty RV1103 setup asset");
        const auto digest=connection.execute(QString("set -eu; umask 077; cat > %1; chmod %2 %1; sha256sum %1")
            .arg(file,executable?"700":"600"),30000,data);
        require(digest.simplified().split(' ').value(0)==tunerHash(data).toLatin1(),
                "Uploaded RV1103 setup asset checksum mismatch");
    };
    connection.execute("set -eu; umask 077; mkdir "+quoted+"; cd "+quoted
        +"; mkdir depth candidate candidate/dcts-litesr-384x256-v3-depth-only candidate/dcts-tiny-nv12-128x80-v1");
    QString phase;
    try {
        for (const auto &asset : bootstrapAssets) {
            phase=QString("upload %1").arg(QString::fromLatin1(asset.destination));
            auto data=readFile(runtime.filePath(QString("rv1103_bootstrap/")+asset.name));
            if (QByteArrayView(asset.name)=="M48_geometry.bin")
                require(tunerHash(data)==device.geometrySha(),"Bundled RV1103 geometry differs from the F1 profile");
            if (QByteArrayView(asset.name)=="session.sh") data=withAutomaticTof(data);
            upload(QString::fromLatin1(asset.destination),data,asset.executable);
        }
        phase="upload protected LiteSR runtime";
        upload("candidate/dcts-litesr-384x256-v3-depth-only/litesr_near_gui",liteSr.rectExecutable,true);
        upload("candidate/dcts-litesr-384x256-v3-depth-only/model.dctsm",liteSr.rectModel,false);
        upload("candidate/native_hd_gui",liteSr.nativeCapture,true);
        upload("candidate/dcts-tiny-nv12-128x80-v1/tiny_nv12",liteSr.tinyExecutable,true);
        upload("candidate/dcts-tiny-nv12-128x80-v1/tiny_nv12_camera",liteSr.tinyCamera,true);
        upload("candidate/dcts-tiny-nv12-128x80-v1/model.dctsm",liteSr.tinyModel,false);
        upload("candidate/litesr_gui.sh",liteSr.launcher,true);
        phase="board preflight, model check and activation";
        const auto capabilities=QJsonDocument::fromJson(connection.execute(command("activate"),75000,setup)).object();
        require(capabilities["native_capture_protocol"]==LiteSrBoardContract::NativeCaptureProtocolV2
            && capabilities["fov"]==device.fov() && capabilities["m16"]==true
            && capabilities["model_inference"]==false && capabilities["photon_enabled"]==false,
            "Installed board Native16 capabilities differ from the device profile");
    } catch (const std::exception &error) {
        // The board owns rollback after activation begins. An in-flight transaction
        // retains staging if the SSH channel disconnects before its trap completes.
        try { connection.execute("test ! -d /run/dcts-v120-install.lock && rm -rf "+quoted); } catch (...) {}
        throw Error(QString("V1.2.0 setup (%1): %2").arg(phase,QString::fromUtf8(error.what())));
    }
    connection.execute("rm -rf "+quoted);
}

