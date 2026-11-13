#pragma once
#include "../sdk/DepthSdkExports.h"
#include "RvTofFrame.h"
#include <QtCore/QJsonArray>
#include <QtCore/QPointF>
#include <QtCore/QSize>
#include <array>
#include <vector>

namespace RvCameraView {struct Profile;}

namespace RvAssistFusion {
enum PointReason : char {NoMeasurement=0, Invalid=1, Quality=2, Outside=3, Boundary=4,
    GlobalOutlier=5, Duplicate=6, GlobalRejected=7, LocalConflict=8, Adopted=9,
    WeakLocalGroup=10, AlreadySupported=11};
inline constexpr char FieldOfViewPolicy[] = "dtof_geometric_fov_v1";
// Native radial admission only, independent of camera-Z and display colour range.
inline constexpr double MinimumRadialMetres = .01;
struct Calibration {
    int columns=48,rows=32;
    int nativeCount() const {return columns*rows;}
    std::array<double,3> translation{};
    std::array<std::array<double,3>,48*32> cameraRays{};
    bool valid = false;
};
struct Result {
    QByteArray depth;
    QSize size;
    QJsonObject diagnostics;
    QString error;
    QByteArray globalReasons,localReasons; // One reason per native ID; memory only.
    bool ok() const { return error.isEmpty() && !depth.isEmpty(); }
};
// Frozen GEO2 rays already include the native sensor mapping and camera rotation.
DCTS_DEPTH_INTERNAL const Calibration &calibrationForFov(int fov);
DCTS_DEPTH_INTERNAL const Calibration &calibrationForNativeGrid(int fov,int columns,int rows);
DCTS_DEPTH_INTERNAL bool loadCalibration(const QByteArray &bytes, Calibration &out, QString &error);
struct NativeProjection {
    QPointF pixel;
    double cameraZ;
};
// Uses the calibrated camera grid and integer-centred pixel coordinates.
DCTS_DEPTH_INTERNAL NativeProjection projectNativePoint(double radius,int nativeId,const Calibration &calibration,
                                    const RvCameraView::Profile &view);
using NativeProjector=NativeProjection (*)(double,int,const Calibration &,const RvCameraView::Profile &);
inline constexpr char NativeProjectionField[]="assist_native_projection";
inline constexpr char CloseNative48Projection[]="f6_close_range_offset_48_v1";
DCTS_DEPTH_INTERNAL NativeProjection projectCloseNative48(double radius,int nativeId,const Calibration &calibration,
                                      const RvCameraView::Profile &view);
// A live paired host/F6 frame selects the correction. An absent field in a
// saved frame denotes the calibrated projection used at the time of capture.
DCTS_DEPTH_INTERNAL QJsonValue defaultNativeProjection(const RvTofFrame &tof);
DCTS_DEPTH_INTERNAL NativeProjector nativeProjector(const RvTofFrame &tof);
// An explicit RGB-only estimate has no fabricated native measurement identity.
DCTS_DEPTH_INTERNAL Result monocular(const std::vector<float> &monocularZ, const char *reason,QSize size);
// Retained consensus is rechecked after outlier rejection.
DCTS_DEPTH_INTERNAL Result fuseGlobalScale(const std::vector<float> &monocularZ,const RvTofFrame &tof,
                       const Calibration &calibration,const RvCameraView::Profile &view);
// Scalar agreement diagnostics against this measurement, not independent GT.
// Does not retain/export images, tensors, or individual measured distances.
DCTS_DEPTH_INTERNAL QJsonObject agreement(const std::vector<float> &monocularZ,const Result &output,
                      const RvTofFrame &tof,const Calibration &calibration,const RvCameraView::Profile &view,
                      NativeProjector project=projectNativePoint);
// Geometric outer field only. Uses the calibrated sensor cone and current Z
// for parallax; validity/SNR/selected anchors never carve holes in the image.
DCTS_DEPTH_INTERNAL Result limitTofFieldOfView(Result output,const Calibration &calibration,const RvCameraView::Profile &view);
inline constexpr char ReuseRgbRevision[]="global_scale_reuse_latest_rgb_portrait_v1";
inline bool isReuseRgbRevision(const QJsonObject &metadata) {
    return RvTofFrame::isHostFusionExperiment(metadata["assist_experiment"])
        &&metadata["assist_fusion_revision"]==QLatin1String(ReuseRgbRevision);
}

// Retain the last applied scalar and its reference identity for the session.
class DCTS_DEPTH_INTERNAL GlobalRgbContinuity {
public:
    Result apply(Result result,qint64 nowMs,qint64 epoch,qint64 generation,qint64 frameId);
    void reset() { *this=GlobalRgbContinuity{}; }
private:
    bool initialized=false, hasReference=false;
    qint64 lastMs=0, referenceMs=0, referenceFrame=0, boundEpoch=0, boundGeneration=0;
    double logApplied=0;
};

DCTS_DEPTH_INTERNAL QString pointReasonName(char reason);
}
