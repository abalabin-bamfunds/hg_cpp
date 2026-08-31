#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace bg {

class RuntimeGraph;

namespace details {

struct DynamicSubscriber {
    DynamicSubscriber(RuntimeGraph &target_graph, std::uint32_t parameter_index, std::uint32_t node_index, bool active,
                      bool required) noexcept
        : graph{&target_graph}, parameter_index{parameter_index},
          node_and_flags{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                         (active && !required ? active_optional_flag : 0)} {}

    [[nodiscard]] std::size_t node_index() const noexcept { return node_and_flags & node_index_mask; }
    [[nodiscard]] bool active() const noexcept { return (node_and_flags & active_flag) != 0; }
    [[nodiscard]] bool required() const noexcept { return (node_and_flags & required_flag) != 0; }
    [[nodiscard]] bool active_optional() const noexcept { return (node_and_flags & active_optional_flag) != 0; }

    RuntimeGraph *graph;
    std::uint32_t parameter_index;
    std::uint32_t node_and_flags;

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t active_optional_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t node_index_mask = active_optional_flag - 1;
};

struct DynamicSubscribers {
    explicit DynamicSubscribers(std::pmr::memory_resource &memory) : records{&memory} {}

    std::pmr::vector<DynamicSubscriber> records;
};

static_assert(sizeof(DynamicSubscriber) == 2 * sizeof(void *));

} // namespace details
} // namespace bg
