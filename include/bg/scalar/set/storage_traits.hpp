#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/set/type.hpp>

#include <memory>

namespace bg::details {

template <typename T> struct scalar_storage_traits<Set<T>> {
    using ItemTraits = scalar_storage_traits<T>;
    using stored_type = scalar_set_storage<T>;
    using value_type = Set<T>;

    static constexpr bool supports_equality = ItemTraits::supports_equality;
    static constexpr bool supports_hash = ItemTraits::supports_hash;
    static constexpr bool uses_variable_memory = true;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value_type{value}; }

    [[nodiscard]] static std::size_t hash(value_type value) {
        auto result = std::size_t{};
        for (const auto item : value) {
            result ^= ItemTraits::hash(item);
        }

        return result;
    }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &memory) {
        std::construct_at(destination);
        try {
            for (const auto item : value) {
                (void)destination->items.insert_with(
                    item, memory, [&](auto *stored) { ItemTraits::construct(stored, item, memory); },
                    [&](auto *stored) noexcept { ItemTraits::destroy(stored, memory); });
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    template <std::ranges::input_range Range>
        requires std::convertible_to<std::ranges::range_reference_t<const Range>, typename ItemTraits::value_type>
    static void construct(stored_type *destination, const Range &value, std::pmr::memory_resource &memory) {
        std::construct_at(destination);
        try {
            for (const auto &item : value) {
                (void)destination->items.insert_with(
                    item, memory, [&](auto *stored) { ItemTraits::construct(stored, item, memory); },
                    [&](auto *stored) noexcept { ItemTraits::destroy(stored, memory); });
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
        replace_collection_storage<Set<T>>(destination, value, memory);
    }

    static void destroy(stored_type *value, std::pmr::memory_resource &memory) noexcept {
        value->items.release_with(memory, [&](auto *item) noexcept { ItemTraits::destroy(item, memory); });
        std::destroy_at(value);
    }
};

} // namespace bg::details
