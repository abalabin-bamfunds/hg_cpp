#pragma once

#include <bg/time_series/details/time_series_properties.hpp>
#include <bg/time_series/details/type_access.hpp>

#include <type_traits>

namespace bg {

template <typename K, typename Item, auto... Properties>
struct TSD : details::time_series_type_access<TSD<K, Item, Properties...>> {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using key_type = K;
    using item_type = Item;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

namespace details {

template <typename T> struct time_series_dict : std::false_type {};

template <typename K, typename Item, auto... Properties>
struct time_series_dict<TSD<K, Item, Properties...>> : std::true_type {
    using key_type = K;
    using item_type = Item;
};

template <typename T> inline constexpr bool is_time_series_dict = time_series_dict<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg

#include <bg/type/details/time_series_dict_type_traits.hpp>
