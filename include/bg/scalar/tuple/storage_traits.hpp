#pragma once

#include <bg/scalar/data.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>

#include <cstdint>
#include <type_traits>

namespace bg {

template <typename T> class Scalar;

template <typename... Items> struct scalar_storage_traits<Tuple<Items...>> {
    using stored_type = std::uintptr_t;
    using value_type = Scalar<Tuple<Items...>>;

    static constexpr bool supports_equality = details::scalar_handle_traits<Tuple<Items...>>::supports_equality;
    static constexpr bool supports_hash = details::scalar_handle_traits<Tuple<Items...>>::supports_hash;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_shared_handle = true;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = true;

    static value_type read(const stored_type &value);
    static value_type read_value(const ScalarData &value);
    static std::size_t hash(value_type value);
    static bool equals(value_type left, value_type right);
    static void construct(stored_type *destination, value_type value, allocator &memory);
    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory);
    static void assign(stored_type &destination, value_type value, allocator &memory);
    static void destroy(stored_type *value, allocator &memory) noexcept;
};

} // namespace bg

static_assert(sizeof(bg::scalar_storage_traits<bg::Tuple<int>>::stored_type) == sizeof(void *));
