#include "RvInferenceSettings.h"
#include "RvInferenceRuntime.h"
#include "RvAssistDepth.h"
#include "../RuntimeDiscovery.h"
#include <QCoreApplication>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLibraryInfo>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <functional>
#include <utility>

namespace {
constexpr int StartupDetectionTimeoutMs = 5000;

int candidatePriority(const QString &id)
{
    if (id=="tensorrt") return 0;
    if (id.startsWith("directml:")) return 1;
    if (id.startsWith("openvino:GPU")) return 2;
    if (id=="openvino:CPU") return 3;
    if (id=="cpu") return 4;
    return 5;
}

QProcessEnvironment inferenceProcessEnvironment(const QString &program, const QString &runtime)
{
    auto environment=RuntimeDiscovery::nativeWorkerEnvironment(program);
    // Development launches can load Qt from its SDK before windeployqt runs.
    const auto qtBin=QDir::toNativeSeparators(QLibraryInfo::path(QLibraryInfo::BinariesPath));
    if(!qtBin.isEmpty())environment.insert("PATH",environment.value("PATH")+';'+qtBin);
    environment.insert("RV_ASSIST_RGB672_VGA_FP16_ROOT",runtime);
    environment.insert("RV_ASSIST_COMPAT_ROOT",RuntimeDiscovery::rvCompatibleRoot());
    environment.insert("RV_TEST_OFFLINE_ONLY","1");
    environment.remove("RV_INFERENCE_BACKEND");
    return environment;
}

bool waitForInferenceOperation(RvInferenceSettings *settings, const std::function<void()> &start, int timeoutMs)
{
    QEventLoop loop;
    bool finished=false;
    QObject::connect(settings,&RvInferenceSettings::finished,&loop,[&]{finished=true;loop.quit();});
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    deadline.start(timeoutMs);
    start();
    if(!finished)loop.exec();
    return finished&&!settings->scanning();
}
}

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
    constexpr double UnmeasuredLatencyMs = 5000;
    for(const auto &value:m_available){
        const auto row=value.toObject();
        if(row["id"]==id)return row["probe"].toObject()["maximum_ms"].toDouble(UnmeasuredLatencyMs);
    }
    return UnmeasuredLatencyMs;
}
void RvInferenceSettings::setSelected(const QString &id){
    if(m_selected==id)return;
    m_selected=id;
    emit selectionChanged();
}
bool RvInferenceSettings::choose(const QString &id,bool remember){
    for(const auto &value:m_available){const auto row=value.toObject();if(row["id"]==id){
        if(remember){m_preference=row["key"].toString();RuntimeDiscovery::depthSettings().setValue("inference/preferredDevice",m_preference);}
        setSelected(id);emit changed();return true;
    }}return false;
}
QJsonObject RvInferenceSettings::diagnostics()const{
    return {{"schema",1},{"scanning",m_scanning},{"selected",m_selected},{"available",m_available},
        {"benchmark_probes",m_benchmarkProbes},{"rejected",m_rejected},{"status",m_status},{"hardware_capture_started",false}};
}

void RvInferenceSettings::detectDevices() { startDetection(false); }
void RvInferenceSettings::benchmarkDevices() { startDetection(true); }

void RvInferenceSettings::startDetection(bool benchmark)
{
    if(m_scanning)return;
    const bool firstDetection=!m_started;
    m_started=true;m_scanning=true;m_enumerating=true;m_benchmarkRequested=benchmark;
    m_index=-1;m_benchmarkProbes=0;m_rejected={};m_candidates={};m_benchmarked={};
    m_preference=RuntimeDiscovery::depthSettings().value("inference/preferredDevice").toString();
    if(firstDetection)emit selectionChanged();
    if(benchmark)emit benchmarkingChanged(true);
    m_status=tr("Detecting inference devices...");emit changed();
    if(!m_work.isValid()){complete(tr("Cannot create the inference test directory."));return;}
    m_report=m_work.filePath("devices.json");QFile::remove(m_report);
    const auto program=RuntimeDiscovery::inferenceWorker();
    m_process.setProcessEnvironment(inferenceProcessEnvironment(program,RuntimeDiscovery::rvAssistRoot()));
    m_process.setProgram(program);
    m_process.setArguments({"--inference-enumerate",m_report});
    m_timedOut=false;
    m_timeout.start(benchmark?30000:StartupDetectionTimeoutMs);
    m_process.start();
}

void RvInferenceSettings::startBenchmark()
{
    QString worker,runtime;m_paths=RvAssistDepth::prepareProbeWorker(worker,runtime);
    if(!m_paths){complete(tr("Cannot prepare the bundled inference worker."));return;}
    m_process.setProcessEnvironment(inferenceProcessEnvironment(worker,runtime));
    m_process.setProgram(worker);
    next();
}

void RvInferenceSettings::next(){
    ++m_index;
    if(m_index>=m_candidates.size()){
        m_available=std::exchange(m_benchmarked,{});
        complete(m_available.isEmpty()?tr("No usable inference device was found."):tr("Benchmark complete."));
        return;
    }
    const auto row=m_candidates[m_index].toObject();
    m_status=tr("Benchmarking %1 (%2/%3)...").arg(row["name"].toString()).arg(m_index+1).arg(m_candidates.size());
    m_report=m_work.filePath(QString("probe-%1.json").arg(m_index));QFile::remove(m_report);
    m_process.setArguments({"--inference-probe",row["id"].toString(),m_report});
    ++m_benchmarkProbes;
    m_timedOut=false;m_timeout.start(180000);m_process.start();emit changed();
}
void RvInferenceSettings::processFinished(int code){
    if(!m_scanning)return;m_timeout.stop();
    QFile file(m_report);QJsonObject result;
    if(file.open(QIODevice::ReadOnly)&&file.size()<1048576)result=QJsonDocument::fromJson(file.readAll()).object();
    const auto detail=QString::fromUtf8(m_process.readAllStandardError()).right(1500);
    m_process.readAllStandardOutput();
    if(m_enumerating){
        if(result["schema"]==1){
            QSet<QString> seen;
            for(const auto &value:result["candidates"].toArray()){
                const auto row=value.toObject();
                const auto id=row["id"].toString();
                if(!RvInferenceRuntime::validBackend(id)||row["key"].toString().isEmpty()||seen.contains(id))continue;
                seen.insert(id);m_candidates.append(row);
            }
        }
        if(code!=0||result["schema"]!=1||m_timedOut)m_rejected.append(QJsonObject{{"phase","enumeration"},{"exit_code",code},{"error",detail},
            {"timed_out",m_timedOut},
            {"program",m_process.program()},{"report",m_report},{"report_present",file.exists()}});
        m_available=m_candidates;
        if(m_benchmarkRequested&&!m_available.isEmpty()){
            m_enumerating=false;
            setSelected(preferredCandidate());
            emit detectionFinished();
            startBenchmark();
        }else{
            complete(m_available.isEmpty()?tr("No usable inference device was found.")
                :m_timedOut?tr("Using the inference candidates found before detection timed out.")
                :tr("Inference candidates detected. Run a benchmark to measure FPS."));
        }
        return;
    }else if(m_index>=0&&m_index<m_candidates.size()){
        auto row=m_candidates[m_index].toObject();
        if(code==0&&!m_timedOut&&result["pass"]==true&&result["backend"]==row["id"]
            &&result["checks"].toInt()>0&&result["mean_ms"].toDouble()>0){
            row["probe"]=result;m_benchmarked.append(row);
        }else{
            row["error"]=m_timedOut?tr("The device test timed out."):result["error"].toString(detail.isEmpty()?m_process.errorString():detail);
            row["exit_code"]=code;row["report_present"]=file.exists();row["report_path"]=m_report;
            m_rejected.append(row);
        }
    }
    emit changed();QTimer::singleShot(0,this,&RvInferenceSettings::next);
}
void RvInferenceSettings::complete(const QString &status){
    const bool wasDetecting=detecting(),wasBenchmarking=benchmarking();
    m_scanning=false;m_enumerating=false;m_benchmarkRequested=false;
    m_paths.reset();
    m_status=status;
    setSelected(preferredCandidate());
    if(wasBenchmarking)emit benchmarkingChanged(false);
    emit changed();
    if(wasDetecting)emit detectionFinished();
    emit finished();
}

QString RvInferenceSettings::preferredCandidate() const
{
    QString best;
    int bestPriority=5;
    for(const auto &value:m_available){
        const auto row=value.toObject();
        const auto id=row["id"].toString();
        if(!m_preference.isEmpty()&&row["key"]==m_preference)return id;
        const int priority=candidatePriority(id);
        if(priority<bestPriority){best=id;bestPriority=priority;}
    }
    return best;
}

bool RvInferenceSettings::fallback(const QString &failed,const QString &reason){
    if(!m_started||m_scanning||failed.isEmpty()||failed!=m_selected||qEnvironmentVariableIsSet("RV_INFERENCE_BACKEND"))return false;
    for(int i=m_available.size()-1;i>=0;--i)if(m_available[i].toObject()["id"]==failed){
        auto row=m_available[i].toObject();row["phase"]="runtime";row["error"]=reason;m_rejected.append(row);m_available.removeAt(i);
    }
    const auto nextBackend=preferredCandidate();
    m_status=nextBackend.isEmpty()?tr("No usable inference device was found.")
        :tr("The selected device failed. Trying the next inference candidate.");
    setSelected(nextBackend);
    emit changed();
    return !nextBackend.isEmpty();
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
    auto *benchmarkButton=buttons->addButton(tr("Run Benchmark"),QDialogButtonBox::ActionRole);
    benchmarkButton->setObjectName("inferenceBenchmark");
    layout->addStretch();layout->addWidget(buttons);
    const auto update=[this,combo,status,note,benchmarkButton]{
        const QSignalBlocker block(combo);combo->clear();
        for(const auto &value:m_available){const auto row=value.toObject();
            const auto name=row["id"]=="openvino:INT8"?tr("CPU / OpenVINO INT8 (mixed precision)"):row["name"].toString();
            const auto probe=row["probe"].toObject();
            const auto label=probe["pass"]==true
                ?tr("%1 — %2 — about %3 FPS").arg(name,row["hardware"].toString()).arg(probe["fps"].toDouble(),0,'f',1)
                :tr("%1 — %2 — awaiting benchmark").arg(name,row["hardware"].toString());
            combo->addItem(label,row["id"].toString());
        }
        combo->setCurrentIndex(combo->findData(m_selected));combo->setEnabled(!m_available.isEmpty()&&!m_scanning);
        status->setText(m_status);benchmarkButton->setEnabled(!m_scanning);
        note->setText(m_selected=="openvino:INT8"
            ?tr("Mixed INT8 trades some depth accuracy for speed. Local testing found 1.34% mean output difference from the original model, with larger differences in some scenes. Verify it for your application.")
            :tr("The benchmark estimates model FPS and pauses host depth inference while it runs."));
    };
    connect(this,&RvInferenceSettings::changed,dialog,update);
    connect(combo,qOverload<int>(&QComboBox::activated),dialog,[this,combo](int){choose(combo->currentData().toString());});
    connect(benchmarkButton,&QPushButton::clicked,this,&RvInferenceSettings::benchmarkDevices);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);update();
    if(!m_started)QTimer::singleShot(0,this,&RvInferenceSettings::detectDevices);
    return dialog;
}
int runRvInferenceDiscoveryTest(const QString &path){
    auto *settings=RvInferenceSettings::instance();
    QElapsedTimer startupClock;
    startupClock.start();
    const bool detected=waitForInferenceOperation(settings,[&]{settings->detectDevices();},10000);
    auto startup=settings->diagnostics();
    startup["elapsed_ms"]=double(startupClock.elapsed());
    QJsonObject checks;
    const auto candidates=settings->available();
    bool unmeasured=true, detectedTensorRt=false;
    for(const auto &value:candidates){
        const auto row=value.toObject();
        unmeasured&=!row.contains("probe");
        detectedTensorRt|=row["id"]=="tensorrt";
    }
    checks["startup_detects_candidates_without_benchmark"]=detected&&!candidates.isEmpty()
        &&unmeasured&&startup["benchmark_probes"]==0;
    checks["startup_prefers_tensorrt"]=!detectedTensorRt||settings->selected()=="tensorrt";

    // Exercise the full priority chain even on a machine with only a CPU.
    RvInferenceSettings policy;
    policy.m_started=true;
    for(const auto *id:{"cpu","openvino:INT8","openvino:CPU","openvino:GPU","directml:0","tensorrt"})
        policy.m_available.append(QJsonObject{{"id",id},{"key",id}});
    policy.m_preference="openvino:INT8";
    checks["saved_device_preference_restored"]=policy.preferredCandidate()=="openvino:INT8";
    policy.m_preference="tensorrt";
    policy.setSelected(policy.preferredCandidate());
    checks["runtime_fallback_tries_remaining_gpu_then_cpu"]=policy.fallback("tensorrt","offline simulated device loss")
        &&policy.selected()=="directml:0"&&policy.fallback("directml:0","offline simulated device loss")
        &&policy.selected()=="openvino:GPU"&&policy.fallback("openvino:GPU","offline simulated device loss")
        &&policy.selected()=="openvino:CPU"&&policy.fallback("openvino:CPU","offline simulated device loss")
        &&policy.selected()=="cpu"&&policy.m_preference=="tensorrt";
    checks["int8_requires_explicit_choice"]=!policy.fallback("cpu","offline simulated device loss")
        &&policy.selected().isEmpty()&&policy.choose("openvino:INT8",false);

    const bool benchmarked=waitForInferenceOperation(settings,[&]{settings->benchmarkDevices();},900000);
    auto result=settings->diagnostics();
    result["startup"]=startup;
    const auto available=settings->available();bool valid=!available.isEmpty(),hasTrt=false,hasCpu=false;
    for(const auto &value:available){const auto row=value.toObject();valid&=row["probe"].toObject()["pass"]==true;
        hasTrt|=row["id"]=="tensorrt";hasCpu|=row["id"]=="cpu";}
    checks["manual_benchmark_completed_with_valid_choices"]=benchmarked&&valid&&result["benchmark_probes"].toInt()>0;
    checks["tensorrt_default_when_available"]=!hasTrt||settings->selected()=="tensorrt";
    const auto previous=settings->selected();checks["unknown_choice_rejected"]=!settings->choose("unsupported")&&settings->selected()==previous;
    checks["cpu_choice_and_preference"]=hasCpu&&settings->choose("cpu")&&QSettings().value("inference/preferredDevice")=="cpu";
    bool passed=true;for(auto it=checks.begin();it!=checks.end();++it)passed&=it.value().toBool();
    result["pass"]=passed;result["policy_checks"]=checks;result["checks"]=checks.size();
    QFile file(path);if(!file.open(QIODevice::WriteOnly))return 3;
    file.write(QJsonDocument(result).toJson());return result["pass"].toBool()?0:1;
}

int runRvInferenceUiTest(QWidget *window,const QString &path){
    auto *settings=RvInferenceSettings::instance();
    const bool detected=waitForInferenceOperation(settings,[&]{settings->detectDevices();},10000);
    auto *dialog=settings->createDialog(window);dialog->show();QApplication::processEvents();
    auto *combo=dialog->findChild<QComboBox*>("inferenceDeviceCombo");
    auto *benchmark=dialog->findChild<QPushButton*>("inferenceBenchmark");
    QJsonObject checks;
    checks["startup_candidates_available_before_benchmark"]=detected&&combo&&combo->isEnabled()
        &&combo->count()>0&&combo->count()==settings->available().size()
        &&settings->diagnostics()["benchmark_probes"]==0&&!combo->currentText().contains("FPS");
    checks["candidate_screenshot"]=dialog->grab().save(path+".candidates.png");
    const bool benchmarked=benchmark&&benchmark->isEnabled()
        &&waitForInferenceOperation(settings,[&]{benchmark->click();},900000);
    checks["benchmark_button_shows_measured_fps"]=benchmarked&&combo&&combo->isEnabled()
        &&combo->currentText().contains("FPS")&&settings->diagnostics()["benchmark_probes"].toInt()>0;
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
