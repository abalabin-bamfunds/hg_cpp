#pragma once

#include <cstddef>
#include <cstdint>

namespace bg {

class RuntimeGraph;

namespace details {

struct DynamicNodeSubscriber {
    DynamicNodeSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset, std::uint32_t node_index, bool active,
                          bool required, bool controlled_activity, bool all_valid) noexcept
        : graph{&target_graph}, input_offset{input_offset},
          node_and_flags{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                         (active && !required ? active_optional_flag : 0) |
                         (controlled_activity ? controlled_activity_flag : 0) |
                         (all_valid ? all_valid_flag : 0)} {}

    DynamicNodeSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset,
                          std::uint32_t node_and_flags) noexcept
        : graph{&target_graph}, input_offset{input_offset}, node_and_flags{node_and_flags} {}

    std::size_t node_index() const noexcept { return node_and_flags & node_index_mask; }
    bool active() const noexcept { return (node_and_flags & active_flag) != 0; }
    bool required() const noexcept { return (node_and_flags & required_flag) != 0; }
    bool active_optional() const noexcept { return (node_and_flags & active_optional_flag) != 0; }
    bool controlled_activity() const noexcept { return (node_and_flags & controlled_activity_flag) != 0; }
    bool all_valid() const noexcept { return (node_and_flags & all_valid_flag) != 0; }

    struct Identity {
        RuntimeGraph *graph;
        std::uint32_t input_offset;
    };

    Identity identity() const noexcept { return {graph, input_offset}; }

    RuntimeGraph *graph;
    std::uint32_t input_offset;
    std::uint32_t node_and_flags;

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t active_optional_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t controlled_activity_flag = std::uint32_t{1} << 28;
    static constexpr std::uint32_t all_valid_flag = std::uint32_t{1} << 27;
    static constexpr std::uint32_t node_index_mask = all_valid_flag - 1;
};

static_assert(sizeof(DynamicNodeSubscriber) == 2 * sizeof(void *));

} // namespace details
} // namespace bg
