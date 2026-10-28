#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/collection_storage.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/tuple/runtime_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>

#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg {

template <typename T> class Scalar;

namespace details {

template <typename T> struct is_tuple_source : std::false_type {};

template <typename... Items> struct is_tuple_source<std::tuple<Items...>> : std::true_type {};

template <typename First, typename Second> struct is_tuple_source<std::pair<First, Second>> : std::true_type {};

template <typename T> struct tuple_scalar_items {};

template <typename... Items> struct tuple_scalar_items<Tuple<Items...>> {
    using type = std::tuple<Items...>;
};

template <typename Target, typename Source> consteval bool tuple_item_source_matches();

template <typename Tuple, typename Source, std::size_t... Index>
consteval bool tuple_source_matches(std::index_sequence<Index...>) {
    if constexpr (!is_tuple_source<std::remove_cvref_t<Source>>::value) {
        return false;
    } else if constexpr (std::tuple_size_v<std::remove_cvref_t<Source>> != sizeof...(Index)) {
        return false;
    } else {
        return (tuple_item_source_matches<
                    std::tuple_element_t<Index, Tuple>,
                    decltype(std::get<Index>(std::declval<const Source &>()))>() && ...);
    }
}

template <typename Target, typename Source> consteval bool tuple_item_source_matches() {
    if constexpr (scalar_view_source_for<Source, Target> || scalar_source_for<Target, Source>) {
        return true;
    } else if constexpr (std::same_as<std::remove_cvref_t<Source>,
                                      std::remove_cvref_t<typename scalar_storage_traits<Target>::value_type>>) {
        return true;
    } else if constexpr (requires { typename tuple_scalar_items<Target>::type; } &&
                         is_tuple_source<std::remove_cvref_t<Source>>::value) {
        using Items = typename tuple_scalar_items<Target>::type;
        return tuple_source_matches<Items, Source>(std::make_index_sequence<std::tuple_size_v<Items>>{});
    } else {
        return false;
    }
}

template <typename Target, typename Source>
void construct_tuple_item(typename scalar_storage_traits<Target>::stored_type *destination, Source &&source,
                          allocator &memory) {
    using Traits = scalar_storage_traits<Target>;

    if constexpr (scalar_view_source_for<Source, Target>) {
        scalar_data(source).copy_construct(destination, cpp_scalar_runtime_type<Target>(), memory);
    } else if constexpr (scalar_source_for<Target, Source>) {
        Traits::construct(destination, scalar_source_value<Target>(std::forward<Source>(source)), memory);
    } else {
        Traits::construct(destination, std::forward<Source>(source), memory);
    }
}

template <typename Tuple, typename Source>
concept tuple_source_for =
    tuple_source_matches<Tuple, std::remove_cvref_t<Source>>(
        std::make_index_sequence<std::tuple_size_v<Tuple>>{});

} // namespace details

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

    template <typename Source>
        requires details::tuple_source_for<std::tuple<Items...>, Source>
    static void construct(stored_type *destination, const Source &value, allocator &memory) {
        struct Context {
            const Source *value;
        } context{std::addressof(value)};

        static_cast<const ScalarTupleRuntimeType &>(cpp_scalar_runtime_type<Tuple<Items...>>()).construct_fields_with(
            destination, memory,
            {&context, [](void *opaque_context, std::size_t index, const ScalarRuntimeType &, void *item_destination,
                          allocator &item_memory) {
                const auto &value = *static_cast<const Context *>(opaque_context)->value;
                return construct_field(value, index, item_destination, item_memory,
                                       std::make_index_sequence<sizeof...(Items)>{});
            }});
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory);
    static void assign(stored_type &destination, value_type value, allocator &memory);
    static void destroy(stored_type *value, allocator &memory) noexcept;

  private:
    template <std::size_t Index, typename Source>
    static bool construct_field_at(const Source &value, std::size_t index, void *destination, allocator &memory) {
        if (index != Index) {
            return false;
        }

        using Item = std::tuple_element_t<Index, std::tuple<Items...>>;
        details::construct_tuple_item<Item>(static_cast<typename scalar_storage_traits<Item>::stored_type *>(
                                                destination),
                                            std::get<Index>(value), memory);
        return true;
    }

    template <typename Source, std::size_t... Index>
    static bool construct_field(const Source &value, std::size_t index, void *destination, allocator &memory,
                                std::index_sequence<Index...>) {
        return (construct_field_at<Index>(value, index, destination, memory) || ...);
    }
};

} // namespace bg

static_assert(sizeof(bg::scalar_storage_traits<bg::Tuple<int>>::stored_type) == sizeof(void *));
