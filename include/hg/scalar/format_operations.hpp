#pragma once

#include <hg/export.hpp>
#include <hg/scalar/dispatch.hpp>
#include <hg/scalar/scalar.hpp>

#include <fmt/format.h>

namespace hg {

// Register operations before formatting begins on concurrent threads. The erased scalar and format context are
// borrowed for the callback duration. Do not retain either argument.
struct ScalarFormatOperations {
    using Format = void (*)(Scalar<>, fmt::format_context &);

    Format format{};
};

template <> HG_EXPORT ScalarDispatch<ScalarFormatOperations> &
ScalarDispatch<ScalarFormatOperations>::instance() noexcept;

HG_EXPORT void format_scalar(Scalar<> value, fmt::format_context &context);

} // namespace hg
