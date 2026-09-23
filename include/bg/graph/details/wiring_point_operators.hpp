#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/operators.hpp>
#include <bg/type/details/time_series_type_traits.hpp>

#include <utility>

namespace bg {

namespace details {

template <typename TimeSeries, typename = void> struct getitem_result {
    using type = TS_T;
};

template <typename TimeSeries>
    requires supported_time_series<typename dereferenced_time_series_t<TimeSeries>::item_type>
struct getitem_result<TimeSeries,
                      std::void_t<typename dereferenced_time_series_t<TimeSeries>::item_type>> {
    using Item = typename dereferenced_time_series_t<TimeSeries>::item_type;
    using type = dereferenced_time_series_t<Item>;
};

template <typename TimeSeries> using getitem_result_t = typename getitem_result<TimeSeries>::type;

} // namespace details

template <typename TimeSeries, auto... Properties>
template <typename Key>
auto wp<TimeSeries, Properties...>::operator[](Key &&key) const {
    return operators::getitem(*this, std::forward<Key>(key)).template as<details::getitem_result_t<TimeSeries>>();
}

} // namespace bg
