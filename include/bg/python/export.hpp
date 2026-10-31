#pragma once

#if defined(_WIN32)
#if defined(BG_PYTHON_BUILDING)
#define BG_PYTHON_API __declspec(dllexport)
#else
#define BG_PYTHON_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define BG_PYTHON_API __attribute__((visibility("default")))
#else
#define BG_PYTHON_API
#endif
