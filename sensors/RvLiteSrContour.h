#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>

// Stateless presentation refinement of the restored area_full prediction.
// Native measurements identify repair regions only. Interpolated values come
// from this frame's restored LiteSR prediction, never from RGB or history.
namespace RvLiteSrContour {
struct RepairSettings {
    int holeMode=1; // 0 off, 1 <=4 native cells, 2 <=64 native cells.
    bool removeOutliers=true;
    bool operator==(const RepairSettings &other) const {return holeMode==other.holeMode&&removeOutliers==other.removeOutliers;}
};
struct Result { QByteArray depth,coverage; QJsonObject diagnostic; };
// Original contour-only function retained for reproducible comparison.
Result refine(const QByteArray &areaFullDepth);
Result refine(const QByteArray &areaFullDepth,const QByteArray &nativeSamples,
              int inputRotation,const RepairSettings &settings);
// Board INT8 output includes separately restored positive returns below 10 cm.
// Reuse the board's spatial algorithm on the PC without changing that policy.
Result refineBoard(const QByteArray &areaFullDepth,const QByteArray &nativeSamples,
                   const RepairSettings &settings);
}
