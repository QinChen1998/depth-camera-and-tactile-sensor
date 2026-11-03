#pragma once
#include <array>
#include <cstdint>
#include <string>

// Offline experiment only. No Qt, RGB, calibration, global/static history or GUI.
namespace DtofM0 {
constexpr int N=48*32, W=384, H=256, P=W*H;
enum Observation : uint8_t { Unknown, InRange, Outside, Malformed };
enum Phase : uint8_t { Off, Pending, On, HoldEdge, HoldInner, HardOut };
enum Reason : uint32_t {
    NoHistory=1, Age=2, Count=4, NoSupport=8, LostSurface=16, Changed=32,
    Channel=64, HoleOpen=128, HoleArea=256, HoleBox=512, HoleRing=1024,
    HoleLayer=2048, Hard=4096, ColdHole=8192, Ambiguous=16384
};
enum Source : uint8_t { Numeric=1, SpatialEstimate=2, HistoryShape=4 };
struct Config {
    bool temporal=true, ownerOutside=true, holeHistory=true, hardLayer=true;
    double layerM=.05, changeM=.05, holeSpreadM=.06, sigmaM=.03;
    int edgeMisses=1, innerMisses=2, edgeMaxMs=150, innerMaxMs=220;
    int initialPeriodMs=67, jitterMs=10, freshMs=220, resetGapMs=500;
};
struct Frame {
    std::array<uint16_t,N> uq{};
    std::array<uint8_t,N> valid{};
    uint64_t sequence=0;
    int64_t captureNs=0; // Sensor monotonic domain. Never subtract host time.
    std::string identity; // Caller binds source/boot/generation/mode/unit/config.
};
struct Cell {
    uint16_t lastUq=0, pendingUq=0;
    uint8_t misses=0, hits=0;
    Phase phase=Off;
    bool inner=false;
    int64_t rawHostMs=-1, rawCaptureNs=0, deadlineMs=-1;
};
struct Timing { double stateMs=0, prepareMs=0, contourMs=0, outputMs=0, totalMs=0; };
struct Result {
    std::array<float,P> depth{};
    std::array<uint8_t,P> coverage{}, source{};
    std::array<uint8_t,N> observation{}, accepted{}, shape{}, estimated{}, overlap{};
    std::array<uint32_t,N> reason{}, estimateSupport{}; // 5x5 bitset of CURRENT accepted raw IDs
    std::array<double,N> prepared{}, smooth{}, contour{};
    uint64_t sequence=0;
    int64_t captureNs=0, receiveMs=-1;
    Timing timing;
    bool newMeasurement=false, fresh=false;
    std::string event="reset";
};
class Processor {
public:
    explicit Processor(Config config={});
    const Result& onFrame(const Frame&,int64_t hostMonotonicMs);
    const Result& expire(int64_t hostMonotonicMs);
    void reset(); // Stop, replay seek and caller lifecycle revocation.
    void configure(Config); // Always resets; no mutable configuration alias.
    const Result& result() const { return out_; }
    const std::array<Cell,N>& cells() const { return cells_; }
    const Config& config() const { return config_; }
    double periodMs() const;
private:
    Config config_;
    std::array<Cell,N> cells_{};
    Frame current_;
    Result out_;
    std::array<double,8> periods_{};
    int periodsCount_=0, periodCursor_=0;
    int64_t lastHost_=-1;
    void render();
};
}
