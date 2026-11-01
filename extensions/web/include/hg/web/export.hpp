#pragma once

#if defined(_WIN32)
#if defined(BG_BUILDING_WEB)
#define BG_WEB_EXPORT __declspec(dllexport)
#else
#define BG_WEB_EXPORT __declspec(dllimport)
#endif
#else
#define BG_WEB_EXPORT __attribute__((visibility("default")))
#endif
