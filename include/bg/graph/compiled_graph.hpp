#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/compiled_storage_header.hpp>
#include <bg/graph/details/scheduling_info.hpp>
#include <bg/graph/wiring_path.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace bg {

class GraphWiring;
class CompiledGraphBuilder;
class TimeSeriesRuntimeType;

// Owns the immutable packed runtime_type of a wired graph and the initial runtime storage image.
class CompiledGraph {
  public:
    explicit CompiledGraph(const GraphWiring &wiring);
    CompiledGraph(const GraphWiring &wiring, allocator memory);
    ~CompiledGraph();

    CompiledGraph(const CompiledGraph &) = delete;
    CompiledGraph &operator=(const CompiledGraph &) = delete;
    CompiledGraph(CompiledGraph &&) = delete;
    CompiledGraph &operator=(CompiledGraph &&) = delete;

    std::size_t node_count() const noexcept { return node_count_and_fixed_alignment_ & count_mask; }

    const CompiledNode &node(std::size_t index) const noexcept {
        return *reinterpret_cast<const CompiledNode *>(fixed_storage_ +
                                                       storage_header().node_offsets(node_count())[index]);
    }

    std::size_t fixed_storage_size() const noexcept { return fixed_storage_size_and_flags_ & fixed_storage_size_mask; }

    std::size_t runtime_storage_size() const noexcept { return runtime_storage_size_; }

    std::size_t runtime_storage_alignment() const noexcept {
        return std::size_t{1} << (scalar_count_and_runtime_alignment_ >> alignment_shift);
    }

    std::size_t schedule_word_count() const noexcept { return schedule_word_count(node_count()); }

    const WiringPath &wiring_path(std::size_t node_index) const noexcept;

    std::size_t inherited_time_series_count() const noexcept;
    std::size_t service_binding_count() const noexcept;
    std::span<const details::RuntimeInputDescriptor> runtime_inputs() const noexcept;
    std::size_t runtime_input_count() const noexcept { return runtime_inputs().size(); }
    std::span<const Subscriber>
    runtime_input_subscribers(const details::RuntimeInputDescriptor &binding) const noexcept;
    const OutputNotification *runtime_input_notification(
        const details::RuntimeInputDescriptor &binding) const noexcept;

    struct NodeFunctions {
        std::span<const NodeFunction> functions;
        std::span<const std::uint32_t> nodes;
    };

    struct OutputDestruction {
        std::span<const TimeSeriesRuntimeType *const> runtime_types;
        std::span<const std::uint32_t> offsets;
    };

    using ScheduleWord = details::ScheduleWord;
    using SchedulingInfo = details::SchedulingInfo;

    static constexpr std::size_t schedule_word_bits = std::numeric_limits<ScheduleWord>::digits;

    bool has_timed_scheduling() const noexcept { return (fixed_storage_size_and_flags_ & timed_scheduling_flag) != 0; }

    bool has_cycle_callbacks() const noexcept {
        return (storage_header().variable_memory_offset & cycle_callbacks_flag) != 0;
    }

    std::size_t cycle_callbacks_offset() const noexcept {
        auto offset = std::size_t{};
        if (has_timed_scheduling()) {
            offset += sizeof(void *);
        }

        return (offset + alignof(void *) - 1) & ~(alignof(void *) - 1);
    }

    std::span<const SchedulingInfo> initial_scheduling_info() const noexcept;
    const std::byte *initial_runtime_storage() const noexcept;
    NodeFunctions start_functions() const noexcept;
    NodeFunctions stop_functions() const noexcept;
    OutputDestruction output_destruction() const noexcept;
    std::optional<std::size_t> constant_start_time_offset() const noexcept;
    std::span<const InputNotificationSubscriber> constant_notifications() const noexcept;
    std::span<const details::InheritedTimeSeriesBinding> inherited_time_series() const noexcept;
    const details::ServiceBindingMetadata *service_binding_metadata() const noexcept;
    std::span<const std::uint32_t> service_binding_provider_nodes() const noexcept;
    std::span<const Subscriber>
    inherited_time_series_subscribers(const details::InheritedTimeSeriesBinding &requirement) const noexcept;
    const OutputNotification *
    inherited_time_series_notification(const details::InheritedTimeSeriesBinding &requirement) const noexcept;

  private:
    const std::byte *initial_runtime_image() const noexcept;

    struct CompilationTag {};

    friend class CompiledGraphBuilder;
    friend std::shared_ptr<const CompiledGraph> details::compile_graph(const GraphWiring &, allocator,
                                                                       std::span<std::size_t>);

  public:
    CompiledGraph(CompilationTag, const GraphWiring &wiring, allocator memory, std::span<std::size_t> node_indices);

  private:
    static constexpr std::size_t schedule_word_count(std::size_t node_count) noexcept {
        return (node_count + schedule_word_bits - 1) / schedule_word_bits;
    }

    static constexpr std::uint32_t variable_memory_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t timed_scheduling_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t cycle_callbacks_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t constants_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t inherited_time_series_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t service_bindings_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t fixed_storage_size_mask = timed_scheduling_flag - 1;
    static constexpr std::uint32_t variable_memory_offset_mask = constants_flag - 1;
    static constexpr std::uint32_t initial_runtime_image_offset_mask = service_bindings_flag - 1;
    static constexpr std::uint32_t alignment_shift = 26;
    static constexpr std::uint32_t count_mask = (std::uint32_t{1} << alignment_shift) - 1;

    bool has_variable_memory() const noexcept { return (fixed_storage_size_and_flags_ & variable_memory_flag) != 0; }

    bool has_constants() const noexcept { return (storage_header().variable_memory_offset & constants_flag) != 0; }

    bool has_inherited_time_series() const noexcept {
        return (storage_header().initial_runtime_image_offset & inherited_time_series_flag) != 0;
    }

    bool has_service_bindings() const noexcept {
        return (storage_header().initial_runtime_image_offset & service_bindings_flag) != 0;
    }

    std::size_t variable_memory_offset() const noexcept {
        return storage_header().variable_memory_offset & variable_memory_offset_mask;
    }

    const details::ConstantMetadata *constant_metadata() const noexcept {
        if (!has_constants()) {
            return nullptr;
        }

        return storage_header().constant_metadata(node_count());
    }

    std::size_t fixed_storage_alignment() const noexcept {
        return std::size_t{1} << (node_count_and_fixed_alignment_ >> alignment_shift);
    }

    std::size_t scalar_destruction_count() const noexcept { return scalar_count_and_runtime_alignment_ & count_mask; }

    NodeFunctions node_functions(std::uint32_t functions_offset, std::uint32_t count) const noexcept;

    const details::CompiledStorageHeader &storage_header() const noexcept {
        return *reinterpret_cast<const details::CompiledStorageHeader *>(fixed_storage_);
    }

    allocator fixed_allocator_;
    std::byte *fixed_storage_{};
    std::uint32_t fixed_storage_size_and_flags_{};
    std::uint32_t runtime_storage_size_{};
    std::uint32_t node_count_and_fixed_alignment_{};
    std::uint32_t scalar_count_and_runtime_alignment_{};
};

static_assert(sizeof(CompiledGraph) == 32);

} // namespace bg
