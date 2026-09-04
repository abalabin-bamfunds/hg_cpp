#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bg::details {

// Describes the variable sections of compiled graph storage. Node offsets and wiring-path indices follow this header.
struct CompiledStorageHeader {
    [[nodiscard]] std::span<const std::uint32_t> node_offsets(std::size_t node_count) const noexcept {
        return {reinterpret_cast<const std::uint32_t *>(this + 1), node_count};
    }

    [[nodiscard]] std::span<const std::uint32_t> node_paths(std::size_t node_count) const noexcept {
        return {node_offsets(node_count).data() + node_count, node_count};
    }

    std::uint32_t wiring_paths_offset;
    std::uint32_t wiring_path_count;
    std::uint32_t start_functions_offset;
    std::uint32_t start_count;
    std::uint32_t stop_functions_offset;
    std::uint32_t stop_count;
    std::uint32_t scalar_destruction_runtime_types_offset;
    std::uint32_t output_destruction_count;
    std::uint32_t variable_memory_offset;
    std::uint32_t initial_runtime_storage_offset;
};

static_assert(sizeof(CompiledStorageHeader) == 40);
static_assert(std::is_trivially_copyable_v<CompiledStorageHeader>);

} // namespace bg::details
