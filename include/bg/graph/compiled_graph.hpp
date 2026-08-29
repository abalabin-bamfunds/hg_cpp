#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/compiled_storage_header.hpp>
#include <bg/graph/details/scheduling_info.hpp>
#include <bg/graph/wiring_path.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>

namespace bg {

class GraphWiring;
class RuntimeGraph;
class CompiledGraphBuilder;
class TimeSeriesStorageRepresentation;

// Owns the immutable packed representation of a wired graph and the initial runtime storage image.
class CompiledGraph {
  public:
    explicit CompiledGraph(const GraphWiring &wiring);
    CompiledGraph(const GraphWiring &wiring, std::pmr::memory_resource &memory);
    ~CompiledGraph();

    CompiledGraph(const CompiledGraph &) = delete;
    CompiledGraph &operator=(const CompiledGraph &) = delete;
    CompiledGraph(CompiledGraph &&) = delete;
    CompiledGraph &operator=(CompiledGraph &&) = delete;

    [[nodiscard]] std::size_t node_count() const noexcept { return node_count_and_fixed_alignment_ & count_mask; }

    [[nodiscard]] const CompiledNode &node(std::size_t index) const noexcept {
        return *reinterpret_cast<const CompiledNode *>(fixed_storage_ +
                                                       storage_header().node_offsets(node_count())[index]);
    }

    [[nodiscard]] std::size_t fixed_storage_size() const noexcept {
        return fixed_storage_size_and_flags_ & fixed_storage_size_mask;
    }

    [[nodiscard]] std::size_t runtime_storage_size() const noexcept { return runtime_storage_size_; }

    [[nodiscard]] std::size_t runtime_storage_alignment() const noexcept {
        return std::size_t{1} << (scalar_count_and_runtime_alignment_ >> alignment_shift);
    }

    [[nodiscard]] std::size_t schedule_word_count() const noexcept { return schedule_word_count(node_count()); }

    [[nodiscard]] const WiringPath &wiring_path(std::size_t node_index) const noexcept;

  private:
    friend class CompiledGraphBuilder;
    friend class RuntimeGraph;

    struct NodeFunctions {
        std::span<const EvaluationFunction> functions;
        std::span<const std::uint32_t> nodes;
    };

    struct OutputDestruction {
        std::span<const TimeSeriesStorageRepresentation *const> representations;
        std::span<const std::uint32_t> offsets;
    };

    using ScheduleWord = details::ScheduleWord;
    using SchedulingInfo = details::SchedulingInfo;

    static constexpr std::size_t schedule_word_bits = std::numeric_limits<ScheduleWord>::digits;

    [[nodiscard]] static constexpr std::size_t schedule_word_count(std::size_t node_count) noexcept {
        return (node_count + schedule_word_bits - 1) / schedule_word_bits;
    }

    static constexpr std::uint32_t variable_memory_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t future_scheduling_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t fixed_storage_size_mask = future_scheduling_flag - 1;
    static constexpr std::uint32_t alignment_shift = 26;
    static constexpr std::uint32_t count_mask = (std::uint32_t{1} << alignment_shift) - 1;

    [[nodiscard]] bool has_variable_memory() const noexcept {
        return (fixed_storage_size_and_flags_ & variable_memory_flag) != 0;
    }

    [[nodiscard]] bool has_future_scheduling() const noexcept {
        return (fixed_storage_size_and_flags_ & future_scheduling_flag) != 0;
    }

    [[nodiscard]] std::size_t future_schedule_offset() const noexcept {
        const auto scheduling_size = schedule_word_count() * sizeof(SchedulingInfo);

        return (scheduling_size + alignof(void *) - 1) & ~(alignof(void *) - 1);
    }

    [[nodiscard]] std::size_t fixed_storage_alignment() const noexcept {
        return std::size_t{1} << (node_count_and_fixed_alignment_ >> alignment_shift);
    }

    [[nodiscard]] std::size_t scalar_destruction_count() const noexcept {
        return scalar_count_and_runtime_alignment_ & count_mask;
    }

    [[nodiscard]] const std::byte *initial_runtime_storage() const noexcept;
    [[nodiscard]] NodeFunctions node_functions(std::uint32_t functions_offset, std::uint32_t count) const noexcept;
    [[nodiscard]] NodeFunctions start_functions() const noexcept;
    [[nodiscard]] NodeFunctions stop_functions() const noexcept;
    [[nodiscard]] OutputDestruction output_destruction() const noexcept;

    [[nodiscard]] const details::CompiledStorageHeader &storage_header() const noexcept {
        return *reinterpret_cast<const details::CompiledStorageHeader *>(fixed_storage_);
    }

    std::pmr::memory_resource *fixed_memory_;
    std::byte *fixed_storage_{};
    std::uint32_t fixed_storage_size_and_flags_{};
    std::uint32_t runtime_storage_size_{};
    std::uint32_t node_count_and_fixed_alignment_{};
    std::uint32_t scalar_count_and_runtime_alignment_{};
};

static_assert(sizeof(CompiledGraph) == 32);

} // namespace bg
