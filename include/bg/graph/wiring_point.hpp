#pragma once

#include <cstddef>
#include <type_traits>

namespace bg {

class GraphWiring;

class WiringPoint {
  public:
    [[nodiscard]] const GraphWiring &graph() const noexcept { return *graph_; }
    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_; }

  private:
    WiringPoint(GraphWiring &graph, std::size_t node_index) noexcept : graph_(&graph), node_index_(node_index) {}

    GraphWiring *graph_;
    std::size_t node_index_;

    friend class GraphWiring;
};

template <typename TimeSeries> class wp {
  public:
    wp(WiringPoint point) noexcept : point_(point) {}

    [[nodiscard]] operator WiringPoint() const noexcept { return point_; }

    using time_series_type = TimeSeries;

  private:
    WiringPoint point_;
};

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
