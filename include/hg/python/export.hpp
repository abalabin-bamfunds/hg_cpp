#pragma once

#if defined(_WIN32)
#if defined(HG_PYTHON_BUILDING)
#define HG_PYTHON_API __declspec(dllexport)
#else
#define HG_PYTHON_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define HG_PYTHON_API __attribute__((visibility("default")))
#else
#define HG_PYTHON_API
#endif
