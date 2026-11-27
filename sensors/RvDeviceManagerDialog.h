#pragma once

#include <QDialog>
#include <QJsonObject>

class QLabel;
class QListWidget;
class QPushButton;
class DepthSdkClient;
class RvSessionController;
class QComboBox;

class RvDeviceManagerDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RvDeviceManagerDialog(RvSessionController *session, QWidget *parent = nullptr);
    void importBoardFilesForStart(const QString &id);
    void connectViaUsb(quint64 checkId = 0);
    bool operationPending() const;
    void finishUsbEnrollment(bool added);

protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    friend int runPortSmokeTests(class SensorMainWindow &, const QString &);
    enum class BoardOperation { None, RepairForStart, Maintenance, Recovery };
    QString selectedId() const;
    bool canManageSavedDevice() const;
    void refresh(const QString &id);
    void syncControls();
    void initializeLabels();
    void selectDevice();
    void removeDevice();
    void checkBoardFiles();
    void runBoardOperation(BoardOperation operation, const QString &id);
    void updateBoardOperation(const QJsonObject &status);
    void updateUsbOperation(const QJsonObject &status);
    void message(const QString &text, bool error = false);

    RvSessionController *const m_imaging;
    DepthSdkClient *const m_session;
    QListWidget *const m_devices;
    QPushButton *const m_usb;
    QPushButton *const m_select;
    QPushButton *const m_remove;
    QPushButton *const m_maintain;
    QPushButton *const m_recover;
    QComboBox *m_rotation = nullptr;
    QLabel *const m_status;
    bool m_registryReady = false;
    bool m_usbPending = false;
    BoardOperation m_operation = BoardOperation::None;
    QString m_operationDevice;
    quint64 m_operationStopRevision = 0;
};
