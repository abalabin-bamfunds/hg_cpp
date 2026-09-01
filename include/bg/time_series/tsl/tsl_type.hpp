#pragma once

#include <bg/time_series/details/time_series_properties.hpp>

#include <cstddef>
#include <span>
#include <type_traits>

namespace bg {

template <typename Item, std::size_t Size = std::dynamic_extent, auto... Properties> struct TSL {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using item_type = Item;

    static constexpr std::size_t size = Size;
    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

namespace details {

template <typename T> struct time_series_list : std::false_type {
    static constexpr std::size_t size = std::dynamic_extent;
};

template <typename Item, std::size_t Size, auto... Properties>
struct time_series_list<TSL<Item, Size, Properties...>> : std::true_type {
    using item_type = Item;

    static constexpr std::size_t size = Size;
};

template <typename T> inline constexpr bool is_time_series_list = time_series_list<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
