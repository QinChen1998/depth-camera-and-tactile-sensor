// Included inside the native SSH worker's private namespace.
QString shellQuote(QString value){value.replace("'","'\\''");return "'"+value+"'";}
QString tunerHash(const QByteArray &bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
void tunerToken(const QString &value){require(QRegularExpression("^[0-9a-f]{32}$").match(value).hasMatch(),"Invalid tuner token");}
void tunerDigest(const QString &value){require(QRegularExpression("^[0-9a-f]{64}$").match(value).hasMatch(),"Invalid configuration snapshot");}
QString tunerPrelude(){
    const auto script=readFile(QDir(QCoreApplication::applicationDirPath()).filePath("dtof_tuner/coordinator.sh"));
    const auto marker=script.indexOf("action=$1;shift");require(marker>0,"Missing tuner coordinator contract");return QString::fromUtf8(script.left(marker));
}
QByteArray tunerCall(Ssh &connection,const QString &action,const QStringList &args={}){
    const auto script=readFile(QDir(QCoreApplication::applicationDirPath()).filePath("dtof_tuner/coordinator.sh"));
    QString command="sh -c "+shellQuote(QString::fromUtf8(script))+" tuner "+shellQuote(action);
    for(const auto &v:args)command+=" "+shellQuote(v);
    return connection.execute(command,70000);
}
QByteArray tunerRead(Ssh &connection,const QString &path){return connection.execute("cat "+shellQuote(path));}
void tunerDependencies(Ssh &connection){
    const QDir root(QCoreApplication::applicationDirPath());
    for(const auto &name:{"session.sh","tmf8829_tuner","bounded_log.awk"}){
        const auto digest=sha(root.filePath(QString("dtof_tuner/")+name));
        const auto actual=connection.execute("sha256sum /root/dtof_tuner/"+QString(name)).simplified().split(' ').value(0);
        require(actual==digest.toLatin1(),QString("dToF runtime missing or SHA mismatch: %1. Install the approved tuner payload while idle; no capture was stopped.").arg(name));
    }
}
QJsonObject tunerInspect(Ssh &connection,const QJsonObject &request){
    const QString owner=request["owner_generation"].toString("-"),boot=request["owner_boot"].toString("-");
    require(owner=="-"||QRegularExpression("^[0-9a-f]{1,32}$").match(owner).hasMatch(),"Invalid shared owner generation");
    require(boot=="-"||QRegularExpression("^[0-9a-f-]{36}$").match(boot).hasMatch(),"Invalid shared boot");
    tunerDependencies(connection);
    const auto lines=tunerCall(connection,"inspect",{owner,boot}).trimmed().split('\n');
    require(lines.size()==4,"Malformed tuner inspection");
    if(!request["shared_receiving"].toBool())connection.execute(tunerPrelude()+"\nreceivers_gone\n");
    const auto native=tunerRead(connection,"/root/r144_depth/service.sh"),candidate=tunerRead(connection,"/root/dtof_candidates_20261009/session.sh");
    const auto settings=RvTuner::scriptSettings(native),lite=RvTuner::scriptSettings(candidate,true);
    require(tunerCall(connection,"snapshot").trimmed()==lines[1],"Configuration changed while reading; retry");
    QNetworkAccessManager network;network.setProxy(QNetworkProxy::NoProxy);const auto current=status(network);
    require(current["boot_id"].toString().toLatin1()==lines[0],"Status endpoint boot mismatch");
    const auto service=current["service"].toObject();
    require(service["model"]=="Native"&&(service["state"]=="stopped"||service["state"]=="running"),"Formal service is not healthy Native/stopped; no retired model may be restored");
    if(lines[2]=="native")require(service["state"]=="running"&&service["generation"].toVariant().toString().toLatin1()==lines[3],"Native owner differs from formal status");
    if(lines[2]=="idle"||lines[2]=="capture")require(service["state"]=="stopped","Native status contradicts the actual capture owner");
    return {{"boot_id",QString::fromLatin1(lines[0])},{"snapshot",QString::fromLatin1(lines[1])},{"owner",QString::fromLatin1(lines[2])},
        {"settings",settings.json()},{"capture_settings",lite.json()},{"service_sha",tunerHash(native)},{"capture_sha",tunerHash(candidate)},{"state","idle"}};
}
QString tunerWrite(const QString &path,const QByteArray &bytes){return "printf '%s' "+shellQuote(QString::fromLatin1(bytes.toBase64()))+" | base64 -d > "+shellQuote(path)+"\n";}
QString tunerSaveCommand(const QJsonObject &r,const RvTuner::Settings &s,const QByteArray &beforeNative,const QByteArray &beforeLite,const QString &id){
    s.validate();
    const auto token=r["token"].toString(),expected=r["snapshot"].toString();tunerToken(token);tunerDigest(expected);
    const QString nativePath="/root/r144_depth/service.sh",litePath="/root/dtof_candidates_20261009/session.sh";
    const auto afterNative=RvTuner::patchScript(beforeNative,s),afterLite=RvTuner::patchScript(beforeLite,s,true);
    tunerToken(id);const QString dir="/root/dtof_tuner/persistence/"+id;
    QString body=tunerPrelude()+"\nlocks;health\nowned "+token+"\n[ \"$(snapshot)\" = "+expected+" ] || fail 'Configuration drift; nothing saved'\n";
    // Verify the actual experiment command before writing any permanent file.
    body+="p=$(cut -d' ' -f1 \"$state/tof.identity\");b=$(cut -d' ' -f2 \"$state/tof.identity\")\nsame \"$p\" \"$b\" || fail 'Sensor identity lost'\n";
    body+="protected_before=$(protected_snapshot)\numask 077;mkdir -p "+dir+"\n";
    const QStringList paths{nativePath,litePath};const QList<QByteArray> before{beforeNative,beforeLite},after{afterNative,afterLite};
    for(int i=0;i<2;++i){
        body+="[ \"$(sha256sum "+paths[i]+" | cut -d' ' -f1)\" = "+tunerHash(before[i])+" ] || fail 'Script drift before save'\n";
        body+="cp -p "+paths[i]+" "+dir+"/"+QString::number(i)+".before\n";
        body+="[ \"$(sha256sum "+dir+"/"+QString::number(i)+".before | cut -d' ' -f1)\" = "+tunerHash(before[i])+" ] || fail 'Backup readback mismatch'\n";
        body+=tunerWrite(paths[i]+".dtof_"+id+".pending",after[i]);
        body+="chmod 755 "+paths[i]+".dtof_"+id+".pending\nsh -n "+paths[i]+".dtof_"+id+".pending\n";
        body+="[ \"$(sha256sum "+paths[i]+".dtof_"+id+".pending | cut -d' ' -f1)\" = "+tunerHash(after[i])+" ] || fail 'Pending configuration SHA mismatch'\n";
    }
    const QJsonObject manifest{{"kind","dtof_only_native_and_capture"},{"settings",s.json()},{"before_snapshot",expected},{"token",token},
        {"native_before",tunerHash(beforeNative)},{"native_after",tunerHash(afterNative)},{"capture_before",tunerHash(beforeLite)},{"capture_after",tunerHash(afterLite)}};
    body+=tunerWrite(dir+"/manifest.json",QJsonDocument(manifest).toJson());
    // Commit both files inside a rollback-owning subshell. Never overwrite drift.
    body+="(set -e\nchanged=0\nrollback(){\n code=$?;trap - EXIT;set +e\n if [ \"$code\" != 0 ] && [ \"$changed\" = 1 ];then\n bad=0\n";
    for(int i=0;i<2;++i)body+="actual=$(sha256sum "+paths[i]+" | cut -d' ' -f1)\nif [ \"$actual\" = "+tunerHash(after[i])+" ];then cp -p "+dir+"/"+QString::number(i)+".before "+paths[i]+".dtof_"+id+".restore && mv "+paths[i]+".dtof_"+id+".restore "+paths[i]+" || bad=1\nelif [ \"$actual\" != "+tunerHash(before[i])+" ];then bad=1;fi\n";
    body+="sync\nif [ \"$bad\" = 1 ];then touch \"$state/fault\";echo rollback_fault > "+dir+"/result;else echo rolled_back > "+dir+"/result;fi\nfi\nexit \"$code\"\n}\ntrap rollback EXIT\necho installing > "+dir+"/result\nchanged=1\n";
    for(int i=0;i<2;++i)body+="mv "+paths[i]+".dtof_"+id+".pending "+paths[i]+"\n[ \"$(sha256sum "+paths[i]+" | cut -d' ' -f1)\" = "+tunerHash(after[i])+" ]\n";
    body+="owned "+token+";health\n[ \"$(protected_snapshot)\" = \"$protected_before\" ] || fail 'Control script or selection drift during save'\nsync\necho committed > "+dir+"/result\n)\nsnapshot\n";
    return body;
}
QJsonObject tunerPersist(Ssh &connection,const QJsonObject &r,const RvTuner::Settings &s){
    const auto token=r["token"].toString();tunerToken(token);
    const QString nativePath="/root/r144_depth/service.sh",litePath="/root/dtof_candidates_20261009/session.sh";
    const auto beforeNative=tunerRead(connection,nativePath),beforeLite=tunerRead(connection,litePath);
    const auto afterNative=RvTuner::patchScript(beforeNative,s),afterLite=RvTuner::patchScript(beforeLite,s,true);
    const QString id=QUuid::createUuid().toString(QUuid::Id128),dir="/root/dtof_tuner/persistence/"+id;
    const auto command=connection.execute(tunerPrelude()+"\nowned "+token+"\np=$(cut -d' ' -f1 \"$state/tof.identity\");b=$(cut -d' ' -f2 \"$state/tof.identity\");same \"$p\" \"$b\";tr '\\000' ' ' < /proc/$p/cmdline");
    require(RvTuner::scriptSettings(command)==s,"Apply and verify these experiment settings before permanent save");
    const auto body=tunerSaveCommand(r,s,beforeNative,beforeLite,id);
    const QString snapshot=QString::fromLatin1(connection.execute("sh -c "+shellQuote(body),70000).trimmed());tunerDigest(snapshot);
    require(tunerRead(connection,nativePath)==afterNative&&tunerRead(connection,litePath)==afterLite,"Permanent configuration readback failed; inspect backup and retained fault evidence");
    require(QString::fromLatin1(tunerCall(connection,"snapshot").trimmed())==snapshot,"Configuration drift after permanent save");
    return {{"state","running"},{"persisted",true},{"settings",s.json()},{"snapshot",snapshot},{"backup",dir}};
}
QJsonObject tunerOperation(Ssh &connection,const QString &action,const QJsonObject &r){
    if(action=="inspect")return tunerInspect(connection,r);
    const QString token=r["token"].toString();tunerToken(token);
    if(action=="stop"||action=="heartbeat"){
        QString stopping=token;
        if(action=="stop"&&!r["old_token"].toString().isEmpty()){
            const auto old=r["old_token"].toString();tunerToken(old);
            const auto actual=connection.execute("cat /run/r144_fusion_20260923/session.generation 2>/dev/null || true").trimmed();
            if(actual==("dtof-"+old).toLatin1())stopping=old;
        }
        const auto text=tunerCall(connection,action,{stopping}).trimmed();require(text==(action=="stop"?"stopped":"running"),"Tuner lifecycle acknowledgement mismatch");
        return {{"state",QString::fromLatin1(text)},{"token",token}};
    }
    const auto s=RvTuner::Settings::fromJson(r["settings"].toObject());
    if(action=="persist")return tunerPersist(connection,r,s);
    require(action=="start"||action=="switch","Unknown tuner operation");const auto snapshot=r["snapshot"].toString();tunerDigest(snapshot);tunerDependencies(connection);
    QStringList args{snapshot};
    if(action=="start"){
        const auto owner=r["owner_generation"].toString("-"),boot=r["owner_boot"].toString("-");
        require(owner=="-"||QRegularExpression("^[0-9a-f]{1,32}$").match(owner).hasMatch(),"Invalid handover identity");
        require(boot=="-"||QRegularExpression("^[0-9a-f-]{36}$").match(boot).hasMatch(),"Invalid handover boot");args<<owner<<boot;
    }else {const auto old=r["old_token"].toString();tunerToken(old);args<<old;}
    args<<token;args+=s.arguments().split(' ');
    const auto lines=tunerCall(connection,action,args).trimmed().split('\n');
    const auto shape=s.size();require(lines.size()==2&&lines[0]==QString("%1 %2 %3").arg(shape.height()).arg(shape.width()).arg(s.objects).toLatin1()&&RvTuner::scriptSettings(lines[1])==s,"Experiment runtime parameters or complete result shape mismatch");
    const auto clock=connection.execute("awk '{printf \"%.0f\\n\",$1*1000000000}' /proc/uptime").trimmed();
    bool clockOk=false;clock.toULongLong(&clockOk);require(clockOk,"Invalid board monotonic clock receipt");
    return {{"state","running"},{"token",token},{"settings",s.json()},{"snapshot",snapshot},{"boot_id",r["boot_id"]},{"remote_now_ns",QString::fromLatin1(clock)},{"port",8833}};
}
