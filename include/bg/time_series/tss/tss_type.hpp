#pragma once

#include <bg/time_series/details/time_series_properties.hpp>

#include <type_traits>

namespace bg {

template <typename T, auto... Properties> struct TSS {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using item_type = T;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

namespace details {

template <typename T> struct time_series_set : std::false_type {};

template <typename Item, auto... Properties> struct time_series_set<TSS<Item, Properties...>> : std::true_type {
    using item_type = Item;
};

template <typename T> inline constexpr bool is_time_series_set = time_series_set<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
