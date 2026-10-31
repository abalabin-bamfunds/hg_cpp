#pragma once

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define BG_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define BG_LIFETIMEBOUND [[msvc::lifetimebound]]
#else
#define BG_LIFETIMEBOUND
#endif
#else
#define BG_LIFETIMEBOUND
#endif
