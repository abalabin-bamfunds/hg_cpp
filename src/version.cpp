#include <hg/version.hpp>

#ifndef HG_VERSION
#error "HG_VERSION must be supplied by the build system"
#endif

namespace hg {

std::string_view version() noexcept {
    return HG_VERSION;
}

} // namespace hg
