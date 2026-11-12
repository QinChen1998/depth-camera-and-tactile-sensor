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
#include "../sensors/RvDepthCoverage.h"
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
#include <QProgressBar>
#include <QQuickWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <utility>

namespace {
void settle(int milliseconds){QElapsedTimer timer;timer.start();while(timer.elapsed()<milliseconds){QApplication::processEvents();QThread::msleep(5);}}
}

int runPortSmokeTests(SensorMainWindow &window,LanguageManager &language,const QString &path)
{
    QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
    QTextStream out(&file);bool pass=true;
    const auto check=[&](const QString &name,bool ok){out<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;pass&=ok;};
    {
        // Compare with the original public Qt conversion, especially around
        // every hue rounding boundary and the user range/invalid-value rules.
        bool hues=true,ranges=true;quint32 random=0x2834af17;
        for(int i=0;i<=24000;++i)for(float offset:{-.501f,-.5f,-.499f,0.f,.499f,.5f,.501f}){
            const float hue=std::clamp((float(i)+offset)/36000.f,0.f,2.f/3);
            hues&=DepthColorScale::hueColor(hue)==QColor::fromHsvF(hue,1,1).rgb();
        }
        const auto reference=[](float metres,double lo,double hi,bool valid,bool black){
            if(!valid||!std::isfinite(metres)||metres<=0||!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo)return qRgb(0,0,0);
            if(black&&(metres<float(lo)||metres>float(hi)))return qRgb(0,0,0);
            return QColor::fromHsvF((1-std::clamp((double(metres)-lo)/(hi-lo),0.,1.))*2/3,1,1).rgb();
        };
        for(const auto range:{std::pair<double,double>{.01,3},{.01,11},{.37,2.19},{1,1},{3,1}}){
            for(int i=0;i<100000;++i){
                random=random*1664525u+1013904223u;
                const float metres=(int(random>>8)-0x400000)/float(0x800000)*12;
                for(bool black:{false,true})ranges&=DepthColorScale::color(metres,range.first,range.second,true,black)==reference(metres,range.first,range.second,true,black);
            }
            for(float metres:{0.f,-1.f,float(range.first),float(range.second),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
                for(bool valid:{false,true})for(bool black:{false,true})
                    ranges&=DepthColorScale::color(metres,range.first,range.second,valid,black)==reference(metres,range.first,range.second,valid,black);
        }
        check("PALETTE_ALL_HUE_BOUNDARIES_MATCH_QT",hues);
        check("PALETTE_RANGE_AND_INVALID_PIXELS_MATCH_QT",ranges);
    }
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
                         "rvLiteSrWhiteCounts","rvLiteSrInputRotation","depthMonocularConfidence"})
        check(QString("NO_OBSOLETE_CONTROL_")+name,!window.depth()->findChild<QWidget *>(name));
    const auto *toolbar=window.findChild<QToolBar *>("sensorToolBar");
    const auto *viewMenu=window.findChild<QMenu *>("viewMenu");
    bool viewToggles=viewMenu;
    for(auto *dock:docks)viewToggles&=viewMenu&&viewMenu->actions().contains(dock->toggleViewAction());
    check("MODULES_AVAILABLE_ONLY_IN_VIEW_MENU",!toolbar&&viewToggles);
    for(const auto *name:{"tactileDock","depthMapDock","rtspCameraDock"})
        check(QString("DOCK_")+name,window.findChild<QDockWidget *>(name)!=nullptr);
    auto *models=window.findChild<QComboBox *>("rvDepthModel");
    auto *tofPointsButton=window.depth()->findChild<QPushButton *>("depthPairedTofPoints");
    auto *monocularButton=window.depth()->findChild<QPushButton *>("depthMonocularOnly");
    auto *pairedRgbButton=window.depth()->findChild<QPushButton *>("depthPairedRgb");
    const auto visibleHostModel=QString::fromLatin1(RvCameraView::UniDepthVgaNative48);
    const auto visibleHostScheme=QString::fromLatin1(RvSessionController::GlobalRgbImagingScheme);
    const auto reuseScheme=QString::fromLatin1(RvSessionController::ReuseLatestTofImagingScheme);
    const QString liteSrScheme="board_litesr_near_256";
    check("EXACT_IMAGING_SCHEMES",models&&models->count()==5&&models->itemData(0)==visibleHostScheme
        &&models->itemData(1)==reuseScheme&&models->itemData(2)=="board_native_tof48"
        &&models->itemData(3)=="board_native_tof48_high_accuracy"&&models->itemData(4)==liteSrScheme);
    check("OTHER_IMAGING_SCHEMES_ABSENT",models
        &&models->findData("host_unidepth_wide_576_fp16_tof16")==-1
        &&models->findData("board_litesr_near_full_host_contour_hd")==-1
        &&models->findData("board_litesr_near_128x128_depth_only")==-1);
    check("NO_HOST_DIRECTION_OR_CORRECTION_SELECTORS",
          !window.depth()->findChild<QComboBox *>("rvAssistInputRotation")
          &&!window.depth()->findChild<QComboBox *>("rvUniDepthExperiment"));
    auto *session=window.session();
    check("HOST_RGB_PREPARATION_KEEPS_NEW_PAIR_AFTER_CANCELLATION",session->testHostHdPreparation());
    check("DEFAULT_HOST_G_USES_NATIVE48",session->assistHostModel()==visibleHostModel&&session->assistExperiment()=="G"&&session->dtofCandidateMode()==24);
    check("FIRST_RUN_VISIBLE_HOST_SCHEME",session->currentImagingScheme()==visibleHostScheme
        &&models&&models->currentData()==session->currentImagingScheme()&&session->assistDepthEnabled());
    check("PAIRED_TOF_BUTTON_DEFAULT_OFF",tofPointsButton&&!tofPointsButton->isHidden()&&tofPointsButton->isCheckable()
        &&!tofPointsButton->isChecked());
    check("MONOCULAR_BUTTON_DEFAULT_OFF",monocularButton&&!monocularButton->isHidden()&&monocularButton->isCheckable()
        &&!monocularButton->isChecked());
    check("NO_EXPERIMENTAL_REGIONAL_CORRECTION_BUTTON",!window.depth()->findChild<QPushButton *>("rvRegionalCorrection"));
    check("PAIRED_RGB_BUTTON_DEFAULT_OFF",pairedRgbButton&&!pairedRgbButton->isHidden()
        &&pairedRgbButton->isCheckable()&&!pairedRgbButton->isChecked());
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
    for(const auto* retired:{"host_unidepth_rgb672_vga_fp16_tof16","host_unidepth_rgb672_vga_fp16_tof48",
        "host_unidepth_rgb672_vga_fp16_tof48_i","board_litesr_near_384x256_host_contour_hd","board_litesr_near_384x256_high_accuracy_host_contour_hd","board_litesr_near_full_host_contour_hd","board_litesr_near_128x128_depth_only","board_litesr_384x256_contour_relative","board_tiny_nv12_128x80","unknown"}){
        check(QString("RETIRED_SCHEME_ABSENT_")+retired,models->findData(retired)==-1);
        session->selectImagingScheme(retired,{});
        check(QString("RETIRED_SCHEME_API_REJECTED_")+retired,session->currentImagingScheme()==visibleHostScheme&&session->userStopped());
        QSettings().setValue("depth/imagingScheme",retired);
        QSettings().setValue("depth/boardLiteSrHostHoleMode",2);
        QSettings().setValue("depth/uniDepthExperiment","I");
        QSettings().setValue("depth/hostModel","unidepth_rgb672_vga_fp16_tof16");
        RvSessionController reopened;
        const QSettings migrated;
        check(QString("RETIRED_SCHEME_MIGRATES_TO_G_")+retired,reopened.currentImagingScheme()==visibleHostScheme
            &&reopened.userStopped()&&reopened.assistDepthEnabled()&&reopened.dtofCandidateMode()==24
            &&migrated.value("depth/imagingScheme")==visibleHostScheme&&!migrated.contains("depth/boardLiteSrHostHoleMode")
            &&!migrated.contains("depth/uniDepthExperiment")&&!migrated.contains("depth/hostModel"));
    }
    for(const auto *scheme:{"board_native_tof48","board_native_tof48_high_accuracy"}){
        session->selectImagingScheme(scheme,{});
        check(QString("NATIVE_MODE_SELECTS_WHILE_STOPPED_")+scheme,session->userStopped()&&!session->busy()&&!session->assistDepthEnabled()
            &&session->currentImagingScheme()==scheme&&window.depth()->selectedWidth()==48&&window.depth()->selectedHeight()==32);
        check(QString("PAIRED_TOF_BUTTON_HIDDEN_")+scheme,tofPointsButton&&tofPointsButton->isHidden());
        check(QString("MONOCULAR_BUTTON_HIDDEN_")+scheme,monocularButton&&monocularButton->isHidden());
        check(QString("PAIRED_RGB_BUTTON_HIDDEN_")+scheme,pairedRgbButton&&pairedRgbButton->isHidden());
        RvSessionController reopened;
        check(QString("NATIVE_MODE_PERSISTS_")+scheme,reopened.currentImagingScheme()==scheme&&!reopened.assistDepthEnabled()&&reopened.userStopped());
    }
    session->selectImagingScheme(liteSrScheme,{});
    check("LITESR_MODE_SELECTS_WHILE_STOPPED",session->userStopped()&&!session->busy()&&!session->assistDepthEnabled()
        &&session->currentImagingScheme()==liteSrScheme&&session->dtofCandidateMode()==13
        &&session->cameraWidth()==2304&&window.depth()->selectedWidth()==256&&window.depth()->selectedHeight()==256);
    {
        RvSessionController reopened;
        check("LITESR_MODE_PERSISTS",reopened.currentImagingScheme()==liteSrScheme&&reopened.userStopped()
            &&!reopened.assistDepthEnabled()&&reopened.dtofCandidateMode()==13);
    }
    session->selectImagingScheme(visibleHostScheme,{});
    check("G48_OPTION_SELECTS_SHARED_MODEL_AND_CAPTURE_PRESET",
        session->currentImagingScheme()==visibleHostScheme&&models->currentData()==visibleHostScheme
        &&session->assistHostModel()==QLatin1String(RvCameraView::UniDepthVgaNative48)
        &&session->assistExperiment()=="G"&&session->dtofCandidateMode()==24
        &&session->assistDepthEnabled()&&session->userStopped()&&!session->busy());
    {
        RvSessionController reopened;
        check("G48_SELECTION_PERSISTS_WITHOUT_STARTING",reopened.currentImagingScheme()==visibleHostScheme
            &&reopened.assistExperiment()=="G"&&reopened.dtofCandidateMode()==24&&reopened.userStopped());
    }
    {
        const auto captureOptions=session->m_dtofOptions;
        const int index=models->findData(reuseScheme);
        models->setCurrentIndex(index);
        if(index>=0)QMetaObject::invokeMethod(models,"activated",Qt::DirectConnection,Q_ARG(int,index));
        check("B_SELECTION_USES_SAME_MODEL_AND_CAPTURE_PRESET",
            session->currentImagingScheme()==reuseScheme&&models->currentData()==reuseScheme
            &&session->assistExperiment()=="B"&&session->assistHostModel()==visibleHostModel
            &&session->dtofCandidateMode()==24&&session->m_dtofOptions==captureOptions
            &&session->assistDepthEnabled()&&session->userStopped()&&!session->busy());
        RvSessionController reopened;
        check("B_SELECTION_PERSISTS_WITHOUT_STARTING",reopened.currentImagingScheme()==reuseScheme
            &&reopened.assistExperiment()=="B"&&reopened.dtofCandidateMode()==24&&reopened.userStopped());

        const QString boot="11111111-2222-3333-4444-555555555555",token(32,'a');
        reopened.m_dtofToken=token;
        reopened.m_dtofReceipt={{"boot_id",boot}};
        const qint64 rgbNs=1000000000;
        const auto native=[&](qint64 id,qint64 deltaNs,int warning=0){
            return RvTofFrame{QByteArray(6144,0),{
                {"protocol",RvCaptureContract::PairedProtocol},{"candidate_mode",24},{"engine",RvCaptureContract::engine(24)},
                {"model_sha256",""},{"capture_only",true},{"depth_present",false},{"photon_enabled",false},
                {"work_mode","dtof_candidate"},{"boot_id",boot},{"generation",token},{"frame_id",id},
                {"tof_sequence",id},{"tof_timestamp_ns",rgbNs+deltaNs},{"tof_reused",false},{"tof_warning_flags",warning},
                {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},{"dtof_fov_correction",fixture.fov()},
                {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
                {"dtof_source","tmf8829_ipc_first_peak"},{"dtof_coordinate","sensor_radial_native"}}};
        };
        auto foreign=native(4,160000000);
        foreign.metadata["generation"]=QString(32,'b');
        reopened.m_hdNativeFrames={native(1,-5000000),native(2,110000000),native(3,150000000,8),
                                   foreign,native(5,165000001)};
        const auto pairedSequence=[&]{
            const auto *paired=reopened.pairedHostTof(rgbNs);
            return paired?paired->metadata["tof_sequence"].toInteger():0;
        };
        check("B_SELECTS_LATEST_ELIGIBLE_TOF",pairedSequence()==2);
        reopened.m_hostScheme=visibleHostScheme;
        check("G_STILL_SELECTS_NEAREST_TOF",pairedSequence()==1);
        reopened.m_hostScheme=reuseScheme;
        reopened.m_hdNativeFrames={native(6,-165000000)};
        const bool atLimit=pairedSequence()==6;
        reopened.m_hdNativeFrames={native(7,-165000001)};
        check("B_USES_THE_SAME_TIMESTAMP_BUDGET",atLimit&&pairedSequence()==0);
        session->selectImagingScheme(visibleHostScheme,{});
        check("B_SWITCH_BACK_TO_G_WITHOUT_STARTING",session->currentImagingScheme()==visibleHostScheme
            &&session->assistExperiment()=="G"&&session->m_dtofOptions==captureOptions&&session->userStopped());
    }
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
            check("AUTOMATIC_START_BLOCKED_OFFLINE",session->userStopped()&&!session->busy()&&!session->automaticConnecting());
            check("USB_AUTOMATIC_CHECK_BLOCKED_OFFLINE",!session->automaticStartAllowed(session->deviceProfile().id));
            const auto stopRevision=session->stopRevision();session->stop();
            check("USB_PENDING_CHECK_REVOKED_BY_STOP",session->stopRevision()>stopRevision);

            using Stage = RvSessionController::AutomaticConnectionStage;
            auto *overlay = window.findChild<QWidget *>("rvImagingSwitchOverlay");
            auto *progress = window.findChild<QProgressBar *>("rvImagingSwitchProgress");
            auto *progressTitle = window.findChild<QLabel *>("rvImagingProgressTitle");
            auto *progressDetail = window.findChild<QLabel *>("rvImagingProgressDetail");
            check("AUTO_CONNECT_PROGRESS_HIDDEN_WHEN_IDLE",overlay && overlay->isHidden());
            if (overlay && progress && progressTitle && progressDetail) {
                const auto priorLanguage = language.language();
                language.setLanguage(LanguageManager::Language::English, false);
                session->setAutomaticConnectionStage(Stage::PreparingInference);
                check("AUTO_CONNECT_PROGRESS_WHILE_DETECTING_INFERENCE",overlay->isVisible()
                    && progress->minimum()==0 && progress->maximum()==0 && !progress->isTextVisible()
                    && progressTitle->text()=="Connecting to RV1103 automatically..."
                    && progressDetail->text()=="RV1103 detected. Checking available inference devices...");
                session->setAutomaticConnectionStage(Stage::CheckingDevice);
                check("AUTO_CONNECT_SHARED_CONTROLS",overlay->isVisible()
                    && !depthStart->isEnabled() && !cameraStart->isEnabled()
                    && !depthReconnect->isEnabled() && !cameraReconnect->isEnabled()
                    && depthStop->isEnabled() && cameraStop->isEnabled());
                const auto checkingRevision = session->stopRevision();
                cameraStop->click();
                check("AUTO_CONNECT_STOP_CANCELS_PENDING_CHECK",overlay->isHidden()
                    && session->stopRevision()>checkingRevision && session->userStopped()
                    && depthReconnect->isEnabled() && cameraReconnect->isEnabled());

                session->setAutomaticConnectionStage(Stage::StartingCapture);
                session->failPreviewControl("Offline RV1103 connection failure");
                check("AUTO_CONNECT_FAILURE_HIDES_PROGRESS",overlay->isHidden()
                    && session->userStopped() && depthReconnect->isEnabled());

                // Feed the normal host result callback without starting a worker or a board.
                session->m_userStopped = false;
                session->setAutomaticConnectionStage(Stage::WaitingForDepth);
                language.setLanguage(LanguageManager::Language::SimplifiedChinese, false);
                settle(40);
                check("AUTO_CONNECT_PROGRESS_CHINESE",overlay->isVisible()
                    && progressTitle->text()==QStringLiteral("RV1103 已连接")
                    && progressDetail->text()==QStringLiteral("正在等待首帧深度图…"));
                const auto epoch = session->presentationEpoch();
                QImage camera(32, 24, QImage::Format_RGB888);
                camera.fill(Qt::black);
                emit session->cameraFrameReady(camera);
                const auto &profile = RvCameraView::outputProfile(session->assistHostModel());
                QJsonObject metadata{{"generation", double(epoch)}, {"boot_id", "offline-rv1103"}, {"frame_id", 1},
                    {"assist_host_model", session->assistHostModel()}, {"assist_experiment", session->assistExperiment()},
                    {"assist_input_rotation_degrees", session->assistInputRotationDegrees()},
                    {"width", profile.outputWidth}, {"height", profile.outputHeight}, {"depth_unit", "camera-Z_m_float32"},
                    {"assist_output_grid", RvCameraView::PortraitGrid}, {"assist_model_depth_size", QJsonArray{640,480}},
                    {"assist_model_crop_xywh", QJsonArray{140,0,360,480}}};
                session->m_currentMetadata = metadata;
                QByteArray depth(profile.depthBytes(), '\0');
                qToLittleEndian<quint32>(0x3f800000, depth.data());
                emit session->m_assist->resultReady(depth, metadata, camera, epoch-1, {}, {}, {});
                check("AUTO_CONNECT_WAITS_FOR_CURRENT_DEPTH",session->automaticConnecting() && overlay->isVisible());
                overlay->grab().save(path+".auto-connect.png");
                emit session->m_assist->resultReady(depth, metadata, camera, epoch, {}, {}, {});
                auto *canvas = static_cast<RvDepthView *>(window.findChild<QWidget *>("rvAssistDepthCanvas"));
                QElapsedTimer renderWait;
                renderWait.start();
                while (!overlay->isHidden() && renderWait.elapsed()<900) settle(10);
                check("AUTO_CONNECT_FINISHES_AT_FIRST_RENDERABLE_DEPTH",!session->automaticConnecting() && overlay->isHidden()
                    && canvas && canvas->hasRenderableFrame() && depthReconnect->isEnabled() && cameraReconnect->isEnabled());
                session->stop();
                language.setLanguage(priorLanguage, false);
            }
        }
    }
    session->selectImagingScheme(visibleHostScheme,{});
    for(const auto *id:{"unidepth_rgb672_vga_fp16_tof16","unidepth","unidepth_wide","unidepth_wide_576","unidepth_wide_576_fp16","unidepth_wide_576_fp16_tof16","colorsr_rgb","colorsr_nv12","depthor_v3_fp32","depthor_v3_fp16","depthor_v3_int8"})
        check(QString("REMOVED_MODEL_REJECTED_")+id,!RvAssistDepth::supportsHostModel(id)&&RuntimeDiscovery::rvAssistModelRoot(id).isEmpty());
    const auto original=session->dtofCandidateMode();
    session->selectImagingScheme("host_depthor_v3_fp32",{});
    session->selectImagingScheme("board_litesr_near_high_resolution",{});
    check("REMOVED_SCHEME_API_REJECTED",session->dtofCandidateMode()==original&&session->assistHostModel()==visibleHostModel);
    check("REMOVED_BOARD_MODE_REJECTED",!session->setDtofCandidate(15,{}));
    check("FP16_SELECTION_STOPPED",session->dtofCandidateMode()==24&&session->userStopped()&&session->assistDepthEnabled()&&session->assistExperiment()=="G");
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
    check("CHINESE_IMAGING_SCHEMES",models&&models->count()==5
        &&models->itemText(0)==QStringLiteral("上位机辅助推理")
        &&models->itemText(1)==QStringLiteral("上位机辅助推理（B，复用 ToF）")
        &&models->itemText(2)==QStringLiteral("板端推理")
        &&models->itemText(3)==QStringLiteral("板端推理高精度")
        &&models->itemText(4)==QStringLiteral("板端 LiteSR 近距（INT8，实验）"));
    check("CHINESE_SHARED_CONTROL_LABELS",controlsPresent&&depthStart->text()==cameraStart->text()
        &&depthStop->text()==cameraStop->text()&&depthReconnect->text()==cameraReconnect->text());
    check("CHINESE_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()==QStringLiteral("选择深度图的生成方式。"));
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
                geometry&=rendered.pixelColor((point*rendered.devicePixelRatio()).toPoint())==QColor(native->colorAt(index.x(),index.y()));
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
        const auto &profile=RvCameraView::outputProfile(model);
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
            {"assist_host_model",model},{"assist_experiment","G"},{"assist_fusion_revision",RvAssistFusion::GlobalRgbRevision},
            {"assist_input_orientation_checked",true},{"assist_output_grid",RvCameraView::PortraitGrid},
            {"assist_model_depth_size",QJsonArray{640,480}},{"assist_model_crop_xywh",QJsonArray{140,0,360,480}},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},
            {"assist_input_rotation_degrees",0},{"assist_tof_fov_correction",6},{"dtof_present",false},{"rectify_map_sha256",profile.mapSha},
            {"assist_graph_sha256",profile.graphSha},{"depth_K",RvCameraView::intrinsics(profile)},{"assist_fusion",fit}};
        {
            std::vector<float> decoded;RvDepthCoverage::DepthField field;
            check("FUSED_DEPTH_SCAN_PRESERVES_GRID",RvDepthCoverage::decodeDepth(metres,metadata,decoded,field)
                &&field.sourceRect==QRect(0,0,width,height)&&decoded.size()==size_t(pixels)
                &&decoded[0]==0&&decoded[size_t(width/2)]==2);
            auto mismatch=metadata;auto wrongFit=fit;wrongFit["fov_pixels"]=pixels/2+1;mismatch["assist_fusion"]=wrongFit;
            check("FUSED_DEPTH_SCAN_REJECTS_MASK_COUNT",RvDepthCoverage::decodeDepth(metres,mismatch,decoded,field)&&!field.valid());
            auto invalid=metres;qToLittleEndian<quint32>(0x7fc00000,invalid.data());
            check("FUSED_DEPTH_SCAN_REJECTS_NONFINITE",!RvDepthCoverage::decodeDepth(invalid,metadata,decoded,field)&&decoded.empty());
            auto negative=metres;qToLittleEndian<quint32>(0xbf800000,negative.data());
            check("FUSED_DEPTH_SCAN_KEEPS_NEGATIVE_VALUE_WITHOUT_ADMITTING_FIELD",
                RvDepthCoverage::decodeDepth(negative,metadata,decoded,field)&&!field.valid()&&decoded[0]==-1.f);
            check("FUSED_DEPTH_SCAN_REJECTS_WRONG_BYTE_COUNT",
                !RvDepthCoverage::decodeDepth(metres.chopped(1),metadata,decoded,field)&&!field.valid()&&decoded.empty());
            auto wrongIdentity=metadata;wrongIdentity["assist_graph_sha256"]="wrong";
            check("FUSED_DEPTH_SCAN_REJECTS_GRAPH_FIELD",RvDepthCoverage::decodeDepth(metres,wrongIdentity,decoded,field)&&!field.valid());
        }
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
    {
        const auto &model=visibleHostModel;
        const auto &vga=RvCameraView::outputProfile(model);
        check("HOST_OUTPUT_PORTRAIT",window.depth()->selectedWidth()==480&&window.depth()->selectedHeight()==640);
        QByteArray metres(vga.depthBytes(),0),diagnostic(vga.depthBytes()+2*RvDepthDiagnostic::ReasonBytes,0);
        for(int i=0;i<vga.depthBytes()/4;++i){
            if(i%480>=240)qToLittleEndian<quint32>(0x40000000,metres.data()+i*4);
            qToLittleEndian<quint32>(0x3fc00000,diagnostic.data()+i*4);
        }
        const int adoptedId=15*48+20,otherId=17*48+28;
        const auto policy=QString::fromLatin1(RvAssistFusion::FieldOfViewPolicy);
        const QJsonObject fit{{"fusion_mode","global_scale"},{"scale",1.25},{"local_factor_limit",1},
            {"dtof_corrected",true},{"used_native_indices",QJsonArray{adoptedId}},
            {"output_policy",policy},{"fov_geometry_only",true},{"fov_pixels",640*480/2}};
        const QJsonObject metadata{{"width",480},{"height",640},{"depth_unit","camera-Z_m_float32"},
            {"assist_output_grid",RvCameraView::PortraitGrid},{"assist_model_depth_size",QJsonArray{640,480}},
            {"assist_model_crop_xywh",QJsonArray{140,0,360,480}},
            {"assist_host_model",model},{"assist_experiment","G"},{"assist_fusion_revision",RvAssistFusion::GlobalRgbRevision},
            {"assist_input_orientation_checked",true},{"assist_native_projection",RvAssistFusion::CloseNative48Projection},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},
            {"assist_input_rotation_degrees",0},{"dtof_fov_correction",6},{"rectify_map_sha256",vga.mapSha},
            {"frame_id",21},{"generation",5},{"dtof_present",true},{"work_mode","rgb_tof"},{"source_protocol","HDA1"},
            {"boot_id","11111111-2222-3333-4444-555555555555"},{"source_generation",QString(32,'a')},
            {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",6144},{"dtof_range_unit","0.25_mm"},
            {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_coordinate","sensor_radial_native"},
            {"dtof_source","tmf8829_ipc_first_peak"},{"tof_sequence",20},{"tof_timestamp_ns",1000000000.},
            {"rgb_timestamp_ns",990000000.},{"pair_delta_ms",10.},{"pair_clock_uncertainty_ms",35.},
            {"pair_clock_source","rtcp_prft_board_realtime_monotonic"},{"pairing_policy",RvTofFrame::FreshRgbPairPolicy},
            {"tof_reused",false},{"tof_warning_flags",0},
            {"assist_graph_sha256",vga.graphSha},{"depth_K",RvCameraView::intrinsics(vga)},{"assist_fusion",fit}};
        RvTofFrame tof{QByteArray(6144,0),metadata};
        for(const auto &[id,radius]:{std::pair{adoptedId,1600},std::pair{otherId,4000}}){
            qToLittleEndian<quint16>(quint16(radius),tof.samples.data()+4*id);
            tof.samples[4*id+2]=id==adoptedId?12:3;
            tof.samples[4*id+3]=1;
        }
        QImage rgb(480,640,QImage::Format_RGB888);rgb.fill(QColor(37,68,91));
        window.depth()->acceptAssistSnapshot(metres,metadata,rgb,tof,diagnostic);
        auto *canvas=static_cast<RvDepthView *>(window.depth()->findChild<QWidget *>("rvAssistDepthCanvas"));
        const auto snapshot=window.depth()->snapshot();
        check("VGA_METRIC_SNAPSHOT_AND_SIDE_DISPLAY",canvas&&canvas->depthAt(120,320)==0&&canvas->depthAt(360,320)==2
            &&canvas->property("sidePredictionActive").toBool()&&snapshot.metresFloat32LE==metres
            &&snapshot.cameraImage.size()==QSize(480,640)&&snapshot.depthImage.size()==QSize(480,640));
        if(canvas){
            // The 48×32 overlay uses a two-pixel radius. The filled pixel
            // nearest a subpixel center can round to the neighbouring pixel.
            const auto markerColor=[](const QImage &image,QPointF center,QRgb color){
                const auto pixel=(center*image.devicePixelRatio()).toPoint();
                for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
                    const auto nearby=pixel+QPoint(x,y);
                    if(image.valid(nearby)&&image.pixel(nearby)==color)return true;
                }
                return false;
            };
            const int rotation=canvas->displayRotationDegrees();
            canvas->setDisplayRotationDegrees(0);
            const auto rendered=canvas->grab().toImage();
            const auto area=canvas->property("presentationRect").toRectF();
            check("UNIDEPTH_DISPLAY_REMAINS_UNMIRRORED",!area.isEmpty()
                &&rendered.pixelColor((QPointF(area.left()+area.width()/4,area.center().y())*rendered.devicePixelRatio()).toPoint())==QColor(canvas->colorAt(120,320))
                &&rendered.pixelColor((QPointF(area.left()+area.width()*3/4,area.center().y())*rendered.devicePixelRatio()).toPoint())==QColor(canvas->colorAt(360,320))
                &&!snapshot.metadata.contains("native_tof_display_mirror_horizontal"));
            if(pairedRgbButton){
                pairedRgbButton->click();
                const auto painted=canvas->grab().toImage();
                const double dpi=painted.devicePixelRatio();
                const auto &cal=RvAssistFusion::calibrationForFov(6);
                const auto point=RvAssistFusion::projectNativePoint(.4,adoptedId,cal,vga);
                const QPointF center(area.left()+(point.pixel.x()+.5)*area.width()/480,
                                     area.top()+(point.pixel.y()+.5)*area.height()/640);
                const QPointF backdrop(area.left()+area.width()/4,area.top()+area.height()/4);
                const auto color=DepthColorScale::color(point.cameraZ,canvas->displayMinimum(),canvas->displayMaximum(),
                    true,canvas->blackOutsideRange());
                check("PAIRED_RGB_RENDERS_SAME_FRAME_AND_TOF_DEPTH_COLOR",pairedRgbButton->isChecked()&&painted!=rendered
                    &&markerColor(painted,center,color)&&painted.pixelColor((backdrop*dpi).toPoint())==QColor(37,68,91));
                const auto saved=window.depth()->snapshot();
                check("PAIRED_RGB_VIEW_PRESERVES_SNAPSHOT",saved.metresFloat32LE==snapshot.metresFloat32LE
                    &&saved.depthImage==snapshot.depthImage&&saved.cameraImage==snapshot.cameraImage);
                pairedRgbButton->click();
                check("PAIRED_RGB_RETURN_TO_DEPTH",!pairedRgbButton->isChecked()&&canvas->grab().toImage()==rendered);
            }
            if(monocularButton){
                const auto sampleColor=[&](float depth){return DepthColorScale::color(depth,canvas->displayMinimum(),
                    canvas->displayMaximum(),true,canvas->blackOutsideRange());};
                const QPointF left(area.left()+area.width()/4,area.center().y());
                const QPointF right(area.left()+area.width()*3/4,area.center().y());
                monocularButton->click();
                const auto monoPainted=canvas->grab().toImage();
                const auto dpi=monoPainted.devicePixelRatio();
                check("MONOCULAR_TOGGLE_DISPLAYS_RAW_DEPTH_WITHOUT_MASK_OR_SCALE",monoPainted!=rendered
                    &&monoPainted.pixel((left*dpi).toPoint())==sampleColor(1.5f)
                    &&monoPainted.pixel((right*dpi).toPoint())==sampleColor(1.5f));
                const auto saved=window.depth()->snapshot();
                check("MONOCULAR_PREVIEW_PRESERVES_CORRECTED_SNAPSHOT",canvas->depthAt(120,320)==0&&canvas->depthAt(360,320)==2
                    &&saved.metresFloat32LE==snapshot.metresFloat32LE&&saved.depthImage==snapshot.depthImage);
                QMouseEvent move(QEvent::MouseMove,right,canvas->mapToGlobal(right.toPoint()),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(canvas,&move);
                const auto hover=canvas->property("lastHoverText").toString();
                const auto translated=QCoreApplication::translate("RvDepthView","(%1, %2) monocular: %3 m camera-Z");
                check("MONOCULAR_HOVER_REPORTS_RAW_DEPTH",hover.endsWith(translated.section("%3",1).prepend("1.5000")));
                window.depth()->grab().save(path+".monocular.png");

                auto unpaired=metadata;
                unpaired["frame_id"]=22;unpaired["dtof_present"]=false;
                unpaired["assist_fusion"]=QJsonObject{{"fusion_mode","monocular"},{"scale",1}};
                QByteArray nextDiagnostic=diagnostic;
                for(int i=0;i<vga.depthBytes();i+=4)qToLittleEndian<quint32>(0x3f400000,nextDiagnostic.data()+i);
                window.depth()->acceptAssistSnapshot(QByteArray(vga.depthBytes(),0),unpaired,rgb,{},nextDiagnostic);
                QElapsedTimer frameWait;frameWait.start();
                while(canvas->property("coloredFrameId").toInt()!=22&&frameWait.elapsed()<2000)settle(15);
                const auto nextPainted=canvas->grab().toImage();
                check("MONOCULAR_VIEW_UPDATES_WITHOUT_TOF_OR_FUSED_DEPTH",canvas->property("coloredFrameId").toInt()==22
                    &&nextPainted.pixel((right*nextPainted.devicePixelRatio()).toPoint())==sampleColor(.75f));

                window.depth()->acceptAssistSnapshot(metres,metadata,rgb,tof,{});
                check("MONOCULAR_VIEW_WAITS_FOR_VALID_DIAGNOSTIC",canvas->presentationImage().isNull()
                    &&window.depth()->snapshot().depthImage==snapshot.depthImage);
                window.depth()->acceptAssistSnapshot(metres,metadata,rgb,tof,diagnostic);
                monocularButton->click();
                check("MONOCULAR_TOGGLE_RESTORES_CORRECTED_VIEW",!monocularButton->isChecked()
                    &&canvas->grab().toImage()==rendered&&canvas->property("sidePredictionActive").toBool());
            }
            if(tofPointsButton){
                tofPointsButton->click();
                const auto &cal=RvAssistFusion::calibrationForFov(6);
                const auto projected=[&](int id,int degrees,const QRectF &viewport){
                    const auto &ray=cal.cameraRays[id];
                    const double radius=tof.distanceMm(id)/1000.;
                    const double z=radius*ray[2]+cal.translation[2];
                    const double u=(vga.focal*(radius*ray[0]+cal.translation[0])/z+vga.cx()+.5)/480.;
                    const double v=(vga.focal*(radius*ray[1]+cal.translation[1])/z+vga.cy()+.5)/640.;
                    const QPointF f=degrees==90?QPointF(1-v,u):degrees==180?QPointF(1-u,1-v):degrees==270?QPointF(v,1-u):QPointF(u,v);
                    const QPointF center(viewport.left()+f.x()*viewport.width(),viewport.top()+f.y()*viewport.height());
                    const auto color=DepthColorScale::color(float(z),canvas->displayMinimum(),canvas->displayMaximum(),true,canvas->blackOutsideRange());
                    return std::make_pair(center,color);
                };
                bool rotatedColors=true;
                for(int degrees:{0,90,180,270}){
                    canvas->setDisplayRotationDegrees(degrees);
                    const auto overlay=canvas->grab().toImage();
                    const auto target=canvas->property("presentationRect").toRectF();
                    const auto [solid,solidColor]=projected(adoptedId,degrees,target);
                    const auto [other,otherColor]=projected(otherId,degrees,target);
                    rotatedColors&=markerColor(overlay,solid,solidColor)&&markerColor(overlay,other,otherColor)&&solidColor!=otherColor;
                }
                check("PAIRED_TOF_DEPTH_COLORS_AND_ROTATION",rotatedColors);
                canvas->setDisplayRotationDegrees(0);
                const double minimum=canvas->displayMinimum(),maximum=canvas->displayMaximum();
                const bool blackOutside=canvas->blackOutsideRange();
                canvas->setDisplayRange(.6,3.);
                canvas->setBlackOutsideRange(true);
                canvas->presentationImage();
                const auto limited=canvas->grab().toImage();
                const auto limitedTarget=canvas->property("presentationRect").toRectF();
                const auto [outside,outsideColor]=projected(adoptedId,0,limitedTarget);
                const auto [inside,insideColor]=projected(otherId,0,limitedTarget);
                check("PAIRED_TOF_USES_DISPLAY_RANGE_AND_BLACK_OUTSIDE",outsideColor==qRgb(0,0,0)&&insideColor!=outsideColor
                    &&markerColor(limited,outside,outsideColor)&&markerColor(limited,inside,insideColor));
                canvas->setDisplayRange(minimum,maximum);
                canvas->setBlackOutsideRange(blackOutside);
                canvas->presentationImage();
                const auto withTof=canvas->grab().toImage();
                const auto saved=window.depth()->snapshot();
                check("PAIRED_TOF_OVERLAY_EXCLUDED_FROM_SNAPSHOT",withTof!=rendered&&saved.metresFloat32LE==snapshot.metresFloat32LE
                    &&saved.depthImage==snapshot.depthImage);
                window.depth()->grab().save(path+".paired-tof.png");
                tofPointsButton->click();
                check("PAIRED_TOF_TOGGLE_RESTORES_VIEW",canvas->grab().toImage()==rendered);
                tofPointsButton->click();
                auto unmatched=metadata;
                unmatched["frame_id"]=22;
                window.depth()->acceptAssistSnapshot(metres,unmatched,rgb,tof,diagnostic);
                canvas->presentationImage();
                check("PAIRED_TOF_CANNOT_FOLLOW_WRONG_FRAME",canvas->grab().toImage()==rendered);
                window.depth()->acceptAssistSnapshot(metres,metadata,rgb,tof,diagnostic);
                canvas->presentationImage();
                canvas->holdFrame();
                check("PAIRED_TOF_CLEARED_WITH_HELD_METRIC_FRAME",canvas->grab().toImage()==rendered);
                tofPointsButton->click();
            }
            const auto hasAspect=[&](int degrees,int width,int height){
                canvas->setDisplayRotationDegrees(degrees);
                const auto rendered=canvas->grab();
                const auto rect=canvas->property("presentationRect").toRectF();
                out<<"VGA_DRAW_RECT "<<degrees<<' '<<rect.width()<<'x'<<rect.height()<<'\n';
                return !rendered.isNull()&&!rect.isEmpty()
                    &&std::abs(rect.width()/rect.height()-double(width)/height)<.001;
            };
            check("VGA_RENDERED_ASPECT_RATIO",hasAspect(0,3,4)&&hasAspect(90,4,3));
            canvas->holdFrame();
            check("VGA_HELD_FRAME_RETAINS_ASPECT",hasAspect(0,3,4));
            if(pairedRgbButton){
                using namespace RvAssistFusion;
                auto projected=metadata;
                projected["frame_id"]=23;
                const int id=15*48+23;
                auto currentFit=fit;
                currentFit["used_native_indices"]=QJsonArray{id};
                projected["assist_fusion"]=currentFit;
                RvTofFrame native{QByteArray(6144,0),projected};
                constexpr quint16 radialUq2=620;
                qToLittleEndian(radialUq2,native.samples.data()+4*id);
                native.samples[4*id+2]=20;native.samples[4*id+3]=1;
                projected.insert(NativeProjectionField,defaultNativeProjection(native));
                native.metadata=projected;
                window.depth()->acceptAssistSnapshot(metres,projected,rgb,native,diagnostic);
                canvas->setDisplayRotationDegrees(0);
                canvas->presentationImage();
                pairedRgbButton->click();
                const auto picture=canvas->grab().toImage();
                const auto rect=canvas->property("presentationRect").toRectF();
                const auto point=projectNativePoint(radialUq2*.00025,id,calibrationForNativeGrid(6,48,32),vga);
                const auto position=[&](QPointF p){
                    return (QPointF(rect.left()+(p.x()+.5)*rect.width()/480,rect.top()+(p.y()+.5)*rect.height()/640)
                            *picture.devicePixelRatio()).toPoint();
                };
                const auto center=position(point.pixel+QPointF(.0065,.00725)*(vga.focal/point.cameraZ));
                const auto oldCenter=position(point.pixel);
                const auto color=DepthColorScale::color(point.cameraZ,canvas->displayMinimum(),canvas->displayMaximum(),
                                                        true,canvas->blackOutsideRange());
                // A subpixel ellipse centre can cover the neighbouring raster pixel.
                bool centerColor=false;
                for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
                    const auto pixel=center+QPoint(x,y);
                    centerColor|=picture.valid(pixel)&&picture.pixel(pixel)==color;
                }
                picture.save(path+".host48-projection.png");
                check("HOST48_RGB_OVERLAY_USES_DEFAULT_CLOSE_PROJECTION",picture.valid(center)&&picture.valid(oldCenter)
                    &&centerColor&&picture.pixelColor(oldCenter)==QColor(37,68,91));
                pairedRgbButton->click();

            }
            canvas->setDisplayRotationDegrees(rotation);
        }
        window.depth()->clearAssistDepth({});
    }
    {
        session->selectImagingScheme("board_native_tof48",{});
        check("NATIVE48_BOARD_MODE_REMAINS_DISTINCT",!session->hostHdAssist()&&!session->assistDepthEnabled()
            &&session->dtofCandidateMode()==24&&window.depth()->selectedWidth()==48&&window.depth()->selectedHeight()==32);
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
        devices.findChild<QPushButton *>("rvDeviceAdd")->click();
        auto *fingerprint=devices.findChild<QLabel *>("rvDeviceFingerprint");
        auto *key=devices.findChild<QWidget *>("rvDeviceKey");
        check("USB_BLANK_PROFILE_HAS_AUTOMATIC_KEY_HINT",fingerprint
            &&fingerprint->text()==QString::fromUtf8("点击“USB 一键连接”后会自动读取公钥。"));
        key->setProperty("text","incomplete");
        check("USB_INVALID_TYPED_KEY_STILL_REJECTED",fingerprint->text()==QString::fromUtf8("请输入完整的 Ed25519 公钥"));
        key->setProperty("text",QString());
        check("USB_CLEAR_KEY_RESTORES_AUTOMATIC_HINT",fingerprint->text()==QString::fromUtf8("点击“USB 一键连接”后会自动读取公钥。"));
    }
    language.setLanguage(LanguageManager::Language::English);settle(100);
    check("ENGLISH_WINDOW",window.windowTitle()==QStringLiteral("触想工作台（内测版）"));
    check("ENGLISH_ABOUT_MENU",about && about->text()=="About");
    check("ENGLISH_IMAGING_SCHEMES",models&&models->count()==5
        &&models->itemText(0)==QStringLiteral("Host-assisted inference")
        &&models->itemText(1)==QStringLiteral("Host-assisted inference (B, reuse ToF)")
        &&models->itemText(2)==QStringLiteral("Board inference")
        &&models->itemText(3)==QStringLiteral("Board inference high accuracy")
        &&models->itemText(4)==QStringLiteral("Board LiteSR near depth (INT8, experimental)"));
    check("ENGLISH_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()==QStringLiteral("Select how the depth map is generated."));
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
