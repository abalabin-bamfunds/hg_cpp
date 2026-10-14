#pragma once

#include <bg/scalar/details/handle_traits.hpp>
#include <bg/scalar/details/collection_storage.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <memory>
#include <utility>

namespace bg {

template <typename K, typename V> struct scalar_storage_traits<Dict<K, V>> {
    using KeyTraits = scalar_storage_traits<K>;
    using ValueTraits = scalar_storage_traits<V>;
    using stored_type = details::scalar_dict_storage<K, V>;
    using value_type = Dict<K, V>;

    static constexpr bool supports_equality =
        details::scalar_handle_traits<K>::supports_equality && details::scalar_handle_traits<V>::supports_equality;
    static constexpr bool supports_hash =
        details::scalar_handle_traits<K>::supports_hash && details::scalar_handle_traits<V>::supports_hash;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization =
        KeyTraits::requires_finalization || ValueTraits::requires_finalization;
    static constexpr bool assign_uses_memory_resource = true;

    static value_type read(const stored_type &value) noexcept { return value_type{value}; }

    static std::size_t hash(value_type value) {
        auto result = std::size_t{};
        for (const auto [key, item] : value) {
            result ^= KeyTraits::hash(key) ^ (ValueTraits::hash(item) << 1);
        }

        return result;
    }

    static bool equals(value_type left, value_type right) { return left == right; }

    static void construct(stored_type *destination, value_type value, allocator &memory) {
        copy_construct(destination, value.storage(), memory);
    }

    template <std::ranges::input_range Range>
        requires requires(std::ranges::range_reference_t<const Range> entry) {
            { entry.first } -> std::convertible_to<typename KeyTraits::value_type>;
        } && (details::scalar_view_source_for<
                  decltype(std::declval<std::ranges::range_reference_t<const Range>>().second), V> ||
              requires(typename ValueTraits::stored_type *destination,
                       decltype(std::declval<std::ranges::range_reference_t<const Range>>().second) item,
                       allocator &memory) { ValueTraits::construct(destination, item, memory); })
    static void construct(stored_type *destination, const Range &value, allocator &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto &[key, item] : value) {
            (void)destination->items.try_emplace_with(
                key, memory.memory_resource(), [&](auto *stored) { KeyTraits::construct(stored, key, memory); },
                [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
                [&](auto *stored) {
                    if constexpr (requires { ValueTraits::construct(stored, item, memory); }) {
                        ValueTraits::construct(stored, item, memory);
                    } else {
                        details::scalar_data(item).copy_construct(stored, cpp_scalar_runtime_type<V>(), memory);
                    }
                });
        }
        rollback.release();
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        const auto &key_runtime_type = cpp_scalar_runtime_type<K>();
        const auto &value_runtime_type = cpp_scalar_runtime_type<V>();
        for (const auto [key, value] : source.items) {
            (void)destination->items.try_emplace_with(
                KeyTraits::read(key), memory.memory_resource(),
                [&](auto *stored) { key_runtime_type.copy_construct(stored, std::addressof(key), memory); },
                [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
                [&](auto *stored) { value_runtime_type.copy_construct(stored, std::addressof(value), memory); });
        }
        rollback.release();
    }

    static void assign(stored_type &destination, value_type value, allocator &memory) {
        details::replace_collection_storage<Dict<K, V>>(destination, value, memory);
    }

    static void destroy(stored_type *value, allocator &memory) noexcept {
        value->items.release_with(
            memory.memory_resource(), [&](auto *key) noexcept { KeyTraits::destroy(key, memory); },
            [&](auto *item) noexcept { ValueTraits::destroy(item, memory); });
        std::destroy_at(value);
    }
};

} // namespace bg

namespace bg::details {

template <typename K, typename V> struct scalar_handle_traits<Dict<K, V>> {
    static constexpr bool supports_equality =
        scalar_handle_traits<K>::supports_equality && scalar_handle_traits<V>::supports_equality;
    static constexpr bool supports_hash =
        scalar_handle_traits<K>::supports_hash && scalar_handle_traits<V>::supports_hash;
};

} // namespace bg::details
