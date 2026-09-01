#pragma once

#include <bg/time_series/tsl/tsl_storage.hpp>

#include <cstddef>

namespace bg {

class CompiledNode;

namespace details {

template <std::size_t Size> struct aliased_fixed_time_series_list_storage {
    fixed_time_series_list_changes<Size> changes;
    const CompiledNode *node{};
};

} // namespace details
} // namespace bg
