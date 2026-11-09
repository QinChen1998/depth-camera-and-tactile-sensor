#include "RvInferenceSettings.h"
#include "RvInferenceRuntime.h"
#include "RvAssistDepth.h"
#include "../RuntimeDiscovery.h"
#include <QCoreApplication>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

RvInferenceSettings *RvInferenceSettings::instance(){
    static auto *settings=new RvInferenceSettings(QCoreApplication::instance());return settings;
}
RvInferenceSettings::RvInferenceSettings(QObject *parent):QObject(parent){
    m_timeout.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,[this]{m_timedOut=true;m_process.kill();});
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){processFinished(code);});
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart)processFinished(-1);
    });
}
RvInferenceSettings::~RvInferenceSettings(){
    m_process.disconnect(this);m_timeout.stop();
    if(m_process.state()!=QProcess::NotRunning){m_process.kill();m_process.waitForFinished(2000);}
}
QString RvInferenceSettings::selected()const{
    if(qEnvironmentVariableIsSet("RV_INFERENCE_BACKEND"))return qEnvironmentVariable("RV_INFERENCE_BACKEND");
    return m_started?m_selected:QStringLiteral("tensorrt");
}
double RvInferenceSettings::latencyMs(const QString &id)const{
    for(const auto &value:m_available){const auto row=value.toObject();if(row["id"]==id)return row["probe"].toObject()["maximum_ms"].toDouble(2500);}
    return 2500;
}
void RvInferenceSettings::setSelected(const QString &id){
    if(m_selected==id)return;m_selected=id;emit selectionChanged();emit changed();
}
bool RvInferenceSettings::choose(const QString &id,bool remember){
    for(const auto &value:m_available){const auto row=value.toObject();if(row["id"]==id){
        if(remember){m_preference=row["key"].toString();QSettings().setValue("inference/preferredDevice",m_preference);}
        setSelected(id);return true;
    }}return false;
}
QJsonObject RvInferenceSettings::diagnostics()const{
    return {{"schema",1},{"scanning",m_scanning},{"selected",m_selected},{"available",m_available},
        {"rejected",m_rejected},{"status",m_status},{"hardware_capture_started",false}};
}
void RvInferenceSettings::refresh(){
    if(m_scanning)return;
    m_started=true;m_scanning=true;m_enumerating=true;m_index=-1;m_available={};m_rejected={};m_candidates={};
    m_preference=QSettings().value("inference/preferredDevice").toString();
    m_selected.clear();emit selectionChanged();
    m_status=tr("Detecting inference devices...");emit changed();
    if(!m_work.isValid()){m_status=tr("Cannot create the inference test directory.");complete();return;}
    m_report=m_work.filePath("devices.json");QFile::remove(m_report);
    QString worker,runtime;m_paths=RvAssistDepth::prepareProbeWorker(worker,runtime);
    if(!m_paths){m_status=tr("Cannot prepare the bundled inference worker.");complete();return;}
    auto environment=RuntimeDiscovery::nativeWorkerEnvironment(worker);
    environment.insert("RV_ASSIST_RGB672_VGA_FP16_ROOT",runtime);
    environment.insert("RV_ASSIST_COMPAT_ROOT",RuntimeDiscovery::rvCompatibleRoot());
    environment.insert("RV_TEST_OFFLINE_ONLY","1");environment.remove("RV_INFERENCE_BACKEND");
    m_process.setProcessEnvironment(environment);m_process.setProgram(worker);
    m_process.setArguments({"--inference-enumerate",m_report});m_timedOut=false;m_process.start();m_timeout.start(30000);
}
void RvInferenceSettings::next(){
    ++m_index;
    if(m_index>=m_candidates.size()){complete();return;}
    const auto row=m_candidates[m_index].toObject();
    m_status=tr("Testing %1 (%2/%3)...").arg(row["name"].toString()).arg(m_index+1).arg(m_candidates.size());
    m_report=m_work.filePath(QString("probe-%1.json").arg(m_index));QFile::remove(m_report);
    m_process.setArguments({"--inference-probe",row["id"].toString(),m_report});
    m_timedOut=false;m_process.start();m_timeout.start(180000);emit changed();
}
void RvInferenceSettings::processFinished(int code){
    if(!m_scanning)return;m_timeout.stop();
    QFile file(m_report);QJsonObject result;
    if(file.open(QIODevice::ReadOnly)&&file.size()<1048576)result=QJsonDocument::fromJson(file.readAll()).object();
    const auto detail=QString::fromUtf8(m_process.readAllStandardError()).right(1500);
    m_process.readAllStandardOutput();
    if(m_enumerating){
        m_enumerating=false;
        if(code==0&&result["schema"]==1)m_candidates=result["candidates"].toArray();
        else m_rejected.append(QJsonObject{{"phase","enumeration"},{"exit_code",code},{"error",detail},
            {"program",m_process.program()},{"report",m_report},{"report_present",file.exists()}});
        if(m_candidates.isEmpty())m_candidates.append(QJsonObject{{"id","cpu"},{"name","ONNX Runtime CPU"},{"hardware","CPU"},{"key","cpu"}});
    }else if(m_index>=0&&m_index<m_candidates.size()){
        auto row=m_candidates[m_index].toObject();
        if(code==0&&!m_timedOut&&result["pass"]==true&&result["backend"]==row["id"]
            &&result["checks"].toInt()>0&&result["mean_ms"].toDouble()>0){
            row["probe"]=result;m_available.append(row);
            if(row["key"]==m_preference|| (m_preference.isEmpty()&&row["id"]=="tensorrt"))setSelected(row["id"].toString());
        }else{
            row["error"]=m_timedOut?tr("The device test timed out."):result["error"].toString(detail.isEmpty()?m_process.errorString():detail);
            row["exit_code"]=code;row["report_present"]=file.exists();row["report_path"]=m_report;
            m_rejected.append(row);
        }
    }
    emit changed();QTimer::singleShot(0,this,&RvInferenceSettings::next);
}
void RvInferenceSettings::complete(){
    m_scanning=false;
    m_paths.reset();
    if(m_selected.isEmpty()){
        QString best;double bestMs=1e30;
        for(const auto &value:m_available){const auto row=value.toObject();
            if(row["id"]=="tensorrt"){best="tensorrt";break;}
            // Quantization is an explicit user choice; default selection keeps
            // the original model precision even when INT8 benchmarks faster.
            if(row["quantized"].toBool())continue;
            const auto ms=row["probe"].toObject()["mean_ms"].toDouble(1e30);
            if(ms<bestMs){bestMs=ms;best=row["id"].toString();}
        }
        setSelected(best);
    }
    m_status=m_available.isEmpty()?tr("No usable inference device was found."):tr("Detection complete. Only successfully tested options are shown.");
    emit changed();emit finished();
}
bool RvInferenceSettings::fallback(const QString &failed,const QString &reason){
    if(!m_started||m_scanning||failed!=m_selected)return false;
    for(int i=m_available.size()-1;i>=0;--i)if(m_available[i].toObject()["id"]==failed){
        auto row=m_available[i].toObject();row["error"]=reason;m_rejected.append(row);m_available.removeAt(i);
    }
    for(const auto &id:{QStringLiteral("openvino:CPU"),QStringLiteral("cpu")}){
        if(id!=failed&&choose(id,false)){m_status=tr("The selected device failed. Switched to CPU.");emit changed();return true;}
    }
    setSelected({});emit changed();return false;
}
QDialog *RvInferenceSettings::createDialog(QWidget *parent){
    auto *dialog=new QDialog(parent);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("inferenceDeviceDialog");
    dialog->setWindowTitle(tr("Inference Device"));dialog->resize(720,320);
    auto *layout=new QVBoxLayout(dialog);
    auto *intro=new QLabel(tr("Choose the device used for host-assisted depth. TensorRT-RTX is preferred on first use when available. Your selection is remembered."),dialog);
    intro->setWordWrap(true);layout->addWidget(intro);
    auto *combo=new QComboBox(dialog);combo->setObjectName("inferenceDeviceCombo");layout->addWidget(combo);
    auto *status=new QLabel(dialog);status->setWordWrap(true);status->setObjectName("inferenceDetectionStatus");layout->addWidget(status);
    auto *note=new QLabel(dialog);note->setWordWrap(true);note->setObjectName("inferencePrecisionNote");layout->addWidget(note);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,dialog);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    auto *refreshButton=buttons->addButton(tr("Detect Again"),QDialogButtonBox::ActionRole);refreshButton->setObjectName("inferenceDetectAgain");
    layout->addStretch();layout->addWidget(buttons);
    const auto update=[this,combo,status,note,refreshButton]{
        const QSignalBlocker block(combo);combo->clear();
        for(const auto &value:m_available){const auto row=value.toObject();
            const auto name=row["id"]=="openvino:INT8"?tr("CPU / OpenVINO INT8 (mixed precision)"):row["name"].toString();
            combo->addItem(tr("%1 — %2 — about %3 FPS").arg(name,row["hardware"].toString()).arg(row["probe"].toObject()["fps"].toDouble(),0,'f',1),row["id"].toString());
        }
        combo->setCurrentIndex(combo->findData(m_selected));combo->setEnabled(!m_available.isEmpty()&&!m_scanning);
        status->setText(m_status);refreshButton->setEnabled(!m_scanning);
        note->setText(m_selected=="openvino:INT8"
            ?tr("Mixed INT8 trades some depth accuracy for speed. Local testing found 1.34% mean output difference from the original model, with larger differences in some scenes. Verify it for your application.")
            :tr("FPS is a short local inference estimate. Live depth speed also depends on video and processing. Detection briefly pauses host inference and does not connect to a sensor."));
    };
    connect(this,&RvInferenceSettings::changed,dialog,update);
    connect(combo,qOverload<int>(&QComboBox::activated),dialog,[this,combo](int){choose(combo->currentData().toString());});
    connect(refreshButton,&QPushButton::clicked,this,&RvInferenceSettings::refresh);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);update();
    if(!m_started)QTimer::singleShot(0,this,&RvInferenceSettings::refresh);
    return dialog;
}
int runRvInferenceDiscoveryTest(const QString &path){
    auto *settings=RvInferenceSettings::instance();QEventLoop loop;bool timedOut=false;
    QObject::connect(settings,&RvInferenceSettings::finished,&loop,&QEventLoop::quit);
    QTimer deadline;deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{timedOut=true;loop.quit();});
    deadline.start(900000);settings->refresh();loop.exec();
    auto result=settings->diagnostics();QJsonObject checks;
    const auto available=settings->available();bool valid=!available.isEmpty(),hasTrt=false,hasCpu=false;
    for(const auto &value:available){const auto row=value.toObject();valid&=row["probe"].toObject()["pass"]==true;
        hasTrt|=row["id"]=="tensorrt";hasCpu|=row["id"]=="cpu";}
    checks["completed_with_valid_choices"]=!timedOut&&!settings->scanning()&&valid;
    checks["tensorrt_default_when_available"]=!hasTrt||settings->selected()=="tensorrt";
    const auto previous=settings->selected();checks["unknown_choice_rejected"]=!settings->choose("unsupported")&&settings->selected()==previous;
    checks["cpu_choice_and_preference"]=hasCpu&&settings->choose("cpu")&&QSettings().value("inference/preferredDevice")=="cpu";
    if(hasTrt){settings->choose("tensorrt",false);checks["gpu_failure_falls_back_to_cpu"]=settings->fallback("tensorrt","offline simulated device loss")
        &&(settings->selected()=="cpu"||settings->selected()=="openvino:CPU");}
    bool passed=true;for(auto it=checks.begin();it!=checks.end();++it)passed&=it.value().toBool();
    result["pass"]=passed;result["policy_checks"]=checks;result["checks"]=checks.size();
    QFile file(path);if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(result).toJson());return result["pass"].toBool()?0:1;
}

int runRvInferenceUiTest(QWidget *window,const QString &path){
    auto *settings=RvInferenceSettings::instance();QEventLoop loop;bool timedOut=false;
    QObject::connect(settings,&RvInferenceSettings::finished,&loop,&QEventLoop::quit);
    QTimer deadline;deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{timedOut=true;loop.quit();});
    deadline.start(900000);settings->refresh();loop.exec();
    auto *dialog=settings->createDialog(window);dialog->show();QApplication::processEvents();
    auto *combo=dialog->findChild<QComboBox*>("inferenceDeviceCombo");
    QJsonObject checks;
    checks["detection_completed"]=!timedOut&&!settings->scanning()&&!settings->available().isEmpty();
    checks["only_verified_options"]=combo&&combo->count()==settings->available().size()&&combo->isEnabled();
    checks["selection_matches_controller"]=combo&&combo->currentData().toString()==settings->selected();
    checks["settings_screenshot"]=dialog->grab().save(path+".settings.png");
    if(combo&&combo->findData("openvino:INT8")>=0){
        const int index=combo->findData("openvino:INT8");combo->setCurrentIndex(index);
        QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index));QApplication::processEvents();
        checks["user_can_select_int8"]=settings->selected()=="openvino:INT8";
        checks["quantization_note_visible"]=dialog->findChild<QLabel*>("inferencePrecisionNote")->text().contains("INT8");
        checks["int8_screenshot"]=dialog->grab().save(path+".int8.png");
    }
    bool passed=true;for(auto it=checks.begin();it!=checks.end();++it)passed&=it.value().toBool();
    auto report=settings->diagnostics();report["pass"]=passed;report["checks"]=checks.size();report["ui_checks"]=checks;
    dialog->close();QFile file(path);if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(report).toJson());return passed?0:1;
}
