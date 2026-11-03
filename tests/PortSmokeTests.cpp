#include "../SensorMainWindow.h"
#include "../LanguageManager.h"
#include "../RuntimeDiscovery.h"
#include "../TactileDockWidget.h"
#include "../RenderingSettings.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/RvAssistDepth.h"
#include "../sensors/RvAssistFusion.h"
#include "../sensors/RvCameraView.h"
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
    settle(900);
    const auto docks=window.findChildren<QDockWidget *>(QString(),Qt::FindDirectChildrenOnly);
    check("EXACTLY_THREE_MODULES",docks.size()==3);
    check("NO_TENSORRT_PREVIEW_MENU",!window.findChild<QAction *>("tensorRtPreviewAction"));
    check("NO_DTOF_TUNER_UI",!window.findChild<RvTunerWidget *>());
    const auto *toolbar=window.findChild<QToolBar *>("sensorToolBar");
    const auto *viewMenu=window.findChild<QMenu *>("viewMenu");
    bool viewToggles=viewMenu;
    for(auto *dock:docks)viewToggles&=viewMenu&&viewMenu->actions().contains(dock->toggleViewAction());
    check("MODULES_AVAILABLE_ONLY_IN_VIEW_MENU",!toolbar&&viewToggles);
    for(const auto *name:{"tactileDock","depthMapDock","rtspCameraDock"})
        check(QString("DOCK_")+name,window.findChild<QDockWidget *>(name)!=nullptr);
    auto *models=window.findChild<QComboBox *>("rvDepthModel");
    check("EXACT_TWO_IMAGING_SCHEMES",models&&models->count()==2&&models->itemData(0)=="host_unidepth_wide_576_fp16_tof16"&&models->itemData(1)=="board_litesr_near_full_host_contour_hd");
    check("NO_HOST_DIRECTION_OR_CORRECTION_SELECTORS",
          !window.depth()->findChild<QComboBox *>("rvAssistInputRotation")
          &&!window.depth()->findChild<QComboBox *>("rvUniDepthExperiment"));
    auto *session=window.session();
    check("FP16_NATIVE16_DEFAULT_H",session->assistHostModel()=="unidepth_wide_576_fp16_tof16"&&session->assistExperiment()=="H");
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
            session->selectImagingScheme("board_litesr_near_full_host_contour_hd",{});
            checkSharedStop("BOARD_DEPTH_START_CAMERA_STOP",depthStart,cameraStop);
            checkSharedStop("BOARD_CAMERA_RECONNECT_DEPTH_STOP",cameraReconnect,depthStop);
            session->selectImagingScheme("host_unidepth_wide_576_fp16_tof16",{});
        }
        if(qEnvironmentVariable("RV_TEST_OFFLINE_ONLY")=="1"){
            window.monitorCameraConnection();
            session->startAutomatically();
            settle(50);
            check("AUTOMATIC_START_BLOCKED_OFFLINE",session->userStopped()&&!session->busy());
        }
    }
    session->setAssistExperiment("A");session->setAssistExperiment("B");session->setAssistExperiment("G");
    check("OTHER_CORRECTIONS_REJECTED",session->assistExperiment()=="H");
    for(const auto *id:{"unidepth","unidepth_wide","unidepth_wide_576","unidepth_wide_576_fp16","colorsr_rgb","colorsr_nv12","depthor_v3_fp32","depthor_v3_fp16","depthor_v3_int8"})
        check(QString("REMOVED_MODEL_REJECTED_")+id,!RvAssistDepth::supportsHostModel(id)&&RuntimeDiscovery::rvAssistModelRoot(id).isEmpty());
    const auto original=session->dtofCandidateMode();
    session->selectImagingScheme("host_depthor_v3_fp32",{});
    session->selectImagingScheme("board_litesr_near_high_resolution",{});
    check("REMOVED_SCHEME_API_REJECTED",session->dtofCandidateMode()==original&&session->assistHostModel()=="unidepth_wide_576_fp16_tof16");
    check("REMOVED_BOARD_MODE_REJECTED",!session->setDtofCandidate(15,{}));
    session->selectImagingScheme("board_litesr_near_full_host_contour_hd",{});
    check("BOARD_SELECTION_STOPPED",session->dtofCandidateMode()==13&&session->userStopped()&&!session->assistDepthEnabled());
    {
        RvSessionController reopened;
        check("USER_IMAGING_SCHEME_RESTORED_WITHOUT_CAPTURE",
            reopened.dtofCandidateMode()==13&&reopened.userStopped()&&!reopened.assistDepthEnabled());
    }
    session->selectImagingScheme("host_unidepth_wide_576_fp16_tof16",{});
    const bool fp16Available=RvAssistDepth::hostModelAvailable("unidepth_wide_576_fp16_tof16");
    if(fp16Available)
        check("FP16_SELECTION_STOPPED",session->dtofCandidateMode()==14&&session->userStopped()&&session->assistDepthEnabled()&&session->assistExperiment()=="H");
    else
        check("FP16_ABSENT_KEEPS_BOARD_SELECTED",session->dtofCandidateMode()==13&&session->userStopped()&&!session->assistDepthEnabled());
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
    check("CHINESE_WINDOW",window.windowTitle()==QStringLiteral("触想工作台"));
    auto *about = window.menuBar()->findChild<QAction *>("aboutAction");
    check("CHINESE_ABOUT_MENU",about && window.menuBar()->actions().contains(about)
        && about->text()==QStringLiteral("关于"));
    if(about)about->trigger();
    auto *aboutDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    bool aboutName=false, aboutVersion=false, aboutLogo=false;
    if(aboutDialog){
        for(auto *label:aboutDialog->findChildren<QLabel *>()){
            aboutName |= label->text()==QStringLiteral("触想工作台");
            aboutVersion |= label->text()==QStringLiteral("v1.0");
            aboutLogo |= !label->pixmap().isNull();
        }
    }
    check("ABOUT_BRANDING",aboutDialog && aboutDialog->windowTitle()==QStringLiteral("关于")
        && aboutName && aboutVersion && aboutLogo && !window.windowIcon().isNull());
    if(aboutDialog)aboutDialog->reject();
    check("CHINESE_TWO_SCHEMES",models&&models->itemText(0)==QString::fromUtf8("上位机辅助推理")&&models->itemText(1)==QString::fromUtf8("板端推理"));
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
    emit session->streamCleared();
    auto *canvas=static_cast<RvDepthView *>(window.findChild<QWidget *>("rvAssistDepthCanvas"));
    if (fp16Available && canvas) {
        constexpr int width=576,height=324,pixels=width*height;
        QByteArray metres(pixels*4,'\0'), diagnostic(RvDepthDiagnostic::PayloadBytes,'\0');
        for(int i=0;i<pixels;++i){
            if(i%width>=width/2)qToLittleEndian<quint32>(0x40000000,metres.data()+i*4);
            qToLittleEndian<quint32>(0x3fc00000,diagnostic.data()+i*4);
        }
        const auto model=session->assistHostModel();
        const auto &profile=RvCameraView::profile(model);
        const QString policy=QLatin1String(RvAssistFusion::FieldOfViewPolicy);
        const QJsonObject fit{{"fusion_mode","global_scale"},{"scale",1},{"local_factor_limit",1},
            {"output_policy",policy},{"fov_geometry_only",true},{"fov_pixels",pixels/2}};
        const QJsonObject metadata{{"width",width},{"height",height},{"depth_unit","camera-Z_m_float32"},
            {"assist_host_model",model},{"assist_experiment","H"},{"assist_input_orientation_checked",true},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},
            {"assist_input_rotation_degrees",0},{"dtof_fov_correction",1},{"rectify_map_sha256",profile.mapSha},
            {"assist_graph_sha256",profile.graphSha},{"depth_K",RvCameraView::intrinsics(true)},{"assist_fusion",fit}};
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
    } else if (fp16Available) {
        check("OUTSIDE_FIELD_HOVER_ONE_LINE",false);
    }
    session->selectImagingScheme("board_litesr_near_full_host_contour_hd",{});
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
    session->selectImagingScheme("host_unidepth_wide_576_fp16_tof16",{});
    auto *view=window.tactile()->display()->findChild<QQuickWidget *>("tactileQuickView");
    check("TACTILE_QML_READY",view&&view->status()==QQuickWidget::Ready);
    const auto *tactile=window.tactile()->display();
    check("FIRST_RUN_136_TACTILE_OUTPUT_POINTS",tactile->displayPointCount()==136);
    check("OFFLINE_TACTILE_STAYS_SIMULATED",tactile->frame().valid&&tactile->frame().sourceMode=="Mock"
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
    check("ENGLISH_WINDOW",window.windowTitle()==QStringLiteral("触想工作台"));
    check("ENGLISH_ABOUT_MENU",about && about->text()=="About");
    check("ENGLISH_TWO_SCHEMES",models&&models->itemText(0)=="Host-assisted inference"&&models->itemText(1)=="On-board inference");
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
