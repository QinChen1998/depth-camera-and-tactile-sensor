#include "../SensorMainWindow.h"
#include "../LanguageManager.h"
#include "../RuntimeDiscovery.h"
#include "../TactileDockWidget.h"
#include "../RenderingSettings.h"
#include "RvDeviceFixture.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvAssistDepth.h"
#include "../sensors/RvAssistFusion.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RtspCameraPage.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvDeviceManagerDialog.h"
#include "../sensors/RvUsbEnrollment.h"
#include "../sensors/RvCaptureCalibration.h"
#include "../sensors/RvTunerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QFormLayout>
#include <QLabel>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QQuickWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>

namespace {
void settle(int milliseconds){QElapsedTimer timer;timer.start();while(timer.elapsed()<milliseconds){QApplication::processEvents();QThread::msleep(5);}}
}

int runPortSmokeTests(SensorMainWindow &window,LanguageManager &language,const QString &path)
{
    QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
    QTextStream out(&file);bool pass=true;
    const auto check=[&](const QString &name,bool ok){out<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;pass&=ok;};
    settle(900);
    const auto docks=window.findChildren<QDockWidget *>(QString(),Qt::FindDirectChildrenOnly);
    check("EXACTLY_THREE_MODULES",docks.size()==3);
    const auto *inferenceAction=window.findChild<QAction *>("inferenceDeviceAction");
    const auto *settingsMenu=window.findChild<QMenu *>("settingsMenu");
    check("INFERENCE_DEVICE_IN_SETTINGS",inferenceAction&&settingsMenu&&settingsMenu->actions().contains(inferenceAction));
    check("NO_TENSORRT_PREVIEW_MENU",!window.findChild<QAction *>("tensorRtPreviewAction"));
    check("NO_DTOF_TUNER_UI",!window.findChild<RvTunerWidget *>());
    check("NO_INACTIVE_DEPTH_PAGES",!window.depth()->findChild<QWidget *>("depthMapCanvas")
        &&!window.depth()->findChild<QWidget *>("rvLiteSrDepthCanvas"));
    for(const auto *name:{"depthSourceControls","depthMapResolutionCombo","depthMapDisplayCombo",
                         "rvDepthResolution","rvDepthOnBoard","rvDepthMethod","rvColorOutput",
                         "rvLiteSrWhiteCounts","rvLiteSrInputRotation"})
        check(QString("NO_OBSOLETE_CONTROL_")+name,!window.depth()->findChild<QWidget *>(name));
    const auto *toolbar=window.findChild<QToolBar *>("sensorToolBar");
    const auto *viewMenu=window.findChild<QMenu *>("viewMenu");
    bool viewToggles=viewMenu;
    for(auto *dock:docks)viewToggles&=viewMenu&&viewMenu->actions().contains(dock->toggleViewAction());
    check("MODULES_AVAILABLE_ONLY_IN_VIEW_MENU",!toolbar&&viewToggles);
    for(const auto *name:{"tactileDock","depthMapDock","rtspCameraDock"})
        check(QString("DOCK_")+name,window.findChild<QDockWidget *>(name)!=nullptr);
    auto *models=window.findChild<QComboBox *>("rvDepthModel");
    const bool vgaSupported=RvAssistDepth::supportsHostModel(QLatin1String(RvCameraView::UniDepthVgaNative16));
    const QString visibleHostModel=QLatin1String(vgaSupported?RvCameraView::UniDepthVgaNative16:RvCameraView::UniDepthVgaNative16);
    const QString visibleHostScheme="host_"+visibleHostModel;
    check("EXACT_IMAGING_SCHEMES",models&&models->count()==3&&models->itemData(0)==visibleHostScheme);
    check("OTHER_IMAGING_SCHEMES_ABSENT",models
        &&models->findData("host_unidepth_wide_576_fp16_tof16")==-1
        &&models->findData("board_litesr_near_full_host_contour_hd")==-1
        &&models->findData("board_litesr_near_128x128_depth_only")==-1);
    check("NO_HOST_DIRECTION_OR_CORRECTION_SELECTORS",
          !window.depth()->findChild<QComboBox *>("rvAssistInputRotation")
          &&!window.depth()->findChild<QComboBox *>("rvUniDepthExperiment"));
    auto *session=window.session();
    check("FP16_NATIVE16_DEFAULT_H",session->assistHostModel()=="unidepth_rgb672_vga_fp16_tof16"&&session->assistExperiment()=="H");
    check("FIRST_RUN_VISIBLE_HOST_SCHEME",session->currentImagingScheme()==visibleHostScheme
        &&models&&models->currentData()==session->currentImagingScheme()&&session->assistDepthEnabled());
    const QSettings firstRun;
    const auto initialPresentation=ImageDisplaySettings::shared();
    check("FIRST_RUN_PROFILE",firstRun.value("tactile/display/arrowGain").toDouble()==25
        &&firstRun.value("tactile/display/arrowWidthGain").toDouble()==1.2
        &&firstRun.value("tactile/display/resultantSizeGain").toDouble()==1.5
        &&firstRun.value("tactile/display/reverseZArrows").toBool()
        &&firstRun.value("tactile/serial/baud").toInt()==921600&&!firstRun.contains("tactile/serial/port")
        &&initialPresentation->rotationDegrees()==0&&initialPresentation->blackOutsideRange()
        &&initialPresentation->colorMinimum()==.01&&initialPresentation->colorMaximum()==3);
    check("STARTUP_CAPTURE_STOPPED",session->userStopped());
    RvDeviceStore initialDevices;
    check("FIRST_RUN_HAS_NO_TRUSTED_OR_ACTIVE_DEVICE",initialDevices.profiles().isEmpty()
        &&initialDevices.activeId().isEmpty()&&session->deviceProfile().id.isEmpty()
        &&!session->controlledSource());
    session->startAutomatically();
    check("FIRST_RUN_NO_HISTORICAL_SSH_PROBE",session->userStopped()&&!session->busy());
    // The remaining tests exercise the controls for an explicitly enrolled device.
    const auto fixture=offlineRvDevice();
    initialDevices.save({fixture},fixture.id);
    session->reloadDeviceProfiles();
    for(const auto* retired:{"board_litesr_near_384x256_host_contour_hd","board_litesr_near_384x256_high_accuracy_host_contour_hd","board_litesr_near_full_host_contour_hd","board_litesr_near_128x128_depth_only","board_litesr_384x256_contour_relative","board_tiny_nv12_128x80","unknown"}){
        check(QString("RETIRED_SCHEME_ABSENT_")+retired,models->findData(retired)==-1);
        session->selectImagingScheme(retired,{});
        check(QString("RETIRED_SCHEME_API_REJECTED_")+retired,session->currentImagingScheme()==visibleHostScheme&&session->userStopped());
        QSettings().setValue("depth/imagingScheme",retired);
        QSettings().setValue("depth/boardLiteSrHostHoleMode",2);
        RvSessionController reopened;
        check(QString("RETIRED_SCHEME_MIGRATES_TO_H_")+retired,reopened.currentImagingScheme()==visibleHostScheme&&reopened.userStopped()&&reopened.assistDepthEnabled()&&reopened.dtofCandidateMode()==14&&!QSettings().contains("depth/boardLiteSrHostHoleMode"));
    }
    for(const auto *scheme:{"board_native_tof48","board_native_tof48_high_accuracy"}){
        session->selectImagingScheme(scheme,{});
        check(QString("NATIVE_MODE_SELECTS_WHILE_STOPPED_")+scheme,session->userStopped()&&!session->busy()&&!session->assistDepthEnabled()
            &&session->currentImagingScheme()==scheme&&window.depth()->selectedWidth()==48&&window.depth()->selectedHeight()==32);
        RvSessionController reopened;
        check(QString("NATIVE_MODE_PERSISTS_")+scheme,reopened.currentImagingScheme()==scheme&&!reopened.assistDepthEnabled()&&reopened.userStopped());
    }
    session->selectImagingScheme(visibleHostScheme,{});
    check("NO_BOARD_DEPTH_OR_REPAIR_UI",!window.depth()->findChild<QWidget*>("rvDtofFastDepthCanvas")&&!window.depth()->findChild<QWidget*>("rvLiteSrRepairSettings")&&!window.depth()->findChild<QWidget*>("rvBoardLiteSrHostContour"));
    auto *depthStart=window.findChild<QPushButton *>("rvDepthStart");
    auto *depthStop=window.findChild<QPushButton *>("rvDepthStop");
    auto *depthReconnect=window.findChild<QPushButton *>("rvDepthReconnect");
    auto *cameraStart=window.findChild<QPushButton *>("rtspCameraStart");
    auto *cameraStop=window.findChild<QPushButton *>("rtspCameraStop");
    auto *cameraReconnect=window.findChild<QPushButton *>("rtspCameraReconnect");
    const bool controlsPresent=depthStart&&depthStop&&depthReconnect&&cameraStart&&cameraStop&&cameraReconnect;
    check("SHARED_CONTROLS_IDLE",controlsPresent&&depthStart->isEnabled()&&cameraStart->isEnabled()
        &&!depthStop->isEnabled()&&!cameraStop->isEnabled()&&depthReconnect->isEnabled()&&cameraReconnect->isEnabled());
    if(controlsPresent){
        session->selectImagingScheme(visibleHostScheme,{});
        const auto checkSharedStop=[&](const QString &name,QPushButton *begin,QPushButton *stop){
            bool linked=false;
            QMetaObject::Connection observation;
            observation=QObject::connect(session,&RvSessionController::configurationChanged,&window,[&]{
                if(session->userStopped())return;
                QObject::disconnect(observation);
                linked=!depthStart->isEnabled()&&!cameraStart->isEnabled()
                    &&depthStop->isEnabled()&&cameraStop->isEnabled()
                    &&!depthReconnect->isEnabled()&&!cameraReconnect->isEnabled();
                stop->click();
            });
            begin->click();
            QObject::disconnect(observation);
            check(name,linked&&session->userStopped()&&depthStart->isEnabled()&&cameraStart->isEnabled());
        };
        const auto preflight=session->hostInferencePreflightError();
        if(preflight.isEmpty()){
            checkSharedStop("DEPTH_START_CAMERA_STOP",depthStart,cameraStop);
            checkSharedStop("CAMERA_RECONNECT_DEPTH_STOP",cameraReconnect,depthStop);
        }else{
            depthStart->click();
            settle(50);
            auto *warning=window.findChild<QMessageBox *>("hostInferenceUnavailable");
            check("HOST_GPU_WARNING_WITH_RUNTIME_ADVICE",warning&&warning->isVisible()
                &&warning->text()==preflight
                &&warning->informativeText()==QCoreApplication::translate("SensorMainWindow",
                    "Open Settings > Inference Device to detect usable devices, then click Start again.")
                &&session->userStopped()&&!session->busy());
            if(warning){warning->accept();settle(20);}
        }
        if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"){
            watchRvUsbDevices(session,&window);
            session->startAutomatically();
            session->startAutomatically(true);
            settle(50);
            check("AUTOMATIC_START_BLOCKED_OFFLINE",session->userStopped()&&!session->busy());
            check("USB_AUTOMATIC_CHECK_BLOCKED_OFFLINE",!session->automaticStartAllowed(session->deviceProfile().id));
            const auto stopRevision=session->stopRevision();session->stop();
            check("USB_PENDING_CHECK_REVOKED_BY_STOP",session->stopRevision()>stopRevision);
        }
    }
    session->selectImagingScheme(visibleHostScheme,{});
    session->setAssistExperiment("A");session->setAssistExperiment("B");session->setAssistExperiment("G");
    check("OTHER_CORRECTIONS_REJECTED",session->assistExperiment()=="H");
    for(const auto *id:{"unidepth","unidepth_wide","unidepth_wide_576","unidepth_wide_576_fp16","unidepth_wide_576_fp16_tof16","colorsr_rgb","colorsr_nv12","depthor_v3_fp32","depthor_v3_fp16","depthor_v3_int8"})
        check(QString("REMOVED_MODEL_REJECTED_")+id,!RvAssistDepth::supportsHostModel(id)&&RuntimeDiscovery::rvAssistModelRoot(id).isEmpty());
    const auto original=session->dtofCandidateMode();
    session->selectImagingScheme("host_depthor_v3_fp32",{});
    session->selectImagingScheme("board_litesr_near_high_resolution",{});
    check("REMOVED_SCHEME_API_REJECTED",session->dtofCandidateMode()==original&&session->assistHostModel()==visibleHostModel);
    check("REMOVED_BOARD_MODE_REJECTED",!session->setDtofCandidate(15,{}));
    check("FP16_SELECTION_STOPPED",session->dtofCandidateMode()==14&&session->userStopped()&&session->assistDepthEnabled()&&session->assistExperiment()=="H");
    bool idle=true;for(auto *process:window.findChildren<QProcess *>())idle&=process->state()==QProcess::NotRunning;
    check("NO_WORKER_OR_HARDWARE_STARTED",idle);
    const auto worker=RuntimeDiscovery::rvControlForLaunch();
    check("NATIVE_WORKER_MANIFEST_VALID",worker.ok()&&worker.error.isEmpty());
    check("NATIVE_WORKER_OWN_PROJECT",worker.executable.startsWith(RuntimeDiscovery::applicationRoot(),Qt::CaseInsensitive));
    for(const auto *name:{"renderModeMenu","languageMenu","layoutPresetMenu"})
        check(QString("MENU_")+name,window.findChild<QMenu *>(name)!=nullptr);
    auto *rendering=RenderingSettings::instance();const auto prior=rendering->lowCostRendering();
    check("STARTUP_SMOOTH_SELECTED",prior&&window.findChild<QAction *>("actionRenderSmooth")->isChecked());
    window.findChild<QAction *>("actionRenderSmooth")->trigger();
    check("SMOOTH_RENDERING_WORKS",rendering->lowCostRendering());
    window.findChild<QAction *>("actionRenderStandard")->trigger();
    check("STANDARD_RENDERING_WORKS",!rendering->lowCostRendering());rendering->setLowCostRendering(prior);
    language.setLanguage(LanguageManager::Language::SimplifiedChinese);settle(100);
    check("CHINESE_WINDOW",window.windowTitle()==QStringLiteral("触想工作台（内测版）"));
    auto *about = window.menuBar()->findChild<QAction *>("aboutAction");
    check("CHINESE_ABOUT_MENU",about && window.menuBar()->actions().contains(about)
        && about->text()==QStringLiteral("关于"));
    if(about)about->trigger();
    auto *aboutDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    bool aboutName=false, aboutVersion=false, aboutLogo=false;
    if(aboutDialog){
        for(auto *label:aboutDialog->findChildren<QLabel *>()){
            aboutName |= label->text()==QStringLiteral("触想工作台");
            aboutVersion |= label->text()==QStringLiteral("v1.0 · 内测版");
            aboutLogo |= !label->pixmap().isNull();
        }
    }
    check("ABOUT_BRANDING",aboutDialog && aboutDialog->windowTitle()==QStringLiteral("关于")
        && aboutName && aboutVersion && aboutLogo && !window.windowIcon().isNull());
    if(aboutDialog)aboutDialog->reject();
    check("CHINESE_HOST_ONLY_SCHEME",models&&models->count()==3&&models->itemText(0)==QString::fromUtf8("上位机辅助推理（640×480）"));
    check("CHINESE_SHARED_CONTROL_LABELS",controlsPresent&&depthStart->text()==cameraStart->text()
        &&depthStop->text()==cameraStop->text()&&depthReconnect->text()==cameraReconnect->text());
    check("CHINESE_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()==QString::fromUtf8("上位机辅助深度推理（640×480）。"));
    check("REMOVED_DEPTH_UI",!window.findChild<QPushButton *>("rvUniDepthCorrectionPoints")
        && !window.findChild<QPushButton *>("rvUniDepthDiagnostic")
        && !window.findChild<QPushButton *>("rvUniDepthDiagnosticSequence")
        && !window.findChild<QLabel *>("rvDepthInfo"));
    const auto *depthFps=window.findChild<QLabel *>("depthFpsLabel");
    const auto *depthSave=window.findChild<QPushButton *>("depthMapSave");
    check("DEPTH_FPS_AND_SAVE_PRESENT",depthFps&&depthSave&&depthSave->isVisible()
        && depthSave->parentWidget()==depthFps->parentWidget()
        && depthSave->text()==QString::fromUtf8("保存深度图"));
    check("REMOVED_IMAGING_ACTIONS",!window.findChild<QAction *>("rvDepthCoverageAction")
        && !window.findChild<QAction *>("rvStreamAddressAction")
        && !window.findChild<QAction *>("rvBlackOutsideRangeAction")
        && !window.findChild<QAction *>("rvTofProjectionAction") && window.menuBar()->actions().size()==4);
    auto *blackOutside=window.findChild<QCheckBox *>("depthBlackOutsideRange");
    check("BLACK_OUTSIDE_AT_RIGHT_OF_DEPTH_FOOTER",blackOutside&&blackOutside->isVisible()&&depthSave
        && blackOutside->parentWidget()==depthSave->parentWidget()&&blackOutside->x()>depthSave->x()
        && blackOutside->text()==QString::fromUtf8("超范围黑色"));
    if(blackOutside){
        const auto settings=ImageDisplaySettings::shared();
        const bool priorBlack=settings->blackOutsideRange();
        blackOutside->click();
        check("BLACK_OUTSIDE_CONTROL_UPDATES_SHARED_PALETTE",settings->blackOutsideRange()!=priorBlack
            && QSettings().value("imageDisplay/blackOutsideRange").toBool()==settings->blackOutsideRange());
        settings->setBlackOutsideRange(priorBlack);
    }
    auto *cameraInfo=window.findChild<QLabel *>("rtspCameraVideoInfo");
    auto *rotation=window.findChild<QComboBox *>("rvDisplayRotation");
    auto *cameraCanvas=window.findChild<QWidget *>("rtspCameraLatestFrame");
    check("ROTATION_ON_CAMERA_FOOTER_RIGHT",rotation&&cameraInfo
        && rotation->parentWidget()==cameraInfo->parentWidget()&&rotation->x()>cameraInfo->x()
        && rotation->count()==4&&!window.findChild<QMenu *>("rvImagingSettingsMenu"));
    if(rotation&&cameraCanvas){
        const int priorRotation=window.depth()->displayRotationDegrees();
        const int selected=(priorRotation+90)%360;
        rotation->setCurrentIndex(rotation->findData(selected));
        check("ROTATION_STILL_LINKS_DEPTH_AND_CAMERA",window.depth()->displayRotationDegrees()==selected
            && cameraCanvas->property("displayRotationDegrees").toInt()==selected);
        rotation->setCurrentIndex(rotation->findData(priorRotation));
    }
    {
        auto *angleButton=window.depth()->findChild<QPushButton*>("rvNativeTofAngle");
        auto *native=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvNativeDepthCanvas"));
        auto *host=static_cast<RvDepthView*>(window.depth()->findChild<QWidget*>("rvAssistDepthCanvas"));
        const auto priorScheme=session->currentImagingScheme();
        const auto priorGlobal=window.depth()->displayRotationDegrees();
        const auto priorInput=session->assistInputRotationDegrees();
        check("NATIVE_ANGLE_DEFAULT_90_HOST_HIDDEN",angleButton&&native&&host&&angleButton->isHidden()
            &&window.depth()->nativeTofRotationDegrees()==90&&native->nativeDisplayRotationDegrees()==90);
        session->selectImagingScheme("board_native_tof48",{});settle(20);
        check("NATIVE_ANGLE_VISIBLE_FOR_ORDINARY",angleButton&&!angleButton->isHidden());
        QByteArray raw(48*32*4,0);
        for(int i=0;i<48*32;++i){float value=.2f+.001f*i;quint32 bits;std::memcpy(&bits,&value,4);qToLittleEndian(bits,raw.data()+4*i);}
        window.depth()->acceptRvDepth(raw,{{"width",48},{"height",32},{"depth_unit","sensor_radial_m_float32"},{"frame_id",123},{"generation",456}});
        settle(80);
        const auto frozen=window.depth()->snapshot();
        const auto chooseAngle=[&](int degrees,bool accept){
            bool opened=false;
            QTimer::singleShot(0,&window,[&]{
                if(auto *dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())){
                    opened=dialog->objectName()=="rvNativeTofAngleDialog"&&dialog->intMinimum()==0&&dialog->intMaximum()==359;
                    dialog->setIntValue(degrees);dialog->done(accept?QDialog::Accepted:QDialog::Rejected);
                }
            });
            angleButton->click();return opened;
        };
        bool geometry=true,readout=true;
        const auto checkMirroredView=[&] {
            const auto rendered=native->grab().toImage();
            const double radians=((window.depth()->nativeTofRotationDegrees()+window.depth()->displayRotationDegrees())%360)*std::acos(-1.)/180.;
            const double c=std::cos(radians),s=std::sin(radians);
            const auto area=QRectF(native->rect()).adjusted(8,8,-8,-8);
            const double scale=std::min(area.width()/(48*std::abs(c)+32*std::abs(s)),area.height()/(48*std::abs(s)+32*std::abs(c)));
            // Check asymmetric source locations after screen-horizontal mirroring.
            for(const QPoint index:{QPoint(5,7),QPoint(39,21)}){
                const QPointF source(index.x()+.5-24,index.y()+.5-16);
                const QPointF point=area.center()+QPointF(-(c*source.x()-s*source.y()),s*source.x()+c*source.y())*scale;
                geometry&=rendered.pixelColor(point.toPoint())==QColor(native->colorAt(index.x(),index.y()));
                QMouseEvent hover(QEvent::MouseMove,point,native->mapToGlobal(point.toPoint()),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(native,&hover);
                const float expected=.2f+.001f*(index.y()*48+index.x());
                readout&=native->property("lastHoverText").toString()==QString("(%1, %2) dToF radial: %3 m")
                    .arg(index.x()).arg(index.y()).arg(expected,0,'f',4);
            }
        };
        for(int angle:{90,37,180,270,0}){
            check(QString("NATIVE_ANGLE_DIALOG_ACCEPTS_%1").arg(angle),chooseAngle(angle,true));
            for(int global:{0,90,180,270}){
                window.depth()->setDisplayRotationDegrees(global);checkMirroredView();
            }
            window.depth()->setDisplayRotationDegrees(priorGlobal);
            const auto current=window.depth()->snapshot();
            check(QString("NATIVE_ANGLE_PRESERVES_DATA_AND_UNIDEPTH_%1").arg(angle),current.metresFloat32LE==raw
                &&current.depthImage==frozen.depthImage&&current.metadata["native_tof_display_rotation_degrees"]==angle
                &&current.metadata["native_tof_display_mirror_horizontal"]==true
                &&host->displayRotationDegrees()==priorGlobal&&window.depth()->displayRotationDegrees()==priorGlobal
                &&session->assistInputRotationDegrees()==priorInput);
        }
        check("NATIVE_MIRROR_AFTER_ROTATION_PIXEL_GEOMETRY",geometry);
        check("NATIVE_MIRROR_HOVER_RETURNS_ORIGINAL_SAMPLE",readout);
        check("NATIVE_ANGLE_CANCEL_KEEPS_VALUE",chooseAngle(123,false)&&window.depth()->nativeTofRotationDegrees()==0);
        chooseAngle(137,true);
        session->selectImagingScheme("board_native_tof48_high_accuracy",{});settle(20);
        check("NATIVE_ANGLE_SHARED_BY_BOTH_MODES",!angleButton->isHidden()&&native->nativeDisplayRotationDegrees()==137);
        {DepthMapWidget reopened;check("NATIVE_ANGLE_SAVED_ON_REOPEN",reopened.nativeTofRotationDegrees()==137
            &&QSettings().value("depth/nativeTofRotationDegrees").toInt()==137);}
        chooseAngle(90,true);
        window.depth()->acceptRvDepth(raw,{{"width",48},{"height",32},{"depth_unit","sensor_radial_m_float32"},{"frame_id",124},{"generation",457}});
        geometry=true;readout=true;
        for(int global:{0,90,180,270}){window.depth()->setDisplayRotationDegrees(global);checkMirroredView();}
        window.depth()->setDisplayRotationDegrees(priorGlobal);
        const auto highSnapshot=window.depth()->snapshot();
        check("NATIVE_HIGH_ACCURACY_MIRROR_AND_RAW_SNAPSHOT",geometry&&readout
            &&highSnapshot.metresFloat32LE==raw&&highSnapshot.depthImage==frozen.depthImage
            &&highSnapshot.metadata["native_tof_display_mirror_horizontal"]==true);
        settle(80);window.depth()->grab().save(path+".native-tof-angle.png");
        window.depth()->clearRvDepth(QString());session->selectImagingScheme(priorScheme,{});
        check("NATIVE_ANGLE_HIDDEN_FOR_UNIDEPTH",angleButton->isHidden()&&host->displayRotationDegrees()==priorGlobal);
    }
    auto *minimum=window.findChild<QDoubleSpinBox *>("rvColorMinimum");
    auto *maximum=window.findChild<QDoubleSpinBox *>("rvColorMaximum");
    auto *applyRange=window.findChild<QPushButton *>("rvColorRangeApply");
    check("COLOR_RANGE_ON_DEPTH_FOOTER",minimum&&maximum&&applyRange&&depthFps&&blackOutside
        && depthFps->parentWidget()->isAncestorOf(minimum)&&depthFps->parentWidget()->isAncestorOf(maximum)
        && applyRange->parentWidget()==depthFps->parentWidget()
        && applyRange->text()==QString::fromUtf8("应用"));
    if(minimum&&maximum&&applyRange){
        const auto settings=ImageDisplaySettings::shared();
        const double oldMin=settings->colorMinimum(),oldMax=settings->colorMaximum();
        QString invalidText="1e1";
        int cursor=invalidText.size();
        check("COLOR_RANGE_REJECTS_NON_DECIMAL_AND_OUTSIDE_LIMITS",minimum->validate(invalidText,cursor)==QValidator::Invalid
            && minimum->minimum()==.01&&maximum->maximum()==11
            && !settings->setColorRange(0,11)&&!settings->setColorRange(.01,11.01));
        maximum->setValue(1);
        minimum->setValue(2);
        check("COLOR_RANGE_DISALLOWS_INVERTED_VALUES",!applyRange->isEnabled()
            && settings->colorMinimum()==oldMin&&settings->colorMaximum()==oldMax);
        minimum->setValue(.01);
        maximum->setValue(11);
        check("COLOR_RANGE_EDIT_WAITS_FOR_APPLY",settings->colorMinimum()==oldMin&&settings->colorMaximum()==oldMax);
        applyRange->click();
        check("COLOR_RANGE_APPLIES_LIMITS",settings->colorMinimum()==.01&&settings->colorMaximum()==11);
        minimum->setValue(oldMin);
        maximum->setValue(oldMax);
        applyRange->click();
    }
    check("CAMERA_ONLY_RESOLUTION_AND_FPS",cameraInfo&&cameraInfo->x()==0
        && cameraInfo->text()==QString::fromUtf8("分辨率：- | FPS：-")
        && !window.findChild<QComboBox *>("rvCameraResolution")
        && !window.findChild<QLabel *>("rtspCameraState")
        && !window.findChild<QLabel *>("rtspCameraDetail"));
    QImage syntheticVideo(576,324,QImage::Format_RGB32);
    syntheticVideo.fill(Qt::black);
    emit session->cameraFrameReady(syntheticVideo);
    check("CAMERA_RESOLUTION_USES_RECEIVED_FRAME",cameraInfo&&cameraInfo->text().contains(QStringLiteral("576×324")));
    {
        auto *page=window.findChild<RtspCameraPage*>();auto *canvas=window.findChild<QWidget*>("rtspCameraLatestFrame");
        bool geometry=page&&canvas;const int priorRotation=page?page->displayRotationDegrees():0;
        if(geometry)for(int degrees:{0,90,180,270}){
            page->setDisplayRotationDegrees(degrees);canvas->grab();
            const auto rect=canvas->property("presentationRect").toRectF();
            const double expected=(degrees==90||degrees==270)?324./576:576./324;
            geometry&=rect.height()>0&&std::abs(rect.width()/rect.height()-expected)<1e-6&&page->currentFrameImage()==syntheticVideo;
        }
        check("HOST_CAMERA_ASPECT_ROTATION_AND_RAW_PIXELS",geometry);
        if(page)page->setDisplayRotationDegrees(priorRotation);
        auto *cameraDock=window.findChild<QDockWidget*>("rtspCameraDock");
        session->selectImagingScheme(visibleHostScheme,{});
        emit session->cameraFrameReady(syntheticVideo);
        check("HOST_RESTORES_CAMERA_DOCK",cameraDock&&!cameraDock->isHidden()&&cameraDock->toggleViewAction()->isEnabled());
        check("HOST_CAMERA_RETURN_TO_ORIGINAL_ASPECT",canvas&&canvas->property("presentationSize").toSizeF()==QSizeF(576,324)&&page&&page->currentFrameImage()==syntheticVideo);
    }
    emit session->streamCleared();
    if (auto *canvas=static_cast<RvDepthView *>(window.findChild<QWidget *>("rvAssistDepthCanvas"))) {
        const auto model=session->assistHostModel();
        const auto &profile=RvCameraView::profile(model);
        const int width=profile.outputWidth,height=profile.outputHeight,pixels=width*height;
        QByteArray metres(pixels*4,'\0'), diagnostic(profile.depthBytes()+2*RvDepthDiagnostic::ReasonBytes,'\0');
        for(int i=0;i<pixels;++i){
            if(i%width>=width/2)qToLittleEndian<quint32>(0x40000000,metres.data()+i*4);
            qToLittleEndian<quint32>(0x3fc00000,diagnostic.data()+i*4);
        }
        const QString policy=QLatin1String(RvAssistFusion::FieldOfViewPolicy);
        const QJsonObject fit{{"fusion_mode","global_scale"},{"scale",1},{"local_factor_limit",1},
            {"output_policy",policy},{"fov_geometry_only",true},{"fov_pixels",pixels/2}};
        const QJsonObject metadata{{"width",width},{"height",height},{"depth_unit","camera-Z_m_float32"},
            {"assist_host_model",model},{"assist_experiment","H"},{"assist_input_orientation_checked",true},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},
            {"assist_input_rotation_degrees",0},{"dtof_fov_correction",1},{"rectify_map_sha256",profile.mapSha},
            {"assist_graph_sha256",profile.graphSha},{"depth_K",RvCameraView::intrinsics(profile)},{"assist_fusion",fit}};
        const auto priorColoredFrames=canvas->property("coloredFrames").toULongLong();
        window.depth()->acceptAssistSnapshot(metres,metadata,{},RvTofFrame{},diagnostic);
        QElapsedTimer colorWait;
        colorWait.start();
        while(canvas->property("coloredFrames").toULongLong()==priorColoredFrames && colorWait.elapsed()<2000)
            settle(20);
        settle(20);
        const auto area=canvas->property("presentationRect").toRectF();
        const QPointF point(area.left()+area.width()/4,area.center().y());
        QMouseEvent move(QEvent::MouseMove,point,canvas->mapToGlobal(point.toPoint()),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(canvas,&move);
        const auto hover=canvas->property("lastHoverText").toString();
        const auto predictionText=QCoreApplication::translate("RvDepthView","(%1, %2) prediction: %3 m camera-Z")
            .arg(0).arg(0).arg(1.5,0,'f',4).section(')',1).trimmed();
        check("OUTSIDE_FIELD_HOVER_ONE_LINE",hover.contains(predictionText)
            && !hover.contains('\n') && canvas->depthAt(width/4,height/2)==0);
        window.depth()->clearAssistDepth({});
    } else {
        check("OUTSIDE_FIELD_HOVER_ONE_LINE",false);
    }
    if(vgaSupported){
        const auto model=QString::fromLatin1(RvCameraView::UniDepthVgaNative16);
        const auto &vga=RvCameraView::profile(model);
        session->selectImagingScheme("host_"+model,{});
        check("VGA_STOPPED_SELECTION",session->userStopped()&&session->assistHostModel()==model
            &&models->currentData()=="host_"+model&&window.depth()->selectedWidth()==640&&window.depth()->selectedHeight()==480);
        RvSessionController reopened;
        check("VGA_SELECTION_PERSISTS",reopened.assistHostModel()==model&&reopened.assistExperiment()=="H");
        QByteArray metres(vga.depthBytes(),0),diagnostic(vga.depthBytes()+2*RvDepthDiagnostic::ReasonBytes,0);
        for(int i=0;i<vga.depthBytes()/4;++i){
            if(i%640>=320)qToLittleEndian<quint32>(0x40000000,metres.data()+i*4);
            qToLittleEndian<quint32>(0x3fc00000,diagnostic.data()+i*4);
        }
        const auto policy=QString::fromLatin1(RvAssistFusion::FieldOfViewPolicy);
        const QJsonObject fit{{"fusion_mode","global_scale"},{"scale",1},{"local_factor_limit",1},
            {"output_policy",policy},{"fov_geometry_only",true},{"fov_pixels",640*480/2}};
        const QJsonObject metadata{{"width",640},{"height",480},{"depth_unit","camera-Z_m_float32"},
            {"assist_host_model",model},{"assist_experiment","H"},{"assist_input_orientation_checked",true},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},
            {"assist_input_rotation_degrees",0},{"dtof_fov_correction",1},{"rectify_map_sha256",vga.mapSha},
            {"assist_graph_sha256",vga.graphSha},{"depth_K",RvCameraView::intrinsics(vga)},{"assist_fusion",fit}};
        QImage rgb(640,480,QImage::Format_RGB888);rgb.fill(Qt::black);
        window.depth()->acceptAssistSnapshot(metres,metadata,rgb,RvTofFrame{},diagnostic);
        auto *canvas=static_cast<RvDepthView *>(window.depth()->findChild<QWidget *>("rvAssistDepthCanvas"));
        const auto snapshot=window.depth()->snapshot();
        check("VGA_METRIC_SNAPSHOT_AND_SIDE_DISPLAY",canvas&&canvas->depthAt(160,240)==0&&canvas->depthAt(480,240)==2
            &&canvas->property("sidePredictionActive").toBool()&&snapshot.metresFloat32LE==metres
            &&snapshot.cameraImage.size()==QSize(640,480)&&snapshot.depthImage.size()==QSize(640,480));
        if(canvas){
            const int rotation=canvas->displayRotationDegrees();
            canvas->setDisplayRotationDegrees(0);
            const auto rendered=canvas->grab().toImage();
            const auto area=canvas->property("presentationRect").toRectF();
            check("UNIDEPTH_DISPLAY_REMAINS_UNMIRRORED",!area.isEmpty()
                &&rendered.pixelColor(QPointF(area.left()+area.width()/4,area.center().y()).toPoint())==QColor(canvas->colorAt(160,240))
                &&rendered.pixelColor(QPointF(area.left()+area.width()*3/4,area.center().y()).toPoint())==QColor(canvas->colorAt(480,240))
                &&!snapshot.metadata.contains("native_tof_display_mirror_horizontal"));
            const auto hasAspect=[&](int degrees,int width,int height){
                canvas->setDisplayRotationDegrees(degrees);
                const auto rendered=canvas->grab();
                const auto rect=canvas->property("presentationRect").toRectF();
                out<<"VGA_DRAW_RECT "<<degrees<<' '<<rect.width()<<'x'<<rect.height()<<'\n';
                return !rendered.isNull()&&!rect.isEmpty()
                    &&std::abs(rect.width()/rect.height()-double(width)/height)<.001;
            };
            check("VGA_RENDERED_ASPECT_RATIO",hasAspect(0,4,3)&&hasAspect(90,3,4));
            canvas->holdFrame();
            check("VGA_HELD_FRAME_RETAINS_ASPECT",hasAspect(0,4,3));
            canvas->setDisplayRotationDegrees(rotation);
        }
        window.depth()->clearAssistDepth({});
    }
    session->selectImagingScheme(visibleHostScheme,{});
    auto *view=window.tactile()->display()->findChild<QQuickWidget *>("tactileQuickView");
    check("TACTILE_QML_READY",view&&view->status()==QQuickWidget::Ready);
    const auto *tactile=window.tactile()->display();
    check("FIRST_RUN_136_TACTILE_OUTPUT_POINTS",tactile->displayPointCount()==136);
    check("OFFLINE_TACTILE_WAITS_FOR_DEVICE",!tactile->frame().valid&&tactile->history().samples().empty()
        &&tactile->findChild<QPushButton *>("tactileConnect")->isEnabled()&&!tactile->hasDisplayBaseline());
    check("TACTILE_CONNECTION_SETTINGS_OPEN_WITHOUT_SENSOR",tactile->findChild<QPushButton *>("tactileConnectionSettings")->isChecked());
    check("TACTILE_DEFAULT_REQUEST_GAP_10MS",tactile->findChild<QSpinBox *>("tactileInterval")->value()==10);
    window.grab().save(path+".png");
    const auto device=RvDeviceStore().active();
    auto temporary=device;temporary.temporaryFovCalibration=true;
    check("TEMPORARY_CALIBRATION_EXPLICIT_PROFILE_ROUNDTRIP",!device.temporaryFovCalibration
        &&!RvDeviceProfile::fromJson(temporary.json()).temporaryFovCalibration);
    const QString calibrationGeneration(32,'a');
    const QJsonObject calibration{{"schema",1},{"run_id","dtof_"+calibrationGeneration},
        {"sensor_fov_correction",6},{"processing_fov_correction",1},{"temporary_calibration",true}};
    check("TEMPORARY_CALIBRATION_REQUIRES_OPT_IN",!RvCaptureCalibration::valid(calibration,calibrationGeneration,1,false)
        &&RvCaptureCalibration::valid(calibration,calibrationGeneration,1,true));
    check("TEMPORARY_CALIBRATION_REJECTS_STALE_GENERATION",!RvCaptureCalibration::valid(calibration,QString(32,'b'),1,true));
    auto wrongCalibration=calibration;wrongCalibration["temporary_calibration"]=false;
    check("TEMPORARY_CALIBRATION_REJECTS_HIDDEN_MISMATCH",!RvCaptureCalibration::valid(wrongCalibration,calibrationGeneration,1,true));
    check("TEMPORARY_CALIBRATION_REJECTS_OTHER_GEOMETRY",!RvCaptureCalibration::valid(calibration,calibrationGeneration,2,true));
    QJsonObject annotated;RvCaptureCalibration::annotate(annotated,{{"tof_calibration",calibration}});
    check("TEMPORARY_CALIBRATION_RETAINS_SENSOR_IDENTITY",annotated["sensor_fov_correction"]==6
        &&annotated["processing_fov_correction"]==1&&annotated["temporary_fov_calibration"]==true);
    const int priorDirection=device.inputRotation==180?0:180;
    const QString legacyKey="depth/device/"+device.id
        +(device.cameraMountRotation==180?"/cameraMount180":"")+"/inputRotation";
    QSettings().setValue(legacyKey,priorDirection);
    session->reloadDeviceProfiles();
    check("RELOAD_PRESERVES_LEGACY_INPUT_DIRECTION",session->assistInputRotationDegrees()==priorDirection);
    {
        RvSessionController reopened;
        check("STARTUP_PRESERVES_LEGACY_INPUT_DIRECTION",reopened.assistInputRotationDegrees()==priorDirection);
    }
    {
        RvDeviceManagerDialog devices(session);
        auto *direction=devices.findChild<QComboBox *>("rvDeviceRotation");
        auto *form=qobject_cast<QFormLayout *>(direction->parentWidget()->layout());
        auto *label=form?qobject_cast<QLabel *>(form->labelForField(direction)):nullptr;
        check("CHINESE_HOST_DIRECTION_LABEL",label&&label->text()==QString::fromUtf8("上位机推理输入朝向"));
        check("DEVICE_MANAGER_SHOWS_EFFECTIVE_DIRECTION",direction->currentData().toInt()==priorDirection);
        check("DEVICE_MANAGER_NO_CAMERA_MOUNT_CONTROL",!devices.findChild<QWidget *>("rvDeviceCameraMount"));
        auto *confirm=devices.findChild<QCheckBox *>("rvDeviceConfirm");
        auto *temporaryFov=devices.findChild<QCheckBox *>("rvDeviceTemporaryFov");
        auto *save=devices.findChild<QPushButton *>("rvDeviceSave");
        auto *automaticCalibration=devices.findChild<QLabel *>("rvDeviceCalibration");
        check("CALIBRATION_AUTOMATIC_WITHOUT_FOV_SELECTOR",!temporaryFov&&automaticCalibration&&automaticCalibration->text().contains("F=6"));
        confirm->setChecked(true);save->click();
        auto *installCapture=devices.findChild<QPushButton *>("rvDeviceInstallCapture");
        auto *connectUsb=devices.findChild<QPushButton *>("rvDeviceConnectUsb");
        check("USB_CONNECT_ACTION",connectUsb&&connectUsb->isEnabled()
            &&connectUsb->text()==QString::fromUtf8("USB 一键连接"));
        if(connectUsb&&qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"){
            bool requested=false;
            const auto connection=QObject::connect(&devices,&RvDeviceManagerDialog::usbConnectionRequested,&devices,
                [&requested]{requested=true;});
            connectUsb->click();
            QObject::disconnect(connection);
            check("USB_CONNECT_OFFLINE_GUARD",!requested
                &&devices.findChild<QLabel *>("rvDeviceStatus")->text()==QString::fromUtf8("离线模式下无法通过 USB 连接设备。"));
        }
        check("CAPTURE_INSTALL_ACTION",installCapture&&installCapture->isEnabled()
            &&installCapture->text()==QString::fromUtf8("安装板端采集支持…"));
        if(installCapture&&qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"){
            installCapture->click();
            bool noImportProcess=true;
            for(auto *process:devices.findChildren<QProcess *>())noImportProcess&=process->state()==QProcess::NotRunning;
            check("CAPTURE_INSTALL_OFFLINE_GUARD",noImportProcess
                &&devices.findChild<QLabel *>("rvDeviceStatus")->text()==QString::fromUtf8("离线模式无法安装板端采集支持。"));
        }
        confirm->setChecked(true);save->click();
        check("SAVE_ADOPTS_LEGACY_DIRECTION",RvDeviceStore().active().inputRotation==priorDirection
            &&session->assistInputRotationDegrees()==priorDirection&&!QSettings().contains(legacyKey)
            &&RvDeviceStore().active().cameraMountRotation==device.cameraMountRotation);
        direction->setCurrentIndex(direction->findData(device.inputRotation));
        confirm->setChecked(true);save->click();
        check("DEVICE_MANAGER_UPDATES_EFFECTIVE_DIRECTION",RvDeviceStore().active().inputRotation==device.inputRotation
            &&session->assistInputRotationDegrees()==device.inputRotation&&session->userStopped());
    }
    language.setLanguage(LanguageManager::Language::English);settle(100);
    check("ENGLISH_WINDOW",window.windowTitle()==QStringLiteral("触想工作台（内测版）"));
    check("ENGLISH_ABOUT_MENU",about && about->text()=="About");
    check("ENGLISH_HOST_ONLY_SCHEME",models&&models->count()==3&&models->itemText(0)==QString::fromUtf8("Host-assisted inference (640×480)"));
    check("ENGLISH_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()==QString::fromUtf8("Host-assisted depth inference (640×480)."));
    {
        QSettings settings;
        const auto oldUrl=settings.value("camera/streamUrlV4");
        settings.setValue("camera/streamUrlV4","rtsp://127.0.0.1/obsolete");
        RvSessionController reopened;
        check("DEVICE_PROFILE_OVERRIDES_OLD_CUSTOM_URL",reopened.controlledSource()
            && reopened.streamUrl()==reopened.deviceProfile().streamUrl());
        if(oldUrl.isValid())settings.setValue("camera/streamUrlV4",oldUrl);
        else settings.remove("camera/streamUrlV4");
    }
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1") {
        const auto answerImport=[&](QMessageBox::StandardButton answer){
            bool prompted=false;
            QTimer responder;
            QObject::connect(&responder,&QTimer::timeout,&window,[&]{
                auto *prompt=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if(!prompt||prompt->windowTitle()!="Install required board files")return;
                prompted=true;
                prompt->button(answer)->click();
            });
            responder.start(10);
            emit session->boardFilesRequired(session->deviceProfile());
            settle(80);
            return prompted;
        };
        check("MISSING_FILES_DECLINE_STAYS_STOPPED",answerImport(QMessageBox::No)&&session->userStopped()
            &&!window.findChild<RvDeviceManagerDialog *>());
        const bool accepted=answerImport(QMessageBox::Yes);
        auto *devices=window.findChild<RvDeviceManagerDialog *>();
        const auto *status=devices?devices->findChild<QLabel *>("rvDeviceStatus"):nullptr;
        check("MISSING_FILES_CONFIRM_REUSES_IMPORT",accepted&&devices&&devices->isModal()&&status
            &&status->text()=="Board capture installation is unavailable in offline mode."&&session->userStopped());
        if(devices)devices->close();
        settle(30);
        const auto answerUsbSetup=[&](bool existing,bool network,QMessageBox::StandardButton answer,bool cancelPending=false){
            bool prompted=false,noParameters=false;
            QTimer responder;
            QObject::connect(&responder,&QTimer::timeout,&window,[&]{
                auto *prompt=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if(!prompt||prompt->objectName()!="rvUsbSetupPrompt")return;
                prompted=true;
                noParameters=prompt->findChildren<QComboBox *>().isEmpty()
                    && prompt->findChildren<QSpinBox *>().isEmpty();
                const auto expected=!existing
                    ? "A new RV1103 was detected. Go to Settings to add it automatically?"
                    : network ? "The RV1103 connection needs setup. Go to Settings to complete it automatically?"
                              : "RV1103 board files need updating. Go to Settings to update them automatically?";
                noParameters &= prompt->text()==QCoreApplication::translate("SensorMainWindow",expected);
                prompt->button(answer)->click();
                if(cancelPending)QTimer::singleShot(0,session,&RvSessionController::stop);
            });
            responder.start(10);
            window.offerUsbSetup(existing,network);
            settle(50);
            return prompted&&noParameters;
        };
        check("USB_NEW_DEVICE_DECLINE_STAYS_STOPPED",answerUsbSetup(false,false,QMessageBox::No)
            && !window.findChild<RvDeviceManagerDialog *>() && session->userStopped());
        {
            const bool prompted=answerUsbSetup(false,false,QMessageBox::Yes,true);
            auto *manager=window.findChild<RvDeviceManagerDialog *>();
            const auto *usbStatus=manager?manager->findChild<QLabel *>("rvDeviceStatus"):nullptr;
            check("USB_SETUP_STOP_BEFORE_DISPATCH_CANCELS",prompted&&manager&&usbStatus
                &&usbStatus->text()!="USB connection is unavailable in offline mode."&&session->userStopped());
            if(manager)manager->close();
            settle(30);
        }
        for(int scenario=0;scenario<3;++scenario) {
            const bool prompted=answerUsbSetup(scenario!=0,scenario==2,QMessageBox::Yes);
            auto *manager=window.findChild<RvDeviceManagerDialog *>();
            const auto *usbStatus=manager?manager->findChild<QLabel *>("rvDeviceStatus"):nullptr;
            bool noProcess=true;
            if(manager)for(auto *process:manager->findChildren<QProcess *>())noProcess&=process->state()==QProcess::NotRunning;
            check(QString("USB_SETUP_CONFIRM_AUTOMATICALLY_DISPATCHES_")+QString::number(scenario),prompted
                && manager && manager->isVisible() && usbStatus && noProcess && session->userStopped()
                && usbStatus->text()=="USB connection is unavailable in offline mode.");
            if(manager)manager->close();
            settle(30);
        }
        language.setLanguage(LanguageManager::Language::SimplifiedChinese);settle(30);
        check("USB_NEW_DEVICE_PROMPT_CHINESE",answerUsbSetup(false,false,QMessageBox::No));
        check("USB_UPDATE_PROMPT_CHINESE",answerUsbSetup(true,false,QMessageBox::No));
        language.setLanguage(LanguageManager::Language::English);settle(30);
    }
    check("DEPTH_SNAPSHOT_EMPTY_BEFORE_INPUT",window.depth()->snapshot().depthImage.isNull());
    window.resetLayout();settle(100);
    check("RESET_LAYOUT_CAPTURE_STOPPED",session->userStopped());
    check("HOST_ONLY_UI_IMAGE_SAVED",window.grab().save(path+".png"));
    window.close();settle(100);
    check("CLOSE_CAPTURE_STOPPED",session->userStopped());
    out<<"RESULT "<<(pass?"PASS":"FAIL")<<Qt::endl;return pass?0:1;
}
