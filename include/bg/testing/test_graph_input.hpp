#pragma once

#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/value.hpp>

#include <optional>
#include <span>

namespace bg::testing {

struct TestGraphInput {
    const ScalarRuntimeType &runtime_type;
    std::span<const std::optional<ScalarValue<>>> ticks;
};

} // namespace bg::testing
