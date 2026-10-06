#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bg {
class TimeSeriesRuntimeType;
}

namespace bg::details {

struct ConstantMetadata {
    std::uint32_t start_time_offset;
    std::uint32_t notification_offset;
    std::uint32_t notification_count;
};

static_assert(sizeof(ConstantMetadata) == 12);
static_assert(std::is_trivially_copyable_v<ConstantMetadata>);

struct InheritedTimeSeriesBinding {
    std::uint32_t last_updated_offset;
    std::uint32_t value_offset;
    std::uint32_t subscribers_offset;
    std::uint32_t subscriber_count;
    std::uint32_t notification_offset;
};

static_assert(sizeof(InheritedTimeSeriesBinding) == 20);
static_assert(std::is_trivially_copyable_v<InheritedTimeSeriesBinding>);

struct InheritedTimeSeriesMetadata {
    std::uint32_t requirements_offset;
    std::uint32_t requirement_count;
};

static_assert(sizeof(InheritedTimeSeriesMetadata) == 8);
static_assert(std::is_trivially_copyable_v<InheritedTimeSeriesMetadata>);

struct ServiceBindingMetadata {
    std::uint32_t runtime_bindings_offset;
    std::uint32_t provider_nodes_offset;
    std::uint32_t binding_count;
};

static_assert(sizeof(ServiceBindingMetadata) == 12);
static_assert(std::is_trivially_copyable_v<ServiceBindingMetadata>);

struct RuntimeInputMetadata {
    std::uint32_t descriptors_offset;
    std::uint32_t descriptor_count;
};

static_assert(sizeof(RuntimeInputMetadata) == 8);
static_assert(std::is_trivially_copyable_v<RuntimeInputMetadata>);

struct RuntimeInputDescriptor {
    std::uint32_t slot_offset;
    std::uint32_t subscribers_offset;
    std::uint32_t subscriber_count;
    std::uint32_t notification_offset;
    const TimeSeriesRuntimeType *runtime_type;
};

static_assert(sizeof(RuntimeInputDescriptor) == 24);
static_assert(std::is_trivially_copyable_v<RuntimeInputDescriptor>);

// Describes the variable sections of compiled graph storage. Node offsets, paths, and optional metadata follow.
struct CompiledStorageHeader {
    std::span<const std::uint32_t> node_offsets(std::size_t node_count) const noexcept {
        return {reinterpret_cast<const std::uint32_t *>(this + 1), node_count};
    }

    std::span<const std::uint32_t> node_paths(std::size_t node_count) const noexcept {
        return {node_offsets(node_count).data() + node_count, node_count};
    }

    const ConstantMetadata *constant_metadata(std::size_t node_count) const noexcept {
        return reinterpret_cast<const ConstantMetadata *>(node_paths(node_count).data() + node_count);
    }

    const InheritedTimeSeriesMetadata *inherited_time_series_metadata(std::size_t node_count,
                                                                      bool has_constants) const noexcept {
        const auto *metadata = reinterpret_cast<const std::byte *>(node_paths(node_count).data() + node_count);
        if (has_constants) {
            metadata += sizeof(ConstantMetadata);
        }
        return reinterpret_cast<const InheritedTimeSeriesMetadata *>(metadata);
    }

    const ServiceBindingMetadata *service_binding_metadata(std::size_t node_count, bool has_constants,
                                                            bool has_inherited_time_series) const noexcept {
        const auto *metadata = reinterpret_cast<const std::byte *>(node_paths(node_count).data() + node_count);
        if (has_constants) {
            metadata += sizeof(ConstantMetadata);
        }
        if (has_inherited_time_series) {
            const auto *inherited_metadata = reinterpret_cast<const InheritedTimeSeriesMetadata *>(metadata);
            metadata += sizeof(InheritedTimeSeriesMetadata) +
                        inherited_metadata->requirement_count * sizeof(InheritedTimeSeriesBinding);
        }
        return reinterpret_cast<const ServiceBindingMetadata *>(metadata);
    }

    const RuntimeInputMetadata *runtime_input_metadata(std::size_t node_count, bool has_constants,
                                                       bool has_inherited_time_series,
                                                       bool has_service_bindings) const noexcept {
        auto *metadata = reinterpret_cast<const std::byte *>(node_paths(node_count).data() + node_count);
        if (has_constants) {
            metadata += sizeof(ConstantMetadata);
        }
        if (has_inherited_time_series) {
            const auto *inherited_metadata = reinterpret_cast<const InheritedTimeSeriesMetadata *>(metadata);
            metadata += sizeof(InheritedTimeSeriesMetadata) +
                        inherited_metadata->requirement_count * sizeof(InheritedTimeSeriesBinding);
        }
        if (has_service_bindings) {
            const auto *service_metadata = reinterpret_cast<const ServiceBindingMetadata *>(metadata);
            metadata += sizeof(ServiceBindingMetadata) + service_metadata->binding_count * sizeof(std::uint32_t);
        }
        return reinterpret_cast<const RuntimeInputMetadata *>(metadata);
    }

    bool has_runtime_inputs() const noexcept { return (wiring_paths_offset_and_flags & runtime_inputs_flag) != 0; }

    bool supports_borrowed_evaluation() const noexcept {
        return (wiring_paths_offset_and_flags & borrowed_evaluation_flag) != 0;
    }

    std::uint32_t wiring_paths_offset() const noexcept { return wiring_paths_offset_and_flags & offset_mask; }

    std::uint32_t wiring_paths_offset_and_flags;
    std::uint32_t wiring_path_count;
    std::uint32_t start_functions_offset;
    std::uint32_t start_count;
    std::uint32_t stop_functions_offset;
    std::uint32_t stop_count;
    std::uint32_t scalar_destruction_runtime_types_offset;
    std::uint32_t output_destruction_count;
    std::uint32_t variable_memory_offset;
    std::uint32_t initial_runtime_image_offset;

    // Fixed-storage offsets stay below 1 GiB, leaving the high two bits for metadata flags.
    static constexpr std::uint32_t runtime_inputs_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t borrowed_evaluation_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t offset_mask = borrowed_evaluation_flag - 1;
};

static_assert(sizeof(CompiledStorageHeader) == 40);
static_assert(std::is_trivially_copyable_v<CompiledStorageHeader>);

} // namespace bg::details
