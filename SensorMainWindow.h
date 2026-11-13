#pragma once
#include <QMainWindow>
#include <QPointer>
#include <QVector>
#include <QJsonObject>

class LanguageManager;
class TactileDockWidget;
class DepthMapWidget;
class RtspCameraPage;
class RvSessionController;
class RvDeviceManagerDialog;
class QMenu;
class QAction;
class QMessageBox;
struct RvDeviceProfile;

class SensorMainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit SensorMainWindow(LanguageManager *language, QWidget *parent=nullptr);
    ~SensorMainWindow() override;
    RvSessionController *session() const { return m_session; }
    DepthMapWidget *depth() const { return m_depth; }
    TactileDockWidget *tactile() const { return m_tactile; }
    void resetLayout();
    void offerUsbSetup(bool existingDevice, bool networkSetup = false, const QJsonObject &identity = {});
signals:
    void usbDeviceConnectionRequested(RvDeviceManagerDialog *dialog);
protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void retranslate();
    void showAbout();
    void saveLayout();
    void refreshPresets();
    void savePreset();
    RvDeviceManagerDialog *showDeviceManager(bool modal = false);
    void offerBoardImport(const RvDeviceProfile &device);
    LanguageManager *m_language;
    RvSessionController *m_session;
    TactileDockWidget *m_tactile=nullptr;
    DepthMapWidget *m_depth;
    RtspCameraPage *m_camera;
    QDockWidget *m_depthDock, *m_cameraDock, *m_pointCloudDock;
    void updateCameraVisibility();
    QPointer<RvDeviceManagerDialog> m_devices;
    QVector<QDockWidget *> m_docks;
    QMenu *m_file, *m_view, *m_settings, *m_languages, *m_rendering, *m_presets;
    QAction *m_exit, *m_reset, *m_savePreset, *m_deletePreset, *m_deviceAction, *m_inferenceAction;
    QAction *m_english, *m_chinese, *m_standard, *m_smooth, *m_about;
    QPointer<QMessageBox> m_inferenceWarning;
    bool m_closing=false, m_tunerCloseReady=false;
};
