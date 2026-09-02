#pragma once

#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/util/named_argument.hpp>

#include <cstddef>
#include <type_traits>

namespace bg {

class GraphWiring;
class TimeSeriesType;

template <typename TimeSeries> class wp;

template <typename K, typename Item, auto... Properties>
[[nodiscard]] auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>>;

class WiringPoint {
  public:
    [[nodiscard]] const GraphWiring &graph() const noexcept { return *graph_; }
    [[nodiscard]] std::size_t node_index() const noexcept;
    [[nodiscard]] const TimeSeriesType &type() const noexcept;

  private:
    WiringPoint(GraphWiring &graph, std::size_t record_index) noexcept : graph_(&graph), record_index_(record_index) {}

    [[nodiscard]] WiringPoint item(std::size_t index) const;
    [[nodiscard]] std::size_t record_index() const noexcept { return record_index_; }

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

    [[nodiscard]] operator WiringPoint() const noexcept { return point_; }

    template <typename T = TimeSeries>
        requires details::is_time_series_list<T>
    [[nodiscard]] auto operator[](std::size_t index) const -> wp<typename details::time_series_list<T>::item_type> {
        return point_.item(index);
    }

    template <util::compile_time_string Name, typename T = TimeSeries>
        requires details::is_time_series_bundle<T>
    [[nodiscard]] auto operator[](details::named_argument_name<Name>) const {
        using Bundle = details::time_series_bundle<T>;
        constexpr auto Index = details::bundle_field_index<Name, Bundle::template schema_type>;
        static_assert(Index != details::missing_bundle_field, "bundle schema has no field with this name");
        using Item = details::bundle_field_time_series<Name, Bundle::template schema_type>;
        return wp<Item>{point_.item(Index)};
    }

    using time_series_type = TimeSeries;

  private:
    template <typename K, typename Item, auto... Properties>
    friend auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>>;

    WiringPoint point_;
};

template <typename K, typename Item, auto... Properties>
[[nodiscard]] auto keys(wp<TSD<K, Item, Properties...>> source) -> wp<TSS<K, Properties...>> {
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
