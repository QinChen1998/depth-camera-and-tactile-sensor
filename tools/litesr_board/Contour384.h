// Portable board CPU implementation of sensors/RvLiteSrContour.cpp.
// Same current-frame repair rules and complete four-pass contour refinement.
// Keep changes checked against the original Qt implementation by the driver.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <type_traits>
#include <limits>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
namespace LiteSrBoardContour384 {
constexpr int W=384,H=256;
#include "Contour384Grid.inc"
}
