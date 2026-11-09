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

// Discovery/model probes live in bounded child processes. The GUI only exposes
// successful probes from this launch; saved user preferences never imply support.
class RvInferenceSettings final:public QObject {
    Q_OBJECT
public:
    static RvInferenceSettings *instance();
    explicit RvInferenceSettings(QObject *parent=nullptr);
    ~RvInferenceSettings() override;
    QString selected() const;
    bool started() const{return m_started;}
    bool scanning() const{return m_scanning;}
    QJsonArray available() const{return m_available;}
    QJsonObject diagnostics() const;
    double latencyMs(const QString &id) const;
    bool choose(const QString &id,bool remember=true);
    bool fallback(const QString &failed,const QString &reason);
    void refresh();
    QDialog *createDialog(QWidget *parent);
signals:
    void changed();
    void selectionChanged();
    void finished();
private:
    void next();
    void processFinished(int code);
    void complete();
    void setSelected(const QString &id);
    QProcess m_process;
    QTimer m_timeout;
    QTemporaryDir m_work;
    std::shared_ptr<void> m_paths;
    QJsonArray m_candidates,m_available,m_rejected;
    QString m_selected,m_preference,m_report,m_status;
    int m_index=-1;
    bool m_started=false,m_scanning=false,m_enumerating=false,m_timedOut=false;
};

int runRvInferenceDiscoveryTest(const QString &reportPath);
int runRvInferenceUiTest(QWidget *window,const QString &reportPath);
