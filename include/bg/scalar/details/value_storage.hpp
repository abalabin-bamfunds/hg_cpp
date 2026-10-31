#pragma once

#include <bg/allocator.hpp>
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
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::details {

struct ScalarValueAllocator {
    allocator memory;
};

template <typename T> inline constexpr bool scalar_uses_shared_handle = [] {
    if constexpr (requires { scalar_storage_traits<T>::uses_shared_handle; }) {
        return scalar_storage_traits<T>::uses_shared_handle;
    } else {
        return false;
    }
}();

struct OwnedScalarData {
    const ScalarRuntimeType *runtime_type;
    std::uintptr_t data;
    ScalarDataMode mode;
};

template <typename T, typename Source>
concept scalar_storage_constructible_from =
    (!std::same_as<T, void>) && requires(typename scalar_storage_traits<T>::stored_type *destination, Source &&source,
                                         allocator &memory) {
        scalar_storage_traits<T>::construct(destination, std::forward<Source>(source), memory);
    };

template <typename T>
inline constexpr bool scalar_storage_has_borrow_value = [] {
    if constexpr (std::same_as<T, void>) {
        return false;
    } else {
        return requires(const typename scalar_storage_traits<T>::value_type &value) {
            { scalar_storage_traits<T>::borrow_value(value) } -> std::same_as<ScalarData>;
        };
    }
}();

template <typename T>
inline constexpr bool scalar_storage_has_runtime_type_for_copy = [] {
    if constexpr (std::same_as<T, void>) {
        return false;
    } else {
        return requires {
            static_cast<const ScalarRuntimeType &(*)(const ScalarData &)>(
                &scalar_storage_traits<T>::runtime_type_for_copy);
        };
    }
}();

template <typename T> consteval bool scalar_source_is_nothrow() {
    using Traits = scalar_storage_traits<T>;
    using Value = typename Traits::value_type;

    if constexpr (scalar_storage_has_borrow_value<T>) {
        return noexcept(Traits::borrow_value(std::declval<const Value &>()));
    } else if constexpr (requires(const ScalarType &type) {
                             { Traits::runtime_type(type) } -> std::same_as<const ScalarRuntimeType &>;
                         }) {
        return false;
    } else {
        return true;
    }
}

[[nodiscard]] OwnedScalarData copy_scalar_value(const ScalarData &source, allocator *memory);

template <typename T>
[[nodiscard]] OwnedScalarData copy_scalar_value(const ScalarData &source, allocator *memory);

inline std::size_t allocation_header_size(ScalarDataMode mode) noexcept {
    return mode == ScalarDataMode::owned_explicit_allocator ? sizeof(ScalarValueAllocator) : 0;
}

inline std::size_t allocation_value_offset(const ScalarRuntimeType &runtime_type,
                                                         ScalarDataMode mode) noexcept {
    const auto header = allocation_header_size(mode);
    return (header + runtime_type.alignment() - 1) / runtime_type.alignment() * runtime_type.alignment();
}

inline std::size_t allocation_alignment(const ScalarRuntimeType &runtime_type,
                                                      ScalarDataMode mode) noexcept {
    const auto header_alignment =
        mode == ScalarDataMode::owned_explicit_allocator ? alignof(ScalarValueAllocator) : std::size_t{1};
    return std::max(header_alignment, runtime_type.alignment());
}

inline std::optional<allocator> scalar_value_allocator(const ScalarData &value) {
    switch (value.mode()) {
    case ScalarDataMode::owned_explicit_allocator: {
        const auto offset = allocation_value_offset(value.runtime_type(), value.mode());
        const auto *allocation = static_cast<const std::byte *>(value.data()) - offset;
        return reinterpret_cast<const ScalarValueAllocator *>(allocation)->memory;
    }
    case ScalarDataMode::owned_context_allocator:
        return current_scalar_allocator();
    case ScalarDataMode::owned_default_allocator:
        return allocator{};
    default:
        return std::nullopt;
    }
}

template <typename Construct>
[[nodiscard]] OwnedScalarData allocate_scalar_value(const ScalarRuntimeType &runtime_type, allocator *requested_memory,
                                                    Construct &&construct) {
    auto mode = ScalarDataMode::owned_default_allocator;
    auto memory = allocator{};
    if (requested_memory != nullptr) {
        mode = ScalarDataMode::owned_explicit_allocator;
        memory = *requested_memory;
    } else if (const auto *owner = current_scalar_allocation_owner()) {
        mode = ScalarDataMode::owned_context_allocator;
        memory = owner->memory;
    }

    const auto value_offset = allocation_value_offset(runtime_type, mode);
    const auto size = value_offset + runtime_type.size();
    const auto alignment = allocation_alignment(runtime_type, mode);
    auto *allocation = static_cast<std::byte *>(memory.allocate_bytes(size, alignment));
    if (mode == ScalarDataMode::owned_explicit_allocator) {
        std::construct_at(reinterpret_cast<ScalarValueAllocator *>(allocation), memory);
    }

    auto *data = allocation + value_offset;
    auto rollback = util::scope_exit{[&]() noexcept { memory.deallocate_bytes(allocation, size, alignment); }};
    std::invoke(std::forward<Construct>(construct), data, memory);
    rollback.release();

    if (mode == ScalarDataMode::owned_context_allocator) {
#ifndef NDEBUG
        record_context_scalar_allocation(data);
#endif
    }

    return {&runtime_type, reinterpret_cast<std::uintptr_t>(data), mode};
}

[[nodiscard]] inline OwnedScalarData copy_scalar_value(const ScalarData &source,
                                                       const ScalarRuntimeType &runtime_type,
                                                       allocator *memory) {
    if (&runtime_type == &source.runtime_type()) {
        if (source.mode() == ScalarDataMode::shared_view || source.mode() == ScalarDataMode::shared_value) {
            auto shared_data = std::uintptr_t{};
            const auto *shared_type = source.runtime_type().retain_shared(source, shared_data);
            return {shared_type, shared_data, ScalarDataMode::shared_value};
        }

        auto inline_data = std::uintptr_t{};
        if (source.runtime_type().copy_inline(source.mode(), source.data_word(), inline_data)) {
            return {&runtime_type, inline_data, ScalarDataMode::inline_value};
        }
    }

    return allocate_scalar_value(runtime_type, memory, [&](void *destination, allocator &resource) {
        runtime_type.construct_from_input(destination, source, resource);
    });
}

inline allocator scalar_value_allocator(allocator *requested_memory) {
    if (requested_memory != nullptr) {
        return *requested_memory;
    }
    if (const auto *owner = current_scalar_allocation_owner()) {
        return owner->memory;
    }
    return allocator{};
}

void release_scalar_value(const ScalarData &value) noexcept;

template <typename T>
[[nodiscard]] ScalarData make_scalar_source(const typename scalar_storage_traits<T>::value_type &value)
    noexcept(scalar_source_is_nothrow<T>());

template <typename T, typename Source>
[[nodiscard]] OwnedScalarData make_scalar_value_from_value(Source &&source, allocator *memory) {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    if constexpr (scalar_uses_shared_handle<T>) {
        auto storage = Stored{};
        auto value_memory = scalar_value_allocator(memory);
        Traits::construct(std::addressof(storage), std::forward<Source>(source), value_memory);
        return {&cpp_scalar_runtime_type<T>(), reinterpret_cast<std::uintptr_t>(storage),
                ScalarDataMode::shared_value};
    }

    if constexpr (inline_scalar_value<T> && std::same_as<std::remove_cvref_t<Source>, typename Traits::value_type>) {
        auto data = std::uintptr_t{};
        std::memcpy(std::addressof(data), std::addressof(source), sizeof(Stored));
        return {&cpp_scalar_runtime_type<T>(), data, ScalarDataMode::inline_value};
    }

    const auto &runtime_type = cpp_scalar_runtime_type<T>();
    return allocate_scalar_value(runtime_type, memory, [&](void *destination, allocator &resource) {
        Traits::construct(static_cast<Stored *>(destination), std::forward<Source>(source), resource);
    });
}

template <typename T, typename Source>
[[nodiscard]] OwnedScalarData make_scalar_value(Source &&source, allocator *memory) {
    using Traits = scalar_storage_traits<T>;
    using Value = typename Traits::value_type;

    if constexpr (scalar_storage_has_borrow_value<T> && std::constructible_from<Value, Source &&>) {
        Value requested(std::forward<Source>(source));
        return copy_scalar_value<T>(make_scalar_source<T>(requested), memory);
    } else {
        return make_scalar_value_from_value<T>(std::forward<Source>(source), memory);
    }
}

template <typename T>
[[nodiscard]] OwnedScalarData copy_scalar_value(const details::ScalarData &source, allocator *memory) {
    if constexpr (scalar_storage_has_runtime_type_for_copy<T> || scalar_storage_has_borrow_value<T>) {
        const auto &declared_type = cpp_scalar_type<T>();
        if (&declared_type != &source.type() && !declared_type.accepts(source.type())) {
            throw std::invalid_argument{"incompatible scalar type"};
        }
    }

    if constexpr (hierarchical_scalar_declaration<T>) {
        if constexpr (scalar_declaration_traits<T>::generic) {
            const auto *hierarchical = type_cast<HierarchicalScalarType>(&source.type());
            if (hierarchical == nullptr || !hierarchical->is_resolved()) {
                throw std::invalid_argument{"incompatible hierarchical scalar type"};
            }
        } else {
            if (!cpp_scalar_runtime_type<T>().accepts_input(source.runtime_type())) {
                throw std::invalid_argument{"incompatible hierarchical scalar type"};
            }
        }
    }

    if constexpr (scalar_storage_has_runtime_type_for_copy<T>) {
        const auto &runtime_type = scalar_storage_traits<T>::runtime_type_for_copy(source);
        if (&runtime_type == &source.runtime_type()) {
            return copy_scalar_value(source, runtime_type, memory);
        }

        if constexpr (inline_scalar_value<T>) {
            if (&runtime_type == &cpp_scalar_runtime_type<T>()) {
                return make_scalar_value_from_value<T>(scalar_data_value<T>(source), memory);
            }
        }

        return copy_scalar_value(source, runtime_type, memory);
    } else if constexpr (scalar_uses_shared_handle<T>) {
        const auto &runtime_type = cpp_scalar_runtime_type<T>();
        if (!runtime_type.accepts_input(source.runtime_type())) {
            throw std::invalid_argument{"incompatible scalar runtime representations"};
        }

        auto shared_data = std::uintptr_t{};
        if (const auto *shared_type = source.runtime_type().retain_shared(source, shared_data);
            shared_type != nullptr) {
            return {shared_type, shared_data, ScalarDataMode::shared_value};
        }

        auto value = scalar_data_value<T>(source);
        return make_scalar_value<T>(value, memory);
    } else if constexpr (hierarchical_scalar_declaration<T>) {
        return copy_scalar_value(source, memory);
    } else if constexpr (scalar_declaration_traits<T>::generic) {
        if (!cpp_scalar_type<T>().accepts(source.type())) {
            throw std::invalid_argument{"incompatible generic scalar type"};
        }
        return copy_scalar_value(source, memory);
    } else if constexpr (inline_scalar_value<T>) {
        return make_scalar_value_from_value<T>(scalar_data_value<T>(source), memory);
    } else {
        const auto &runtime_type = cpp_scalar_runtime_type<T>();
        return copy_scalar_value(source, runtime_type, memory);
    }
}

[[nodiscard]] inline OwnedScalarData copy_scalar_value(const details::ScalarData &source,
                                                       allocator *memory) {
    const auto &runtime_type = source.runtime_type().runtime_type_for_copy(source);
    return copy_scalar_value(source, runtime_type, memory);
}

template <typename T>
[[nodiscard]] ScalarData make_scalar_source(
    const typename scalar_storage_traits<T>::value_type &value) noexcept(scalar_source_is_nothrow<T>()) {
    if constexpr (scalar_storage_has_borrow_value<T>) {
        return scalar_storage_traits<T>::borrow_value(value);
    } else {
        return {cpp_scalar_runtime_type<T>(), std::addressof(value), ScalarDataMode::source_view};
    }
}

} // namespace bg::details
