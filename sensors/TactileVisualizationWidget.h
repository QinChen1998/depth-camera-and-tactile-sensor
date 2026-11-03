#pragma once
#include "TactileMockSource.h"
#include "TactileHistory.h"
#include "TactileMetrics.h"
#include "TactileSerialSource.h"
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <QSet>
#include <array>
#include <functional>
class QLabel;
class QPushButton;
class QToolButton;
class QComboBox;
class QButtonGroup;
class QQuickWidget;
class TactileSceneAdapter;
class QCheckBox;
class TactileCurveCanvas;
class QSplitter;
class QSlider;
class QTableWidget;
class QSpinBox;
class QDoubleSpinBox;
class QVBoxLayout;

class TactileVisualizationWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TactileVisualizationWidget(QWidget *parent = nullptr);
    ~TactileVisualizationWidget() override;
    // Construction/show never starts a source. Dock owner must explicitly call start().
    void start();
    // The normal application explicitly requests one attempt; offline tests and
    // standalone widgets do not acquire hardware just by being shown.
    void connectDefaultSerial();
    void serialPortChanged(const QString &port,bool arrived);
    void stop();
    void setPaused(bool paused);
    void setScenario(tactile::Scenario scenario);
    bool running() const;
    bool paused() const { return m_userPaused; }
    void setDockManaged(bool managed) { m_dockManaged=managed; }
    void setSuspended(bool suspended);
    const tactile::TactileHistory &history() const { return m_finger==m_hardwareType?m_serialHistory:m_history; }
    quint64 paintCount() const { return m_paintCount; }
    void recordPaint() { ++m_paintCount; if(m_metrics.enabled)m_metrics.curve.record(m_refreshCount,m_metrics.sampleMs); }
    void recordView() { if(m_metrics.enabled)m_metrics.view.record(m_refreshCount,m_metrics.sampleMs); }
    tactile::DisplayMetrics &metrics() { return m_metrics; }
    quint64 refreshCount() const { return m_refreshCount; }
    const tactile::Frame &frame() const { return m_frame; }
    tactile::Force selectedForce() const;
    tactile::Force chartForce() const;
    tactile::Force curveForce(const tactile::Force &raw,int point) const;
    tactile::Force displayPointForce(const QVector<tactile::Force> &raw,int point) const;
    int displayPointCount() const;
    bool hasDisplayBaseline() const;
    int selectedPoint() const { return qBound(0,m_selectedPoints[m_finger],qMax(0,displayPointCount()-1)); }
    int selectedFinger() const { return m_finger; }
    int selectedModule() const { return 0; }
    bool moduleMode() const { return false; }
protected:
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    friend class TactileCurveCanvas;
    friend int runTactileVisualizationTests(const QString &path);
    void receiveSerialSample(const tactile::SerialSample &received);
    void refresh();
    void refreshDisplay(bool liveChanged);
    void updateValues();
    void updatePanel();
    void updatePanelStatus();
    void updateReadings();
    void updateCurve();
    void updateDisplayPointOptions();
    void selectPoint(int point);
    void retranslate();
    void updateSettingsStyle();
    void updateRightPanelLayout();
    void updateSensorChoices();
    void createSerialControls(QVBoxLayout *root);
    QStringList refreshSerialPorts();
    void scanIdleSerialPorts();
    void restoreSerialSettings();
    void finishInitialSerialConnection(const tactile::SerialSample &sample);
    void restoreDisplaySettings();
    void saveDisplaySettings();
    void restoreSplitterLayout();
    void saveSplitterLayout();
    enum class SerialAttempt { Manual, Hotplug, Retry };
    void connectSerial(SerialAttempt attempt=SerialAttempt::Manual);
    void cancelSerialRetry();
    void disconnectSerial();
    void setDeviceConfigStatus(const QString &text);
    void deviceSettingsEdited();
    void writeDeviceAddress();
    void receiveDeviceConfig(const tactile::DeviceConfigResult &result);
    void selectFrame();
    void updateSerialStatus();
    double noiseDisplayThreshold() const;
    tactile::Force displayBaselineForce(int point) const;
    tactile::Frame visualFrame() const;
    tactile::Frame tableFrame() const;
    tactile::Frame arrowFrame() const;
    tactile::Frame hardwareDisplayFrame(bool interpolate) const;
    void clearHardwareDisplay();
    void setDisplayZero();
    void applyDisplayZero(const tactile::Frame &baseline);
    tactile::TactileDisplayFilter m_displayFilter;
    tactile::TactileSerialSource m_serial;
    tactile::TactileHistory m_serialHistory;
    tactile::SerialSample m_serialSample;
    tactile::Frame m_mockFrame, m_liveFrame, m_heldTableFrame;
    tactile::Frame m_arrowFrom;
    qint64 m_trustedReceivedMs=-1;
    QElapsedTimer m_arrowTransition;
    int m_hardwareType=-1;
    QComboBox *m_port, *m_baud;
    std::function<QStringList()> m_availableSerialPorts;
    QTimer m_portScanTimer;
    int m_missingPortScans=0;
    QString m_removedSerialPort;
    QStringList m_observedSerialPorts;
    QSet<QString> m_portsReportedRemoved;
    bool m_autoConnectEnabled=false;
    QTimer m_initialResponseTimer;
    QTimer m_serialRetryTimer;
    QString m_serialRetryPort;
    QString m_defaultSerialPort;
    bool m_defaultConnectRequested=false, m_defaultConnectAttempted=false;
    bool m_waitingForFirstHardwareFrame=false;
    quint64 m_saveReminderGeneration=0;
    QSpinBox *m_slave, *m_timeout, *m_interval, *m_jumpThreshold;
    QPushButton *m_noiseEnabled;
    QPushButton *m_reverseZArrows;
    QSpinBox *m_noiseThreshold;
    QPushButton *m_writeAddress;
    QLabel *m_configStatus, *m_slaveLabel, *m_baudLabel;
    tactile::SerialSettings m_connectedSettings;
    int m_pendingAddress=0, m_pendingBaud=0;
    QString m_pendingPort;
    bool m_verifyingAddress=false;
    QTimer m_configTimer;
    bool m_configAwaitClose=false;
    QLabel *m_filterStatus;
    QDoubleSpinBox *m_arrowGain;
    QDoubleSpinBox *m_arrowWidthGain;
    QDoubleSpinBox *m_resultantSizeGain;
    QCheckBox *m_crc;
    QPushButton *m_statusToggle;
    QPushButton *m_hideReadingsToggle;
    QPushButton *m_displaySettings;
    QToolButton *m_minimizeConnection, *m_minimizeDisplay;
    QWidget *m_displayControls;
    QPushButton *m_connect, *m_disconnect, *m_simulate, *m_connectionSettings, *m_scanPorts;
    QLabel *m_serialStatus;
    QWidget *m_serialControls;
    tactile::TactileMockSource m_source;
    tactile::TactileHistory m_history;
    tactile::DisplayMetrics m_metrics;
    bool m_userPaused=false, m_suspended=false, m_dockManaged=false;
    quint64 m_paintCount=0;
    void applyPause();
    tactile::Frame m_frame;
    QTimer m_timer;
    QTimer m_readingsTimer, m_curveTimer;
    QVector<tactile::Force> m_readingsBaseline;
    quint64 m_refreshCount = 0;
    int m_finger = 0;
    std::array<int,3> m_selectedPoints{};
    tactile::Frame m_tareFrame;
    QQuickWidget *m_canvas;
    TactileSceneAdapter *m_adapter;
    QPushButton *m_pointsToggle;
    QPushButton *m_resultantToggle, *m_componentsToggle;
    QSlider *m_opacitySlider;
    QLabel *m_opacityLabel, *m_opacityValue;
    QPushButton *m_resetView, *m_frontView;
    QLabel *m_viewError;
    QLabel *m_chartLabel, *m_tableLabel;
    QTableWidget *m_pointTable;
    QPushButton *m_tareButton;
    TactileCurveCanvas *m_curves;
    QSplitter *m_splitter;
    bool m_splitterReady=false,m_restoringSplitter=false;
    QWidget *m_fingerBox = nullptr;
    QToolButton *m_foldSensors;
    std::array<QLabel *,3> m_values;
    QButtonGroup *m_fingers;
    QPushButton *m_pause;
    QComboBox *m_scenarios;
    QComboBox *m_density;
    QComboBox *m_displayMode;
};
