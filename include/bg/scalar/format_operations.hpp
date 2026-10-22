#pragma once

#include <bg/export.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/scalar.hpp>

#include <fmt/format.h>

namespace bg {

// Register operations before formatting begins on concurrent threads. The erased scalar and format context are
// borrowed for the callback duration. Do not retain either argument.
struct ScalarFormatOperations {
    using Format = void (*)(Scalar<>, fmt::format_context &);

    Format format{};
};

template <> BG_EXPORT ScalarDispatch<ScalarFormatOperations> &
ScalarDispatch<ScalarFormatOperations>::instance() noexcept;

BG_EXPORT void format_scalar(Scalar<> value, fmt::format_context &context);

} // namespace bg
