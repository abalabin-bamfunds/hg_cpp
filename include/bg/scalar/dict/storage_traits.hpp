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

    static void construct(stored_type *destination, value_type value, allocator &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto [key, item] : value) {
            (void)destination->items.try_emplace_with(
                key, memory.memory_resource(), [&](auto *stored) { KeyTraits::construct(stored, key, memory); },
                [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
                [&](auto *stored) { ValueTraits::construct(stored, item, memory); });
        }
        rollback.release();
    }

    template <std::ranges::input_range Range>
        requires requires(std::ranges::range_reference_t<const Range> entry) {
            { entry.first } -> std::convertible_to<typename KeyTraits::value_type>;
        } && (std::convertible_to<decltype(std::declval<std::ranges::range_reference_t<const Range>>().second),
                                  typename ValueTraits::value_type> ||
              details::scalar_view_source_for<
                  decltype(std::declval<std::ranges::range_reference_t<const Range>>().second), V>)
    static void construct(stored_type *destination, const Range &value, allocator &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto &[key, item] : value) {
            (void)destination->items.try_emplace_with(
                key, memory.memory_resource(), [&](auto *stored) { KeyTraits::construct(stored, key, memory); },
                [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
                [&](auto *stored) {
                    if constexpr (details::scalar_view_source_for<decltype(item), V>) {
                        details::scalar_data(item).copy_construct(stored, cpp_scalar_runtime_type<V>(), memory);
                    } else {
                        ValueTraits::construct(stored, item, memory);
                    }
                });
        }
        rollback.release();
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        construct(destination, read(source), memory);
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
