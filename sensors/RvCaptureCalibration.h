#pragma once
#include <QtCore/QJsonObject>

namespace RvCaptureCalibration {
inline bool valid(const QJsonObject &o,const QString &generation,int expected,bool allowTemporary)
{
    const int sensor=o["sensor_fov_correction"].toInt(-1),processing=o["processing_fov_correction"].toInt(-1);
    return o.size()==5 && o["schema"]==1 && o["run_id"]=="dtof_"+generation
        && sensor>=0 && sensor<=15 && o["sensor_fov_correction"].toDouble(-1)==sensor
        && processing==expected && o["processing_fov_correction"].toDouble(-1)==processing
        && o["temporary_calibration"].isBool() && o["temporary_calibration"].toBool()==(sensor!=processing)
        && (allowTemporary || sensor==processing);
}
inline void annotate(QJsonObject &metadata,const QJsonObject &receipt)
{
    const auto c=receipt["tof_calibration"].toObject();
    if(c.isEmpty())return;
    metadata["tof_calibration"]=c;
    metadata["sensor_fov_correction"]=c["sensor_fov_correction"];
    metadata["processing_fov_correction"]=c["processing_fov_correction"];
    metadata["temporary_fov_calibration"]=c["temporary_calibration"];
}
}
