#pragma once

#if defined(_WIN32)
#if defined(BG_BUILDING_LIBRARY)
#define BG_LIBRARY_EXPORT __declspec(dllexport)
#else
#define BG_LIBRARY_EXPORT __declspec(dllimport)
#endif
#else
#define BG_LIBRARY_EXPORT
#endif
