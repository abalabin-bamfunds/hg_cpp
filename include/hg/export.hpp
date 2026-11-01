#pragma once

#if defined(_WIN32)
#if defined(BG_BUILDING_ENGINE)
#define BG_EXPORT __declspec(dllexport)
#else
#define BG_EXPORT __declspec(dllimport)
#endif
#else
#define BG_EXPORT
#endif
