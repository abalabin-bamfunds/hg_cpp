#pragma once

#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/handle_traits.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/scalar/storage_traits.hpp>

#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename... T> struct hierarchical_capability_stack {};

template <typename T, typename Stack, bool Hash> struct hierarchical_capability {
    static constexpr bool value =
        Hash ? scalar_handle_traits<T>::supports_hash : scalar_handle_traits<T>::supports_equality;
};

template <typename T, typename... Seen, bool Hash>
struct hierarchical_capability<List<T>, hierarchical_capability_stack<Seen...>, Hash>
    : hierarchical_capability<T, hierarchical_capability_stack<Seen...>, Hash> {};

template <typename T, typename... Seen, bool Hash>
struct hierarchical_capability<Set<T>, hierarchical_capability_stack<Seen...>, Hash>
    : hierarchical_capability<T, hierarchical_capability_stack<Seen...>, Hash> {};

template <typename K, typename V, typename... Seen, bool Hash>
struct hierarchical_capability<Dict<K, V>, hierarchical_capability_stack<Seen...>, Hash> {
    static constexpr bool value = hierarchical_capability<K, hierarchical_capability_stack<Seen...>, Hash>::value &&
                                  hierarchical_capability<V, hierarchical_capability_stack<Seen...>, Hash>::value;
};

template <template <template <typename, auto...> typename> typename Schema, typename... Seen, bool Hash>
struct hierarchical_capability<Bundle<Schema>, hierarchical_capability_stack<Seen...>, Hash> {
  private:
    using Declaration = Bundle<Schema>;
    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    using Next = hierarchical_capability_stack<Seen..., Declaration>;

    template <std::size_t... Index> static consteval bool direct_fields(std::index_sequence<Index...>) {
        return (hierarchical_capability<typename Reflection::template direct_field<Index>::scalar_type, Next,
                                        Hash>::value &&
                ...);
    }

    static consteval bool calculate() {
        if constexpr ((std::same_as<Declaration, Seen> || ...)) {
            return true;
        } else {
            auto result = direct_fields(std::make_index_sequence<Reflection::direct_field_count>{});
            if constexpr (Reflection::has_base) {
                using Base = typename Reflection::base_schema;
                result = result && hierarchical_capability<Bundle<Base::template apply>, Next, Hash>::value;
            }
            return result;
        }
    }

  public:
    static constexpr bool value = calculate();
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_handle_traits<Bundle<Schema>> {
    static constexpr bool supports_equality =
        hierarchical_capability<Bundle<Schema>, hierarchical_capability_stack<>, false>::value;
    static constexpr bool supports_hash =
        hierarchical_capability<Bundle<Schema>, hierarchical_capability_stack<>, true>::value;
};

template <typename Source>
concept hierarchical_scalar_handle = requires(const Source &source) {
    typename std::remove_cvref_t<Source>::scalar_type;
    requires hierarchical_scalar_declaration<typename std::remove_cvref_t<Source>::scalar_type>;
    { scalar_data(source) } -> std::same_as<const ScalarData &>;
};

} // namespace bg::details

namespace bg {

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_storage_traits<Bundle<Schema>> {
    using Declaration = Bundle<Schema>;
    using stored_type = details::scalar_bundle_storage;
    using value_type = Declaration;

    static constexpr bool supports_equality = details::scalar_handle_traits<Declaration>::supports_equality;
    static constexpr bool supports_hash = details::scalar_handle_traits<Declaration>::supports_hash;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value_type{value}; }

    static std::size_t hash(value_type value) noexcept {
        return cpp_scalar_runtime_type<Declaration>().hash(value.storage_);
    }

    static bool equals(value_type left, value_type right) noexcept {
        const auto &runtime_type = cpp_scalar_runtime_type<Declaration>();
        return runtime_type.equals(left.storage_, runtime_type.borrow(right.storage_));
    }

    static void construct(stored_type *destination, value_type value, allocator &memory) {
        cpp_scalar_runtime_type<Declaration>().copy_construct(destination, value.storage_, memory);
    }

    template <details::hierarchical_scalar_handle Source>
    static void construct(stored_type *destination, const Source &source, allocator &memory) {
        const auto &value = details::scalar_data(source);
        const auto &runtime_type = cpp_scalar_runtime_type<Declaration>();
        if (!runtime_type.type().accepts(value.type())) {
            throw std::invalid_argument{"incompatible hierarchical scalar type"};
        }

        runtime_type.construct_from_input(destination, value, memory);
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        cpp_scalar_runtime_type<Declaration>().copy_construct(destination, &source, memory);
    }

    static void assign(stored_type &destination, value_type value, allocator &) {
        const auto &runtime_type = cpp_scalar_runtime_type<Declaration>();
        runtime_type.assign_from_input(&destination, details::ScalarData{runtime_type, value.storage_});
    }

    template <details::hierarchical_scalar_handle Source>
    static void assign(stored_type &destination, const Source &source, allocator &) {
        const auto &value = details::scalar_data(source);
        const auto &runtime_type = cpp_scalar_runtime_type<Declaration>();
        if (!runtime_type.type().accepts(value.type())) {
            throw std::invalid_argument{"incompatible hierarchical scalar type"};
        }

        runtime_type.assign_from_input(&destination, value);
    }

    static void destroy(stored_type *value, allocator &memory) noexcept {
        cpp_scalar_runtime_type<Declaration>().destroy(value, memory);
    }
};

} // namespace bg
