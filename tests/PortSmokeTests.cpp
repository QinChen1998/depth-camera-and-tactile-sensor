#include "../SensorMainWindow.h"
#include "../LanguageManager.h"
#include "../RuntimeDiscovery.h"
#include "../TactileDockWidget.h"
#include "../RenderingSettings.h"
#include "RvDeviceFixture.h"
#include "../sensors/DepthColorScale.h"
#include "../sensors/DepthMapWidget.h"
#include "../sensors/MeasurementCaptureDialog.h"
#include "../sensors/PointCloudColorScale.h"
#include "../sensors/RvAssistDepth.h"
#include "../sensors/RvAssistFusion.h"
#include "../sensors/RvCameraView.h"
#include "../sensors/RvCameraStream.h"
#include "../sensors/RtspCameraPage.h"
#include "../sensors/RvDepthDiagnostic.h"
#include "../sensors/RvDepthCoverage.h"
#include "../sensors/RvDepthView.h"
#include "../sensors/RvDeviceManagerDialog.h"
#include "../sensors/RvPointCloudWidget.h"
#include "../sensors/SceneInstanceBuffer.h"
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
#include <QLineEdit>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QQuick3DGeometry>
#include <QQuickItem>
#include <QQuickWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QSpinBox>
#include <QSettings>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QWheelEvent>
#include <QtEndian>
#include <cstring>

namespace {
void settle(int milliseconds){QElapsedTimer timer;timer.start();while(timer.elapsed()<milliseconds){QApplication::processEvents();QThread::msleep(5);}}
}

int runPortSmokeTests(SensorMainWindow &window,LanguageManager &language,const QString &path)
{
    QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 3;
    QTextStream out(&file);bool pass=true;
    const auto check=[&](const QString &name,bool ok){out<<name<<' '<<(ok?"PASS":"FAIL")<<Qt::endl;pass&=ok;};
    {
        auto *action = window.findChild<QAction *>("measurementCaptureAction");
        if (action)
            action->trigger();
        auto *page = window.findChild<MeasurementCaptureDialog *>("measurementCaptureDialog");
        auto *source = page ? page->findChild<QComboBox *>("measurementSource") : nullptr;
        auto *duration = page ? page->findChild<QSpinBox *>("measurementDuration") : nullptr;
        check("MEASUREMENT_PAGE_HAS_TWO_SOURCES_AND_DURATION_RANGE",
            source && source->count() == 2 && source->findData("rv1103") >= 0 && source->findData("gemini335") >= 0
            && duration && duration->minimum() == 30 && duration->maximum() == 60 && duration->value() == 30);
        if (page)
            page->hide();
        QTemporaryDir output(QFileInfo(path).absolutePath() + "/measurement-test-XXXXXX");
        if (output.isValid()) {
            DepthMeasurement::Job job;
            qint64 sequence = 0;
            const auto record = DepthMeasurement::record({output.path(), "fixture", "flat", 30, 750.0}, job, [&] {
                ++sequence;
                return DepthMeasurement::Frame{QByteArray::fromHex("f4010000"), {{"encoding", "y16_le"}}, {},
                    sequence, sequence * 1'000'000'000, DepthMeasurement::nowNs()};
            });
            QFile manifest(QDir(record.directory).filePath("session.json"));
            const auto data = manifest.open(QIODevice::ReadOnly)
                ? QJsonDocument::fromJson(manifest.readAll()).object() : QJsonObject();
            check("MEASUREMENT_WRITES_COMPLETE_INDEX_AND_PAYLOAD",
                record.status == "complete" && record.frames == 31 && record.spanNs == 30'000'000'000
                && data["status"] == "complete" && data["reference_distance_mm"] == 750.0
                && QFileInfo(QDir(record.directory).filePath("depth.bin")).size() == 124);

            DepthMeasurement::Job canceled;
            int calls = 0;
            const auto partial = DepthMeasurement::record({output.path(), "fixture", {}, 60, 0}, canceled, [&] {
                ++calls;
                if (calls == 2)
                    canceled.stop();
                return DepthMeasurement::Frame{QByteArray::fromHex("f401"), {{"encoding", "y16_le"}}, {},
                    calls, qint64(calls) * 1'000'000'000, DepthMeasurement::nowNs()};
            });
            check("MEASUREMENT_STOP_FINALIZES_PARTIAL_FRAMES",
                partial.status == "stopped" && partial.frames == 1
                && QFileInfo(QDir(partial.directory).filePath("depth.bin")).size() == 2);
            DepthMeasurement::Job offline;
            const auto denied = DepthMeasurement::recordGemini({output.path(), "gemini335", {}, 30, 0}, offline);
            check("MEASUREMENT_OFFLINE_DISALLOWS_GEMINI_CAPTURE",
                denied.status == "failed" && denied.frames == 0 && denied.detail.contains("offline"));
        } else {
            check("MEASUREMENT_OFFLINE_OUTPUT_DIRECTORY_CREATED", false);
        }
    }
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
    check("POINT_CLOUD_JET_MATCHES_ORBBEC_RGB_SAMPLES", PointCloudColorScale::jet(0.) == qRgb(0, 0, 127)
        && PointCloudColorScale::jet(64. / 255.) == qRgb(0, 128, 255)
        && PointCloudColorScale::jet(.5) == qRgb(127, 255, 127)
        && PointCloudColorScale::jet(.75) == qRgb(255, 127, 0)
        && PointCloudColorScale::jet(1.) == qRgb(127, 0, 0));
    check("POINT_CLOUD_JET_CLAMPS_OUT_OF_RANGE_Z", PointCloudColorScale::color(.005) == qRgb(0, 0, 127)
        && PointCloudColorScale::color(4.) == qRgb(127, 0, 0));
    {
        const auto z = [](double metres) { return metres; };
        const std::array<double, 7> samples{.45, .55, 0., .05, 11.,
            std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
        const PointCloudColorScale::DynamicHistogram colors(samples, z);
        const PointCloudColorScale::DynamicHistogram noSamples(std::array<double, 1>{0.}, z);
        check("POINT_CLOUD_DYNAMIC_JET_EQUALIZES_ONLY_DEPTHS_IN_RANGE",
            colors.color(.45) == PointCloudColorScale::jet(std::pow(.5, .65))
            && colors.color(.55) == qRgb(127, 0, 0) && colors.color(.05) == qRgb(0, 0, 127)
            && colors.color(11.) == qRgb(127, 0, 0)
            && colors.color(std::numeric_limits<double>::quiet_NaN()) == qRgb(0, 0, 0));
        check("POINT_CLOUD_DYNAMIC_JET_HANDLES_EMPTY_HISTOGRAM",
            noSamples.color(5.05) == PointCloudColorScale::jet(std::pow(.5, .65))
            && noSamples.color(0.) == qRgb(0, 0, 0));
    }
    settle(900);
    const auto docks=window.findChildren<QDockWidget *>(QString(),Qt::FindDirectChildrenOnly);
    check("EXACTLY_FOUR_MODULES",docks.size()==4);
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
    for(const auto *name:{"tactileDock","depthMapDock","pointCloudDock","rtspCameraDock"})
        check(QString("DOCK_")+name,window.findChild<QDockWidget *>(name)!=nullptr);
    auto *models=window.findChild<QComboBox *>("rvDepthModel");
    const bool vgaSupported=RvAssistDepth::supportsHostModel(QLatin1String(RvCameraView::UniDepthVgaNative48));
    const QString visibleHostModel=QLatin1String(RvCameraView::UniDepthVgaNative48);
    const QString visibleHostScheme=QLatin1String(RvSessionController::GlobalRgbImagingScheme);
    const QString reuseHostScheme=QLatin1String(RvSessionController::ReuseLatestTofImagingScheme);
    const QString depthPro2Model=QLatin1String(RvCameraView::DepthPro2VgaModel);
    const QString depthPro2Scheme=QLatin1String(RvSessionController::DepthPro2ImagingScheme);
    const QString depthPro2BModel=QLatin1String(RvCameraView::DepthPro2BVgaModel);
    const QString depthPro2BScheme=QLatin1String(RvSessionController::DepthPro2BImagingScheme);
    const bool depthPro2Supported=RvAssistDepth::supportsHostModel(depthPro2Model);
    const bool depthPro2BSupported=RvAssistDepth::supportsHostModel(depthPro2BModel);
    const int researchModes=2*(int(depthPro2Supported)+int(depthPro2BSupported));
    const int depthPro2BIndex=2+2*int(depthPro2Supported);
    check("EXACT_IMAGING_SCHEMES",models&&models->count()==6+researchModes&&models->itemData(0)==visibleHostScheme
        &&models->itemData(1)==reuseHostScheme
        &&models->findData(depthPro2Scheme)==(depthPro2Supported?2:-1)
        &&models->findData(RvSessionController::depthPro2Scheme(depthPro2Model,true))==(depthPro2Supported?3:-1)
        &&models->findData(depthPro2BScheme)==(depthPro2BSupported?depthPro2BIndex:-1)
        &&models->findData(RvSessionController::depthPro2Scheme(depthPro2BModel,true))==(depthPro2BSupported?depthPro2BIndex+1:-1)
        &&models->itemData(3+researchModes)==QLatin1String(RvSessionController::Native48ExtendedIntegrationScheme)
        &&models->itemData(5+researchModes)==QLatin1String(RvSessionController::Native48HighAccuracy1200KScheme));
    check("OTHER_IMAGING_SCHEMES_ABSENT",models
        &&models->findData("host_unidepth_wide_576_fp16_tof16")==-1
        &&models->findData("board_litesr_near_full_host_contour_hd")==-1
        &&models->findData("board_litesr_near_128x128_depth_only")==-1);
    check("NO_HOST_DIRECTION_OR_CORRECTION_SELECTORS",
          !window.depth()->findChild<QComboBox *>("rvAssistInputRotation")
          &&!window.depth()->findChild<QComboBox *>("rvUniDepthExperiment"));
    auto *session=window.session();
    check("HOST_RGB_PREPARATION_KEEPS_NEW_PAIR_AFTER_CANCELLATION",session->testHostHdPreparation());
    check("FP16_NATIVE48_DEFAULT_G",session->assistHostModel()==visibleHostModel&&session->assistExperiment()=="G");
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
    if(researchModes){
        const auto &calibration=RvCameraStream::hdVgaCalibration();
        QImage decoded(2304,1296,QImage::Format_RGB888);
        for(int y=0;y<decoded.height();++y){
            auto *row=decoded.scanLine(y);
            for(int x=0;x<decoded.width();++x){
                row[3*x]=uchar((x+3*y)&255);
                row[3*x+1]=uchar((7*x+y)&255);
                row[3*x+2]=uchar((x/17+y/11)&255);
            }
        }
        const auto vga=RvCameraStream::prepareHd(decoded,calibration,RvCameraView::VgaInputRoi);
        check("DEPTHPRO2_DIRECT_REMAP_EQUALS_CENTER_OF_EXISTING_MAP",vga.size()==QSize(640,480)
            &&vga==RvCameraStream::prepareHd(decoded,calibration).copy(RvCameraView::VgaInputRoi));
        bool testedResearchMode=false;
        for(const auto &scheme:{depthPro2Scheme,depthPro2BScheme}){
            const auto model=RvSessionController::depthPro2ModelForScheme(scheme);
            if(!RvAssistDepth::supportsHostModel(model)||!RvAssistDepth::preflightError(model).isEmpty())continue;
            testedResearchMode=true;
            const auto *runtime=RvCameraView::depthPro2Runtime(model);
            const auto &profile=runtime->view;
            const QString variant=scheme==depthPro2BScheme?QStringLiteral("DEPTHPRO2_B"):QStringLiteral("DEPTHPRO2");
            const auto reuseScheme=RvSessionController::depthPro2Scheme(model,true);
            session->selectImagingScheme(reuseScheme,session->dtofCandidateSettings());
            check(variant+"_SELECTS_TOF_B_WITHOUT_STARTING_CAMERA",
                session->currentImagingScheme()==reuseScheme&&models&&models->currentData()==reuseScheme
                &&session->assistHostModel()==model&&session->assistExperiment()=="B"
                &&session->userStopped()&&QSettings().value("depth/imagingScheme")==reuseScheme);
            session->selectImagingScheme(scheme,session->dtofCandidateSettings());
            check(variant+"_SELECTS_TOF_G_WITHOUT_STARTING_CAMERA",
                session->currentImagingScheme()==scheme&&models&&models->currentData()==scheme
                &&session->assistHostModel()==model&&session->assistExperiment()=="G"
                &&session->hostHdAssist()&&session->userStopped()&&!session->splitCorrectionAvailable()
                &&QSettings().value("depth/imagingScheme")==scheme);
            RvAssistDepth worker;
            worker.setHostModel(model);
            worker.setInputRotationDegrees(180);
            worker.setEnabled(true);
            int accepted=0;
            bool sameFrame=false;
            QObject::connect(&worker,&RvAssistDepth::resultReady,&window,
                [&](const QByteArray &depth,const QJsonObject &m,const QImage &camera,
                    quint64 epoch,const RvTofFrame &tof,const QByteArray &diagnostic){
                    ++accepted;
                    sameFrame=epoch==1&&tof.samples.isEmpty()&&camera==vga
                        &&m["assist_host_model"]==model&&m["assist_engine_sha256"]==QLatin1String(runtime->engineSha)
                        &&depth.size()==profile.depthBytes()&&RvCameraView::validOutputGrid(m)
                        &&m["depth_K"].toArray()==RvCameraView::intrinsics(profile)
                        &&m["assist_input_rotation_degrees"]==180
                        &&m["assist_backend"]==QLatin1String(RvCameraView::DepthPro2Backend)&&m["dtof_present"]==false
                        &&m["assist_fusion_revision"]==QLatin1String(RvAssistFusion::vgaRevision("G"))
                        &&m["assist_fusion"].toObject()["fusion_mode"]=="monocular"
                        &&RvDepthDiagnostic::monocularDepth(diagnostic,m)==depth;
                    window.depth()->acceptAssistSnapshot(depth,m,camera,tof,diagnostic);
                });
            const auto planar=RvAssistDepth::preparePlanarInput(vga);
            QElapsedTimer testClock;testClock.start();
            for(int frame=1;!accepted&&!worker.failed()&&testClock.elapsed()<15000;++frame){
                const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const QJsonObject source{{"source_protocol","HDA1"},{"source_transport","rtsp_h265"},
                    {"source_candidate_mode",RvCaptureContract::Native48},{"model","Native"},
                    {"source_rgb_width",2304},{"source_rgb_height",1296},{"rgb_width",640},{"rgb_height",480},
                    {"dtof_present",false},{"rectify_map_sha256",profile.mapSha},{"assist_camera_profile","wide"},
                    {"frame_id",frame},{"generation",1},{"host_decoded_monotonic_ns",double(ns)},
                    {"boot_id","abcdef01-2345-6789-abcd-ef0123456789"},{"work_mode","host_rgb"},{"assist_experiment","G"}};
                worker.submitHd(vga,source,1,{},planar);
                settle(30);
            }
            check(variant+"_TENSORRT_WORKER_PUBLISHES_SAME_FRAME",accepted>0&&sameFrame);
            const auto snapshot=window.depth()->snapshot();
            const auto *canvas=static_cast<RvDepthView *>(window.depth()->findChild<QWidget *>("rvAssistDepthCanvas"));
            check(variant+"_VGA_DISPLAY_AND_SNAPSHOT",canvas&&canvas->presentationSize()==QSizeF(640,480)
                &&snapshot.depthImage.size()==QSize(640,480)&&snapshot.cameraImage==vga
                &&snapshot.metresFloat32LE.size()==profile.depthBytes()
                &&snapshot.metadata["depth_K"].toArray()==RvCameraView::intrinsics(profile));
        }
        if(testedResearchMode){
            session->selectImagingScheme(visibleHostScheme,session->dtofCandidateSettings());
            check("DEPTHPRO2_SWITCH_BACK_TO_G",session->currentImagingScheme()==visibleHostScheme
                &&session->assistHostModel()==visibleHostModel&&session->assistExperiment()=="G"&&session->userStopped());
        }
    }
    {
        auto *dock = window.findChild<QDockWidget *>("pointCloudDock");
        auto *depthDock = window.findChild<QDockWidget *>("depthMapDock");
        check("POINT_CLOUD_TAB_WITH_DEPTH", dock && window.tabifiedDockWidgets(depthDock).contains(dock));
        if (dock) {
            dock->show();
            dock->raise();
            settle(80);
            auto *view = dock->findChild<QQuickWidget *>("rvPointCloudView");
            auto *instances = dock->findChild<SceneInstanceBuffer *>("rvPointCloudInstances");
            auto *depthInstances = dock->findChild<SceneInstanceBuffer *>("rvPointCloudDepthInstances");
            auto *mesh = dock->findChild<QQuick3DGeometry *>("rvPointCloudMeshGeometry");
            auto *displayMode = dock->findChild<QComboBox *>("rvPointCloudDisplayMode");
            auto *colorMode = dock->findChild<QComboBox *>("rvPointCloudColorMode");
            auto *status = dock->findChild<QLabel *>("rvPointCloudStatus");
            auto *legend = dock->findChild<QLabel *>("rvPointCloudColorLegend");
            auto *pause = dock->findChild<QPushButton *>("rvPointCloudPause");
            auto *accumulate = dock->findChild<QPushButton *>("rvPointCloudAccumulate");
            auto *reset = dock->findChild<QPushButton *>("rvPointCloudReset");
            auto *diameter = dock->findChild<QDoubleSpinBox *>("rvPointCloudPointDiameter");
            auto *colorMinimum = dock->findChild<QDoubleSpinBox *>("rvPointCloudColorMinimum");
            auto *colorMaximum = dock->findChild<QDoubleSpinBox *>("rvPointCloudColorMaximum");
            auto *applyRange = dock->findChild<QPushButton *>("rvPointCloudColorApply");
            const auto pointCloudText = [](const char *text) {
                return QCoreApplication::translate("RvPointCloudWidget", text);
            };
            check("POINT_CLOUD_CAPTURE_PROMPT", status
                && status->text() == pointCloudText("Start capture in Depth Map or Camera raw video."));
            check("POINT_CLOUD_STOP_CONTROL", pause && pause->isEnabled() && !pause->isChecked()
                && pause->text() == pointCloudText("Stop point cloud"));
            check("POINT_CLOUD_QML_READY", depthInstances && view && view->status() == QQuickWidget::Ready);
            if (view && view->status() == QQuickWidget::Error)
                for (const auto &error : view->errors())
                    out << error.toString() << '\n';
            check("POINT_CLOUD_DISPLAY_MODE_DEFAULTS_TO_POINTS", mesh && displayMode && displayMode->isEnabled()
                && displayMode->currentText() == pointCloudText("Points")
                && displayMode->count() == 2 && mesh->indexData().isEmpty());
            check("POINT_CLOUD_ACCUMULATION_DISABLED_FOR_ASSISTED", accumulate && accumulate->isCheckable()
                && !accumulate->isChecked() && !accumulate->isEnabled());
            check("POINT_CLOUD_ASSISTED_DISPLAY_DEFAULTS", diameter && colorMinimum && colorMaximum && applyRange
                && diameter->isVisible() && diameter->value() == 2.4 && colorMinimum->value() == .01
                && colorMaximum->value() == 3.0 && !applyRange->isEnabled());
            check("POINT_CLOUD_COLOR_MODE_DEFAULTS_TO_FIXED_Z", colorMode && colorMode->count() == 2
                && colorMode->currentText() == pointCloudText("Fixed Z color range"));
            const auto ramp = legend ? legend->pixmap().toImage() : QImage{};
            check("POINT_CLOUD_LEGEND_USES_JET", ramp.size() == QSize(256, 1)
                && ramp.pixel(0, 0) == qRgb(0, 0, 127) && ramp.pixel(64, 0) == qRgb(0, 128, 255)
                && ramp.pixel(255, 0) == qRgb(127, 0, 0));
            if (instances && depthInstances && view && view->status() == QQuickWidget::Ready
                && mesh && displayMode && colorMode && pause && accumulate
                && diameter && colorMinimum && colorMaximum && applyRange) {
                session->selectImagingScheme("board_native_tof48", {});
                check("POINT_CLOUD_NATIVE_SHOWS_SHARED_RANGE_AND_HIDES_ASSISTED_DIAMETER", !diameter->isVisible()
                    && colorMinimum->isVisible() && colorMaximum->isVisible());
                const QString generation(32, 'a');
                RvTofFrame frame;
                frame.samples = QByteArray(48 * 32 * 4, 0);
                frame.metadata = {{"dtof_width", 48}, {"dtof_height", 32}, {"dtof_bytes", frame.samples.size()},
                    {"dtof_format", "range_uq2_u16le_snr_u8_valid_u8"}, {"dtof_range_unit", "0.25_mm"},
                    {"dtof_source", "tmf8829_ipc_first_peak"}, {"dtof_coordinate", "sensor_radial_native"},
                    {"protocol", "LSG2"}, {"work_mode", "dtof_candidate"}, {"candidate_mode", 24},
                    {"engine", "native-hd-capture-v1"}, {"model_sha256", ""}, {"capture_only", true},
                    {"depth_present", false}, {"photon_enabled", false}, {"dtof_fov_correction", 6},
                    {"generation", generation}, {"boot_id", "12345678-1234-1234-1234-123456789abc"},
                    {"frame_id", 1}, {"tof_sequence", 1}, {"tof_timestamp_ns", 1}, {"tof_reused", false},
                    {"raw_age_ms", 0}, {"tof_calibration", QJsonObject{{"schema", 1},
                        {"run_id", "dtof_" + generation}, {"sensor_fov_correction", 6},
                        {"processing_fov_correction", 6}, {"temporary_calibration", false}}}};
                const auto sample = [&frame](int index, quint16 range, char valid) {
                    qToLittleEndian(range, frame.samples.data() + index * 4);
                    frame.samples[index * 4 + 2] = 9;
                    frame.samples[index * 4 + 3] = valid;
                };
                sample(2, 2500, 1);
                sample(3, 8000, 0);
                sample(4, 0, 1);
                sample(32 * 48 - 1, 4300, 1);
                auto bad = frame;
                bad.metadata["tof_calibration"] = QJsonObject{};
                emit session->tofFrameReady(bad);
                check("POINT_CLOUD_REJECTS_UNBOUND_CALIBRATION", instances->count() == 0);
                emit session->tofFrameReady(frame);
                const auto &cal = RvAssistFusion::calibrationForFov(6);
                const auto expected = [&](int index) {
                    const double d = frame.distanceMm(index);
                    const auto &ray = cal.cameraRays[index];
                    return QVector3D(float(d * ray[0] + cal.translation[0] * 1000),
                                    -float(d * ray[1] + cal.translation[1] * 1000),
                                    -float(d * ray[2] + cal.translation[2] * 1000));
                };
                check("POINT_CLOUD_F6_XYZ_SKIPS_INVALID_AND_ZERO", instances->count() == 2
                    && (instances->instancePosition(0) - expected(2)).length() < .001f
                    && (instances->instancePosition(1) - expected(32 * 48 - 1)).length() < .001f);
                check("POINT_CLOUD_NATIVE_JET_USES_PROJECTED_Z", instances->count() == 2
                    && instances->instanceColor(0).rgb() == PointCloudColorScale::color(-expected(2).z() / 1000.)
                    && instances->instanceColor(0).rgb() != PointCloudColorScale::color(frame.distanceMm(2) / 1000.));

                const auto cameraDistance = [&] { return view->rootObject()->property("cameraDistance").toDouble(); };
                const double initialDistance = cameraDistance();
                const auto initialTarget = view->rootObject()->property("cameraTarget").value<QVector3D>();
                const auto checkFixedView = [&] {
                    return cameraDistance() == initialDistance
                        && view->rootObject()->property("cameraTarget").value<QVector3D>() == initialTarget;
                };
                const auto meshVisible = [&] { return view->rootObject()->property("meshVisible").toBool(); };
                const auto framebufferHasColor = [&](QRgb rgb) {
                    const auto image = view->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
                    for (int y = 0; y < image.height(); ++y) {
                        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                        for (int x = 0; x < image.width(); ++x)
                            if (line[x] == rgb)
                                return true;
                    }
                    return false;
                };
                const auto triangleCount = [&] { return mesh->indexData().size() / (3 * sizeof(quint32)); };
                const auto meshPosition = [&](int index) {
                    float xyz[3]{};
                    const qsizetype offset = qsizetype(index) * mesh->stride();
                    const auto vertices = mesh->vertexData();
                    if (offset + qsizetype(sizeof(xyz)) <= vertices.size())
                        std::memcpy(xyz, vertices.constData() + offset, sizeof(xyz));
                    return QVector3D(xyz[0], xyz[1], xyz[2]);
                };
                const auto meshColor = [&](int index) -> QRgb {
                    const auto vertices = mesh->vertexData();
                    const qsizetype offset = qsizetype(index) * mesh->stride() + sizeof(QVector3D);
                    float rgba[4]{};
                    if (offset + qsizetype(sizeof(rgba)) <= vertices.size())
                        std::memcpy(rgba, vertices.constData() + offset, sizeof(rgba));
                    return qRgb(qRound(rgba[0] * 255), qRound(rgba[1] * 255), qRound(rgba[2] * 255));
                };
                const auto waitForTriangles = [&](qsizetype expectedCount) {
                    QElapsedTimer timer;
                    timer.start();
                    while (triangleCount() != expectedCount && timer.elapsed() < 2000)
                        settle(10);
                    return triangleCount() == expectedCount;
                };
                sample(32 * 48 - 1, 22000, 1);
                frame.metadata["tof_sequence"] = 2;
                emit session->tofFrameReady(frame);
                settle(30);
                check("POINT_CLOUD_FRAME_DOES_NOT_REFIT", checkFixedView() && instances->count() == 2
                    && (instances->instancePosition(1) - expected(32 * 48 - 1)).length() < .001f);
                const auto *axes = view->rootObject()->findChild<QObject *>("rvPointCloudAxes");
                const auto *axisLabels = view->rootObject()->findChild<QObject *>("rvPointCloudAxisLabels");
                check("POINT_CLOUD_XYZ_REFERENCE_WITHOUT_GRID", axes && axes->property("count").toInt() == 3
                    && axisLabels && axisLabels->property("count").toInt() == 3
                    && !view->rootObject()->property("gridStepMm").isValid());
                view->rootObject()->setProperty("cameraDistance", initialDistance * 1.8);
                view->grabFramebuffer().save(path + ".point-cloud-axes.png");
                view->rootObject()->setProperty("cameraDistance", initialDistance);

                const auto sendPointFrame = [&](int number) {
                    sample(2, quint16(3000 + number * 20), 1);
                    frame.metadata["frame_id"] = number + 2;
                    frame.metadata["tof_sequence"] = number + 2;
                    frame.metadata["tof_timestamp_ns"] = number + 2;
                    emit session->tofFrameReady(frame);
                    return expected(2);
                };
                const auto matchesNativePoint = [&](int index, const QVector3D &position) {
                    return (instances->instancePosition(index) - position).length() < .001f;
                };
                const auto waitForNativePoints = [&](int pointCount, const QVector3D &oldest, const QVector3D &latest) {
                    const auto matches = [&] {
                        return instances->count() == pointCount && matchesNativePoint(0, oldest)
                            && matchesNativePoint(pointCount - 2, latest);
                    };
                    QElapsedTimer timer;
                    timer.start();
                    while (!matches() && timer.elapsed() < 2000)
                        settle(10);
                    return matches();
                };
                const auto first = expected(2);
                check("POINT_CLOUD_ACCUMULATION_AVAILABLE_FOR_NATIVE", accumulate->isEnabled() && !accumulate->isChecked());
                accumulate->click();
                const auto second = sendPointFrame(1);
                check("POINT_CLOUD_ACCUMULATES_FRAMES", accumulate->isChecked() && waitForNativePoints(4, first, second));
                const auto third = sendPointFrame(2);
                QVector3D newest;
                for (int number = 3; number < 100; ++number)
                    newest = sendPointFrame(number);
                check("POINT_CLOUD_ACCUMULATION_KEEPS_100_FRAMES", waitForNativePoints(200, first, newest));
                newest = sendPointFrame(100);
                check("POINT_CLOUD_ACCUMULATION_FRAME_101_EVICTS_FRAME_1", waitForNativePoints(200, second, newest));
                newest = sendPointFrame(101);
                check("POINT_CLOUD_ACCUMULATION_FRAME_102_EVICTS_FRAME_2", waitForNativePoints(200, third, newest));
                const auto colorBefore = instances->instanceColor(0);
                initialPresentation->setColorRange(1, 3);
                check("POINT_CLOUD_NATIVE_IGNORES_DEPTH_MAP_COLOR_RANGE", instances->count() == 200
                    && matchesNativePoint(0, third) && instances->instanceColor(0) == colorBefore);
                initialPresentation->setColorRange(.01, 3);
                colorMinimum->setValue(1.0);
                applyRange->click();
                check("POINT_CLOUD_ACCUMULATION_RECOLORS_HISTORY", instances->count() == 200
                    && matchesNativePoint(0, third) && instances->instanceColor(0).rgb() == qRgb(0, 0, 127)
                    && instances->instanceColor(0) != colorBefore);
                colorMinimum->setValue(.01);
                applyRange->click();
                newest = sendPointFrame(102);
                accumulate->click();
                check("POINT_CLOUD_ACCUMULATION_OFF_RESTORES_LATEST_FRAME", !accumulate->isChecked()
                    && instances->count() == 2 && matchesNativePoint(0, newest) && checkFixedView());

                const auto zoom = [&](int delta) {
                    const QPoint center = view->rect().center();
                    QWheelEvent event(center, view->mapToGlobal(center), {}, QPoint(0, delta),
                                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(view, &event);
                };
                zoom(-120);
                const bool zoomedOut = cameraDistance() > initialDistance;
                zoom(120);
                zoom(120);
                const double manualDistance = cameraDistance();
                check("POINT_CLOUD_WHEEL_ZOOMS", zoomedOut && manualDistance < initialDistance);

                QQuickItem *scene = view->rootObject();
                scene->setProperty("orbitYaw", 42);
                sendPointFrame(103);
                dock->setFloating(true);
                settle(100);
                check("POINT_CLOUD_FLOAT_PRESERVES_VIEW", view->status() == QQuickWidget::Ready && view->rootObject()
                    && view->rootObject()->property("orbitYaw").toInt() == 42
                    && cameraDistance() == manualDistance && instances->count() == 2);
                dock->setFloating(false);
                dock->raise();
                settle(60);
                frame.metadata["raw_age_ms"] = 975;
                emit session->tofFrameReady(frame);
                settle(55);
                check("POINT_CLOUD_EXPIRES", instances->count() == 0);
                frame.metadata["raw_age_ms"] = 0;
                emit session->tofFrameReady(frame);
                check("POINT_CLOUD_NEW_FRAME_PRESERVES_MANUAL_ZOOM", instances->count() == 2
                    && cameraDistance() == manualDistance);
                session->stop();
                check("POINT_CLOUD_CLEARS_ON_STOP", instances->count() == 0);
                emit session->tofFrameReady(frame);
                check("POINT_CLOUD_RESUME_PRESERVES_MANUAL_ZOOM", instances->count() == 2
                    && cameraDistance() == manualDistance);
                session->selectImagingScheme("board_native_tof48_high_accuracy", {});
                frame.metadata["candidate_mode"] = 25;
                emit session->tofFrameReady(frame);
                check("POINT_CLOUD_SCHEME_CHANGE_PRESERVES_MANUAL_ZOOM", instances->count() == 2
                    && cameraDistance() == manualDistance);
                accumulate->click();
                const auto highAccuracyFirst = expected(2);
                frame.metadata["raw_age_ms"] = 975;
                const auto highAccuracyLatest = sendPointFrame(104);
                const bool highAccuracyAccumulated = waitForNativePoints(4, highAccuracyFirst, highAccuracyLatest);
                settle(50);
                check("POINT_CLOUD_HIGH_ACCURACY_ACCUMULATES_ACROSS_LIVE_FRAME_EXPIRY", accumulate->isEnabled()
                    && accumulate->isChecked() && highAccuracyAccumulated && instances->count() == 4);
                accumulate->click();
                check("POINT_CLOUD_ACCUMULATION_OFF_CLEARS_EXPIRED_FRAME", instances->count() == 0);
                frame.metadata["raw_age_ms"] = 0;
                sendPointFrame(105);
                accumulate->click();
                session->stop();
                check("POINT_CLOUD_ACCUMULATION_CLEARS_ON_STOP", instances->count() == 0);
                accumulate->click();

                const int meshCell = 15 * 48 + 4;
                const auto nativePatch = [&](double zMm) {
                    for (int i : {meshCell, meshCell + 1, meshCell + 2,
                                  meshCell + 48, meshCell + 49, meshCell + 50}) {
                        const double target = zMm + ((i - meshCell) % 48 == 2 ? 800. : 0.);
                        const auto range = (target - 1000. * cal.translation[2]) / cal.cameraRays[i][2];
                        sample(i, quint16(qRound(range * 4.)), 1);
                    }
                };
                frame.samples.fill(0);
                nativePatch(800.);
                emit session->tofFrameReady(frame);
                auto *sceneRoot = view->rootObject();
                const auto formerTarget = sceneRoot->property("cameraTarget");
                const auto patchCenter = (expected(meshCell) + expected(meshCell + 49)) * .5f;
                sceneRoot->setProperty("cameraTarget", patchCenter);
                const auto zColor = PointCloudColorScale::color(-expected(meshCell).z() / 1000.);
                const QRgb nativeJet = instances->instanceColor(0).rgb();
                check("POINT_CLOUD_NATIVE_POINTS_RENDER_JET", nativeJet == zColor
                    && framebufferHasColor(nativeJet));
                displayMode->setCurrentIndex(1);
                check("POINT_CLOUD_HIGH_ACCURACY_MESH_USES_PROJECTED_Z_AND_BREAKS_AT_STEP", meshVisible()
                    && triangleCount() == 2 && instances->count() == 0
                    && (meshPosition(0) - expected(meshCell)).length() < .001f && cameraDistance() == manualDistance);
                check("POINT_CLOUD_NATIVE_MESH_MATCHES_POINTS_JET", meshColor(0) == nativeJet
                    && framebufferHasColor(nativeJet));
                // The framebuffer readback can outlast the simulated measurement's freshness window.
                emit session->tofFrameReady(frame);
                colorMode->setCurrentIndex(1);
                const QRgb dynamicNativeMesh = meshColor(0);
                displayMode->setCurrentIndex(0);
                check("POINT_CLOUD_DYNAMIC_NATIVE_POINTS_AND_MESH_USE_SAME_CAMERA_Z",
                    dynamicNativeMesh != nativeJet && dynamicNativeMesh == instances->instanceColor(0).rgb()
                    && matchesNativePoint(0, expected(meshCell)) && !colorMinimum->isVisible());
                displayMode->setCurrentIndex(1);
                colorMode->setCurrentIndex(0);
                sceneRoot->setProperty("cameraTarget", formerTarget);
                session->selectImagingScheme("board_native_tof48", {});
                frame.metadata["candidate_mode"] = 24;
                sample(meshCell, 0, 0);
                emit session->tofFrameReady(frame);
                check("POINT_CLOUD_NATIVE_MESH_LEAVES_HOLES", displayMode->isEnabled() && meshVisible()
                    && triangleCount() == 1 && (meshPosition(0) - expected(meshCell + 1)).length() < .001f);
                accumulate->click();
                for (int number = 1; number <= 100; ++number) {
                    nativePatch(800. + number);
                    frame.metadata["tof_sequence"] = 200 + number;
                    emit session->tofFrameReady(frame);
                }
                check("POINT_CLOUD_ACCUMULATED_MESH_EVICTS_OLDEST_FRAME", waitForTriangles(200)
                    && std::abs(meshPosition(0).z() + 801.f) <= .13f);
                dock->setFloating(true);
                settle(100);
                check("POINT_CLOUD_FLOAT_PRESERVES_MESH_AND_HISTORY", view->status() == QQuickWidget::Ready
                    && meshVisible() && triangleCount() == 200 && cameraDistance() == manualDistance);
                dock->setFloating(false);
                dock->raise();
                displayMode->setCurrentIndex(0);
                check("POINT_CLOUD_MESH_SWITCH_PRESERVES_POINT_HISTORY", !meshVisible()
                    && triangleCount() == 0 && instances->count() == 600);
                const QRgb historyColor = instances->instanceColor(0).rgb();
                const auto historyPosition = instances->instancePosition(0);
                colorMode->setCurrentIndex(1);
                const bool dynamicHistory = instances->count() == 600
                    && instances->instanceColor(0).rgb() != historyColor
                    && instances->instancePosition(0) == historyPosition;
                colorMode->setCurrentIndex(0);
                check("POINT_CLOUD_DYNAMIC_RECOLORS_NATIVE_HISTORY_WITHOUT_DISCARDING_POINTS", dynamicHistory
                    && instances->count() == 600 && instances->instanceColor(0).rgb() == historyColor
                    && instances->instancePosition(0) == historyPosition);
                pause->click();
                emit session->tofFrameReady(frame);
                settle(130);
                check("POINT_CLOUD_STOP_CLEARS_NATIVE_HISTORY", pause->isChecked() && view->isHidden()
                    && instances->count() == 0 && triangleCount() == 0 && !accumulate->isEnabled());
                pause->click();
                const bool waitingForTof = instances->count() == 0 && accumulate->isChecked();
                frame.metadata["tof_sequence"] = 301;
                emit session->tofFrameReady(frame);
                settle(130);
                check("POINT_CLOUD_RESUME_NATIVE_USES_FRESH_FRAMES", waitingForTof && !pause->isChecked()
                    && view->isVisible() && instances->count() == 6 && cameraDistance() == manualDistance);
                session->selectImagingScheme(visibleHostScheme, {});
                check("POINT_CLOUD_ASSISTED_SWITCH_DISARMS_ACCUMULATION", !accumulate->isEnabled() && !accumulate->isChecked());
                emit session->tofFrameReady(frame);
                check("POINT_CLOUD_IGNORES_PREVIOUS_GRID", instances->count() == 0);

                const auto &profile = RvCameraView::outputProfile(visibleHostModel);
                const int pixels = profile.outputWidth * profile.outputHeight;
                QByteArray depth(profile.depthBytes(), 0);
                const auto setDepth = [&](int x, int y, float z) {
                    quint32 bits;
                    std::memcpy(&bits, &z, sizeof(bits));
                    qToLittleEndian(bits, depth.data() + (y * profile.outputWidth + x) * sizeof(bits));
                };
                const QJsonObject metadata{{"assist_host_model", visibleHostModel}, {"depth_unit", "camera-Z_m_float32"},
                    {"width", 480}, {"height", 640}, {"assist_output_grid", RvCameraView::PortraitGrid}, {"assist_model_depth_size", QJsonArray{640,480}}, {"assist_model_crop_xywh", QJsonArray{140,0,360,480}}, {"depth_K", RvCameraView::intrinsics(profile)},
                    {"rectify_map_sha256", profile.mapSha}, {"assist_output_restored_to_calibrated_grid", true}};
                const auto count = [&] { return depthInstances->count(); };
                const auto waitForCount = [&](int expectedCount) {
                    QElapsedTimer timer;
                    timer.start();
                    while (count() != expectedCount && timer.elapsed() < 2000)
                        settle(10);
                    return count() == expectedCount;
                };
                setDepth(4, 2, .75f);
                setDepth(5, 2, std::numeric_limits<float>::quiet_NaN());
                setDepth(6, 2, -1.f);
                setDepth(7, 2, std::numeric_limits<float>::infinity());
                setDepth(476, 638, 1.75f);
                auto invalidDepth = metadata;
                invalidDepth["assist_output_restored_to_calibrated_grid"] = false;
                emit session->assistDepthSnapshotReady(depth, invalidDepth, {});
                invalidDepth = metadata;
                invalidDepth["width"] = 640;
                invalidDepth["height"] = 480;
                emit session->assistDepthSnapshotReady(depth, invalidDepth, {});
                invalidDepth = metadata;
                invalidDepth["depth_K"] = QJsonArray{100, 100, 100, 100};
                emit session->assistDepthSnapshotReady(depth, invalidDepth, {});
                emit session->assistDepthSnapshotReady(depth.chopped(4), metadata, {});
                settle(45);
                check("POINT_CLOUD_REJECTS_WRONG_DEPTH_GEOMETRY", count() == 0);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                const bool hasSparseDepth = waitForCount(2);
                const auto matchesDepth = [&](const QVector3D &point, int x, int y, float metres) {
                    const float z = metres * 1000.f;
                    return std::abs(point.x() - (x - profile.cx()) * z / profile.focal) < .001
                        && std::abs(point.y() - (profile.cy() - y) * z / profile.focal) < .001
                        && point.z() == -z;
                };
                check("POINT_CLOUD_ASSISTED_CAMERA_Z_XYZ_SKIPS_INVALID", hasSparseDepth
                    && matchesDepth(depthInstances->instancePosition(0), 4, 2, .75f)
                    && matchesDepth(depthInstances->instancePosition(1), 476, 638, 1.75f));
                const QRgb originalColor = qRgb(0, 124, 255);
                check("POINT_CLOUD_ASSISTED_COLORS_BY_CAMERA_Z", hasSparseDepth
                    && depthInstances->instanceColor(0).rgb() == originalColor);

                initialPresentation->setColorRange(.1, 5.0);
                settle(45);
                check("POINT_CLOUD_ASSISTED_IGNORES_DEPTH_MAP_COLOR_RANGE", colorMinimum->value() == .01
                    && colorMaximum->value() == 3.0 && depthInstances->instanceColor(0).rgb() == originalColor);
                colorMinimum->setValue(4.0);
                check("POINT_CLOUD_REJECTS_INVERTED_COLOR_RANGE", !applyRange->isEnabled());
                colorMinimum->setValue(.2);
                auto *rangeInput = colorMaximum->findChild<QLineEdit *>();
                if (rangeInput) {
                    colorMaximum->setFocus();
                    rangeInput->selectAll();
                    QKeyEvent erase(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
                    QApplication::sendEvent(rangeInput, &erase);
                    QKeyEvent two(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier, QStringLiteral("2"));
                    QApplication::sendEvent(rangeInput, &two);
                }
                check("POINT_CLOUD_APPLY_AVAILABLE_AFTER_TYPING_COLOR_LIMIT", rangeInput
                    && applyRange->isEnabled() && colorMaximum->value() == 2.0);
                const QVector3D nearPosition = depthInstances->instancePosition(0);
                applyRange->click();
                const QRgb recolored = qRgb(0, 184, 255);
                QElapsedTimer colorWait;
                colorWait.start();
                while (depthInstances->instanceColor(0).rgb() != recolored && colorWait.elapsed() < 2000)
                    settle(10);
                check("POINT_CLOUD_ASSISTED_RECOLORS_WITHOUT_CHANGING_POINTS", count() == 2 && recolored != originalColor
                    && depthInstances->instanceColor(0).rgb() == recolored && depthInstances->instancePosition(0) == nearPosition
                    && initialPresentation->colorMinimum() == .1 && initialPresentation->colorMaximum() == 5.0);
                const double viewDistance = cameraDistance();
                diameter->setValue(4.2);
                const QSettings pointPreferences;
                check("POINT_CLOUD_ASSISTED_DISPLAY_PREFERENCES", view->rootObject()->property("depthPointDiameter").toDouble() == 4.2
                    && cameraDistance() == viewDistance && depthInstances->instancePosition(0) == nearPosition
                    && pointPreferences.value("pointCloud/assistedPointDiameterPx").toDouble() == 4.2
                    && pointPreferences.value("pointCloud/assistedColorMinimumM").toDouble() == .2
                    && pointPreferences.value("pointCloud/assistedColorMaximumM").toDouble() == 2.0);
                initialPresentation->setColorRange(.01, 3.0);

                const auto waitForPointColor = [&](QRgb expected) {
                    QElapsedTimer timer;
                    timer.start();
                    while ((count() != 2 || depthInstances->instanceColor(0).rgb() != expected) && timer.elapsed() < 2000)
                        settle(10);
                    return count() == 2 && depthInstances->instanceColor(0).rgb() == expected;
                };
                colorMode->setCurrentIndex(1);
                const QRgb equalized = PointCloudColorScale::jet(std::pow(.5, .65));
                check("POINT_CLOUD_DYNAMIC_ASSISTED_RECOLORS_LATEST_AND_PERSISTS",
                    waitForPointColor(equalized) && depthInstances->instancePosition(0) == nearPosition
                    && !colorMinimum->isVisible() && !applyRange->isVisible()
                    && QSettings().value("pointCloud/colorMode").toString() == "dynamic");
                {
                    RvPointCloudWidget restored(session);
                    const auto *restoredMode = restored.findChild<QComboBox *>("rvPointCloudColorMode");
                    check("POINT_CLOUD_DYNAMIC_RESTORES_SAVED_MODE", restoredMode && restoredMode->currentIndex() == 1);
                }
                setDepth(476, 638, .6f);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                check("POINT_CLOUD_DYNAMIC_COLOR_FOLLOWS_FRAME_HISTOGRAM",
                    waitForPointColor(qRgb(127, 0, 0)) && depthInstances->instancePosition(0) == nearPosition);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                colorMode->setCurrentIndex(0);
                check("POINT_CLOUD_DYNAMIC_SWITCH_BACK_KEEPS_FIXED_Z_RANGE",
                    waitForPointColor(recolored) && colorMinimum->isVisible() && colorMinimum->value() == .2
                    && colorMaximum->value() == 2.0 && !applyRange->isEnabled());

                depth.fill(0);
                for (int y = 150; y < 330; y += 6)
                    for (int x = 220; x < 420; x += 6)
                        setDepth(x, y, .8f);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                const bool hasDiscFrame = waitForCount(30 * 34);
                const auto coloredPixels = [&] {
                    settle(25);
                    const auto image = view->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
                    int colored = 0;
                    for (int y = 0; y < image.height(); ++y) {
                        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                        for (int x = 0; x < image.width(); ++x) {
                            const QRgb rgb = line[x];
                            colored += std::max({qRed(rgb), qGreen(rgb), qBlue(rgb)})
                                - std::min({qRed(rgb), qGreen(rgb), qBlue(rgb)}) > 125;
                        }
                    }
                    return colored;
                };
                diameter->setValue(2.4);
                const int defaultCoverage = coloredPixels();
                diameter->setValue(4.2);
                check("POINT_CLOUD_ASSISTED_DIAMETER_CHANGES_SCREEN_COVERAGE", hasDiscFrame
                    && defaultCoverage > 100 && coloredPixels() > defaultCoverage);

                depth.fill(0);
                for (int y : {150, 151}) {
                    setDepth(220, y, .8f);
                    setDepth(221, y, .8f);
                    setDepth(222, y, 1.6f);
                }
                setDepth(220, 150, std::numeric_limits<float>::quiet_NaN());
                displayMode->setCurrentIndex(1);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                check("POINT_CLOUD_ASSISTED_MESH_PRESERVES_CAMERA_Z_AND_LEAVES_GAPS", waitForTriangles(1)
                    && meshVisible() && count() == 0 && !diameter->isEnabled() && !accumulate->isEnabled()
                    && matchesDepth(meshPosition(0), 221, 150, .8f)
                    && meshColor(0) == PointCloudColorScale::color(.8f, .2, 2.0));

                // Use a wide enough transition that each local Z step used to pass the mesh limit.
                constexpr int surfaceWidth = 20, transitionWidth = 11, surfaceHeight = 28;
                constexpr int surfaceStartX = 220, surfaceStartY = 150;
                const auto putSurface = [&](int column, int row, float z, bool transpose) {
                    setDepth(surfaceStartX + (transpose ? row : column),
                             surfaceStartY + (transpose ? column : row), z);
                };
                const qsizetype surfaceTriangles = 4 * (surfaceWidth - 1) * (surfaceHeight - 1);
                const int surfacePoints = 2 * surfaceWidth * surfaceHeight;
                for (const bool transpose : {false, true}) {
                    depth.fill(0);
                    for (int row = 0; row < surfaceHeight; ++row) {
                        for (int x = 0; x < surfaceWidth; ++x) {
                            putSurface(x, row, .8f, transpose);
                            putSurface(x + surfaceWidth + transitionWidth, row, 1.22f, transpose);
                        }
                        for (int x = 0; x < transitionWidth; ++x)
                            putSurface(surfaceWidth + x, row, .8f + .035f * (x + 1), transpose);
                    }
                    displayMode->setCurrentIndex(1);
                    emit session->assistDepthSnapshotReady(depth, metadata, {});
                    check(transpose ? "POINT_CLOUD_MESH_CUTS_VERTICAL_DEPTH_TRANSITION"
                                    : "POINT_CLOUD_MESH_CUTS_HORIZONTAL_DEPTH_TRANSITION",
                        waitForTriangles(surfaceTriangles) && mesh->vertexData().size() == surfacePoints * mesh->stride());
                    displayMode->setCurrentIndex(0);
                    check(transpose ? "POINT_CLOUD_POINTS_SKIP_VERTICAL_DEPTH_TRANSITION"
                                    : "POINT_CLOUD_POINTS_SKIP_HORIZONTAL_DEPTH_TRANSITION",
                        waitForCount(surfacePoints) && depthInstances->instancePosition(0).z() == -800.f
                        && depthInstances->instancePosition(surfacePoints - 1).z() == -1220.f);
                }

                depth.fill(0);
                for (int row = 0; row < 36; ++row)
                    for (int x = 0; x < 48; ++x)
                        putSurface(x, row, 1.f / (1.34f - .012f * x), false);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                check("POINT_CLOUD_POINTS_KEEP_SLANTED_PLANE", waitForCount(48 * 36));

                depth.fill(0);
                for (int row = 0; row < 36; ++row)
                    for (int x = 0; x < 41; ++x)
                        putSurface(x, row, x == 20 ? .55f : 1.4f, false);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                check("POINT_CLOUD_POINTS_KEEP_THIN_FOREGROUND", waitForCount(41 * 36)
                    && depthInstances->instancePosition(20).z() == -550.f);

                for (int y = 0; y < profile.outputHeight; ++y)
                    for (int x = 0; x < profile.outputWidth; ++x)
                        setDepth(x, y, 1.f);
                displayMode->setCurrentIndex(1);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                const qsizetype fullMeshTriangles = 2 * (profile.outputWidth - 1) * (profile.outputHeight - 1);
                const bool hasFullMesh = waitForTriangles(fullMeshTriangles);
                check("POINT_CLOUD_ASSISTED_MESH_RENDERS_ALL_GRID_CELLS", hasFullMesh && meshVisible()
                    && mesh->vertexData().size() == pixels * mesh->stride() && coloredPixels() > 1000);
                const QRgb meshJet = meshColor(0);
                check("POINT_CLOUD_ASSISTED_MESH_RENDERS_JET", meshJet == qRgb(70, 255, 184)
                    && framebufferHasColor(meshJet));

                displayMode->setCurrentIndex(0);
                displayMode->setCurrentIndex(1);
                displayMode->setCurrentIndex(0);
                const auto fullResolutionStatus = pointCloudText("%1×%2 %3 · %4 / %5 points")
                    .arg(profile.outputWidth).arg(profile.outputHeight).arg(pointCloudText("assisted depth"))
                    .arg(pixels).arg(pixels);
                check("POINT_CLOUD_ASSISTED_FULL_RESOLUTION", waitForCount(pixels) && instances->count() == 0
                    && triangleCount() == 0 && !meshVisible() && diameter->isEnabled()
                    && status && status->text() == fullResolutionStatus);
                check("POINT_CLOUD_ASSISTED_POINTS_MATCH_MESH_JET", depthInstances->instanceColor(0).rgb() == meshJet
                    && framebufferHasColor(meshJet));
                colorMode->setCurrentIndex(1);
                displayMode->setCurrentIndex(1);
                check("POINT_CLOUD_DYNAMIC_ASSISTED_MESH_RECOLORS_WITHOUT_CHANGING_TRIANGLES",
                    waitForTriangles(fullMeshTriangles) && meshVisible() && meshColor(0) == qRgb(127, 0, 0)
                    && meshPosition(0).z() == -1000.f);
                colorMode->setCurrentIndex(0);
                displayMode->setCurrentIndex(0);
                waitForCount(pixels);

                auto pairedMetadata=metadata;
                pairedMetadata["frame_id"]=123;
                pairedMetadata["assist_diagnostic_payload"]="mono_reasons_v1";
                QByteArray mono(profile.depthBytes()+2*RvDepthDiagnostic::ReasonBytes,0);
                for(int i=0;i<pixels;++i)qToLittleEndian<quint32>(0x3f000000,mono.data()+i*4);
                auto *monoButton=window.depth()->findChild<QPushButton *>("depthMonocularOnly");
                auto *splitButton=window.depth()->findChild<QPushButton *>("depthSplitCorrection");
                auto *assistCanvas=static_cast<RvDepthView *>(window.depth()->findChild<QWidget *>("rvAssistDepthCanvas"));
                emit session->assistDepthSnapshotReady(depth,pairedMetadata,{},RvTofFrame{},mono);
                settle(45);
                const auto correctedImage=window.depth()->snapshot().depthImage;
                if(monoButton)monoButton->click();
                const auto waitForZ=[&](float z) {
                    QElapsedTimer timer;timer.start();
                    while((count()!=pixels||depthInstances->instancePosition(0).z()!=z)&&timer.elapsed()<2500)
                        settle(10);
                    return count()==pixels&&depthInstances->instancePosition(0).z()==z;
                };
                check("MONOCULAR_SWITCH_AFFECTS_DEPTH_VIEW_AND_CLOUD_SAME_FRAME",monoButton&&monoButton->isChecked()
                    &&waitForZ(-500)&&assistCanvas&&assistCanvas->property("frameId").toInt()==123
                    &&!correctedImage.isNull()&&assistCanvas->colorAt(0,0)!=correctedImage.pixel(0,0)
                    &&status&&status->text().contains(pointCloudText("monocular depth"))&&splitButton&&!splitButton->isEnabled());
                check("MONOCULAR_PREVIEW_SAVES_CORRECTED_METRES_AND_IMAGE",
                    window.depth()->snapshot().metresFloat32LE==depth&&window.depth()->snapshot().depthImage==correctedImage);
                displayMode->setCurrentIndex(1);
                check("MONOCULAR_MESH_USES_SAME_UNADJUSTED_Z",waitForTriangles(fullMeshTriangles)&&meshPosition(0).z()==-500.f);
                if(monoButton)monoButton->click();
                displayMode->setCurrentIndex(0);
                check("MONOCULAR_SWITCH_RESTORES_SAME_CORRECTED_FRAME",monoButton&&!monoButton->isChecked()
                    &&waitForZ(-1000)&&assistCanvas&&assistCanvas->colorAt(0,0)==correctedImage.pixel(0,0));

                check("G_SPLIT_BUTTON_IS_OPT_IN",splitButton&&!splitButton->isHidden()&&splitButton->isEnabled()
                    &&!splitButton->isChecked()&&!session->splitCorrectionEnabled());
                if(splitButton)splitButton->click();
                check("G_SPLIT_BUTTON_INVALIDATES_BOTH_OLD_DISPLAYS",splitButton&&splitButton->isChecked()
                    &&session->splitCorrectionEnabled()&&waitForCount(0)&&!assistCanvas->hasFrame());
                auto splitMetadata=pairedMetadata;
                splitMetadata["frame_id"]=124;
                splitMetadata[RvAssistFusion::SplitCorrectionField]=true;
                QByteArray splitDepth(profile.depthBytes(),0);
                for(int i=0;i<pixels;++i)qToLittleEndian<quint32>(0x3f400000,splitDepth.data()+i*4);
                emit session->assistDepthSnapshotReady(splitDepth,splitMetadata,{},RvTofFrame{},mono);
                check("G_SPLIT_SHOWS_SAME_CORRECTED_METRES_IN_DEPTH_AND_CLOUD",waitForZ(-750)
                    &&assistCanvas->property("frameId").toInt()==124&&window.depth()->snapshot().metresFloat32LE==splitDepth);
                if(splitButton)splitButton->click();
                pairedMetadata["frame_id"]=125;
                emit session->assistDepthSnapshotReady(depth,pairedMetadata,{},RvTofFrame{},mono);
                check("G_SPLIT_SWITCH_BACK_RESTORES_ORIGINAL_G",splitButton&&!splitButton->isChecked()
                    &&!session->splitCorrectionEnabled()&&waitForZ(-1000)&&window.depth()->snapshot().metresFloat32LE==depth);

                dock->setFloating(true);
                settle(100);
                check("POINT_CLOUD_ASSISTED_FLOAT_PRESERVES_GEOMETRY_AND_ZOOM", waitForCount(pixels)
                    && cameraDistance() == manualDistance && view->rootObject()->property("orbitYaw").toInt() == 42
                    && view->rootObject()->property("depthPointDiameter").toDouble() == 4.2);
                dock->setFloating(false);
                dock->raise();
                settle(60);
                if (reset)
                    reset->click();
                check("POINT_CLOUD_RESET_VIEW", view->rootObject()
                    && view->rootObject()->property("orbitYaw").toInt() == -25 && checkFixedView()
                    && diameter->value() == 4.2 && colorMaximum->value() == 2.0);

                displayMode->setCurrentIndex(1);
                const bool meshBeforeClear = waitForTriangles(fullMeshTriangles);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                emit session->assistDepthStreamCleared("Expired");
                settle(75);
                check("POINT_CLOUD_ASSISTED_CLEAR_INVALIDATES_CONVERSION", meshBeforeClear && count() == 0
                    && triangleCount() == 0 && !meshVisible());

                emit session->assistDepthSnapshotReady(depth, metadata, {});
                const bool runningMesh = waitForTriangles(fullMeshTriangles);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                emit session->assistDepthSnapshotReady(splitDepth, metadata, {});
                const bool captureWasStopped = session->userStopped();
                const double resumeDistance = cameraDistance();
                pause->click();
                const bool stoppedCloud = pause->isChecked() && view->isHidden() && status
                    && status->text() == pointCloudText("Point cloud stopped")
                    && pause->text() == pointCloudText("Resume point cloud");
                auto pausedMetadata = metadata;
                pausedMetadata["frame_id"] = 126;
                emit session->assistDepthSnapshotReady(splitDepth, pausedMetadata, {});
                QImage video(64, 36, QImage::Format_RGB32);
                video.fill(Qt::yellow);
                emit session->cameraFrameReady(video);
                dock->hide();
                dock->show();
                dock->raise();
                settle(150);
                const auto *cameraPage = window.findChild<RtspCameraPage *>();
                check("POINT_CLOUD_STOP_DISCARDS_ASSISTED_WORK", runningMesh && stoppedCloud
                    && pause->isChecked() && view->isHidden() && count() == 0 && triangleCount() == 0 && !meshVisible());
                check("POINT_CLOUD_STOP_KEEPS_SHARED_DEPTH_AND_VIDEO", assistCanvas && cameraPage
                    && assistCanvas->property("frameId").toInt() == 126 && window.depth()->snapshot().metresFloat32LE == splitDepth
                    && cameraPage->currentFrameImage() == video && session->currentImagingScheme() == visibleHostScheme
                    && session->userStopped() == captureWasStopped);
                pause->click();
                settle(150);
                check("POINT_CLOUD_RESUME_ASSISTED_WAITS_FOR_FRESH_FRAME", !pause->isChecked() && view->isVisible()
                    && triangleCount() == 0 && count() == 0 && cameraDistance() == resumeDistance);
                displayMode->setCurrentIndex(0);
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                const bool converted = waitForCount(pixels);
                check("POINT_CLOUD_RESUME_ASSISTED_PROJECTS_NEW_FRAME", converted
                    && depthInstances->instancePosition(0).z() == -1000.f);
                session->selectImagingScheme("board_native_tof48", {});
                emit session->assistDepthSnapshotReady(depth, metadata, {});
                settle(75);
                check("POINT_CLOUD_NATIVE_MODE_CLEARS_AND_IGNORES_ASSISTED_DEPTH", converted && waitForCount(0));
                check("POINT_CLOUD_SCHEME_SWITCH_KEEPS_SHARED_JET_RANGE", colorMinimum->isVisible()
                    && colorMinimum->value() == .2 && colorMaximum->value() == 2.0 && !applyRange->isEnabled());
                session->selectImagingScheme(visibleHostScheme, {});
                emit session->streamCleared();
            }
            window.resetLayout();
        }
    }
    for(const auto* retired:{"board_litesr_near_384x256_host_contour_hd","board_litesr_near_384x256_high_accuracy_host_contour_hd","board_litesr_near_full_host_contour_hd","board_litesr_near_128x128_depth_only","board_litesr_384x256_contour_relative","board_tiny_nv12_128x80","unknown"}){
        check(QString("RETIRED_SCHEME_ABSENT_")+retired,models->findData(retired)==-1);
        session->selectImagingScheme(retired,{});
        check(QString("RETIRED_SCHEME_API_REJECTED_")+retired,session->currentImagingScheme()==visibleHostScheme&&session->userStopped());
        QSettings().setValue("depth/imagingScheme",retired);
        QSettings().setValue("depth/boardLiteSrHostHoleMode",2);
        RvSessionController reopened;
        check(QString("RETIRED_SCHEME_MIGRATES_TO_G_")+retired,reopened.currentImagingScheme()==visibleHostScheme&&reopened.userStopped()&&reopened.assistDepthEnabled()&&reopened.dtofCandidateMode()==RvDtofCandidates::Native48&&!QSettings().contains("depth/boardLiteSrHostHoleMode"));
    }
    for(const auto *scheme:{"board_native_tof48",RvSessionController::Native48ExtendedIntegrationScheme,
                            "board_native_tof48_high_accuracy",RvSessionController::Native48HighAccuracy1200KScheme}){
        const QString schemeId=QLatin1String(scheme);
        const int expectedMode=schemeId.contains("high_accuracy")?RvDtofCandidates::Native48HighAccuracy:RvDtofCandidates::Native48;
        const bool expected1200K=schemeId.endsWith("_1200k");
        session->selectImagingScheme(scheme,{});
        check(QString("NATIVE_MODE_SELECTS_WHILE_STOPPED_")+scheme,session->userStopped()&&!session->busy()&&!session->assistDepthEnabled()
            &&session->currentImagingScheme()==scheme&&models->currentData()==scheme
            &&session->dtofCandidateMode()==expectedMode&&session->native48ExtendedIntegration()==expected1200K
            &&window.depth()->selectedWidth()==48&&window.depth()->selectedHeight()==32);
        RvSessionController reopened;
        check(QString("NATIVE_MODE_PERSISTS_")+scheme,reopened.currentImagingScheme()==scheme&&!reopened.assistDepthEnabled()&&reopened.userStopped()
            &&reopened.dtofCandidateMode()==expectedMode&&reopened.native48ExtendedIntegration()==expected1200K);
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
                    {"depth_K",RvCameraView::intrinsics(profile)}, {"rectify_map_sha256",profile.mapSha},
                    {"assist_output_restored_to_calibrated_grid",true},{"width", profile.outputWidth}, {"height", profile.outputHeight}, {"depth_unit", "camera-Z_m_float32"},
                    {"assist_output_grid", RvCameraView::PortraitGrid}, {"assist_model_depth_size", QJsonArray{640,480}},
                    {"assist_model_crop_xywh", QJsonArray{140,0,360,480}}};
                session->m_currentMetadata = metadata;
                QByteArray depth(profile.depthBytes(), '\0');
                qToLittleEndian<quint32>(0x3f800000, depth.data());
                emit session->m_assist->resultReady(depth, metadata, camera, epoch-1, {}, {}, camera);
                check("AUTO_CONNECT_WAITS_FOR_CURRENT_DEPTH",session->automaticConnecting() && overlay->isVisible());
                overlay->grab().save(path+".auto-connect.png");
                emit session->m_assist->resultReady(depth, metadata, camera, epoch, {}, {}, camera);
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
    session->selectImagingScheme(reuseHostScheme,{});
    const auto *gSplitButton=window.depth()->findChild<QPushButton *>("depthSplitCorrection");
    check("G_SPLIT_BUTTON_HIDDEN_IN_B",gSplitButton&&gSplitButton->isHidden()&&!session->splitCorrectionEnabled());
    check("B_SCHEME_SWITCHES_ON_SAME_NATIVE48_AND_PORTRAIT_GRID",session->currentImagingScheme()==reuseHostScheme
        &&session->assistExperiment()=="B"&&session->dtofCandidateMode()==RvDtofCandidates::Native48
        &&window.depth()->selectedWidth()==480&&window.depth()->selectedHeight()==640);
    {
        RvSessionController reopened;
        check("B_SCHEME_PERSISTS",reopened.currentImagingScheme()==reuseHostScheme&&reopened.assistExperiment()=="B");
        constexpr qint64 rgbNs=20000000000LL;
        const QString boot="12345678-1234-1234-1234-123456789abc";
        const QString generation(32,'a');
        const auto frameAt=[&](int deltaMs,int sequence,int warnings=0){
            RvTofFrame frame;
            frame.samples=QByteArray(48*32*4,0);
            frame.metadata={{"protocol","LSG2"},{"work_mode","dtof_candidate"},{"candidate_mode",24},
                {"engine","native-hd-capture-v1"},{"model_sha256",""},{"capture_only",true},{"depth_present",false},{"photon_enabled",false},
                {"frame_id",sequence},{"tof_sequence",sequence},{"tof_timestamp_ns",rgbNs+deltaMs*1000000LL},
                {"tof_reused",false},{"generation",generation},{"boot_id",boot},
                {"dtof_width",48},{"dtof_height",32},{"dtof_bytes",frame.samples.size()},
                {"dtof_format","range_uq2_u16le_snr_u8_valid_u8"},{"dtof_range_unit","0.25_mm"},
                {"dtof_coordinate","sensor_radial_native"},{"dtof_source","tmf8829_ipc_first_peak"},
                {"dtof_fov_correction",reopened.deviceProfile().fov()},{"tof_warning_flags",warnings}};
            return frame;
        };
        const std::deque<RvTofFrame> measurements{frameAt(80,3),frameAt(10,1),frameAt(166,4),frameAt(160,5,16)};
        const auto selectedSequence=[&]{
            reopened.m_dtofToken=generation;
            reopened.m_dtofReceipt={{"boot_id",boot}};
            reopened.m_hdBoardSampleNs=rgbNs+150000000;
            reopened.m_hdSampleReceivedNs=rgbNs;
            reopened.m_hdNativeFrames=measurements;
            const auto *selected=reopened.pairedHostTof(rgbNs,rgbNs);
            return selected?selected->metadata["tof_sequence"].toInt():0;
        };
        check("B_PAIRS_LATEST_ELIGIBLE_TOF_BY_CAPTURE_TIME",selectedSequence()==3);
        reopened.selectImagingScheme(visibleHostScheme,{});
        check("G_PAIRS_CLOSEST_ELIGIBLE_TOF_TO_RGB",reopened.assistExperiment()=="G"&&selectedSequence()==1);
    }
    session->selectImagingScheme(visibleHostScheme,{});
    check("G_SCHEME_RESTORES_FRESH_ONLY",session->currentImagingScheme()==visibleHostScheme&&session->assistExperiment()=="G");
    for(const auto *id:{"unidepth","unidepth_wide","unidepth_wide_576","unidepth_wide_576_fp16","unidepth_wide_576_fp16_tof16","colorsr_rgb","colorsr_nv12","depthor_v3_fp32","depthor_v3_fp16","depthor_v3_int8"})
        check(QString("REMOVED_MODEL_REJECTED_")+id,!RvAssistDepth::supportsHostModel(id)&&RuntimeDiscovery::rvAssistModelRoot(id).isEmpty());
    const auto original=session->dtofCandidateMode();
    session->selectImagingScheme("host_depthor_v3_fp32",{});
    session->selectImagingScheme("board_litesr_near_high_resolution",{});
    check("REMOVED_SCHEME_API_REJECTED",session->dtofCandidateMode()==original&&session->assistHostModel()==visibleHostModel);
    check("REMOVED_BOARD_MODE_REJECTED",!session->setDtofCandidate(15,{}));
    check("FP16_SELECTION_STOPPED",session->dtofCandidateMode()==RvDtofCandidates::Native48&&session->userStopped()&&session->assistDepthEnabled()&&session->assistExperiment()=="G");
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
    auto *pointCloudDock = window.findChild<QDockWidget *>("pointCloudDock");
    check("CHINESE_POINT_CLOUD_MENU", pointCloudDock && pointCloudDock->toggleViewAction()->text() == QStringLiteral("点云"));
    auto *colorMode = pointCloudDock ? pointCloudDock->findChild<QComboBox *>("rvPointCloudColorMode") : nullptr;
    check("CHINESE_POINT_CLOUD_COLOR_MODES", colorMode && colorMode->itemText(0) == QStringLiteral("固定 Z 色标")
        && colorMode->itemText(1) == QStringLiteral("动态＋直方图均衡"));
    auto *depthColorMode = window.depth()->findChild<QComboBox *>("rvDepthColorMode");
    check("CHINESE_DEPTH_COLOR_MODES_AND_FIXED_DEFAULT", depthColorMode && depthColorMode->count() == 2
        && depthColorMode->currentIndex() == 0 && depthColorMode->itemText(0) == QStringLiteral("固定色标")
        && depthColorMode->itemText(1) == QStringLiteral("动态＋直方图均衡"));
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
    check("CHINESE_IMAGING_SCHEMES",models&&models->count()==6+researchModes&&models->itemText(0)==QString::fromUtf8("上位机辅助推理（G）")
        &&models->itemText(1)==QString::fromUtf8("上位机辅助推理（B，复用 ToF）")
        &&(!depthPro2Supported||(models->itemText(2)==QString::fromUtf8("Depth Pro 2-S（ToF G）")
            &&models->itemText(3)==QString::fromUtf8("Depth Pro 2-S（ToF B，复用）")))
        &&(!depthPro2BSupported||(models->itemText(depthPro2BIndex)==QString::fromUtf8("Depth Pro 2-B（ToF G）")
            &&models->itemText(depthPro2BIndex+1)==QString::fromUtf8("Depth Pro 2-B（ToF B，复用）")))
        &&models->itemText(3+researchModes)==QString::fromUtf8("板端推理（1200K）")
        &&models->itemText(5+researchModes)==QString::fromUtf8("板端推理高精度（1200K）"));
    check("CHINESE_SHARED_CONTROL_LABELS",controlsPresent&&depthStart->text()==cameraStart->text()
        &&depthStop->text()==cameraStop->text()&&depthReconnect->text()==cameraReconnect->text());
    check("CHINESE_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()==QString::fromUtf8("选择深度图和原视频共享会话的成像方案。"));
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
    check("BLACK_OUTSIDE_FOLLOWS_SAVE_IN_DEPTH_FOOTER",blackOutside&&blackOutside->isVisible()&&depthSave
        && blackOutside->parentWidget()==depthSave->parentWidget()
        && (blackOutside->x()>depthSave->x()||blackOutside->y()>depthSave->geometry().bottom())
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
        if (depthColorMode && colorMode && native) {
            const int previousPointMode = colorMode->currentIndex();
            const auto previousPointSetting = QSettings().value("pointCloud/colorMode");
            const auto *apply = window.depth()->findChild<QPushButton *>("rvColorRangeApply");
            depthColorMode->setCurrentIndex(1);
            QElapsedTimer colorWait;
            colorWait.start();
            while (!native->hasRenderableFrame() && colorWait.elapsed() < 2000) settle(20);
            check("DEPTH_DYNAMIC_RECOLORS_CURRENT_NATIVE_FRAME", native->hasRenderableFrame()
                && native->colorAt(47, 31) == PointCloudColorScale::jet(1.)
                && native->colorAt(47, 31) != highSnapshot.depthImage.pixel(47, 31));
            check("DEPTH_DYNAMIC_HIDES_RANGE_CONTROLS", apply && apply->isHidden()
                && blackOutside && blackOutside->isHidden());
            check("DEPTH_COLOR_MODE_DOES_NOT_CHANGE_POINT_CLOUD", colorMode->currentIndex() == previousPointMode
                && QSettings().value("pointCloud/colorMode") == previousPointSetting);
            colorMode->setCurrentIndex(1);
            colorMode->setCurrentIndex(0);
            check("POINT_CLOUD_COLOR_MODE_DOES_NOT_CHANGE_DEPTH", depthColorMode->currentIndex() == 1
                && QSettings().value("depth/colorMode").toString() == "dynamic"
                && native->colorAt(47, 31) == PointCloudColorScale::jet(1.));
            colorMode->setCurrentIndex(previousPointMode);
            {
                DepthMapWidget reopened;
                const auto *reopenedMode = reopened.findChild<QComboBox *>("rvDepthColorMode");
                check("DEPTH_COLOR_MODE_PERSISTS_ON_REOPEN", reopenedMode && reopenedMode->currentIndex() == 1);
            }
            const auto saved = window.depth()->snapshot();
            check("DYNAMIC_NATIVE_DEPTH_SAVES_FIXED_PALETTE_AND_METRES", saved.metresFloat32LE == raw
                && !saved.depthImage.isNull() && saved.depthImage == highSnapshot.depthImage);
            depthColorMode->setCurrentIndex(0);
            check("DEPTH_FIXED_RESTORES_RANGE_CONTROLS_AND_COLORS", apply && !apply->isHidden()
                && blackOutside && !blackOutside->isHidden() && native->presentationImage() == highSnapshot.depthImage);
        }
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
            {"assist_host_model",model},{"assist_experiment","G"},{"assist_input_orientation_checked",true},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},{"assist_output_grid",RvCameraView::PortraitGrid},{"assist_model_depth_size",QJsonArray{640,480}},{"assist_model_crop_xywh",QJsonArray{140,0,360,480}},
            {"assist_input_rotation_degrees",0},{"dtof_fov_correction",1},{"rectify_map_sha256",profile.mapSha},
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
        if (depthColorMode) {
            const auto fixed = window.depth()->snapshot();
            depthColorMode->setCurrentIndex(1);
            check("DEPTH_DYNAMIC_HOST_EQUALIZES_DISPLAYED_SAMPLES", canvas->colorAt(0, 0)
                == PointCloudColorScale::jet(std::pow(.5, .65))
                && canvas->colorAt(width - 1, 0) == PointCloudColorScale::jet(1.));
            const auto saved = window.depth()->snapshot();
            check("DYNAMIC_HOST_DEPTH_SAVES_MASKED_FIXED_IMAGE_AND_METRES", saved.metresFloat32LE == metres
                && !saved.depthImage.isNull() && saved.depthImage == fixed.depthImage);
            window.depth()->acceptAssistSnapshot(metres, metadata, {});
            check("DEPTH_DYNAMIC_HOST_KEEPS_INVALID_DEPTH_BLACK", canvas->colorAt(0, 0) == qRgb(0, 0, 0)
                && canvas->colorAt(width - 1, 0) == PointCloudColorScale::jet(1.));
            depthColorMode->setCurrentIndex(0);
        }
        window.depth()->clearAssistDepth({});
    } else {
        check("OUTSIDE_FIELD_HOVER_ONE_LINE",false);
    }
    if(vgaSupported){
        const auto model=QString::fromLatin1(RvCameraView::UniDepthVgaNative48);
        const auto &vga=RvCameraView::outputProfile(model);
        session->selectImagingScheme(visibleHostScheme,{});
        check("VGA_STOPPED_SELECTION",session->userStopped()&&session->assistHostModel()==model
            &&models->currentData()==visibleHostScheme&&window.depth()->selectedWidth()==480&&window.depth()->selectedHeight()==640);
        RvSessionController reopened;
        check("VGA_SELECTION_PERSISTS",reopened.assistHostModel()==model&&reopened.assistExperiment()=="G");
        QByteArray metres(vga.depthBytes(),0),diagnostic(vga.depthBytes()+2*RvDepthDiagnostic::ReasonBytes,0);
        for(int i=0;i<vga.depthBytes()/4;++i){
            if(i%480>=240)qToLittleEndian<quint32>(0x40000000,metres.data()+i*4);
            qToLittleEndian<quint32>(0x3fc00000,diagnostic.data()+i*4);
        }
        const auto policy=QString::fromLatin1(RvAssistFusion::FieldOfViewPolicy);
        const QJsonObject fit{{"fusion_mode","global_scale"},{"scale",1},{"local_factor_limit",1},
            {"output_policy",policy},{"fov_geometry_only",true},{"fov_pixels",640*480/2}};
        const QJsonObject metadata{{"width",480},{"height",640},{"depth_unit","camera-Z_m_float32"},
            {"assist_host_model",model},{"assist_experiment","G"},{"assist_input_orientation_checked",true},
            {"assist_diagnostic_capture",true},{"assist_diagnostic_payload","mono_reasons_v1"},
            {"assist_output_policy",policy},{"assist_camera_profile","wide"},{"assist_output_restored_to_calibrated_grid",true},{"assist_output_grid",RvCameraView::PortraitGrid},{"assist_model_depth_size",QJsonArray{640,480}},{"assist_model_crop_xywh",QJsonArray{140,0,360,480}},
            {"assist_input_rotation_degrees",0},{"dtof_fov_correction",1},{"rectify_map_sha256",vga.mapSha},
            {"assist_graph_sha256",vga.graphSha},{"depth_K",RvCameraView::intrinsics(vga)},{"assist_fusion",fit}};
        QImage rgb(480,640,QImage::Format_RGB888);rgb.fill(Qt::black);
        window.depth()->acceptAssistSnapshot(metres,metadata,rgb,RvTofFrame{},diagnostic);
        auto *canvas=static_cast<RvDepthView *>(window.depth()->findChild<QWidget *>("rvAssistDepthCanvas"));
        const auto snapshot=window.depth()->snapshot();
        check("VGA_METRIC_SNAPSHOT_AND_SIDE_DISPLAY",canvas&&canvas->depthAt(120,320)==0&&canvas->depthAt(360,320)==2
            &&canvas->property("sidePredictionActive").toBool()&&snapshot.metresFloat32LE==metres
            &&snapshot.cameraImage.size()==QSize(480,640)&&snapshot.depthImage.size()==QSize(480,640));
        if(canvas){
            const int rotation=canvas->displayRotationDegrees();
            canvas->setDisplayRotationDegrees(0);
            const auto rendered=canvas->grab().toImage();
            const auto area=canvas->property("presentationRect").toRectF();
            check("UNIDEPTH_DISPLAY_REMAINS_UNMIRRORED",!area.isEmpty()
                &&rendered.pixelColor((QPointF(area.left()+area.width()/4,area.center().y())*rendered.devicePixelRatio()).toPoint())==QColor(canvas->colorAt(120,320))
                &&rendered.pixelColor((QPointF(area.left()+area.width()*3/4,area.center().y())*rendered.devicePixelRatio()).toPoint())==QColor(canvas->colorAt(360,320))
                &&!snapshot.metadata.contains("native_tof_display_mirror_horizontal"));
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
    check("ENGLISH_IMAGING_SCHEMES",models&&models->count()==6+researchModes&&models->itemText(0)=="Host-assisted inference (G)"
        &&models->itemText(1)=="Host-assisted inference (B, reuse ToF)"
        &&(!depthPro2Supported||(models->itemText(2)=="Depth Pro 2-S (ToF G)"&&models->itemText(3)=="Depth Pro 2-S (ToF B, reuse)"))
        &&(!depthPro2BSupported||(models->itemText(depthPro2BIndex)=="Depth Pro 2-B (ToF G)"
            &&models->itemText(depthPro2BIndex+1)=="Depth Pro 2-B (ToF B, reuse)"))
        &&models->itemText(3+researchModes)=="Board inference (1200K)"
        &&models->itemText(5+researchModes)=="Board inference high accuracy (1200K)");
    check("ENGLISH_IMAGING_SCHEME_TOOLTIP",models&&models->toolTip()=="Select the imaging scheme for the shared depth and camera session.");
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
        auto *maintain=devices?devices->findChild<QPushButton *>("rvDeviceMaintainCapture"):nullptr;
        if(maintain) maintain->click();
        check("BOARD_MAINTENANCE_OFFLINE_GUARD",maintain&&status
            &&status->text()=="Board file maintenance is unavailable in offline mode."&&session->userStopped());
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
