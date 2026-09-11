#pragma once

#include <bg/scalar/details/capability_traits.hpp>
#include <bg/scalar/details/collection_storage.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/util/scope_exit.hpp>

#include <memory>

namespace bg::details {

template <typename T> struct scalar_storage_traits<Set<T>> {
    using ItemTraits = scalar_storage_traits<T>;
    using stored_type = scalar_set_storage<T>;
    using value_type = Set<T>;

    static constexpr bool supports_equality = scalar_capability_traits<T>::supports_equality;
    static constexpr bool supports_hash = scalar_capability_traits<T>::supports_hash;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = ItemTraits::requires_finalization;
    static constexpr bool assign_uses_memory_resource = true;

    static value_type read(const stored_type &value) noexcept { return value_type{value}; }

    static std::size_t hash(value_type value) {
        auto result = std::size_t{};
        for (const auto item : value) {
            result ^= ItemTraits::hash(item);
        }

        return result;
    }

    static void construct(stored_type *destination, value_type value, allocator &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto item : value) {
            (void)destination->items.insert_with(
                item, memory.memory_resource(), [&](auto *stored) { ItemTraits::construct(stored, item, memory); },
                [&](auto *stored) noexcept { ItemTraits::destroy(stored, memory); });
        }
        rollback.release();
    }

    template <std::ranges::input_range Range>
        requires std::convertible_to<std::ranges::range_reference_t<const Range>, typename ItemTraits::value_type>
    static void construct(stored_type *destination, const Range &value, allocator &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto &item : value) {
            (void)destination->items.insert_with(
                item, memory.memory_resource(), [&](auto *stored) { ItemTraits::construct(stored, item, memory); },
                [&](auto *stored) noexcept { ItemTraits::destroy(stored, memory); });
        }
        rollback.release();
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        construct(destination, read(source), memory);
    }

    static void assign(stored_type &destination, value_type value, allocator &memory) {
        replace_collection_storage<Set<T>>(destination, value, memory);
    }

    static void destroy(stored_type *value, allocator &memory) noexcept {
        value->items.release_with(memory.memory_resource(), [&](auto *item) noexcept { ItemTraits::destroy(item, memory); });
        std::destroy_at(value);
    }
};

template <typename T> struct scalar_capability_traits<Set<T>> {
    static constexpr bool supports_equality = scalar_capability_traits<T>::supports_equality;
    static constexpr bool supports_hash = scalar_capability_traits<T>::supports_hash;
};

} // namespace bg::details
