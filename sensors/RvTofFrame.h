#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaType>
#include <QtCore/QtEndian>
#include <QtCore/QRegularExpression>
#include <cmath>
#include <cstring>
#include "../tools/rv_capture/Contract.h"

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
    // Only explicitly bounded inference pairing opts in. Capture and all other
    // consumers retain the strict fresh-measurement contract by default.
    bool isValid(bool allowInferenceReuse=false) const {
        const auto integer = [this](const char *key, double minimum) {
            const auto v = metadata[key]; const double n = v.toDouble(-1);
            return v.isDouble() && std::isfinite(n) && n >= minimum
                && n <= 9007199254740991.0 && std::floor(n) == n;
        };
        const int mode=metadata["candidate_mode"].toInt(-1);
        const bool protocol=(metadata["protocol"]=="DTF3"&&mode>=0&&mode<10)
            ||((metadata["protocol"]==RvCaptureContract::Protocol||metadata["protocol"]==RvCaptureContract::PairedProtocol)&&RvCaptureContract::supports(mode)
                &&metadata["engine"]==RvCaptureContract::engine(mode)&&metadata["model_sha256"]==""&&metadata["capture_only"]==true&&metadata["depth_present"]==false&&metadata["photon_enabled"]==false);
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
            || (metadata["tof_reused"] != false&&(!allowInferenceReuse||metadata["tof_reused"]!=true))) return false;
        for (int i=0; i<sampleCount(); ++i) if (quint8(samples[i*4+3])>1) return false;
        return true;
    }
    static constexpr double GlobalScaleMaxPairDeltaMs=100.;
    static constexpr const char *GlobalScaleReusePolicy="latest_native_reuse_100ms_global_scale_v1";
    static constexpr double FreshRgbPairBudgetMs=200., PairClockUncertaintyMs=35.;
    static constexpr char FreshRgbReusePolicy[]="bounded_software_time_pair_not_hardware_sync";
    static bool isHostFusionExperiment(const QString &mode){return mode=="G"||mode=="B";}
    bool hasGlobalScaleReuseContract() const {
        const double delta=metadata["pair_delta_ms"].toDouble(1000),uncertainty=metadata["pair_clock_uncertainty_ms"].toDouble(-1);
        const auto mode=metadata["assist_experiment"].toString();
        const auto fov=metadata.contains("dtof_fov_correction")?metadata["dtof_fov_correction"]:metadata["tof_fov_correction"];
        const bool fresh48=metadata["source_candidate_mode"]==RvCaptureContract::Native48
            &&isHostFusionExperiment(mode)&&columns()==48&&rows()==32
            &&metadata["pairing_policy"]==QLatin1String(FreshRgbReusePolicy)
            &&fov.isDouble()&&fov.toDouble()>=0&&fov.toDouble()<=255&&std::floor(fov.toDouble())==fov.toDouble()
            &&(!metadata.contains("dtof_fov_correction")||!metadata.contains("tof_fov_correction")
                ||metadata["dtof_fov_correction"]==metadata["tof_fov_correction"])
            &&metadata["tof_warning_flags"]==0&&std::abs(delta)+uncertainty<=FreshRgbPairBudgetMs;
        const bool legacy=mode=="H"&&columns()==16&&rows()==16&&metadata["pairing_policy"]==QLatin1String(GlobalScaleReusePolicy)
            &&(metadata["tof_warning_flags"]==0||metadata["tof_warning_flags"]==16)
            &&std::abs(delta)<=GlobalScaleMaxPairDeltaMs;
        return metadata["source_protocol"]=="HDA1"
            &&metadata["dtof_present"]==true&&metadata["work_mode"]=="rgb_tof"
            &&(fresh48||legacy)&&std::isfinite(delta)
            &&std::isfinite(uncertainty)&&uncertainty>=0&&uncertainty<=PairClockUncertaintyMs
            &&metadata["rgb_timestamp_ns"].isDouble()&&metadata["rgb_timestamp_ns"].toDouble()>0
            &&std::abs((metadata["tof_timestamp_ns"].toDouble()-metadata["rgb_timestamp_ns"].toDouble())/1e6-delta)<=.001
            &&metadata["pair_clock_source"]=="rtcp_prft_board_realtime_monotonic";
    }
    // Only B, or historical H diagnostics, may reuse the same measurement.
    bool isValidForHostFusion() const {
        const auto mode=metadata["assist_experiment"].toString();
        return hasGlobalScaleReuseContract()&&isValid(mode=="B"||mode=="H");
    }
    quint16 rangeUq2(int index) const { return qFromLittleEndian<quint16>(samples.constData()+index*4); }
    double distanceMm(int index) const { return rangeUq2(index)*0.25; }
    quint8 snr(int index) const { return quint8(samples[index*4+2]); }
    bool valid(int index) const { return samples[index*4+3] == 1; }
    QByteArray radialMetres() const {
        if(!isValid()||columns()!=48||rows()!=32)return {};
        QByteArray result(48*32*4,0);
        for(int i=0;i<48*32;++i){
            const float metres=valid(i)?float(rangeUq2(i))*.00025f:0.f;
            quint32 bits;std::memcpy(&bits,&metres,4);qToLittleEndian(bits,result.data()+i*4);
        }
        return result;
    }
};
Q_DECLARE_METATYPE(RvTofFrame)
