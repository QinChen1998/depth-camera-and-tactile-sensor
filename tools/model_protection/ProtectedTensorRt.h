#pragma once

#include <cstddef>

namespace nvinfer1 {
class ICudaEngine;
class IRuntime;
}

#ifdef DCTS_PROTECTED_TENSORRT_BUILD
#define DCTS_PROTECTED_TENSORRT_API __declspec(dllexport)
#else
#define DCTS_PROTECTED_TENSORRT_API __declspec(dllimport)
#endif

extern "C" DCTS_PROTECTED_TENSORRT_API nvinfer1::ICudaEngine *DctsLoadProtectedTensorRtEngine(
    nvinfer1::IRuntime *runtime, const wchar_t *path, const char *expectedSha256,
    char *error, std::size_t errorBytes) noexcept;

#undef DCTS_PROTECTED_TENSORRT_API
