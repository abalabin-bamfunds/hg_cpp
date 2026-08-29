#pragma once

#include <bg/time_series/dict/type.hpp>
#include <bg/time_series/list/type.hpp>
#include <bg/time_series/set/type.hpp>

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

    [[nodiscard]] operator WiringPoint() const noexcept { return point_; }

    template <typename T = TimeSeries>
        requires details::is_time_series_list<T>
    [[nodiscard]] auto operator[](std::size_t index) const -> wp<typename details::time_series_list<T>::item_type> {
        return point_.item(index);
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
