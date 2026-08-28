#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/dict/type.hpp>

#include <memory>
#include <utility>

namespace bg::details {

template <typename K, typename V> struct scalar_storage_traits<Dict<K, V>> {
    using KeyTraits = scalar_storage_traits<K>;
    using ValueTraits = scalar_storage_traits<V>;
    using stored_type = scalar_dict_storage<K, V>;
    using value_type = Dict<K, V>;

    static constexpr bool supports_equality = KeyTraits::supports_equality && ValueTraits::supports_equality;
    static constexpr bool supports_hash = KeyTraits::supports_hash && ValueTraits::supports_hash;
    static constexpr bool uses_variable_memory = true;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value_type{value}; }

    [[nodiscard]] static std::size_t hash(value_type value) {
        auto result = std::size_t{};
        for (const auto [key, item] : value) {
            result ^= KeyTraits::hash(key) ^ (ValueTraits::hash(item) << 1);
        }

        return result;
    }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &memory) {
        std::construct_at(destination);
        try {
            for (const auto [key, item] : value) {
                (void)destination->items.try_emplace_with(
                    key, memory, [&](auto *stored) { KeyTraits::construct(stored, key, memory); },
                    [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
                    [&](auto *stored) { ValueTraits::construct(stored, item, memory); });
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    template <std::ranges::input_range Range>
        requires requires(std::ranges::range_reference_t<const Range> entry) {
            { entry.first } -> std::convertible_to<typename KeyTraits::value_type>;
        } && (std::convertible_to<decltype(std::declval<std::ranges::range_reference_t<const Range>>().second),
                                  typename ValueTraits::value_type> ||
              scalar_view_source_for<decltype(std::declval<std::ranges::range_reference_t<const Range>>().second), V>)
    static void construct(stored_type *destination, const Range &value, std::pmr::memory_resource &memory) {
        std::construct_at(destination);
        try {
            for (const auto &[key, item] : value) {
                (void)destination->items.try_emplace_with(
                    key, memory, [&](auto *stored) { KeyTraits::construct(stored, key, memory); },
                    [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
                    [&](auto *stored) {
                        if constexpr (scalar_view_source_for<decltype(item), V>) {
                            item.input_view().copy_construct(stored, cpp_scalar_storage_representation<V>(), memory);
                        } else {
                            ValueTraits::construct(stored, item, memory);
                        }
                    });
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    static void copy_construct(stored_type *destination, const stored_type &source, std::pmr::memory_resource &memory) {
        construct(destination, read(source), memory);
    }

    static void assign(stored_type &destination, value_type value, std::pmr::memory_resource &memory) {
        replace_collection_storage<Dict<K, V>>(destination, value, memory);
    }

    static void destroy(stored_type *value, std::pmr::memory_resource &memory) noexcept {
        value->items.release_with(
            memory, [&](auto *key) noexcept { KeyTraits::destroy(key, memory); },
            [&](auto *item) noexcept { ValueTraits::destroy(item, memory); });
        std::destroy_at(value);
    }
};

} // namespace bg::details
