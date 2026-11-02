#pragma once

#if defined(_WIN32)
#if defined(HG_FRAME_BUILDING_LIBRARY)
#define HG_FRAME_EXPORT __declspec(dllexport)
#else
#define HG_FRAME_EXPORT __declspec(dllimport)
#endif
#else
#define HG_FRAME_EXPORT
#endif
