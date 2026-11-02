#pragma once

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define HG_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define HG_LIFETIMEBOUND [[msvc::lifetimebound]]
#else
#define HG_LIFETIMEBOUND
#endif
#else
#define HG_LIFETIMEBOUND
#endif
