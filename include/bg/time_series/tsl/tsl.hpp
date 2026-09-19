#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/tsl/details/wiring.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <array>
#include <cstddef>
#include <type_traits>

namespace bg {

template <typename Item, auto Size, auto... TimeSeriesProperties, auto... WiringProperties>
    requires(details::variadic_mode<WiringProperties...>() != details::VariadicMode::none)
class wp<TSL<Item, Size, TimeSeriesProperties...>, WiringProperties...> {
    using TimeSeries = TSL<Item, Size, TimeSeriesProperties...>;
    static_assert(sizeof...(WiringProperties) == 1,
                  "a variadic TSL wiring point requires exactly one variadic property");
    static_assert(details::variadic_mode<WiringProperties...>() == details::VariadicMode::positional,
                  "a variadic TSL wiring point requires var_args");

  public:
    wp(WiringPoint point) : point_{point} {
        if (!TimeSeries::type().accepts(point.type()) || details::variadic_wiring_record(point) == nullptr) {
            throw WiringError{"a variadic wiring point requires a matching variadic TSL"};
        }
    }

    operator WiringPoint() const noexcept { return point_; }

    std::size_t size() const noexcept { return record().size(); }
    WiringArgument operator[](std::size_t index) const { return record().argument(index); }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> wp<Target> as() const { return point_.template as<Target>(); }

    using time_series_type = TimeSeries;
    static constexpr auto variadic_mode = details::variadic_mode<WiringProperties...>();

  private:
    const details::VariadicWiringRecord &record() const noexcept {
        return *details::variadic_wiring_record(point_);
    }

    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    friend struct details::wiring_point_annotation;

    WiringPoint point_;
};

template <typename... TimeSeries> [[nodiscard]] wp<> tsl(wp<TimeSeries>... items) {
    static_assert(sizeof...(TimeSeries) != 0);
    const auto points = std::array<WiringPoint, sizeof...(TimeSeries)>{static_cast<WiringPoint>(items)...};
    return wp<>{details::make_free_fixed_time_series_list(points)};
}

static_assert(sizeof(wp<TSL<TS_T, SIZE>, var_args>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSL<TS_T, SIZE>, var_args>>);

} // namespace bg
