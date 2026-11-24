#pragma once
namespace RvCaptureStatus {
// The two information bits may appear independently or together.
constexpr bool allowedWarnings(unsigned warnings) {
    return (warnings & ~0x18u) == 0u;
}
constexpr bool accept(unsigned raw, unsigned warnings) {
    return allowedWarnings(warnings) && raw == (0x01u | warnings);
}
}
