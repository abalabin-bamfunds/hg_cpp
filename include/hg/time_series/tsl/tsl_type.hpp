#pragma once

#include <bg/time_series/details/time_series_properties.hpp>
#include <bg/time_series/details/type_access.hpp>
#include <bg/time_series/size_variable.hpp>

#include <cstddef>
#include <span>
#include <type_traits>

namespace bg {

template <typename Item, auto Size = std::dynamic_extent, auto... Properties>
struct TSL : details::time_series_type_access<TSL<Item, Size, Properties...>> {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using item_type = Item;

    static constexpr auto size = Size;
    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

namespace details {

template <typename T> struct time_series_list : std::false_type {
    static constexpr std::size_t size = std::dynamic_extent;
};

template <typename Item, auto Size, auto... Properties>
struct time_series_list<TSL<Item, Size, Properties...>> : std::true_type {
    using item_type = Item;

    static constexpr auto size = Size;
};

template <typename T> inline constexpr bool is_time_series_list = time_series_list<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg

#include <bg/type/details/time_series_list_type_traits.hpp>
