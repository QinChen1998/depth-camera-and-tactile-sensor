#include "../SensorMainWindow.h"
#include "../LanguageManager.h"
#include "../RuntimeDiscovery.h"
#include "../TactileDockWidget.h"
#include "../RenderingSettings.h"
#include "RvDeviceFixture.h"
#include "TinyNv12Tests.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvAssistDepth.h"
#include "../sensors/RvAssistFusion.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RtspCameraPage.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvDeviceManagerDialog.h"
#include "../sensors/RvTunerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
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
    checkTinyNv12(check);
    settle(900);
    const auto docks=window.findChildren<QDockWidget *>(QString(),Qt::FindDirectChildrenOnly);
    check("EXACTLY_THREE_MODULES",docks.size()==3);
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
    check("EXACT_IMAGING_SCHEMES",models&&models->count()==(vgaSupported?3:2)
        &&models->itemData(0)=="board_litesr_near_384x256_host_contour_hd"
        &&(!vgaSupported||models->itemData(1)=="host_unidepth_rgb672_vga_fp16_tof16")
        &&models->itemData(models->count()-1)==TinyNv12Contract::Scheme);
    check("OTHER_IMAGING_SCHEMES_ABSENT",models
        &&models->findData("host_unidepth_wide_576_fp16_tof16")==-1
        &&models->findData("board_litesr_near_full_host_contour_hd")==-1
        &&models->findData("board_litesr_near_128x128_depth_only")==-1);
    check("NO_HOST_DIRECTION_OR_CORRECTION_SELECTORS",
          !window.depth()->findChild<QComboBox *>("rvAssistInputRotation")
          &&!window.depth()->findChild<QComboBox *>("rvUniDepthExperiment"));
    auto *session=window.session();
    check("FP16_NATIVE16_DEFAULT_H",session->assistHostModel()=="unidepth_rgb672_vga_fp16_tof16"&&session->assistExperiment()=="H");
    check("FIRST_RUN_VISIBLE_BOARD_SCHEME",session->currentImagingScheme()=="board_litesr_near_384x256_host_contour_hd"
        &&models&&models->currentData()==session->currentImagingScheme()&&!session->assistDepthEnabled());
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
            check("HOST_GPU_WARNING_WITH_BOARD_ADVICE",warning&&warning->isVisible()
                &&warning->text()==preflight
                &&warning->informativeText()==QCoreApplication::translate("SensorMainWindow",
                    "For now, choose On-board inference under Imaging scheme, then click Start if needed.")
                &&session->userStopped()&&!session->busy());
            if(warning){warning->accept();settle(20);}
            session->selectImagingScheme("board_litesr_near_384x256_host_contour_hd",{});
            checkSharedStop("BOARD_DEPTH_START_CAMERA_STOP",depthStart,cameraStop);
            checkSharedStop("BOARD_CAMERA_RECONNECT_DEPTH_STOP",cameraReconnect,depthStop);
            session->selectImagingScheme(visibleHostScheme,{});
        }
        if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"){
            window.monitorCameraConnection();
            session->startAutomatically();
            settle(50);
            check("AUTOMATIC_START_BLOCKED_OFFLINE",session->userStopped()&&!session->busy());
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
    session->selectImagingScheme("board_litesr_near_128x128_depth_only",{});
    check("RETIRED_NATIVE16_SELECTION_REJECTED",session->dtofCandidateMode()==original);
    QSettings().setValue("depth/imagingScheme","board_litesr_near_128x128_depth_only");
    {RvSessionController reopened;check("HIDDEN_NATIVE16_RESTORES_VISIBLE_BOARD",reopened.dtofCandidateMode()==16&&reopened.userStopped()&&!reopened.assistDepthEnabled());}

    session->selectImagingScheme("board_litesr_near_full_host_contour_hd",{});
    check("RETIRED_BOARD256_SELECTION_REJECTED",session->dtofCandidateMode()==original);
    QSettings().setValue("depth/imagingScheme","board_litesr_near_full_host_contour_hd");
    {
        RvSessionController reopened;
        check("HIDDEN_BOARD256_RESTORES_VISIBLE_BOARD",
            reopened.dtofCandidateMode()==16&&reopened.userStopped()&&!reopened.assistDepthEnabled());
    }
    session->selectImagingScheme(visibleHostScheme,{});
    {QSettings().setValue("depth/imagingScheme","host_unidepth_wide_576_fp16_tof16");
        RvSessionController reopened;check("HIDDEN_HOST576_RESTORES_VISIBLE_SCHEME",reopened.userStopped()
        &&reopened.currentImagingScheme()==(vgaSupported?"host_unidepth_rgb672_vga_fp16_tof16":"board_litesr_near_384x256_host_contour_hd"));}
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
    check("CHINESE_TWO_SCHEMES",models&&models->itemText(0)==QString::fromUtf8("板端推理（384×256）")
        &&(!vgaSupported||models->itemText(1)==QString::fromUtf8("上位机辅助推理（640×480）")));
    check("CHINESE_SHARED_CONTROL_LABELS",controlsPresent&&depthStart->text()==cameraStart->text()
        &&depthStop->text()==cameraStop->text()&&depthReconnect->text()==cameraReconnect->text());
    check("CHINESE_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()==QString::fromUtf8("选择上位机辅助推理或板端推理。"));
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
        auto tinyFixture=fixture;tinyFixture.id=tinyFixture.serial="1234567890abcdea";
        tinyFixture.inputRotation=0;tinyFixture.cameraMountRotation=180;
        initialDevices.save({tinyFixture},tinyFixture.id);session->reloadDeviceProfiles();
        session->selectImagingScheme(TinyNv12Contract::Scheme,{});
        check("TINY_NEW_SERIAL_COMPATIBLE_PROFILE_STAYS_OFFLINE",session->dtofCandidateMode()==18&&session->userStopped()&&!session->busy());
        QImage tinyVideo(80,128,QImage::Format_RGB32);tinyVideo.fill(qRgb(12,34,56));emit session->cameraFrameReady(tinyVideo);
        auto *page=window.findChild<RtspCameraPage*>();auto *canvas=window.findChild<QWidget*>("rtspCameraLatestFrame");
        bool geometry=page&&canvas;const int priorRotation=page?page->displayRotationDegrees():0;
        if(geometry)for(int degrees:{0,90,180,270}){
            page->setDisplayRotationDegrees(degrees);canvas->grab();
            const auto rect=canvas->property("presentationRect").toRectF();
            const double expected=(degrees==90||degrees==270)?1296./1248:1248./1296;
            geometry&=rect.height()>0&&std::abs(rect.width()/rect.height()-expected)<1e-6&&page->currentFrameImage()==tinyVideo;
        }
        check("TINY_CAMERA_CROP_ASPECT_ROTATION_AND_RAW_PIXELS",geometry);
        if(page)page->setDisplayRotationDegrees(priorRotation);
        session->selectImagingScheme("board_litesr_near_384x256_host_contour_hd",{});
        emit session->cameraFrameReady(tinyVideo);
        auto *cameraDock=window.findChild<QDockWidget*>("rtspCameraDock");
        check("LITESR384_CLEARS_VIDEO_AND_HIDES_CAMERA",page&&page->currentFrameImage().isNull()&&cameraDock&&cameraDock->isHidden()&&!cameraDock->toggleViewAction()->isEnabled());
        session->selectImagingScheme(TinyNv12Contract::Scheme,{});
        check("TINY_RESTORES_CAMERA_DOCK",cameraDock&&!cameraDock->isHidden()&&cameraDock->toggleViewAction()->isEnabled());
        initialDevices.save({fixture},fixture.id);session->reloadDeviceProfiles();session->selectImagingScheme(visibleHostScheme,{});
        emit session->cameraFrameReady(syntheticVideo);
        check("TINY_CAMERA_RETURN_TO_ORIGINAL_ASPECT",canvas&&canvas->property("presentationSize").toSizeF()==QSizeF(576,324)&&page&&page->currentFrameImage()==syntheticVideo);
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
        check("OUTSIDE_FIELD_HOVER_ONE_LINE",hover.contains(QString::fromUtf8("预测：1.5000 m camera-Z"))
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
    session->selectImagingScheme("board_litesr_near_384x256_host_contour_hd",{});
    if (auto *contour=window.findChild<QPushButton *>("rvLiteSrRepairSettings"); contour&&contour->isVisible()&&contour->isEnabled()) {
        QTimer::singleShot(0, &window, [&]{
            auto *dialog=window.findChild<QDialog *>("rvLiteSrRepairDialog");
            check("CHINESE_CONTOUR_DIALOG",dialog&&dialog->windowTitle()==QString::fromUtf8("上位机轮廓优化设置"));
            if (dialog) dialog->reject();
        });
        contour->click();
    } else {
        check("CHINESE_CONTOUR_DIALOG",false);
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
        auto *confirm=devices.findChild<QCheckBox *>("rvDeviceConfirm");
        auto *save=devices.findChild<QPushButton *>("rvDeviceSave");
        auto *importLiteSr=devices.findChild<QPushButton *>("rvDeviceImportLiteSr");
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
        check("LITESR_IMPORT_ACTION",importLiteSr&&importLiteSr->isEnabled()
            &&importLiteSr->text()==QString::fromUtf8("安装板端推理与自动切换…"));
        if(importLiteSr&&qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"){
            importLiteSr->click();
            bool noImportProcess=true;
            for(auto *process:devices.findChildren<QProcess *>())noImportProcess&=process->state()==QProcess::NotRunning;
            check("LITESR_IMPORT_OFFLINE_GUARD",noImportProcess
                &&devices.findChild<QLabel *>("rvDeviceStatus")->text()==QString::fromUtf8("离线模式下无法导入板端推理模型。"));
        }
        confirm->setChecked(true);save->click();
        check("SAVE_ADOPTS_LEGACY_DIRECTION",RvDeviceStore().active().inputRotation==priorDirection
            &&session->assistInputRotationDegrees()==priorDirection&&!QSettings().contains(legacyKey));
        direction->setCurrentIndex(direction->findData(device.inputRotation));
        confirm->setChecked(true);save->click();
        check("DEVICE_MANAGER_UPDATES_EFFECTIVE_DIRECTION",RvDeviceStore().active().inputRotation==device.inputRotation
            &&session->assistInputRotationDegrees()==device.inputRotation&&session->userStopped());
    }
    language.setLanguage(LanguageManager::Language::English);settle(100);
    check("ENGLISH_WINDOW",window.windowTitle()==QStringLiteral("触想工作台（内测版）"));
    check("ENGLISH_ABOUT_MENU",about && about->text()=="About");
    check("ENGLISH_TWO_SCHEMES",models&&models->itemText(0)=="On-board inference (384x256)"
        &&(!vgaSupported||models->itemText(1)==QString::fromUtf8("Host-assisted inference (640×480)")));
    check("ENGLISH_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()=="Select host-assisted or on-board depth inference.");
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
            &&status->text()=="On-board model import is unavailable in offline mode."&&session->userStopped());
        if(devices)devices->close();
    }
    check("DEPTH_SNAPSHOT_EMPTY_BEFORE_INPUT",window.depth()->snapshot().depthImage.isNull());
    window.resetLayout();settle(100);
    check("RESET_LAYOUT_CAPTURE_STOPPED",session->userStopped());
    window.close();settle(100);
    check("CLOSE_CAPTURE_STOPPED",session->userStopped());
    out<<"RESULT "<<(pass?"PASS":"FAIL")<<Qt::endl;return pass?0:1;
}
