#include <bg/version.hpp>

#ifndef BG_VERSION
#error "BG_VERSION must be supplied by the build system"
#endif

namespace bg {

std::string_view version() noexcept {
    return BG_VERSION;
}

} // namespace bg
