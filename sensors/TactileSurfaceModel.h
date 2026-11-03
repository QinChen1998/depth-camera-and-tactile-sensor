#pragma once
#include "TactileFrame.h"
#include <QQuaternion>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QVector3D>
#include <array>

struct TactileInterpolatedPoint {
    QVector3D position;
    QVector<int> sources;
    QVector<double> weights;
};

// Offline mesh projection, in metres. The QML sensor root alone converts to mm.
// This is display geometry, not a calibrated sensor or a control/collision model.
struct TactileSurfaceModel {
    bool valid=false;
    QUrl mesh;
    QVector3D meshPosition, boundsCenter, boundsSize, resultantPosition;
    QQuaternion meshRotation;
    QVector<QVector3D> positions, normals;
    std::array<QVector<TactileInterpolatedPoint>,2> interpolation;
};
const TactileSurfaceModel &tactileSurfaceModel(int type);
// Readouts share the existing surface interpolation. Acquisition frames and
// history retain their original 20/16 measurements.
int tactileDisplayPointCount(int type,int gridSize);
tactile::Force sampleTactileDisplayPoint(const QVector<tactile::Force> &raw,int type,int gridSize,int point);
tactile::Force sampleTactileDisplayPoint(const QVector<tactile::Point> &raw,int type,int gridSize,int point);
