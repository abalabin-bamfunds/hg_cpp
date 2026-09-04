#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/details/getitem.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <type_traits>
#include <utility>

namespace bg {

template <typename TimeSeries, scalar_source Key>
    requires details::dictionary_wiring_type<TimeSeries> &&
             std::same_as<
                 canonical_scalar_t<Key>,
                 typename details::time_series_dict<details::referenced_wiring_time_series_t<TimeSeries>>::key_type>
[[nodiscard]] auto getitem(wp<TimeSeries> source, Key &&key) -> wp<REF<details::selected_time_series_t<TimeSeries>>> {
    const auto *source_type = &static_cast<WiringPoint>(source).type();
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(source_type)) {
        source_type = &reference->target_type();
    }
    const auto &node = details::dictionary_item_node_template(
        static_cast<const TimeSeriesDictionaryType &>(*source_type), false);
    const auto value = canonical_scalar(std::forward<Key>(key));
    return wp<REF<details::selected_time_series_t<TimeSeries>>>{*node(source, value)};
}

template <typename TimeSeries, typename KeyTimeSeries>
    requires details::dictionary_wiring_type<TimeSeries> &&
             std::same_as<
                 details::time_series_value_t<details::referenced_wiring_time_series_t<KeyTimeSeries>>,
                 typename details::time_series_dict<details::referenced_wiring_time_series_t<TimeSeries>>::key_type>
[[nodiscard]] auto getitem(wp<TimeSeries> source, wp<KeyTimeSeries> key)
    -> wp<REF<details::selected_time_series_t<TimeSeries>>> {
    const auto *source_type = &static_cast<WiringPoint>(source).type();
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(source_type)) {
        source_type = &reference->target_type();
    }
    const auto &node = details::dictionary_item_node_template(
        static_cast<const TimeSeriesDictionaryType &>(*source_type), true);
    return wp<REF<details::selected_time_series_t<TimeSeries>>>{*node(source, key)};
}

template <typename TimeSeries>
template <scalar_source Key, typename T>
    requires details::dictionary_wiring_type<T> &&
             std::same_as<canonical_scalar_t<Key>,
                          typename details::time_series_dict<details::referenced_wiring_time_series_t<T>>::key_type>
auto wp<TimeSeries>::operator[](Key &&key) const -> wp<REF<details::selected_time_series_t<T>>> {
    return getitem(*this, std::forward<Key>(key));
}

template <typename TimeSeries>
template <typename KeyTimeSeries, typename T>
    requires details::dictionary_wiring_type<T> &&
             std::same_as<details::time_series_value_t<details::referenced_wiring_time_series_t<KeyTimeSeries>>,
                          typename details::time_series_dict<details::referenced_wiring_time_series_t<T>>::key_type>
auto wp<TimeSeries>::operator[](wp<KeyTimeSeries> key) const -> wp<REF<details::selected_time_series_t<T>>> {
    return getitem(*this, key);
}

} // namespace bg
