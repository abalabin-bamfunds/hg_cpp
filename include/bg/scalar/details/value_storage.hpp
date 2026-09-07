#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace bg::details {

struct ScalarValueAllocation {
    std::pmr::memory_resource *memory;
};

struct OwnedScalarData {
    const ScalarRuntimeType *runtime_type;
    std::uintptr_t data;
    ScalarDataMode mode;
};

template <typename T, typename Source>
concept scalar_storage_constructible_from =
    (!std::same_as<T, void>) && requires(typename scalar_storage_traits<T>::stored_type *destination, Source &&source,
                                         std::pmr::memory_resource &memory) {
        scalar_storage_traits<T>::construct(destination, std::forward<Source>(source), memory);
    };

inline std::size_t allocation_header_size(ScalarDataMode mode) noexcept {
    return mode == ScalarDataMode::owned_explicit_resource ? sizeof(ScalarValueAllocation) : 0;
}

inline std::size_t allocation_value_offset(const ScalarRuntimeType &runtime_type,
                                                         ScalarDataMode mode) noexcept {
    const auto header = allocation_header_size(mode);
    return (header + runtime_type.alignment() - 1) / runtime_type.alignment() * runtime_type.alignment();
}

inline std::size_t allocation_alignment(const ScalarRuntimeType &runtime_type,
                                                      ScalarDataMode mode) noexcept {
    const auto header_alignment =
        mode == ScalarDataMode::owned_explicit_resource ? alignof(ScalarValueAllocation) : std::size_t{1};
    return std::max(header_alignment, runtime_type.alignment());
}

template <typename Construct>
[[nodiscard]] OwnedScalarData allocate_scalar_value(const ScalarRuntimeType &runtime_type,
                                                    std::pmr::memory_resource *requested_memory,
                                                    Construct &&construct) {
    auto mode = ScalarDataMode::owned_standard_resource;
    auto *memory = std::pmr::new_delete_resource();
    if (requested_memory != nullptr) {
        mode = ScalarDataMode::owned_explicit_resource;
        memory = requested_memory;
    } else if (const auto *owner = current_scalar_allocation_owner()) {
        mode = ScalarDataMode::owned_context_resource;
        memory = owner->memory;
    }

    const auto value_offset = allocation_value_offset(runtime_type, mode);
    const auto size = value_offset + runtime_type.size();
    const auto alignment = allocation_alignment(runtime_type, mode);
    auto *allocation = static_cast<std::byte *>(memory->allocate(size, alignment));
    if (mode == ScalarDataMode::owned_explicit_resource) {
        std::construct_at(reinterpret_cast<ScalarValueAllocation *>(allocation), memory);
    }

    auto *data = allocation + value_offset;
    auto rollback = util::scope_exit{[&]() noexcept { memory->deallocate(allocation, size, alignment); }};
    std::invoke(std::forward<Construct>(construct), data, *memory);
    rollback.release();

    if (mode == ScalarDataMode::owned_context_resource) {
#ifndef NDEBUG
        record_context_scalar_allocation(data);
#endif
    }

    return {&runtime_type, reinterpret_cast<std::uintptr_t>(data), mode};
}

void release_scalar_value(const ScalarData &value) noexcept;

template <typename T, typename Source>
[[nodiscard]] OwnedScalarData make_scalar_value(Source &&source, std::pmr::memory_resource *memory) {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    if constexpr (inline_scalar_value<T> && std::same_as<std::remove_cvref_t<Source>, typename Traits::value_type>) {
        auto data = std::uintptr_t{};
        std::memcpy(std::addressof(data), std::addressof(source), sizeof(Stored));
        return {&cpp_scalar_runtime_type<T>(), data, ScalarDataMode::inline_value};
    }

    const auto &runtime_type = cpp_scalar_runtime_type<T>();
    return allocate_scalar_value(runtime_type, memory, [&](void *destination, std::pmr::memory_resource &resource) {
        Traits::construct(static_cast<Stored *>(destination), std::forward<Source>(source), resource);
    });
}

template <typename T>
[[nodiscard]] OwnedScalarData copy_scalar_value(const details::ScalarData &source, std::pmr::memory_resource *memory) {
    if constexpr (inline_scalar_value<T>) {
        return make_scalar_value<T>(scalar_data_value<T>(source), memory);
    }

    const auto &runtime_type = cpp_scalar_runtime_type<T>();
    return allocate_scalar_value(runtime_type, memory, [&](void *destination, std::pmr::memory_resource &resource) {
        source.copy_construct(destination, runtime_type, resource);
    });
}

[[nodiscard]] inline OwnedScalarData copy_scalar_value(const details::ScalarData &source,
                                                       std::pmr::memory_resource *memory) {
    auto inline_data = std::uintptr_t{};
    if (source.runtime_type().copy_inline(source.mode(), source.data_word(), inline_data)) {
        return {&source.runtime_type(), inline_data, ScalarDataMode::inline_value};
    }

    const auto &runtime_type = source.runtime_type();
    return allocate_scalar_value(runtime_type, memory, [&](void *destination, std::pmr::memory_resource &resource) {
        source.copy_construct(destination, runtime_type, resource);
    });
}

template <typename T> [[nodiscard]] ScalarData make_scalar_source(const scalar_value_t<T> &value) noexcept {
    return {cpp_scalar_runtime_type<T>(), std::addressof(value), ScalarDataMode::source_view};
}

} // namespace bg::details
