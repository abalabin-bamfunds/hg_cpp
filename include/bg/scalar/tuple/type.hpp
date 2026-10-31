#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace bg {

template <typename... Items> class Tuple {
  public:
    Tuple() = delete;

    static constexpr std::size_t size = sizeof...(Items);
    template <std::size_t Index> using item_type = std::tuple_element_t<Index, std::tuple<Items...>>;
};

template <typename T> struct is_tuple_scalar : std::false_type {};
template <typename... Items> struct is_tuple_scalar<Tuple<Items...>> : std::true_type {};
template <typename T> inline constexpr bool is_tuple_scalar_v = is_tuple_scalar<std::remove_cvref_t<T>>::value;

} // namespace bg
