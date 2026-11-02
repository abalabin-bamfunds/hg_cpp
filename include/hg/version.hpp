#pragma once

#include <string_view>

namespace hg {

/// Returns the semantic version of the linked hg library.
std::string_view version() noexcept;

} // namespace hg
