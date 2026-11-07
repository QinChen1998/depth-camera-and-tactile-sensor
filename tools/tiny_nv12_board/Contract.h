#pragma once
namespace TinyNv12Contract {
constexpr int Mode=18, Width=80, Height=128, Pixels=Width*Height, HeaderBytes=264, PayloadBytes=67315;
// The 80x128 ISP grid samples this raw-camera crop with unequal X/Y scaling.
constexpr int PresentationWidth=1248, PresentationHeight=1296;
constexpr const char* Scheme="board_tiny_nv12_128x80";
constexpr const char* Protocol="TNY6";
constexpr const char* Engine="tiny-nv12-128x80-holes-v2";
constexpr const char* ModelSha="034bb260a7751c24ec133ec01ea6343f9ecf9102684162e0efa55aaef7056434";
constexpr const char* CheckpointSha="f3fb6e51ba0a455faf756aada1da2aa8677e11ff729e429dcb976d114b10509f";
constexpr const char* Calibration="rv20261008-f1";
constexpr const char* Serial="REDACTED_DEVICE_SERIAL";
constexpr const char* Postprocess="enclosed_small_slope_v2";
}
