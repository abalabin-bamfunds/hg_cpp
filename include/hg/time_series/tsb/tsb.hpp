#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/operators.hpp>
#include <bg/schema/arguments.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/util/named_argument.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T> struct named_bundle_wiring_field : std::false_type {};

template <util::compile_time_string Name, typename Value>
struct named_bundle_wiring_field<named_argument<Name, Value>>
    : std::bool_constant<wiring_point_argument<Value> || supported_scalar_argument<Value> ||
                         supported_optional_scalar_argument<Value> || supported_scalar_value_argument<Value> ||
                         std::same_as<std::remove_cvref_t<Value>, WiringArgument>> {};

template <typename T>
inline constexpr bool is_named_bundle_wiring_field = named_bundle_wiring_field<std::remove_cvref_t<T>>::value;

template <typename Argument>
inline constexpr bool typed_bundle_argument = requires(Argument &&argument) {
    WiringArgument{std::forward<Argument>(argument)};
};

template <typename Argument>
inline constexpr bool typed_bundle_wiring_argument =
    typed_bundle_argument<schema_argument_value_t<Argument>>;

template <typename Argument>
inline constexpr bool forces_bundle_wiring = [] {
    using Value = schema_argument_value_t<Argument>;
    return std::same_as<Value, WiringArgument> || wiring_point_argument<Value>;
}();

template <typename Declaration, auto...>
using bundle_wiring_field = wp<typename bundle_field_projection<Declaration>::time_series_type>;

template <template <template <typename, auto...> typename> typename Schema> class BundleWiringFields {
    class Factory {
      public:
        explicit Factory(WiringPoint bundle) noexcept : bundle_{bundle} {}

        template <typename SchemaField> auto make(std::size_t index) const {
            using Field = typename SchemaField::time_series_type;
            const auto &type = TSB<Schema>::type();
            return wp<Field>::from_validated(operators::getattr(bundle_, type.item_name(index)));
        }

      private:
        WiringPoint bundle_;
    };

  public:
    explicit BundleWiringFields(WiringPoint bundle) : fields_{make(bundle)} {}

    const Schema<bundle_wiring_field> *operator->() const noexcept { return &fields_; }

  private:
    static Schema<bundle_wiring_field> make(WiringPoint bundle) {
        auto factory = Factory{bundle};
        return make_bundle_fields<Schema, bundle_wiring_field>(factory);
    }

    Schema<bundle_wiring_field> fields_;
};

template <typename TimeSeries> class TypedBundleConstruction;

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
class TypedBundleConstruction<TSB<Schema, Properties...>> {
    using TimeSeries = TSB<Schema, Properties...>;
    using Reflection = bundle_schema_reflection<Schema>;

  public:
    template <typename Argument, typename Source>
    void add(Source &&source) {
        const auto index = assignment_.add(schema_argument_name<Argument>(), [&](util::symbol name) {
            const auto &semantic_type = type();
            for (auto field = std::size_t{}; field < semantic_type.item_count(); ++field) {
                if (semantic_type.item_name(field) == name) {
                    return std::optional<std::size_t>{field};
                }
            }
            return std::optional<std::size_t>{};
        });

        auto argument = WiringArgument{std::forward<Source>(source)};
        if (!argument.is_time_series() && (!argument.is_scalar() || !argument.present())) {
            throw WiringError{"a TSB field requires a time series or a present scalar value"};
        }

        const auto *field_type = type_cast<TimeSeriesType>(&argument.type());
        if (field_type == nullptr) {
            const auto *scalar_type = type_cast<ScalarType>(&argument.type());
            if (scalar_type == nullptr) {
                throw WiringError{"a TSB field requires a time series or scalar"};
            }
            field_type = &time_series_value_type(*scalar_type, TimeSeriesGuarantee::const_);
        }
        if (!input_type().item_type(index).accepts(*field_type)) {
            throw WiringError{"a TSB field has an incompatible type"};
        }

        fields_[index] = std::move(argument);
    }

    wp<TimeSeries> finish() {
        for (auto index = std::size_t{}; index < Reflection::field_count; ++index) {
            if (!assignment_.assigned(index)) {
                throw WiringError{"a required TSB field was not supplied"};
            }
        }

        return finish(std::make_index_sequence<Reflection::field_count>{});
    }

  private:
    static const TimeSeriesBundleType &input_type() {
        static const auto guarantee = util::static_storage<GuaranteeVariable>{util::symbol{"G"}};
        [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(guarantee.get());
        return static_cast<const TimeSeriesBundleType &>(
            time_series_type_for<TypedBundleConstruction, 0, TimeSeriesTypeRole::input, TimeSeries>(guarantee.get()));
    }

    template <std::size_t... Index> wp<TimeSeries> finish(std::index_sequence<Index...>) {
        const auto fields = std::array<WiringArgument, sizeof...(Index)>{
            WiringArgument{type().item_name(Index), std::move(*fields_[Index])}...};
        return wp<TimeSeries>::from_validated(make_free_time_series_bundle(fields));
    }

    static const TimeSeriesBundleType &type() {
        return static_cast<const TimeSeriesBundleType &>(TimeSeries::type());
    }

    SchemaArgumentAssignment<Reflection::field_count, WiringError> assignment_;
    std::array<std::optional<WiringArgument>, Reflection::field_count> fields_{};
};

template <typename TimeSeries, typename... Arguments>
wp<TimeSeries> make_typed_bundle(Arguments &&...arguments) {
    auto construction = TypedBundleConstruction<TimeSeries>{};
    (construction.template add<Arguments>(schema_argument_value(std::forward<Arguments>(arguments))), ...);
    return construction.finish();
}

} // namespace bg::details

namespace bg {

template <template <template <typename, auto...> typename> typename Schema, auto... TimeSeriesProperties,
          auto... WiringProperties>
    requires(details::variadic_mode<WiringProperties...>() == details::VariadicMode::none)
class wp<TSB<Schema, TimeSeriesProperties...>, WiringProperties...> {
    using TimeSeries = TSB<Schema, TimeSeriesProperties...>;
    using ParsedProperties = details::wiring_point_properties<WiringProperties...>;
    static_assert(ParsedProperties::variadic_mode == details::VariadicMode::none);

  public:
    wp(WiringPoint point) : point_{point.as(TimeSeries::type())} {}

    template <typename Source, auto... SourceProperties>
        requires(wp<Source, SourceProperties...>::validity != optional || ParsedProperties::validity == optional) &&
                (!wp<Source, SourceProperties...>::valid_items || ParsedProperties::valid_items)
    explicit wp(wp<Source, SourceProperties...> source) : wp(static_cast<WiringPoint>(source)) {
    }

    template <typename Source, auto... SourceProperties>
        requires(wp<Source, SourceProperties...>::validity == optional && ParsedProperties::validity != optional) ||
                (wp<Source, SourceProperties...>::valid_items && !ParsedProperties::valid_items)
    explicit wp(wp<Source, SourceProperties...>) = delete;

    operator WiringPoint() const noexcept { return point_; }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> auto as() const {
        using Result = typename details::annotated_wiring_point<Target, ParsedProperties::validity == optional,
                                                                ParsedProperties::valid_items>::type;
        return Result{point_};
    }

    template <typename Target, typename Builder>
    void materialisation_option(Builder &&builder, double cost) const {
        point_.template materialisation_option<Target>(std::forward<Builder>(builder), cost);
    }

    std::vector<WiringMaterialisationOption> materialisation_options() const {
        return point_.materialisation_options();
    }

    static wp from_validated(WiringPoint point) noexcept { return wp{point, unchecked_t{}}; }

    auto operator->() const
        requires(!details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
    {
        return details::BundleWiringFields<Schema>{point_};
    }

    using time_series_type = TimeSeries;
    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr bool context = ParsedProperties::context;
    static constexpr auto variadic_mode = details::VariadicMode::none;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool valid_items = ParsedProperties::valid_items;
    static constexpr details::WiringAnnotations annotations{validity == optional, valid_items};

  private:
    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    WiringPoint point_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... TimeSeriesProperties,
          auto... WiringProperties>
    requires(details::variadic_mode<WiringProperties...>() != details::VariadicMode::none)
class wp<TSB<Schema, TimeSeriesProperties...>, WiringProperties...> {
    using TimeSeries = TSB<Schema, TimeSeriesProperties...>;
    using ParsedProperties = details::wiring_point_properties<WiringProperties...>;
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(details::is_variadic_mode<
                      decltype(WiringProperties)>)) == 1,
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

    template <typename Source, auto... SourceProperties>
        requires(wp<Source, SourceProperties...>::validity != optional || ParsedProperties::validity == optional) &&
                (!wp<Source, SourceProperties...>::valid_items || ParsedProperties::valid_items)
    explicit wp(wp<Source, SourceProperties...> source) : wp(static_cast<WiringPoint>(source)) {
    }

    template <typename Source, auto... SourceProperties>
        requires(wp<Source, SourceProperties...>::validity == optional && ParsedProperties::validity != optional) ||
                (wp<Source, SourceProperties...>::valid_items && !ParsedProperties::valid_items)
    explicit wp(wp<Source, SourceProperties...>) = delete;

    operator WiringPoint() const noexcept { return point_; }

    std::size_t size() const noexcept { return record().size(); }

    WiringArgument operator[](std::size_t index) const { return record().argument(index); }

    WiringArgument operator[](std::string_view name) const {
        auto value = find(name);
        if (!value) {
            throw WiringError{"a variadic wiring parameter has no argument with the requested name"};
        }
        return std::move(*value);
    }

    std::optional<WiringArgument> find(std::string_view name) const {
        for (auto index = std::size_t{}; index < size(); ++index) {
            if (record().item_name(index).text() == name) {
                return record().argument(index);
            }
        }
        return std::nullopt;
    }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> auto as() const {
        using Result = typename details::annotated_wiring_point<Target, ParsedProperties::validity == optional,
                                                                ParsedProperties::valid_items>::type;
        return Result{point_};
    }

    static wp from_validated(WiringPoint point) noexcept { return wp{point, unchecked_t{}}; }

    using time_series_type = TimeSeries;
    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr bool context = ParsedProperties::context;
    static constexpr auto variadic_mode = ParsedProperties::variadic_mode;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool valid_items = ParsedProperties::valid_items;
    static constexpr details::WiringAnnotations annotations{validity == optional, valid_items};

  private:
    const details::VariadicWiringRecord &record() const noexcept { return *details::variadic_wiring_record(point_); }

    struct unchecked_t {};

    wp(WiringPoint point, unchecked_t) noexcept : point_(point) {}

    WiringPoint point_;
};

template <template <template <typename, auto...> typename> typename Schema, typename... Arguments>
    requires details::supported_time_series<TSB<Schema>> &&
             (details::typed_bundle_wiring_argument<Arguments> && ...) &&
             ((details::forces_bundle_wiring<Arguments> || ...) ||
              !details::hierarchical_scalar_schema_supported<Schema>)
[[nodiscard]] wp<TSB<Schema>> bundle(Arguments &&...arguments) {
    return details::make_typed_bundle<TSB<Schema>>(std::forward<Arguments>(arguments)...);
}

template <typename TimeSeries, typename... Arguments>
    requires details::is_time_series_bundle<TimeSeries> && details::supported_time_series<TimeSeries> &&
             (details::typed_bundle_wiring_argument<Arguments> && ...)
[[nodiscard]] wp<TimeSeries> bundle(Arguments &&...arguments) {
    return details::make_typed_bundle<TimeSeries>(std::forward<Arguments>(arguments)...);
}

[[nodiscard]] wp<> bundle(std::span<const WiringArgument> fields);

template <typename... Fields>
    requires(sizeof...(Fields) != 0 && (details::is_named_bundle_wiring_field<Fields> && ...))
[[nodiscard]] wp<> bundle(Fields &&...fields) {
    auto arguments = std::array<WiringArgument, sizeof...(Fields)>{
        WiringArgument{std::forward<Fields>(fields)}...};
    return bundle(std::span<const WiringArgument>{arguments});
}

static_assert(sizeof(wp<TSB<SCHEMA>, var_args>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSB<SCHEMA>, var_args>>);
static_assert(sizeof(wp<TSB<SCHEMA>, var_args, optional>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSB<SCHEMA>, var_args, optional>>);
static_assert(sizeof(wp<TSB<SCHEMA>, var_kwargs, optional>) == sizeof(WiringPoint));
static_assert(std::is_trivially_copyable_v<wp<TSB<SCHEMA>, var_kwargs, optional>>);

} // namespace bg
