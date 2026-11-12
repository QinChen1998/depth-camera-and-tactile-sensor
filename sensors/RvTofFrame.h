#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaType>
#include <QtCore/QtEndian>
#include <QtCore/QRegularExpression>
#include <cmath>
#include <cstring>
#include "../tools/rv_capture/Contract.h"

namespace RvCaptureContract {
inline bool boardImagingIdentity(const QJsonObject& m){
    const int mode=m["candidate_mode"].toInt(-1);
    const bool liteSr=isLiteSr(mode);
    return supports(mode)&&m["engine"]==engine(mode)&&m["model_sha256"]==modelSha(mode)
        &&m["capture_only"]==!liteSr&&m["inference_location"]==(liteSr?"board":"none")
        &&m["depth_present"]==liteSr&&m["photon_enabled"]==liteSr
        &&m["width"]==Width&&m["height"]==Height&&m["fresh_ms"]==FreshMs
        &&(!liteSr||(m["checkpoint_sha256"]==LiteSrCheckpointSha&&m["training_minimum_m"]==0.01
            &&m["input_rotation_degrees"]==0&&m["intensity_white_counts"]==LiteSrWhiteCounts));
}
}

// Native sensor grid, before projection, anchors, network and depth enhancement.
// One cell: range_uq2 uint16 LE (0.25 mm), first-peak SNR uint8,
// valid uint8 (0 or 1), copied from an unchanged native IPC measurement.
// Native radial range, not camera-Z, a histogram, or all multiple peaks.
struct RvTofFrame {
    QByteArray samples;
    QJsonObject metadata;
    int columns() const { return metadata["dtof_width"].toInt(); }
    int rows() const { return metadata["dtof_height"].toInt(); }
    int sampleCount() const { return samples.size()/4; }
    bool isValid() const { return validMeasurement(false); }
private:
    bool validMeasurement(bool allowReuse) const {
        const auto integer = [this](const char *key, double minimum) {
            const auto v = metadata[key]; const double n = v.toDouble(-1);
            return v.isDouble() && std::isfinite(n) && n >= minimum
                && n <= 9007199254740991.0 && std::floor(n) == n;
        };
        const int mode=metadata["candidate_mode"].toInt(-1);
        const bool protocol=(metadata["protocol"]=="DTF3"&&mode>=0&&mode<10)
            ||((metadata["protocol"]==RvCaptureContract::Protocol||metadata["protocol"]==RvCaptureContract::PairedProtocol)
                &&((RvCaptureContract::captureOnly(mode)&&metadata["engine"]==RvCaptureContract::engine(mode)
                    &&metadata["model_sha256"]==""&&metadata["capture_only"]==true&&metadata["depth_present"]==false&&metadata["photon_enabled"]==false)
                   ||(RvCaptureContract::isLiteSr(mode)&&RvCaptureContract::boardImagingIdentity(metadata))));
        const bool candidate = metadata["work_mode"]=="dtof_candidate" && protocol
            && QRegularExpression("^[0-9a-f]{32}$").match(metadata["generation"].toString()).hasMatch()
            && QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(metadata["boot_id"].toString()).hasMatch()
            && metadata["candidate_mode"].isDouble() && integer("candidate_mode",0);
        const bool m48=metadata["dtof_width"]==48&&metadata["dtof_height"]==32;
        const bool m8=metadata["dtof_width"]==8&&metadata["dtof_height"]==8;
        const bool m16=metadata["dtof_width"]==16&&metadata["dtof_height"]==16;
        const auto fov=metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"];
        if((m8||m16)&&(!fov.isDouble()||fov.toDouble()<0||fov.toDouble()>255||std::floor(fov.toDouble())!=fov.toDouble()))return false;
        if(m16&&metadata.contains("dtof_fov_correction")&&metadata.contains("tof_fov_correction")&&metadata["dtof_fov_correction"]!=metadata["tof_fov_correction"])return false;
        if ((!m48&&!m8&&!m16)||samples.size() != (m48?48*32*4:m16?16*16*4:8*8*4)
            || metadata["dtof_format"] != "range_uq2_u16le_snr_u8_valid_u8"
            || metadata["dtof_range_unit"] != "0.25_mm"
            || metadata["dtof_source"] != "tmf8829_ipc_first_peak"
            || metadata["dtof_bytes"].toInt() != samples.size()
            || metadata["dtof_coordinate"] != "sensor_radial_native"
            || !integer("frame_id",1) || !(candidate || ((metadata["work_mode"]=="fusion" || metadata["work_mode"]=="rgb_tof") && integer("generation",0)))
            || !integer("tof_sequence",1) || !integer("tof_timestamp_ns",1)
            || (metadata["tof_reused"] != false && !(allowReuse && metadata["tof_reused"] == true))) return false;
        for (int i=0; i<sampleCount(); ++i) if (quint8(samples[i*4+3])>1) return false;
        return true;
    }
public:
    static constexpr double FreshRgbPairBudgetMs=200.;
    static constexpr double PairClockUncertaintyMs=35.;
    static constexpr const char *FreshRgbPairPolicy="bounded_software_time_pair_not_hardware_sync";
    static bool isHostFusionExperiment(const QJsonValue &experiment) {return experiment=="G"||experiment=="B";}
    bool isValidForHostFusion() const {
        const auto fov=metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"];
        const double delta=metadata["pair_delta_ms"].toDouble(1000),uncertainty=metadata["pair_clock_uncertainty_ms"].toDouble(-1);
        // Older G diagnostics can carry overload warnings 8 and 16.
        const auto warning=metadata["tof_warning_flags"];
        const auto experiment=metadata["assist_experiment"];
        return isHostFusionExperiment(experiment)&&metadata["source_protocol"]=="HDA1"
            &&metadata["dtof_present"]==true&&metadata["work_mode"]=="rgb_tof"
            &&metadata["pairing_policy"]==QLatin1String(FreshRgbPairPolicy)
            &&(warning==0||(experiment=="G"&&(warning==8||warning==16)))&&std::isfinite(delta)&&std::isfinite(uncertainty)
            &&uncertainty>=0&&uncertainty<=PairClockUncertaintyMs&&std::abs(delta)+uncertainty<=FreshRgbPairBudgetMs
            &&metadata["rgb_timestamp_ns"].isDouble()&&metadata["rgb_timestamp_ns"].toDouble()>0
            &&std::abs((metadata["tof_timestamp_ns"].toDouble()-metadata["rgb_timestamp_ns"].toDouble())/1e6-delta)<=.001
            &&metadata["pair_clock_source"]=="rtcp_prft_board_realtime_monotonic"
            &&fov.isDouble()&&fov.toDouble()>=0&&fov.toDouble()<=255&&std::floor(fov.toDouble())==fov.toDouble()
            &&(!metadata.contains("dtof_fov_correction")||!metadata.contains("tof_fov_correction")
                ||metadata["dtof_fov_correction"]==metadata["tof_fov_correction"])&&validMeasurement(experiment=="B");
    }
    quint16 rangeUq2(int index) const { return qFromLittleEndian<quint16>(samples.constData()+index*4); }
    double distanceMm(int index) const { return rangeUq2(index)*0.25; }
    quint8 snr(int index) const { return quint8(samples[index*4+2]); }
    bool valid(int index) const { return samples[index*4+3] == 1; }
    QByteArray radialMetres(const QByteArray& holeFillRanges={},QJsonArray* filledIndices=nullptr) const {
        if(!isValid()||columns()!=48||rows()!=32)return {};
        if(!holeFillRanges.isEmpty()&&holeFillRanges.size()!=RvCaptureContract::HoleFillBytes)return {};
        QByteArray result(48*32*4,0);
        for(int i=0;i<48*32;++i){
            quint16 range=valid(i)?rangeUq2(i):0;
            if(!range&&!holeFillRanges.isEmpty()){
                range=qFromLittleEndian<quint16>(holeFillRanges.constData()+i*2);
                if(range&&filledIndices)filledIndices->append(i);
            }
            const float metres=float(range)*.00025f;
            quint32 bits;std::memcpy(&bits,&metres,4);qToLittleEndian(bits,result.data()+i*4);
        }
        return result;
    }
};
Q_DECLARE_METATYPE(RvTofFrame)
