#pragma once
#include "Contract.h"
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
inline bool tinyNv12Receipt(const QJsonObject& m,const QString& token,const QString& options){
    using namespace TinyNv12Contract;
    return m["state"]=="running"&&m["protocol"]==Protocol&&m["candidate_mode"]==Mode
        &&m["generation"]==token&&QRegularExpression("^[0-9a-f]{32}$").match(token).hasMatch()
        &&options=="1,1"&&m["options"]==options
        &&QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(m["boot_id"].toString()).hasMatch()
        &&m["engine"]==Engine&&m["model_sha256"]==ModelSha&&m["checkpoint_sha256"]==CheckpointSha
        &&m["width"]==Width&&m["height"]==Height&&m["fresh_ms"]==1000&&m["port"]==8846
        &&m["native_columns"]==16&&m["native_rows"]==16&&m["calibration"]==Calibration
        &&m["camera_mount_rotation"]==180&&m["input_rotation_degrees"]==0
        &&m["video_transport"]=="nv12_paired"&&m["video_width"]==Width&&m["video_height"]==Height&&m["video_fps"]==30
        &&m["crop_raw_xywh"].toArray()==QJsonArray{560,0,1248,1296}&&m["postprocess"]==Postprocess
        &&m["depth_unit"]=="camera-Z_m_float32"&&m["pair_limit_ms"]==50&&m["rgb_used"]==true
        &&m["rgb_paused"]==false&&m["inference_location"]=="rv1103";
}
