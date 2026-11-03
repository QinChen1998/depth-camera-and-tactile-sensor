#include "RvControlProtocol.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

namespace rvcontrol {
static void require(bool ok, const QString &message, int code = 2) { if (!ok) throw Error(message, code); }
static bool model(const QString &value) { return value == "Native"; }
void validate(const Configuration &c) {
    require(!(c.recoverFault && c.recoverStreamFault), "Conflicting recovery modes");
    require(c.width == 144 || c.width == 576 || c.width == 2304, "Unsupported preview size");
    require(c.keepModel || model(c.model), "Unsupported model");
    const bool raw = c.depthWidth == 144 && c.location == "none" && c.method == "none";
    const bool enhanced = (c.depthWidth == 288 || c.depthWidth == 384 || c.depthWidth == 576)
        && (c.location == "rv1103" || c.location == "host")
        && (c.method == "bilinear_valid" || c.method == "fast_guided_guarded");
    require(raw && !enhanced, "Capture service does not produce inferred depth");
}
QString command(const Configuration &c) {
    validate(c);
    QString args = QStringLiteral("%1 %2 %3 %4").arg(c.width).arg(c.depthWidth).arg(c.location,c.method);
    if (!c.keepModel) args += ' ' + c.model;
    // The coordinator serializes capture and LiteSR ownership.
    const QString action = c.recoverFault ? "recover-legacy " : c.recoverStreamFault ? "auto-legacy " : "legacy ";
    const QString fallback = (c.recoverFault || c.recoverStreamFault) ? "exit 2" : "/root/r144_depth/control.sh configure " + args;
    return "cat /proc/sys/kernel/random/boot_id && if [ -x /root/dtof_candidates_20261009/control.sh ]; then /root/dtof_candidates_20261009/control.sh "
        + action + args + "; else " + fallback + "; fi";
}
qint64 counter(const QJsonValue &value, bool allowString) {
    if (allowString && value.isString()) {
        bool ok = false; auto result = value.toString().toLongLong(&ok);
        return ok && QRegularExpression("^[0-9]+$").match(value.toString()).hasMatch() && result > 0 ? result : -1;
    }
    const qint64 result = value.toInteger(-1);
    return value.isDouble() && result > 0 && value.toDouble() == double(result) ? result : -1;
}
bool rtspTelemetry(const QJsonObject &s, const QJsonObject &t) {
    for (const auto *key : {"tof_sequence", "tof_timestamp_ns", "pair_delta_ms", "depth_unit", "tof_reused"})
        if (t.contains(QLatin1String(key))) return false;
    return s["preview_format"] == "rtsp_h265" && s["rgb_width"].toInt() == 2304
        && t["transport"] == "rtsp" && t["pixel_format"] == "H265" && t["work_mode"] == "camera_only"
        && t["depth_present"].isBool() && !t["depth_present"].toBool()
        && t["rgb_width"].toInt() == 2304 && t["rgb_height"].toInt() == 1296
        && t["progress_source"] == "venc_info:chn0" && counter(t["frame_id"]) > 0
        && counter(t["frame_id"]) == counter(t["encoded_frame_id"])
        && counter(t["encoder_retrieved_frame_id"]) > 0;
}
Receipt receipt(const QByteArray &output, const Configuration &c) {
    require(output.size() <= 65536, "Oversized control receipt");
    const auto newline = output.indexOf('\n');
    const QString boot = QString::fromUtf8(output.left(newline)).trimmed();
    require(newline > 0 && QRegularExpression("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$").match(boot).hasMatch(), "Invalid board boot receipt");
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(output.mid(newline + 1), &error);
    const auto s = doc.object(); const auto selected = s["model"].toString();
    const auto generation = counter(s["generation"], true);
    require(error.error == QJsonParseError::NoError && doc.isObject() && model(selected)
        && (c.keepModel || selected == c.model) && generation > 0
        && (s["state"] == "starting" || s["state"] == "running"), "Board configuration receipt mismatch");
    return {boot, selected, generation};
}
bool Readiness::observe(const QJsonObject &status) {
    require(status["boot_id"].toString() == m_receipt.boot, "Board boot changed during selection", 4);
    const auto s = status["service"].toObject(), t = status["telemetry"].toObject();
    require(!QStringList{"fault", "cleanup_fault", "stopped"}.contains(s["state"].toString()), "Board lifecycle rejected selection; fault locks are preserved");
    require(counter(s["generation"], true) == m_receipt.generation, "Service generation changed during selection", 4);
    const auto now = counter(status["now_monotonic_ns"]), updated = counter(t["updated_monotonic_ns"]);
    if (s["state"] != "running" || s["model"] != m_receipt.model || t["model"] != m_receipt.model
        || counter(t["generation"], true) != m_receipt.generation || now < 0 || updated < 0
        || now < updated || now - updated >= 3000000000LL) {
        m_seen = false; return false;
    }
    const bool camera = m_config.width == 2304;
    require(camera ? rtspTelemetry(s, t) : t["pixel_format"] == "NV12", "Stream encoding confirmation mismatch");
    require(t["rgb_width"].toInt() == m_config.width && t["work_mode"] == (camera ? "camera_only" : "rgb_tof")
        && t["depth_present"].isBool() && !t["depth_present"].toBool()
        && (camera || (t["capture_only"] == true && t["inference_location"] == "none")),
        "Preview work mode confirmation mismatch");
    if (!camera) require(t["target_width"].toInt(144) == m_config.depthWidth
        && t["requested_location"].toString("none") == m_config.location
        && t["requested_method"].toString("none") == m_config.method, "Depth configuration confirmation mismatch");
    const auto frame = counter(t["frame_id"]), retrieved = camera ? counter(t["encoder_retrieved_frame_id"]) : 0;
    require(frame > 0 && (!camera || retrieved > 0), "Invalid frame progress counter");
    const bool advancing = m_seen && frame > m_frame && (!camera || retrieved > m_retrieved);
    m_frame = frame; m_retrieved = retrieved; m_seen = true;
    return advancing;
}
}
