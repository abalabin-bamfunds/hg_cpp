#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>

namespace test_scalar_provider {

struct value {
    int number;

    bool operator==(const value &) const noexcept = default;
};

struct storage {
    int number;
};

struct compound_value {
    bg::i64 quantity;
    double price;
};

const bg::ScalarType &semantic_type_from_provider() noexcept;
const bg::ScalarRuntimeType &runtime_type_from_provider();
const bg::ScalarType &compound_semantic_type_from_provider();

} // namespace test_scalar_provider

namespace bg {

template <> struct scalar_type_traits<test_scalar_provider::value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<test_scalar_provider::value> {
    using stored_type = test_scalar_provider::storage;
    using value_type = test_scalar_provider::value;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &stored) noexcept { return {stored.number}; }

    static bool equals(value_type left, value_type right) noexcept { return left == right; }

    static void construct(stored_type *destination, value_type source, allocator &) {
        std::construct_at(destination, source.number);
    }

    static void assign(stored_type &destination, value_type source, allocator &) noexcept {
        destination.number = source.number;
    }

    static void destroy(stored_type *stored, allocator &) noexcept { std::destroy_at(stored); }
};

extern template const ScalarRuntimeType &cpp_scalar_runtime_type<test_scalar_provider::value>();

} // namespace bg
