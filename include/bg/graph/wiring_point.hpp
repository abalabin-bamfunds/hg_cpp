#pragma once

#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/util/named_argument.hpp>

#include <cstddef>
#include <type_traits>

namespace bg {

class GraphWiring;
class TimeSeriesType;

template <typename TimeSeries> class wp;

namespace details {

template <typename T> struct referenced_wiring_time_series {
    using type = T;
};

template <typename T, auto... Properties> struct referenced_wiring_time_series<REF<T, Properties...>> {
    using type = T;
};

template <typename T>
using referenced_wiring_time_series_t = typename referenced_wiring_time_series<std::remove_cv_t<T>>::type;

template <typename T>
concept dictionary_wiring_type = is_time_series_dict<referenced_wiring_time_series_t<T>>;

template <typename T>
using dictionary_wiring_item_t = typename time_series_dict<referenced_wiring_time_series_t<T>>::item_type;

template <typename T>
using selected_time_series_t = referenced_wiring_time_series_t<dictionary_wiring_item_t<T>>;

} // namespace details

template <typename K, typename Item, auto... Properties>
auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>>;

class WiringPoint {
  public:
    const GraphWiring &graph() const noexcept { return *graph_; }
    std::size_t node_index() const noexcept;
    const TimeSeriesType &type() const noexcept;

  private:
    WiringPoint(GraphWiring &graph, std::size_t record_index) noexcept : graph_(&graph), record_index_(record_index) {}

    WiringPoint item(std::size_t index) const;
    std::size_t record_index() const noexcept { return record_index_; }

    GraphWiring *graph_;
    std::size_t record_index_;

    friend class GraphWiring;
    template <typename TimeSeries> friend class wp;
    template <typename K, typename Item, auto... Properties>
    friend auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>>;
};

template <typename TimeSeries> class wp {
  public:
    wp(WiringPoint point) noexcept : point_(point) {}

    // Re-annotation retains the actual wiring type. Signature resolution validates it at the next wiring boundary.
    template <typename Source> explicit wp(wp<Source> source) noexcept : point_(static_cast<WiringPoint>(source)) {}

    operator WiringPoint() const noexcept { return point_; }

    template <typename T = TimeSeries>
        requires details::is_time_series_list<T>
    auto operator[](std::size_t index) const -> wp<typename details::time_series_list<T>::item_type> {
        return point_.item(index);
    }

    template <util::compile_time_string Name, typename T = TimeSeries>
        requires details::is_time_series_bundle<T>
    auto operator[](details::named_argument_name<Name>) const {
        using Bundle = details::time_series_bundle<T>;
        constexpr auto Index = details::bundle_field_index<Name, Bundle::template schema_type>;
        static_assert(Index != details::missing_bundle_field, "bundle schema has no field with this name");
        using Item = details::bundle_field_time_series<Name, Bundle::template schema_type>;
        return wp<Item>{point_.item(Index)};
    }

    template <scalar_source Key, typename T = TimeSeries>
        requires details::dictionary_wiring_type<T> &&
                 std::same_as<canonical_scalar_t<Key>,
                              typename details::time_series_dict<details::referenced_wiring_time_series_t<T>>::key_type>
    auto operator[](Key &&key) const -> wp<REF<details::selected_time_series_t<T>>>;

    template <typename KeyTimeSeries, typename T = TimeSeries>
        requires details::dictionary_wiring_type<T> &&
                 std::same_as<details::time_series_value_t<details::referenced_wiring_time_series_t<KeyTimeSeries>>,
                              typename details::time_series_dict<details::referenced_wiring_time_series_t<T>>::key_type>
    auto operator[](wp<KeyTimeSeries> key) const -> wp<REF<details::selected_time_series_t<T>>>;

    using time_series_type = TimeSeries;

  private:
    template <typename K, typename Item, auto... Properties>
    friend auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>>;

    WiringPoint point_;
};

template <typename K, typename Item, auto... Properties>
auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>> {
    return source.point_.item(0);
}

namespace details {

template <typename T> struct typed_wiring_point : std::false_type {};

template <typename TimeSeries> struct typed_wiring_point<wp<TimeSeries>> : std::true_type {
    using time_series_type = TimeSeries;
};

template <typename T> inline constexpr bool is_typed_wiring_point = typed_wiring_point<std::remove_cvref_t<T>>::value;

} // namespace details

static_assert(sizeof(wp<int>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<int>>);

} // namespace bg
