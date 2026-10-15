#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/tsl/details/wiring.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bg {

template <typename Item, auto Size, auto... TimeSeriesProperties, auto... WiringProperties>
    requires(details::variadic_mode<WiringProperties...>() != details::VariadicMode::none)
class wp<TSL<Item, Size, TimeSeriesProperties...>, WiringProperties...> {
    using TimeSeries = TSL<Item, Size, TimeSeriesProperties...>;
    using ParsedProperties = details::wiring_point_properties<WiringProperties...>;
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(details::is_variadic_mode<
                      decltype(WiringProperties)>)) == 1,
                  "a variadic TSL wiring point requires exactly one variadic property");
    static_assert(details::variadic_mode<WiringProperties...>() == details::VariadicMode::positional,
                  "a variadic TSL wiring point requires var_args");

  public:
    wp(WiringPoint point) : point_{point} {
        if (!TimeSeries::type().accepts(point.type()) || details::variadic_wiring_record(point) == nullptr) {
            throw WiringError{"a variadic wiring point requires a matching variadic TSL"};
        }
    }

    template <typename Source, auto... SourceProperties>
    explicit wp(wp<Source, SourceProperties...> source) : wp(static_cast<WiringPoint>(source)) {
        static_assert(wp<Source, SourceProperties...>::validity != optional || ParsedProperties::validity == optional,
                      "an optional wiring point cannot be re-annotated as a required input");
    }

    operator WiringPoint() const noexcept { return point_; }

    std::size_t size() const noexcept { return record().size(); }
    WiringArgument operator[](std::size_t index) const { return record().argument(index); }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> auto as() const {
        if constexpr (ParsedProperties::validity == optional) {
            return wp<Target, optional>{point_};
        } else {
            return point_.template as<Target>();
        }
    }

    static wp from_validated(WiringPoint point) noexcept { return wp{point, unchecked_t{}}; }

    using time_series_type = TimeSeries;
    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr bool context = ParsedProperties::context;
    static constexpr auto variadic_mode = ParsedProperties::variadic_mode;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    const details::VariadicWiringRecord &record() const noexcept { return *details::variadic_wiring_record(point_); }

    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    WiringPoint point_;
};

[[nodiscard]] wp<> tsl(std::span<const WiringPoint> items);
[[nodiscard]] wp<> tsl(const TimeSeriesType &item_type);

template <typename ITEM>
    requires(details::supported_time_series<ITEM> && !details::generic_time_series<ITEM>)
[[nodiscard]] wp<> tsl() {
    return tsl(ITEM::type());
}

template <std::ranges::contiguous_range Range>
    requires std::ranges::sized_range<Range> &&
             details::is_typed_wiring_point<std::ranges::range_value_t<Range>> &&
             (std::ranges::range_value_t<Range>::validity != optional) &&
             (std::ranges::range_value_t<Range>::variadic_mode == details::VariadicMode::none)
[[nodiscard]] wp<> tsl(Range &&items) {
    auto points = std::vector<WiringPoint>{};
    points.reserve(std::ranges::size(items));
    for (const auto &item : items) {
        points.push_back(static_cast<WiringPoint>(item));
    }
    return tsl(points);
}

template <typename... TimeSeries> [[nodiscard]] wp<> tsl(wp<TimeSeries>... items) {
    static_assert(sizeof...(TimeSeries) != 0);
    const auto points = std::array<WiringPoint, sizeof...(TimeSeries)>{static_cast<WiringPoint>(items)...};
    return tsl(std::span<const WiringPoint>{points});
}

static_assert(sizeof(wp<TSL<TS_T, SIZE>, var_args>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSL<TS_T, SIZE>, var_args>>);
static_assert(sizeof(wp<TSL<TS_T, SIZE>, var_args, optional>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSL<TS_T, SIZE>, var_args, optional>>);

} // namespace bg
