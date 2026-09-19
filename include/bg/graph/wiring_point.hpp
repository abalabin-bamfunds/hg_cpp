#pragma once

#include <bg/graph/wiring_error.hpp>
#include <bg/graph/variadic_parameter.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/time_series_type.hpp>

#include <cstddef>
#include <type_traits>

namespace bg {

class GraphWiring;
class WiringPoint;

namespace details {
class WiringRecord;
struct wiring_point_annotation;
const WiringRecord &wiring_record(WiringPoint point) noexcept;
} // namespace details

template <typename TimeSeries = TS_T, auto... Properties> class wp;

class WiringPoint {
  public:
    const GraphWiring &graph() const noexcept;
    const TimeSeriesType &type() const noexcept;

    template <typename TimeSeries> wp<TimeSeries> as() const;

  private:
    explicit WiringPoint(const details::WiringRecord &record) noexcept : record_(&record) {}

    const details::WiringRecord *record_;

    friend class GraphWiring;
    friend class details::WiringRecord;
    friend const details::WiringRecord &details::wiring_record(WiringPoint point) noexcept;
};

template <typename TimeSeries, auto... Properties> class wp {
    static_assert(sizeof...(Properties) == 0, "this wiring point type does not accept properties");

  public:
    wp(WiringPoint point) : point_(point) {
        if (!TimeSeries::type().accepts(point.type())) {
            throw WiringError{"a WiringPoint is incompatible with the requested type"};
        }
    }

    // Re-annotation retains the actual wiring type. Signature resolution validates it at the next wiring boundary.
    template <typename Source> explicit wp(wp<Source> source) : wp(static_cast<WiringPoint>(source)) {}

    operator WiringPoint() const noexcept { return point_; }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> wp<Target> as() const { return point_.template as<Target>(); }

    template <typename Key> auto operator[](Key &&key) const;

    using time_series_type = TimeSeries;
    static constexpr auto variadic_mode = details::variadic_mode<Properties...>();

  private:
    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    friend struct details::wiring_point_annotation;

    WiringPoint point_;
};

template <typename TimeSeries> wp<TimeSeries> WiringPoint::as() const {
    return wp<TimeSeries>{*this};
}

namespace details {

struct wiring_point_annotation {
    template <typename Point> static Point make_point(WiringPoint point) noexcept {
        return Point{point, typename Point::unchecked_t{}};
    }

    template <typename TimeSeries> static wp<TimeSeries> make(WiringPoint point) noexcept {
        return make_point<wp<TimeSeries>>(point);
    }
};

template <typename T> struct typed_wiring_point : std::false_type {};

template <typename TimeSeries, auto... Properties>
struct typed_wiring_point<wp<TimeSeries, Properties...>> : std::true_type {
    using time_series_type = TimeSeries;
};

template <typename T> inline constexpr bool is_typed_wiring_point = typed_wiring_point<std::remove_cvref_t<T>>::value;

} // namespace details

static_assert(sizeof(WiringPoint) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<WiringPoint>);
static_assert(sizeof(wp<>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<>>);

} // namespace bg
