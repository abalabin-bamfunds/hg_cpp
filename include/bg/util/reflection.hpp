#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::util {

namespace detail {

inline constexpr std::size_t maximum_aggregate_members = 32;

struct aggregate_argument {
    template <typename T> constexpr operator T() const noexcept;
};

template <typename T, std::size_t... Index> consteval bool aggregate_initializable(std::index_sequence<Index...>) {
    return requires { T{(static_cast<void>(Index), aggregate_argument{})...}; };
}

template <typename T, std::size_t Count = maximum_aggregate_members + 1>
consteval std::size_t count_aggregate_members() {
    if constexpr (aggregate_initializable<T>(std::make_index_sequence<Count>{})) {
        return Count;
    } else if constexpr (Count > 0) {
        return count_aggregate_members<T, Count - 1>();
    } else {
        return 0;
    }
}

template <std::size_t Count> struct aggregate_tie;

template <> struct aggregate_tie<0> {
    template <typename T> static constexpr auto apply(T &) noexcept { return std::tuple{}; }
};

#define BG_DETAIL_AGGREGATE_TIE(Count, ...)                                                                            \
    template <> struct aggregate_tie<Count> {                                                                          \
        template <typename T> static constexpr auto apply(T &value) noexcept {                                         \
            auto &[__VA_ARGS__] = value;                                                                               \
            return std::tie(__VA_ARGS__);                                                                              \
        }                                                                                                              \
    };

#define BG_DETAIL_MEMBERS_1 m0
#define BG_DETAIL_MEMBERS_2 BG_DETAIL_MEMBERS_1, m1
#define BG_DETAIL_MEMBERS_3 BG_DETAIL_MEMBERS_2, m2
#define BG_DETAIL_MEMBERS_4 BG_DETAIL_MEMBERS_3, m3
#define BG_DETAIL_MEMBERS_5 BG_DETAIL_MEMBERS_4, m4
#define BG_DETAIL_MEMBERS_6 BG_DETAIL_MEMBERS_5, m5
#define BG_DETAIL_MEMBERS_7 BG_DETAIL_MEMBERS_6, m6
#define BG_DETAIL_MEMBERS_8 BG_DETAIL_MEMBERS_7, m7
#define BG_DETAIL_MEMBERS_9 BG_DETAIL_MEMBERS_8, m8
#define BG_DETAIL_MEMBERS_10 BG_DETAIL_MEMBERS_9, m9
#define BG_DETAIL_MEMBERS_11 BG_DETAIL_MEMBERS_10, m10
#define BG_DETAIL_MEMBERS_12 BG_DETAIL_MEMBERS_11, m11
#define BG_DETAIL_MEMBERS_13 BG_DETAIL_MEMBERS_12, m12
#define BG_DETAIL_MEMBERS_14 BG_DETAIL_MEMBERS_13, m13
#define BG_DETAIL_MEMBERS_15 BG_DETAIL_MEMBERS_14, m14
#define BG_DETAIL_MEMBERS_16 BG_DETAIL_MEMBERS_15, m15
#define BG_DETAIL_MEMBERS_17 BG_DETAIL_MEMBERS_16, m16
#define BG_DETAIL_MEMBERS_18 BG_DETAIL_MEMBERS_17, m17
#define BG_DETAIL_MEMBERS_19 BG_DETAIL_MEMBERS_18, m18
#define BG_DETAIL_MEMBERS_20 BG_DETAIL_MEMBERS_19, m19
#define BG_DETAIL_MEMBERS_21 BG_DETAIL_MEMBERS_20, m20
#define BG_DETAIL_MEMBERS_22 BG_DETAIL_MEMBERS_21, m21
#define BG_DETAIL_MEMBERS_23 BG_DETAIL_MEMBERS_22, m22
#define BG_DETAIL_MEMBERS_24 BG_DETAIL_MEMBERS_23, m23
#define BG_DETAIL_MEMBERS_25 BG_DETAIL_MEMBERS_24, m24
#define BG_DETAIL_MEMBERS_26 BG_DETAIL_MEMBERS_25, m25
#define BG_DETAIL_MEMBERS_27 BG_DETAIL_MEMBERS_26, m26
#define BG_DETAIL_MEMBERS_28 BG_DETAIL_MEMBERS_27, m27
#define BG_DETAIL_MEMBERS_29 BG_DETAIL_MEMBERS_28, m28
#define BG_DETAIL_MEMBERS_30 BG_DETAIL_MEMBERS_29, m29
#define BG_DETAIL_MEMBERS_31 BG_DETAIL_MEMBERS_30, m30
#define BG_DETAIL_MEMBERS_32 BG_DETAIL_MEMBERS_31, m31

BG_DETAIL_AGGREGATE_TIE(1, BG_DETAIL_MEMBERS_1)
BG_DETAIL_AGGREGATE_TIE(2, BG_DETAIL_MEMBERS_2)
BG_DETAIL_AGGREGATE_TIE(3, BG_DETAIL_MEMBERS_3)
BG_DETAIL_AGGREGATE_TIE(4, BG_DETAIL_MEMBERS_4)
BG_DETAIL_AGGREGATE_TIE(5, BG_DETAIL_MEMBERS_5)
BG_DETAIL_AGGREGATE_TIE(6, BG_DETAIL_MEMBERS_6)
BG_DETAIL_AGGREGATE_TIE(7, BG_DETAIL_MEMBERS_7)
BG_DETAIL_AGGREGATE_TIE(8, BG_DETAIL_MEMBERS_8)
BG_DETAIL_AGGREGATE_TIE(9, BG_DETAIL_MEMBERS_9)
BG_DETAIL_AGGREGATE_TIE(10, BG_DETAIL_MEMBERS_10)
BG_DETAIL_AGGREGATE_TIE(11, BG_DETAIL_MEMBERS_11)
BG_DETAIL_AGGREGATE_TIE(12, BG_DETAIL_MEMBERS_12)
BG_DETAIL_AGGREGATE_TIE(13, BG_DETAIL_MEMBERS_13)
BG_DETAIL_AGGREGATE_TIE(14, BG_DETAIL_MEMBERS_14)
BG_DETAIL_AGGREGATE_TIE(15, BG_DETAIL_MEMBERS_15)
BG_DETAIL_AGGREGATE_TIE(16, BG_DETAIL_MEMBERS_16)
BG_DETAIL_AGGREGATE_TIE(17, BG_DETAIL_MEMBERS_17)
BG_DETAIL_AGGREGATE_TIE(18, BG_DETAIL_MEMBERS_18)
BG_DETAIL_AGGREGATE_TIE(19, BG_DETAIL_MEMBERS_19)
BG_DETAIL_AGGREGATE_TIE(20, BG_DETAIL_MEMBERS_20)
BG_DETAIL_AGGREGATE_TIE(21, BG_DETAIL_MEMBERS_21)
BG_DETAIL_AGGREGATE_TIE(22, BG_DETAIL_MEMBERS_22)
BG_DETAIL_AGGREGATE_TIE(23, BG_DETAIL_MEMBERS_23)
BG_DETAIL_AGGREGATE_TIE(24, BG_DETAIL_MEMBERS_24)
BG_DETAIL_AGGREGATE_TIE(25, BG_DETAIL_MEMBERS_25)
BG_DETAIL_AGGREGATE_TIE(26, BG_DETAIL_MEMBERS_26)
BG_DETAIL_AGGREGATE_TIE(27, BG_DETAIL_MEMBERS_27)
BG_DETAIL_AGGREGATE_TIE(28, BG_DETAIL_MEMBERS_28)
BG_DETAIL_AGGREGATE_TIE(29, BG_DETAIL_MEMBERS_29)
BG_DETAIL_AGGREGATE_TIE(30, BG_DETAIL_MEMBERS_30)
BG_DETAIL_AGGREGATE_TIE(31, BG_DETAIL_MEMBERS_31)
BG_DETAIL_AGGREGATE_TIE(32, BG_DETAIL_MEMBERS_32)

#undef BG_DETAIL_AGGREGATE_TIE
#undef BG_DETAIL_MEMBERS_32
#undef BG_DETAIL_MEMBERS_31
#undef BG_DETAIL_MEMBERS_30
#undef BG_DETAIL_MEMBERS_29
#undef BG_DETAIL_MEMBERS_28
#undef BG_DETAIL_MEMBERS_27
#undef BG_DETAIL_MEMBERS_26
#undef BG_DETAIL_MEMBERS_25
#undef BG_DETAIL_MEMBERS_24
#undef BG_DETAIL_MEMBERS_23
#undef BG_DETAIL_MEMBERS_22
#undef BG_DETAIL_MEMBERS_21
#undef BG_DETAIL_MEMBERS_20
#undef BG_DETAIL_MEMBERS_19
#undef BG_DETAIL_MEMBERS_18
#undef BG_DETAIL_MEMBERS_17
#undef BG_DETAIL_MEMBERS_16
#undef BG_DETAIL_MEMBERS_15
#undef BG_DETAIL_MEMBERS_14
#undef BG_DETAIL_MEMBERS_13
#undef BG_DETAIL_MEMBERS_12
#undef BG_DETAIL_MEMBERS_11
#undef BG_DETAIL_MEMBERS_10
#undef BG_DETAIL_MEMBERS_9
#undef BG_DETAIL_MEMBERS_8
#undef BG_DETAIL_MEMBERS_7
#undef BG_DETAIL_MEMBERS_6
#undef BG_DETAIL_MEMBERS_5
#undef BG_DETAIL_MEMBERS_4
#undef BG_DETAIL_MEMBERS_3
#undef BG_DETAIL_MEMBERS_2
#undef BG_DETAIL_MEMBERS_1

template <typename T> extern const T reflected_aggregate;

// A compiler signature retains the member name when the member pointer is a
// non-type template argument.
template <typename T> struct pointer_value {
    const T *pointer;
};

template <std::size_t Index, typename T> consteval auto aggregate_member_pointer() {
    auto members = aggregate_tie<count_aggregate_members<T>()>::apply(reflected_aggregate<T>);
    using Member = std::remove_reference_t<decltype(std::get<Index>(members))>;

    return pointer_value<Member>{std::addressof(std::get<Index>(members))};
}

template <auto Value> consteval std::string_view value_signature() {
#if defined(_MSC_VER)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

template <typename T> consteval std::string_view type_signature() {
#if defined(_MSC_VER)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

consteval bool identifier_character(char value) {
    return value == '_' || (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

template <std::size_t Index, typename T> consteval std::string_view aggregate_member_name() {
    constexpr auto signature = value_signature<aggregate_member_pointer<Index, T>()>();
    constexpr auto marker = std::string_view{"reflected_aggregate"};

    constexpr auto marker_position = signature.find(marker);
    constexpr auto member_access = signature.find('.', marker_position);

#if defined(_MSC_VER)
    auto end = signature.rfind(">(void)");
#else
    auto end = signature.find(';', member_access);
    if (end == std::string_view::npos) {
        end = signature.rfind(']');
    }
#endif

    while (end > member_access && !identifier_character(signature[end - 1])) {
        --end;
    }

    auto begin = end;
    while (begin > member_access && identifier_character(signature[begin - 1])) {
        --begin;
    }

    return signature.substr(begin, end - begin);
}

template <typename T, std::size_t... Index> consteval auto aggregate_member_names_impl(std::index_sequence<Index...>) {
    return std::array<std::string_view, sizeof...(Index)>{aggregate_member_name<Index, T>()...};
}

} // namespace detail

template <typename T>
inline constexpr std::size_t aggregate_member_count = [] {
    static_assert(std::is_aggregate_v<T>, "aggregate reflection requires an aggregate type");

    constexpr std::size_t count = detail::count_aggregate_members<T>();
    static_assert(count <= detail::maximum_aggregate_members, "aggregate reflection supports at most 32 members");

    return count;
}();

template <std::size_t Index, typename T>
using aggregate_member_type = std::remove_reference_t<std::tuple_element_t<
    Index, decltype(detail::aggregate_tie<aggregate_member_count<T>>::apply(std::declval<T &>()))>>;

template <typename T>
inline constexpr auto aggregate_member_names =
    detail::aggregate_member_names_impl<T>(std::make_index_sequence<aggregate_member_count<T>>{});

template <typename T> consteval std::string_view type_name() {
    constexpr std::string_view signature = detail::type_signature<T>();

#if defined(_MSC_VER)
    constexpr std::string_view prefix = "type_signature<";
    const std::size_t begin = signature.find(prefix) + prefix.size();
    const std::size_t end = signature.rfind(">(void)");
#else
    constexpr std::string_view prefix = "T = ";
    const std::size_t begin = signature.find(prefix) + prefix.size();
    const std::size_t semicolon = signature.find(';', begin);
    const std::size_t bracket = signature.find(']', begin);
    const std::size_t end = semicolon == std::string_view::npos ? bracket : semicolon;
#endif

    std::string_view name = signature.substr(begin, end - begin);

    for (const std::string_view keyword : {"class ", "struct ", "enum "}) {
        if (name.starts_with(keyword)) {
            name.remove_prefix(keyword.size());
            break;
        }
    }

    return name;
}

template <typename T> consteval std::string_view unqualified_type_name() {
    std::string_view name = type_name<T>();

    const std::size_t template_arguments = name.find('<');
    if (template_arguments != std::string_view::npos) {
        name = name.substr(0, template_arguments);
    }

    const std::size_t namespace_separator = name.rfind("::");
    if (namespace_separator != std::string_view::npos) {
        name.remove_prefix(namespace_separator + 2);
    }

    return name;
}

} // namespace bg::util
