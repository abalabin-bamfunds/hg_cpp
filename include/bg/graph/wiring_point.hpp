#pragma once

#include <bg/graph/variadic_parameter.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/details/input_properties.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/time_series_type.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <type_traits>
#include <vector>

namespace bg {

class GraphWiring;
class Signature;
class WiringPoint;
class WiringArgument;

namespace details {
class WiringRecord;
} // namespace details

struct WiringMaterialisationOption {
    const TimeSeriesType *target_type;
    double cost;
    // Borrowed from the graph-owned option; valid while its GraphWiring lives after options are frozen.
    const Signature *input_signature;
};

// Erased extension builder called with arguments normalized to its declared input signature.
using WiringMaterialisationBuilder = std::function<WiringPoint(std::span<const WiringArgument>)>;

template <typename TimeSeries = TS_T, auto... Properties> class wp;

namespace details {

struct WiringAnnotations {
    bool optional{};
    bool valid_items{};

    constexpr bool operator==(const WiringAnnotations &) const noexcept = default;
};

template <auto... Properties> struct wiring_point_property_list {};

template <auto Property, typename Properties> struct prepend_wiring_point_property;

template <auto Property, auto... Properties>
struct prepend_wiring_point_property<Property, wiring_point_property_list<Properties...>> {
    using type = wiring_point_property_list<Property, Properties...>;
};

template <typename Properties> struct non_validity_wiring_point_properties;

template <>
struct non_validity_wiring_point_properties<wiring_point_property_list<>> {
    using type = wiring_point_property_list<>;
};

template <auto Property, auto... Properties>
struct non_validity_wiring_point_properties<wiring_point_property_list<Property, Properties...>> {
  private:
    using Tail = typename non_validity_wiring_point_properties<wiring_point_property_list<Properties...>>::type;

  public:
    using type = std::conditional_t<is_validity<decltype(Property)>, Tail,
                                    typename prepend_wiring_point_property<Property, Tail>::type>;
};

template <typename Properties> struct optionalize_wiring_point_properties;

template <auto... Properties>
struct optionalize_wiring_point_properties<wiring_point_property_list<Properties...>> {
    using NonValidity = typename non_validity_wiring_point_properties<wiring_point_property_list<Properties...>>::type;
    using type = typename prepend_wiring_point_property<optional, NonValidity>::type;
};

template <typename TimeSeries, typename Properties> struct make_typed_wiring_point;

template <typename TimeSeries, auto... Properties>
struct make_typed_wiring_point<TimeSeries, wiring_point_property_list<Properties...>> {
    using type = wp<TimeSeries, Properties...>;
};

template <typename Point> struct optional_wiring_point;

template <typename TimeSeries, auto... Properties>
struct optional_wiring_point<wp<TimeSeries, Properties...>> {
    using PropertiesList = wiring_point_property_list<Properties...>;
    using OptionalProperties = typename optionalize_wiring_point_properties<PropertiesList>::type;
    using type = typename make_typed_wiring_point<TimeSeries, OptionalProperties>::type;
};

template <typename Point> struct valid_items_wiring_point;

template <typename TimeSeries, auto... Properties>
struct valid_items_wiring_point<wp<TimeSeries, Properties...>> {
    static constexpr bool already_annotated = (false || ... || is_valid_items<decltype(Properties)>);
    using type = std::conditional_t<already_annotated, wp<TimeSeries, Properties...>,
                                    wp<TimeSeries, Properties..., bg::valid_items>>;
};

} // namespace details

class WiringPoint {
  public:
    WiringPoint() noexcept = default;

    bool has_output() const noexcept { return record_ != nullptr; }
    explicit operator bool() const noexcept { return has_output(); }
    bool has_default_materialisation() const noexcept;

    const GraphWiring &graph() const;
    const TimeSeriesType &type() const;
    const details::WiringRecord *record() const noexcept { return record_; }

    // Extension API for registering an alternative representation with a declared input signature.
    void add_materialisation_option(const TimeSeriesType &target_type, double cost, Signature input_signature,
                                   WiringMaterialisationBuilder builder) const;
    template <typename Target, typename Builder>
    void materialisation_option(Builder &&builder, double cost) const;
    // Inspection freezes the point's option set.
    std::vector<WiringMaterialisationOption> materialisation_options() const;

    template <typename TimeSeries> wp<TimeSeries> as() const;

  private:
    explicit WiringPoint(const details::WiringRecord &record) noexcept : record_(&record) {}

    const details::WiringRecord *record_{};

    friend class details::WiringRecord;
};

template <typename TimeSeries, auto... Properties> class wp {
    using ParsedProperties = details::wiring_point_properties<Properties...>;
    static_assert(ParsedProperties::variadic_mode == details::VariadicMode::none,
                  "this wiring point type does not accept variadic properties");

  public:
    wp() noexcept
        requires(std::same_as<TimeSeries, TS_T> && sizeof...(Properties) == 0)
    = default;

    wp(WiringPoint point) : point_(point) {
        if (TimeSeries::type().accepts(point.type())) {
            return;
        }

        const auto accepts_advertised_form = std::ranges::any_of(
            point.materialisation_options(), [](const auto &option) {
                return TimeSeries::type().accepts(*option.target_type);
            });
        if (accepts_advertised_form) {
            return;
        }
        throw WiringError{"a WiringPoint is incompatible with the requested type"};
    }

    // Re-annotation retains the actual wiring type. Signature resolution validates it at the next wiring boundary.
    template <typename Source, auto... SourceProperties>
    explicit wp(wp<Source, SourceProperties...> source) : wp(static_cast<WiringPoint>(source)) {
        static_assert(wp<Source, SourceProperties...>::validity != optional || ParsedProperties::validity == optional,
                      "an optional wiring point cannot be re-annotated as a required input");
    }

    operator WiringPoint() const noexcept { return point_; }

    bool has_output() const noexcept { return point_.has_output(); }

    const GraphWiring &graph() const { return point_.graph(); }
    const TimeSeriesType &type() const { return point_.type(); }

    template <typename Target> auto as() const {
        if constexpr (ParsedProperties::validity == optional && ParsedProperties::valid_items) {
            return wp<Target, optional, bg::valid_items>{point_};
        } else if constexpr (ParsedProperties::validity == optional) {
            return wp<Target, optional>{point_};
        } else if constexpr (ParsedProperties::valid_items) {
            return wp<Target, bg::valid_items>{point_};
        } else {
            return point_.template as<Target>();
        }
    }

    template <typename Target, typename Builder>
    void materialisation_option(Builder &&builder, double cost) const;

    std::vector<WiringMaterialisationOption> materialisation_options() const {
        return point_.materialisation_options();
    }

    static wp from_validated(WiringPoint point) noexcept { return wp{point, unchecked_t{}}; }

    template <typename Key> auto operator[](Key &&key) const;

    using time_series_type = TimeSeries;
    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr bool context = ParsedProperties::context;
    static constexpr auto variadic_mode = ParsedProperties::variadic_mode;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool valid_items = ParsedProperties::valid_items;
    static constexpr details::WiringAnnotations annotations{validity == optional, valid_items};

  private:
    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    WiringPoint point_;
};

template <typename TimeSeries> wp<TimeSeries> WiringPoint::as() const {
    return wp<TimeSeries>{*this};
}

namespace details {

template <typename T> struct typed_wiring_point : std::false_type {};

template <typename TimeSeries, auto... Properties>
struct typed_wiring_point<wp<TimeSeries, Properties...>> : std::true_type {
    using time_series_type = TimeSeries;
};

template <typename T> inline constexpr bool is_typed_wiring_point = typed_wiring_point<std::remove_cvref_t<T>>::value;

} // namespace details

template <typename Point> auto ValidItems::operator()(Point &&point) const {
    using Source = std::remove_cvref_t<Point>;
    if constexpr (details::is_typed_wiring_point<Source>) {
        using Result = typename details::valid_items_wiring_point<Source>::type;
        return Result{std::forward<Point>(point)};
    } else if constexpr (requires(Source value) { value.require_valid_items(); }) {
        auto result = std::forward<Point>(point);
        result.require_valid_items();
        return result;
    } else {
        static_assert(requires { static_cast<WiringPoint>(point); },
                      "valid_items requires a typed or erased wiring point");
        return wp<TS_T, bg::valid_items>{static_cast<WiringPoint>(point)};
    }
}

static_assert(sizeof(WiringPoint) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<WiringPoint>);
static_assert(sizeof(wp<>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<>>);
static_assert(sizeof(wp<TS_T, optional>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TS_T, optional>>);

inline wp<TS_T, optional> opt(WiringPoint point) { return wp<TS_T, optional>::from_validated(point); }

template <typename Point>
    requires details::is_typed_wiring_point<Point>
auto opt(Point point) {
    using Result = typename details::optional_wiring_point<std::remove_cvref_t<Point>>::type;
    return Result::from_validated(static_cast<WiringPoint>(point));
}

} // namespace bg
