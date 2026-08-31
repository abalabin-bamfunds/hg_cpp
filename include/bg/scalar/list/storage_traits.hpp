#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/util/scope_exit.hpp>

#include <memory>

namespace bg::details {

template <typename T> struct scalar_storage_traits<List<T>> {
    using ItemTraits = scalar_storage_traits<T>;
    using stored_type = scalar_list_storage<T>;
    using value_type = List<T>;

    static constexpr bool supports_equality = ItemTraits::supports_equality;
    static constexpr bool supports_hash = ItemTraits::supports_hash;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = ItemTraits::requires_finalization;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value_type{value}; }

    [[nodiscard]] static std::size_t hash(value_type value) noexcept {
        constexpr auto combine_constant = std::size_t{0x9e3779b9};
        auto result = std::size_t{};

        for (auto i = std::size_t{}; i < value.size(); ++i) {
            result ^= ItemTraits::hash(value[i]) + combine_constant + (result << 6) + (result >> 2);
        }

        return result;
    }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (auto i = std::size_t{}; i < value.size(); ++i) {
            destination->items.emplace_back_with(memory,
                                                 [&](auto *item) { ItemTraits::construct(item, value[i], memory); });
        }
        rollback.release();
    }

    template <std::ranges::contiguous_range Range>
        requires(std::convertible_to<std::ranges::range_reference_t<const Range>, typename ItemTraits::value_type> ||
                 scalar_view_source_for<std::ranges::range_reference_t<const Range>, T>)
    static void construct(stored_type *destination, const Range &value, std::pmr::memory_resource &memory) {
        std::construct_at(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto &item : value) {
            destination->items.emplace_back_with(memory, [&](auto *stored) {
                if constexpr (scalar_view_source_for<decltype(item), T>) {
                    item.input_view().copy_construct(stored, cpp_scalar_storage_representation<T>(), memory);
                } else {
                    ItemTraits::construct(stored, item, memory);
                }
            });
        }
        rollback.release();
    }

    static void copy_construct(stored_type *destination, const stored_type &source, std::pmr::memory_resource &memory) {
        construct(destination, read(source), memory);
    }

    static void assign(stored_type &destination, value_type value, std::pmr::memory_resource &memory) {
        replace_collection_storage<List<T>>(destination, value, memory);
    }

    static void destroy(stored_type *value, std::pmr::memory_resource &memory) noexcept {
        value->items.release_with(memory, [&](auto *item) noexcept { ItemTraits::destroy(item, memory); });
        std::destroy_at(value);
    }
};

} // namespace bg::details
