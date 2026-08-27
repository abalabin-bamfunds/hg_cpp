#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/details/value_type.hpp>
#include <bg/scalar/string.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bg {
template <typename T> class List;
}

namespace bg::details {

template <typename T> struct scalar_storage_traits {
    using stored_type = T;
    using value_type = scalar_value_t<T>;

    static constexpr bool supports_equality = std::equality_comparable<value_type>;
    static constexpr bool supports_hash =
        std::same_as<T, EvaluationTime> || requires(value_type value) { std::hash<value_type>{}(value); };

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value; }

    [[nodiscard]] static std::size_t hash(value_type value) noexcept {
        if constexpr (std::same_as<T, EvaluationTime>) {
            return std::hash<EvaluationTime::rep>{}(value.time_since_epoch().count());
        } else {
            return std::hash<value_type>{}(value);
        }
    }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &) {
        std::construct_at(destination, std::move(value));
    }

    static void assign(stored_type &destination, value_type value) { destination = std::move(value); }
};

template <typename T> struct scalar_storage_traits<List<T>> {
    using ItemTraits = scalar_storage_traits<T>;
    using stored_type = std::pmr::vector<typename ItemTraits::stored_type>;
    using value_type = List<T>;

    static constexpr bool supports_equality = ItemTraits::supports_equality;
    static constexpr bool supports_hash = ItemTraits::supports_hash;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept {
        return value_type{value.data(), value.size()};
    }

    [[nodiscard]] static std::size_t hash(value_type value) noexcept {
        constexpr auto combine_constant = std::size_t{0x9e3779b9};
        auto result = std::size_t{};

        for (auto i = std::size_t{}; i < value.size(); ++i) {
            result ^= ItemTraits::hash(value[i]) + combine_constant + (result << 6) + (result >> 2);
        }

        return result;
    }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &memory) {
        std::construct_at(destination, &memory);

        try {
            assign(*destination, value);
        } catch (...) {
            std::destroy_at(destination);
            throw;
        }
    }

    template <std::ranges::contiguous_range Range>
        requires std::convertible_to<std::ranges::range_reference_t<const Range>, typename ItemTraits::value_type>
    static void construct(stored_type *destination, const Range &value, std::pmr::memory_resource &memory) {
        std::construct_at(destination, &memory);

        try {
            destination->reserve(std::ranges::size(value));
            for (const auto &item : value) {
                destination->emplace_back();
                ItemTraits::assign(destination->back(), item);
            }
        } catch (...) {
            std::destroy_at(destination);
            throw;
        }
    }

    static void assign(stored_type &destination, value_type value) {
        destination.clear();
        destination.reserve(value.size());

        for (auto i = std::size_t{}; i < value.size(); ++i) {
            destination.emplace_back();
            ItemTraits::assign(destination.back(), value[i]);
        }
    }
};

template <> struct scalar_storage_traits<string> {
    using stored_type = std::pmr::string;
    using value_type = std::string_view;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value; }

    [[nodiscard]] static std::size_t hash(value_type value) noexcept { return std::hash<value_type>{}(value); }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &memory) {
        std::construct_at(destination, value, &memory);
    }

    static void assign(stored_type &destination, value_type value) { destination.assign(value); }
};

template <typename T> struct scalar_storage_traits<Opaque<T>> {
    using stored_type = T;
    using value_type = T;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;

    [[nodiscard]] static scalar_value_t<Opaque<T>> read(const stored_type &value) noexcept { return value; }

    template <typename Source>
    static void construct(stored_type *destination, Source &&value, std::pmr::memory_resource &) {
        std::construct_at(destination, std::forward<Source>(value));
    }
};

template <typename T> using scalar_stored_t = typename scalar_storage_traits<T>::stored_type;
} // namespace bg::details
