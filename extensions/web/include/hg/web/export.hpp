#pragma once

#if defined(_WIN32)
#if defined(HG_BUILDING_WEB)
#define HG_WEB_EXPORT __declspec(dllexport)
#else
#define HG_WEB_EXPORT __declspec(dllimport)
#endif
#else
#define HG_WEB_EXPORT __attribute__((visibility("default")))
#endif
