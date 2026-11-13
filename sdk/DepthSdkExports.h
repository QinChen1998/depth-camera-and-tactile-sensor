#pragma once

// C++ symbols used by the matching desktop and worker builds.
#if defined(DCTS_DEPTH_SDK_BUILD)
#define DCTS_DEPTH_INTERNAL __declspec(dllexport)
#elif defined(DCTS_DEPTH_SDK_LINK)
#define DCTS_DEPTH_INTERNAL __declspec(dllimport)
#else
#define DCTS_DEPTH_INTERNAL
#endif
