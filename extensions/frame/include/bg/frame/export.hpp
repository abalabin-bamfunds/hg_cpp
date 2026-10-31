#pragma once

#if defined(_WIN32)
#if defined(BG_FRAME_BUILDING_LIBRARY)
#define BG_FRAME_EXPORT __declspec(dllexport)
#else
#define BG_FRAME_EXPORT __declspec(dllimport)
#endif
#else
#define BG_FRAME_EXPORT
#endif
