#pragma once

#include <bg/time_series/item_id.hpp>

#include <cstddef>

namespace bg {

class TimeSeriesType;
class TimeSeriesRuntimeType;

// Describes one node output or contained output for graph compilation.
struct WiredOutput {
    static constexpr auto npos = static_cast<std::size_t>(-1);

    std::size_t node_index;
    std::size_t parent;
    item_id_t item;
    const TimeSeriesType *type;
    const TimeSeriesRuntimeType *runtime_type{};
};

} // namespace bg
