#pragma once

#include "DepthSnapshot.h"
#include "ImageDisplaySettings.h"
#include "RvSessionController.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QSharedPointer>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>
#include <memory>

class QPushButton;
class QCheckBox;
class RvDepthView;
class QJsonObject;
class QComboBox;
class QLabel;
class QTimer;
class QTabWidget;
class QProgressBar;
namespace RvDepthDiagnostic { class SequenceRecorder; }

class DepthMapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DepthMapWidget(QWidget *parent = nullptr);
    ~DepthMapWidget() override;
    void setSessionController(RvSessionController *controller);
    RvSessionController *sessionController() const;
    void acceptRvDepth(const QByteArray &depth,const QJsonObject &metadata);
    void clearRvDepth(const QString &reason);
    void acceptRvSnapshot(const QByteArray &depth, const QJsonObject &metadata, const QImage &camera);
    void acceptAssistSnapshot(const QByteArray &depth,const QJsonObject &metadata,const QImage &camera,
                              const RvTofFrame &tof=RvTofFrame(),const QByteArray &diagnostic=QByteArray(),
                              const QImage &inferenceInput=QImage());
    QString startDiagnosticRecording(const QString &folder,QString *error=nullptr);
    void clearAssistDepth(const QString &reason);
    DepthSnapshot snapshot() const;
    int selectedWidth() const;
    int selectedHeight() const;
    int displayRotationDegrees() const;
    void setDisplayRotationDegrees(int degrees);
    int nativeTofRotationDegrees() const { return m_nativeTofRotation; }

protected:
    void changeEvent(QEvent *event) override;

private:
    void beginImagingSwitchUi(const QString &scheme,int estimatedMs);
    void finishImagingSwitchUi();
    void cancelImagingSwitchUi();
    void advanceImagingSwitchUi();
    bool eventFilter(QObject *watched,QEvent *event) override;
    void saveCurrentFrame();
    void chooseDiagnosticRecordingFolder();
    void updateDiagnosticRecording();
    void editNativeTofRotation();
    void syncNativeTofRotation();
    QPushButton *m_nativeTofAngle = nullptr;
    int m_nativeTofRotation = 90;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_monocularButton = nullptr;
    QPushButton *m_splitCorrectionButton = nullptr;
    QWidget *m_diagnosticsRow = nullptr;
    QPushButton *m_tofPointsButton = nullptr, *m_pairedRgbButton = nullptr;
    QPushButton *m_recordButton = nullptr, *m_openRecording = nullptr;
    QLabel *m_recordStatus = nullptr;
    QTimer *m_recordTimer = nullptr;
    std::unique_ptr<RvDepthDiagnostic::SequenceRecorder> m_recorder;
    QString m_recordingFolder;
    QCheckBox *m_saveCamera = nullptr;
    QCheckBox *m_blackOutsideRange = nullptr;
    QComboBox *m_colorMode = nullptr;
    QLabel *m_colorMinimumLabel = nullptr, *m_colorMaximumLabel = nullptr;
    QPushButton *m_applyColorRange = nullptr;
    DepthSnapshot m_snapshot;
    DepthSnapshot m_assistSnapshot;
    void retranslateUi();
    void syncSessionControls();
    void syncAutomaticConnectionUi();
    QStringList m_sessionControlsKey;
    void syncDisplayColors();
    void updateRvPresentation();

    QSharedPointer<ImageDisplaySettings> m_imageSettings;
    RvDepthView *m_rvCanvas = nullptr;
    RvDepthView *m_assistCanvas = nullptr;
    QTabWidget *m_depthTabs = nullptr;
    QWidget *m_switchOverlay=nullptr;
    QLabel *m_switchTitle=nullptr,*m_switchDetail=nullptr;
    QProgressBar *m_switchProgress=nullptr;
    QTimer *m_switchUiTimer=nullptr;
    QElapsedTimer m_switchUiClock,m_switchFinishClock;
    int m_switchEstimatedMs=10000,m_switchFinishFrom=0;
    bool m_switchUiActive=false,m_switchUiFinishing=false;
    QString m_switchUiScheme;
    QPointer<RvSessionController> m_session;
    QWidget *m_rvControls = nullptr;
    QLabel *m_rvModelCaption = nullptr;
    QComboBox *m_rvModel = nullptr;
    QPushButton *m_rvStart = nullptr, *m_rvStop = nullptr, *m_rvReconnect = nullptr;


};
