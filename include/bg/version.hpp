#pragma once

#include <string_view>

namespace bg {

/// Returns the semantic version of the linked bg library.
[[nodiscard]] std::string_view version() noexcept;

} // namespace bg
