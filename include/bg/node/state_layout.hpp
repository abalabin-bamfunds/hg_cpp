#pragma once

#include <cstddef>
#include <vector>

namespace bg {

class ScalarRuntimeType;
class TimeSeriesType;

struct NodeStateLayout {
    std::size_t size{};
    std::size_t alignment{1};
    std::vector<std::size_t> field_offsets;
    std::vector<const ScalarRuntimeType *> generic_runtime_types;
    std::size_t state_field_count{};
    std::vector<const TimeSeriesType *> dynamic_reference_input_types;
};

} // namespace bg
