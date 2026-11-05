#pragma once
#include "RvTofFrame.h"
#include <QtGui/QImage>
#include <array>
#include <vector>

namespace RvCameraView {struct Profile;}

namespace RvAssistFusion {
enum PointReason : char {NoMeasurement=0, Invalid=1, Quality=2, Outside=3, Boundary=4,
    GlobalOutlier=5, Duplicate=6, GlobalRejected=7, LocalConflict=8, Adopted=9,
    WeakLocalGroup=10, AlreadySupported=11};
inline constexpr char SupportPolicy[] = "dtof_supported_mesh_v1";
inline constexpr char FieldOfViewPolicy[] = "dtof_geometric_fov_v1";
inline constexpr char RectifySha256[] = "00de89f4e6d95f09758b0d8274c2694168a27b5f601bdfeda667b0b179126b5d";
// Native radial admission only, independent of camera-Z and display colour range.
// Shared by all UniDepth schemes, diagnostics and adopted-point validation.
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
    QJsonObject diagnostics;
    QString error;
    QByteArray globalReasons,localReasons; // One reason per native ID; memory only.
    bool ok() const { return error.isEmpty() && !depth.isEmpty(); }
};
// Frozen GEO2 rays already include the native sensor mapping and camera rotation.
const Calibration &calibrationForFov(int fov);
// Only the independent RGB576 FP16 native16 route may use the 16x16 grid.
const Calibration &calibrationForNativeGrid(int fov,int columns,int rows);
// The same audited R*M transform evaluated at native M8 cell centres.
const std::array<std::array<double,3>,64> &native8RaysForFov(int fov);
bool loadCalibration(const QByteArray &bytes, Calibration &out, QString &error);
// An explicit RGB-only estimate has no fabricated native measurement identity.
Result monocular(const std::vector<float> &monocularZ, const char *reason,QSize size={576,324});
Result fuse(const std::vector<float> &monocularZ, const QImage &rectifiedCamera,
            const RvTofFrame &tof, const Calibration &calibration,bool wideView=false);
// Independent RGB-cadence experiment: the same robust scalar fit, without a
// local residual field. No per-pixel correction is inferred from historical ToF.
// V1 is retained only for recorded snapshot replay. V2 rechecks a retained
// consensus when the pre-filter MAD alone would reject an otherwise sound fit.
enum class GlobalScaleFit {LegacyV1, RetainedConsensusV2};
Result fuseGlobalScale(const std::vector<float> &monocularZ,const QImage &rectifiedCamera,
                       const RvTofFrame &tof,const Calibration &calibration,bool wideView=false,
                       GlobalScaleFit fit=GlobalScaleFit::RetainedConsensusV2);
// G consumes the calibrated grid dimensions, not RGB pixels. Callers that
// already validated planar RGB need not reconstruct an interleaved image.
Result fuseGlobalScale(const std::vector<float> &monocularZ,QSize rectifiedCameraSize,
                       const RvTofFrame &tof,const Calibration &calibration,bool wideView=false,
                       GlobalScaleFit fit=GlobalScaleFit::RetainedConsensusV2);
Result fuseGlobalScale(const std::vector<float> &monocularZ,const RvTofFrame &tof,
                       const Calibration &calibration,const RvCameraView::Profile &view,
                       GlobalScaleFit fit=GlobalScaleFit::RetainedConsensusV2);
// Scalar agreement diagnostics against this measurement, not independent GT.
// Does not retain/export images, tensors, or individual measured distances.
QJsonObject agreement(const std::vector<float> &monocularZ,const Result &output,
                      const RvTofFrame &tof,const Calibration &calibration,bool wideView=false);
// Output validity only: keep the full RGB/context and every retained depth bit.
// Adjacent adopted native points define conservative triangles. Never fill a
// convex hull across missing returns, depth discontinuities, or rejected zones.
Result limitTofSupport(Result output,const RvTofFrame &tof,const Calibration &calibration,bool wideView=false);
// Geometric outer field only. Uses the calibrated sensor cone and current Z
// for parallax; validity/SNR/selected anchors never carve holes in the image.
Result limitTofFieldOfView(Result output,const Calibration &calibration,bool wideView=false);
Result limitTofFieldOfView(Result output,const Calibration &calibration,const RvCameraView::Profile &view);
// One worker/session owns this state. No previous depth image is retained.
class ScaleContinuity {
public:
    Result apply(Result result, qint64 nowMs, qint64 epoch, qint64 generation, qint64 frameId);
    void reset() { *this=ScaleContinuity{}; }
private:
    bool initialized=false, hasReference=false;
    qint64 lastMs=0, referenceMs=0, referenceFrame=0, boundEpoch=0, boundGeneration=0;
    double logApplied=0;
};
// Opt-in experiment. Full calibrated grid, fixed current-frame adjacency; no
// history of local fields, pixel relocation, or support-dependent validity.
enum class RecoveryVersion { SurfaceV3 };
Result fuseResidual(const std::vector<float> &monocularZ,const QImage &camera,
                    const RvTofFrame &tof,const Calibration &calibration,bool wideView=false,
                    ScaleContinuity *continuity=nullptr,qint64 measurementMs=0,
                    qint64 epoch=0,qint64 generation=0,qint64 frameId=0,
                    const Result *recoveryBase=nullptr,
                    RecoveryVersion recoveryVersion=RecoveryVersion::SurfaceV3);
// B keeps the original result wherever it has support; only independent,
// coherent current native groups can recover unsupported local regions.
Result fuseRecovering(const std::vector<float> &monocularZ,const QImage &camera,
                      const RvTofFrame &tof,const Calibration &calibration,bool wideView=false,
                      ScaleContinuity *continuity=nullptr,qint64 measurementMs=0,
                      qint64 epoch=0,qint64 generation=0,qint64 frameId=0);
QString pointReasonName(char reason);
}
