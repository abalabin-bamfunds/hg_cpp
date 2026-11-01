#pragma once

#if defined(_WIN32)
#if defined(HG_BUILDING_ENGINE)
#define HG_EXPORT __declspec(dllexport)
#else
#define HG_EXPORT __declspec(dllimport)
#endif
#else
#define HG_EXPORT
#endif
