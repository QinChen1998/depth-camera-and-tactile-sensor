#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
class QWidget;
class QDialog;

// Startup detects candidates. Model benchmarks run only on an explicit request.
class RvInferenceSettings final:public QObject {
    Q_OBJECT
public:
    static RvInferenceSettings *instance();
    explicit RvInferenceSettings(QObject *parent=nullptr);
    ~RvInferenceSettings() override;
    QString selected() const;
    bool started() const{return m_started;}
    bool scanning() const{return m_scanning;}
    bool detecting() const{return m_enumerating;}
    bool benchmarking() const{return m_scanning&&m_benchmarkRequested;}
    QJsonArray available() const{return m_available;}
    QJsonObject diagnostics() const;
    double latencyMs(const QString &id) const;
    bool choose(const QString &id,bool remember=true);
    bool fallback(const QString &failed,const QString &reason);
    void detectDevices();
    void benchmarkDevices();
    QDialog *createDialog(QWidget *parent);
signals:
    void changed();
    void selectionChanged();
    void detectionFinished();
    void benchmarkingChanged(bool active);
    void finished();
private:
    friend int runRvInferenceDiscoveryTest(const QString &reportPath);
    void startDetection(bool benchmark);
    void startBenchmark();
    void next();
    void processFinished(int code);
    void complete(const QString &status);
    QString preferredCandidate() const;
    void setSelected(const QString &id);
    QProcess m_process;
    QTimer m_timeout;
    QTemporaryDir m_work;
    std::shared_ptr<void> m_paths;
    QJsonArray m_candidates,m_available,m_rejected,m_benchmarked;
    QString m_selected,m_preference,m_report,m_status;
    int m_index=-1,m_benchmarkProbes=0;
    bool m_started=false,m_scanning=false,m_enumerating=false,m_timedOut=false,m_benchmarkRequested=false;
};

int runRvInferenceDiscoveryTest(const QString &reportPath);
int runRvInferenceUiTest(QWidget *window,const QString &reportPath);
