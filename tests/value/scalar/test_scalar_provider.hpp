#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/bundle/scalar.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/type/scalar_type.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>

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
    operator value() const noexcept { return value{get()}; }
};

struct explicit_value_source {
    int number{};

    explicit operator value_view() const noexcept { return {std::addressof(number)}; }
};

struct layout_value {
    int number{};
};

struct copy_only_value {
    int number{};
};

struct copy_only_storage {
    int number{};
};

struct copy_only_runtime_storage {
    std::array<std::uint32_t, 4> prefix{};
    copy_only_value value;
};

template <template <typename, auto...> typename Field> struct bundle_provider_base {
    Field<bg::i64> amount;
};

template <template <typename, auto...> typename Field>
struct bundle_provider_derived : bg::schema_extends<bundle_provider_base, Field> {
    Field<bg::i64> extra;
};

using bundle_provider_base_type = bg::Bundle<bundle_provider_base>;
using bundle_provider_derived_type = bg::Bundle<bundle_provider_derived>;

struct bundle_provider_view {
    bg::Scalar<bundle_provider_base_type> value;
};

struct bundle_provider_value {};

using generic_bundle_provider_type = bg::Bundle<bg::SCHEMA>;

struct generic_bundle_provider_view {
    bg::Scalar<generic_bundle_provider_type> value;
};

struct generic_bundle_provider_value {};

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
int copy_runtime_type_call_count() noexcept;
void record_copy_runtime_type_call() noexcept;
int generic_runtime_type_call_count() noexcept;
void record_generic_runtime_type_call() noexcept;
int generic_copy_runtime_type_call_count() noexcept;
void record_generic_copy_runtime_type_call() noexcept;
const bg::ScalarType &compound_semantic_type_from_provider();
const bg::ScalarType &layout_semantic_type_from_provider();
const bg::ScalarRuntimeType &mismatched_layout_runtime_type();
const bg::ScalarType &copy_only_semantic_type() noexcept;
const bg::ScalarRuntimeType &preserving_copy_only_runtime_type();

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
        if (source.mode() == ScalarDataMode::source_view) {
            return *static_cast<const value_type *>(source.data());
        }
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
        return read(*static_cast<const stored_type *>(source.data()));
    }

    static details::ScalarData borrow_value(const value_type &value) {
        if (value.number == nullptr) {
            throw std::invalid_argument{"a provider value view must have a source"};
        }
        return {cpp_scalar_runtime_type<test_scalar_provider::value>(), std::addressof(value),
                ScalarDataMode::source_view};
    }

    static const ScalarRuntimeType &runtime_type(const ScalarType &type) {
        if (&type != &cpp_scalar_type<test_scalar_provider::value>()) {
            throw std::invalid_argument{"unexpected scalar type"};
        }
        return test_scalar_provider::preserving_runtime_type();
    }

    static value_type read(const stored_type &stored) noexcept { return {std::addressof(stored.number)}; }

    static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source) {
        test_scalar_provider::record_copy_runtime_type_call();
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

template <> struct scalar_type_traits<test_scalar_provider::layout_value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<test_scalar_provider::layout_value> {
    using stored_type = int;
    using value_type = test_scalar_provider::layout_value;

    static const ScalarRuntimeType &runtime_type(const ScalarType &type);
};

template <> struct scalar_type_traits<test_scalar_provider::copy_only_value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<test_scalar_provider::copy_only_value> {
    using stored_type = test_scalar_provider::copy_only_storage;
    using value_type = test_scalar_provider::copy_only_value;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &stored) noexcept { return {stored.number}; }

    static void construct(stored_type *destination, value_type value, allocator &) {
        std::construct_at(destination, stored_type{value.number});
    }

    static void assign(stored_type &destination, value_type value, allocator &) noexcept {
        destination.number = value.number;
    }

    static void destroy(stored_type *stored, allocator &) noexcept { std::destroy_at(stored); }

    static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source);
};

template <> struct scalar_type_traits<test_scalar_provider::bundle_provider_value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<test_scalar_provider::bundle_provider_value> {
    using stored_type = details::scalar_bundle_storage;
    using value_type = test_scalar_provider::bundle_provider_view;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &stored) noexcept {
        return {Scalar<test_scalar_provider::bundle_provider_base_type>::from(
            cpp_scalar_runtime_type<test_scalar_provider::bundle_provider_base_type>(), &stored)};
    }

    static value_type read_value(const details::ScalarData &source) {
        const auto erased = Scalar<>::from(source.runtime_type(), source.data());
        return {Scalar<test_scalar_provider::bundle_provider_base_type>::from(erased)};
    }

    static std::size_t hash(const value_type &value) noexcept { return value.value.hash(); }

    static bool equals(const value_type &left, const value_type &right) noexcept {
        return left.value.equals(right.value);
    }

    static details::ScalarData borrow_value(const value_type &value) noexcept {
        return details::scalar_data(value.value);
    }

    static const ScalarRuntimeType &runtime_type(const ScalarType &type) {
        if (&type != &cpp_scalar_type<test_scalar_provider::bundle_provider_value>()) {
            throw std::invalid_argument{"unexpected scalar type"};
        }
        return cpp_scalar_runtime_type<test_scalar_provider::bundle_provider_base_type>();
    }

    static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source) {
        return source.runtime_type();
    }
};

template <> struct scalar_type_traits<test_scalar_provider::generic_bundle_provider_value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_declaration_traits<test_scalar_provider::generic_bundle_provider_value> {
    static constexpr bool generic = true;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = true;
};

template <> struct scalar_storage_traits<test_scalar_provider::generic_bundle_provider_value> {
    using stored_type = details::scalar_bundle_storage;
    using value_type = test_scalar_provider::generic_bundle_provider_view;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static details::ScalarData borrow_value(const value_type &value) noexcept {
        return details::scalar_data(value.value);
    }

    static const ScalarRuntimeType &runtime_type(const ScalarType &) {
        test_scalar_provider::record_generic_runtime_type_call();
        throw std::invalid_argument{"a generic bundle provider has no canonical runtime"};
    }

    static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source) {
        test_scalar_provider::record_generic_copy_runtime_type_call();
        return source.runtime_type();
    }
};

extern template const ScalarRuntimeType &cpp_scalar_runtime_type<test_scalar_provider::value>();

} // namespace bg
