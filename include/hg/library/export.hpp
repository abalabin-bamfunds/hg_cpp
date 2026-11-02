#pragma once

#if defined(_WIN32)
#if defined(HG_BUILDING_LIBRARY)
#define HG_LIBRARY_EXPORT __declspec(dllexport)
#else
#define HG_LIBRARY_EXPORT __declspec(dllimport)
#endif
#else
#define HG_LIBRARY_EXPORT
#endif
