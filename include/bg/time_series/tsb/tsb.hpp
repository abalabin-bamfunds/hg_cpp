#pragma once

#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/util/named_argument.hpp>

#include <array>
#include <concepts>
#include <functional>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace bg::details {

template <typename T> struct named_bundle_wiring_field : std::false_type {};

template <util::compile_time_string Name, typename Value>
struct named_bundle_wiring_field<named_argument<Name, Value>>
    : std::bool_constant<std::same_as<Value, WiringPoint> || is_typed_wiring_point<Value>> {};

template <typename T>
inline constexpr bool is_named_bundle_wiring_field = named_bundle_wiring_field<std::remove_cvref_t<T>>::value;

template <template <template <typename> typename> typename Schema> class BundleWiringFields {
    class Factory {
      public:
        explicit Factory(WiringPoint bundle) noexcept : bundle_{bundle} {}

        template <typename Field> wp<Field> make(std::size_t index) const {
            const auto &type = TSB<Schema>::type();
            return operators::getattr(bundle_, type.item_name(index)).template as<Field>();
        }

      private:
        WiringPoint bundle_;
    };

  public:
    explicit BundleWiringFields(WiringPoint bundle) : fields_{make(bundle)} {}

    const Schema<wp> *operator->() const noexcept { return &fields_; }

  private:
    static Schema<wp> make(WiringPoint bundle) {
        auto factory = Factory{bundle};
        return make_bundle_fields<Schema, wp>(factory);
    }

    Schema<wp> fields_;
};

} // namespace bg::details

namespace bg {

template <template <template <typename> typename> typename Schema, auto... TimeSeriesProperties,
          auto... WiringProperties>
    requires(details::variadic_mode<WiringProperties...>() == details::VariadicMode::none)
class wp<TSB<Schema, TimeSeriesProperties...>, WiringProperties...> {
    using TimeSeries = TSB<Schema, TimeSeriesProperties...>;
    static_assert(sizeof...(WiringProperties) == 0, "a non-variadic TSB wiring point does not accept properties");

  public:
    wp(WiringPoint point) : point_{point} {
        if (!TimeSeries::type().accepts(point.type())) {
            throw WiringError{"a WiringPoint is incompatible with the requested type"};
        }
    }

    template <typename Source> explicit wp(wp<Source> source) : wp(static_cast<WiringPoint>(source)) {}

    operator WiringPoint() const noexcept { return point_; }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> wp<Target> as() const { return point_.template as<Target>(); }

    auto operator->() const
        requires(!details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
    {
        return details::BundleWiringFields<Schema>{point_};
    }

    using time_series_type = TimeSeries;
    static constexpr auto variadic_mode = details::VariadicMode::none;

  private:
    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    friend struct details::wiring_point_annotation;

    WiringPoint point_;
};

template <template <template <typename> typename> typename Schema, auto... TimeSeriesProperties,
          auto... WiringProperties>
    requires(details::variadic_mode<WiringProperties...>() != details::VariadicMode::none)
class wp<TSB<Schema, TimeSeriesProperties...>, WiringProperties...> {
    using TimeSeries = TSB<Schema, TimeSeriesProperties...>;
    static_assert(sizeof...(WiringProperties) == 1,
                  "a variadic TSB wiring point requires exactly one variadic property");
    static_assert(details::variadic_mode<WiringProperties...>() == details::VariadicMode::positional ||
                      details::variadic_mode<WiringProperties...>() == details::VariadicMode::named,
                  "a variadic TSB wiring point requires var_args or var_kwargs");

  public:
    wp(WiringPoint point) : point_{point} {
        if (!TimeSeries::type().accepts(point.type()) || details::variadic_wiring_record(point) == nullptr) {
            throw WiringError{"a variadic wiring point requires a matching variadic TSB"};
        }
    }

    operator WiringPoint() const noexcept { return point_; }

    std::size_t size() const noexcept { return record().arguments().size(); }

    const WiringArgument &operator[](std::size_t index) const {
        const auto arguments = record().arguments();
        if (index >= arguments.size()) {
            throw std::out_of_range{"a variadic wiring parameter index is out of range"};
        }
        return arguments[index];
    }

    const WiringArgument &operator[](std::string_view name) const {
        const auto value = find(name);
        if (!value) {
            throw WiringError{"a variadic wiring parameter has no argument with the requested name"};
        }
        return value->get();
    }

    std::optional<std::reference_wrapper<const WiringArgument>> find(std::string_view name) const noexcept {
        const auto arguments = record().arguments();
        const auto found =
            std::ranges::find_if(arguments, [&](const auto &argument) { return argument.name()->text() == name; });
        if (found == arguments.end()) {
            return std::nullopt;
        }
        return std::cref(*found);
    }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> wp<Target> as() const { return point_.template as<Target>(); }

    using time_series_type = TimeSeries;
    static constexpr auto variadic_mode = details::variadic_mode<WiringProperties...>();

  private:
    const details::VariadicWiringRecord &record() const noexcept { return *details::variadic_wiring_record(point_); }

    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    friend struct details::wiring_point_annotation;

    WiringPoint point_;
};

template <typename... Fields>
    requires(sizeof...(Fields) != 0 && (details::is_named_bundle_wiring_field<Fields> && ...))
[[nodiscard]] wp<> bundle(Fields &&...fields) {
    auto points = std::array<details::NamedBundleWiringPoint, sizeof...(Fields)>{details::NamedBundleWiringPoint{
        util::symbol{std::remove_cvref_t<Fields>::name.view()}, static_cast<WiringPoint>(fields.value)}...};
    return wp<>{details::make_free_time_series_bundle(points)};
}

static_assert(sizeof(wp<TSB<SCHEMA>, var_args>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSB<SCHEMA>, var_args>>);

} // namespace bg
