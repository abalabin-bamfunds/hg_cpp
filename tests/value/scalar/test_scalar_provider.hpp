#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>

#include <array>
#include <memory>

namespace test_scalar_provider {

struct value_view;

struct value {
    int number;

    operator value_view() const noexcept;

    bool operator==(const value &) const noexcept = default;
};

struct storage {
    int number;
};

struct large_source_view {
    std::array<const void *, 4> padding{};
    const int *number{};
};

struct value_view {
    const int *number{};

    int get() const noexcept { return number == nullptr ? 0 : *number; }
};

inline value::operator value_view() const noexcept {
    return {std::addressof(number)};
}

struct compound_value {
    bg::i64 quantity;
    double price;
};

const bg::ScalarType &semantic_type_from_provider() noexcept;
const bg::ScalarRuntimeType &runtime_type_from_provider();
const bg::ScalarRuntimeType &preserving_runtime_type();
const bg::ScalarRuntimeType &canonicalizing_runtime_type();
int alternative_runtime_read_count();
const bg::ScalarType &compound_semantic_type_from_provider();

} // namespace test_scalar_provider

namespace bg {

template <> struct scalar_type_traits<test_scalar_provider::value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<test_scalar_provider::value> {
    using stored_type = test_scalar_provider::storage;
    using value_type = test_scalar_provider::value_view;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read_value(const details::ScalarData &source) {
        if (&source.runtime_type() == &test_scalar_provider::preserving_runtime_type() ||
            &source.runtime_type() == &test_scalar_provider::canonicalizing_runtime_type()) {
            alignas(test_scalar_provider::large_source_view)
                std::byte storage[sizeof(test_scalar_provider::large_source_view)];
            source.runtime_type().read_value(source.mode(), source.data_word(), storage);
            auto *view = std::launder(reinterpret_cast<test_scalar_provider::large_source_view *>(storage));
            const auto result = value_type{view->number};
            std::destroy_at(view);
            return result;
        }
        if (source.mode() == ScalarDataMode::source_view) {
            return *static_cast<const value_type *>(source.data());
        }
        return read(*static_cast<const stored_type *>(source.data()));
    }

    static value_type read(const stored_type &stored) noexcept { return {std::addressof(stored.number)}; }

    static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source) {
        if (&source.runtime_type() == &test_scalar_provider::preserving_runtime_type()) {
            return source.runtime_type();
        }
        return cpp_scalar_runtime_type<test_scalar_provider::value>();
    }

    static bool equals(value_type left, value_type right) noexcept { return left.get() == right.get(); }

    static void construct(stored_type *destination, test_scalar_provider::value source, allocator &) {
        std::construct_at(destination, source.number);
    }

    static void construct(stored_type *destination, value_type source, allocator &) {
        std::construct_at(destination, source.get());
    }

    static void assign(stored_type &destination, value_type source, allocator &) noexcept {
        destination.number = source.get();
    }

    static void destroy(stored_type *stored, allocator &) noexcept { std::destroy_at(stored); }
};

extern template const ScalarRuntimeType &cpp_scalar_runtime_type<test_scalar_provider::value>();

} // namespace bg
