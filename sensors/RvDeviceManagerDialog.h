#pragma once
#include "RvDeviceStore.h"
#include <QtCore/QPointer>
#include <QtWidgets/QDialog>
#include <memory>
class RvSessionController;
class DexterousHandVisualization;
class QTableWidget;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QProcess;
class QTimer;
class QPlainTextEdit;
class QCloseEvent;
class QShowEvent;
class RvDeviceManagerDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RvDeviceManagerDialog(RvSessionController *session,QWidget *parent=nullptr);
    ~RvDeviceManagerDialog() override;
    void importBoardFilesForStart(const RvDeviceProfile &device);
    void beginUsbEnrollment();
    void finishUsbEnrollment(bool added);
signals:
    void usbConnectionRequested(RvDeviceManagerDialog *dialog);
protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    friend int runRvDeviceTests(DexterousHandVisualization &,const QString &);
    void reload();
    void refresh(const QString &id={});
    void edit(const RvDeviceProfile &profile,bool fresh=false);
    RvDeviceProfile draft() const;
    void save();
    void activate();
    void remove();
    void importDevices();
    void importLiteSrModel(bool confirmed = false);
    void exportDevices();
    void runWorker(const QString &operation);
    void readDeviceInformation();
    void confirmSerialRead();
    void acceptWorkerResult(const QJsonObject &result);
    void operationFailed(const QString &reason,int code=-1);
    void showBoardImportFailure(const QString &reason,const QString &phase,int code);
    void cancelOperation();
    void invalidateSerial();
    void message(const QString &text,bool error=false);
    void busy(bool value);
    void updateFingerprint();
    QPointer<RvSessionController> m_session;
    std::unique_ptr<RvDeviceStore> m_store;
    QString m_id,m_operation,m_probeHost,m_savedFingerprint,m_probeId,m_probeUsername,m_probeKey,m_workerFailure;
    int m_probePort=0;
    bool m_fresh=false,m_readingSerial=false,m_resumeImaging=false;
    bool m_usbEnrollmentPending=false;
    QWidget *m_details;
    QTableWidget *m_table;
    QLineEdit *m_name,*m_host,*m_serial,*m_key,*m_username;
    QSpinBox *m_port;
    QComboBox *m_calibration,*m_rotation,*m_cameraMount;
    QCheckBox *m_legacy,*m_confirm;
    QLabel *m_status,*m_fingerprint,*m_active;
    QPushButton *m_save,*m_use,*m_remove,*m_probe,*m_verify,*m_forget,*m_readSerial,*m_importLiteSr;
    QPlainTextEdit *m_serialHelp;
    QVector<QPushButton*> m_actions;
    QProcess *m_worker;
    QTimer *m_timeout;
};
