#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::util {
namespace details {

template <typename Tag> constexpr std::uintptr_t tag_value(Tag tag) noexcept {
    if constexpr (std::is_enum_v<Tag>) {
        return static_cast<std::uintptr_t>(std::to_underlying(tag));
    } else {
        return static_cast<std::uintptr_t>(tag);
    }
}

template <typename T, typename... Types> struct type_index;

template <typename T, typename... Types> struct type_index<T, T, Types...> : std::integral_constant<std::size_t, 0> {};

template <typename T, typename First, typename... Types>
struct type_index<T, First, Types...> : std::integral_constant<std::size_t, 1 + type_index<T, Types...>::value> {};

template <typename T, typename... Types> inline constexpr auto one_of = (std::same_as<T, Types> || ...);

template <typename... Visitors> struct overloaded : Visitors... {
    using Visitors::operator()...;
};

template <typename... Visitors> overloaded(Visitors...) -> overloaded<Visitors...>;

template <typename... Types> inline constexpr auto all_distinct = true;

template <typename T, typename... Types>
inline constexpr auto all_distinct<T, Types...> = (!std::same_as<T, Types> && ...) && all_distinct<Types...>;

template <typename First, typename... Types>
inline constexpr auto minimum_alignment = [] {
    auto result = alignof(First);
    ((result = std::min(result, alignof(Types))), ...);
    return result;
}();

template <std::size_t Alignment, std::size_t TagBits, typename Tag> class tagged_pointer_bits {
  public:
    static_assert(Alignment != 0 && std::has_single_bit(Alignment));
    static_assert(TagBits <= std::countr_zero(Alignment));
    static_assert(std::integral<Tag> || std::is_enum_v<Tag>);

    static constexpr auto tag_mask = TagBits == 0 ? std::uintptr_t{} : (std::uintptr_t{1} << TagBits) - 1;
    static constexpr auto pointer_mask = ~tag_mask;

    constexpr tagged_pointer_bits() noexcept = default;
    constexpr tagged_pointer_bits(std::nullptr_t) noexcept {}

    template <typename T> constexpr tagged_pointer_bits(T *pointer, Tag tag = {}) noexcept { reset(pointer, tag); }

    template <typename T> constexpr T *get() const noexcept {
        return reinterpret_cast<T *>(bits_ & pointer_mask);
    }

    constexpr Tag tag() const noexcept { return static_cast<Tag>(bits_ & tag_mask); }
    constexpr std::uintptr_t raw() const noexcept { return bits_; }
    constexpr explicit operator bool() const noexcept { return (bits_ & pointer_mask) != 0; }

    template <typename T> constexpr void reset(T *pointer, Tag tag = {}) noexcept {
        if (pointer == nullptr) {
            clear();
            return;
        }

        const auto pointer_bits = reinterpret_cast<std::uintptr_t>(pointer);
        const auto encoded_tag = tag_value(tag);
        assert((pointer_bits & tag_mask) == 0);
        assert((encoded_tag & ~tag_mask) == 0);
        bits_ = pointer_bits | encoded_tag;
    }

    template <typename T> constexpr void reset_pointer(T *pointer) noexcept { reset(pointer, tag()); }
    constexpr void reset_tag(Tag tag) noexcept { reset(get<void>(), tag); }
    constexpr void clear() noexcept { bits_ = 0; }

    bool operator==(const tagged_pointer_bits &) const = default;

  private:
    std::uintptr_t bits_{};
};

} // namespace details

template <typename T, std::size_t TagBits = 1, typename Tag = std::uintptr_t> class tagged_ptr {
  public:
    static_assert(std::is_object_v<T>);

    constexpr tagged_ptr() noexcept = default;
    constexpr tagged_ptr(std::nullptr_t) noexcept {}
    constexpr tagged_ptr(T *pointer, Tag tag = {}) noexcept : value_{pointer, tag} {}

    constexpr T *get() const noexcept { return value_.template get<T>(); }
    constexpr T &operator*() const noexcept { return *get(); }
    constexpr T *operator->() const noexcept { return get(); }
    constexpr Tag tag() const noexcept { return value_.tag(); }
    constexpr std::uintptr_t raw() const noexcept { return value_.raw(); }
    constexpr explicit operator bool() const noexcept { return static_cast<bool>(value_); }

    constexpr void reset(T *pointer = nullptr, Tag tag = {}) noexcept { value_.reset(pointer, tag); }
    constexpr void reset_pointer(T *pointer) noexcept { value_.reset_pointer(pointer); }
    constexpr void reset_tag(Tag tag) noexcept { value_.reset_tag(tag); }

    bool operator==(const tagged_ptr &) const = default;

  private:
    details::tagged_pointer_bits<alignof(T), TagBits, Tag> value_;
};

template <std::size_t Alignment, typename... Types> class variant_ptr_with_alignment {
    static_assert(sizeof...(Types) != 0);
    static_assert((std::is_object_v<Types> && ...));
    static_assert(details::all_distinct<Types...>);

    static constexpr auto alternative_count = sizeof...(Types);
    static constexpr auto tag_bits = alternative_count == 1 ? std::size_t{} : std::bit_width(alternative_count - 1);
    using Storage = details::tagged_pointer_bits<Alignment, tag_bits, std::uintptr_t>;

  public:
    static constexpr auto npos = static_cast<std::size_t>(-1);

    constexpr variant_ptr_with_alignment() noexcept = default;
    constexpr variant_ptr_with_alignment(std::nullptr_t) noexcept {}

    template <typename T>
        requires details::one_of<T, Types...>
    constexpr variant_ptr_with_alignment(T *pointer) noexcept {
        reset(pointer);
    }

    template <typename T>
        requires details::one_of<T, Types...>
    constexpr variant_ptr_with_alignment &operator=(T *pointer) noexcept {
        reset(pointer);
        return *this;
    }

    constexpr variant_ptr_with_alignment &operator=(std::nullptr_t) noexcept {
        clear();
        return *this;
    }

    constexpr std::size_t index() const noexcept {
        return *this ? static_cast<std::size_t>(value_.tag()) : npos;
    }

    template <typename T>
        requires details::one_of<T, Types...>
    constexpr bool holds() const noexcept {
        return *this && index() == details::type_index<T, Types...>::value;
    }

    template <typename T>
        requires details::one_of<T, Types...>
    constexpr T *get() const noexcept {
        return holds<T>() ? value_.template get<T>() : nullptr;
    }

    template <typename T>
        requires details::one_of<T, Types...>
    constexpr void reset(T *pointer) noexcept {
        if (pointer == nullptr) {
            clear();
            return;
        }
        value_.reset(pointer, details::type_index<T, Types...>::value);
    }

    constexpr void clear() noexcept { value_.clear(); }
    constexpr std::uintptr_t raw() const noexcept { return value_.raw(); }
    constexpr explicit operator bool() const noexcept { return static_cast<bool>(value_); }

    template <typename Visitor> constexpr decltype(auto) visit(Visitor &&visitor) const {
        assert(*this);
        return visit<0>(index(), std::forward<Visitor>(visitor));
    }

    template <typename First, typename Second, typename... Rest>
    constexpr decltype(auto) visit(First &&first, Second &&second, Rest &&...rest) const {
        return visit(details::overloaded{std::forward<First>(first), std::forward<Second>(second),
                                         std::forward<Rest>(rest)...});
    }

    bool operator==(const variant_ptr_with_alignment &) const = default;

  private:
    template <std::size_t Index, typename Visitor>
    constexpr decltype(auto) visit(std::size_t active, Visitor &&visitor) const {
        using T = std::tuple_element_t<Index, std::tuple<Types...>>;
        if constexpr (Index + 1 == alternative_count) {
            assert(active == Index);
            return std::invoke(std::forward<Visitor>(visitor), value_.template get<T>());
        } else {
            if (active == Index) {
                return std::invoke(std::forward<Visitor>(visitor), value_.template get<T>());
            }
            return visit<Index + 1>(active, std::forward<Visitor>(visitor));
        }
    }

    Storage value_;
};

template <typename... Types>
using variant_ptr = variant_ptr_with_alignment<details::minimum_alignment<Types...>, Types...>;

static_assert(sizeof(details::tagged_pointer_bits<alignof(void *), 1, std::uintptr_t>) == sizeof(void *));

} // namespace bg::util
