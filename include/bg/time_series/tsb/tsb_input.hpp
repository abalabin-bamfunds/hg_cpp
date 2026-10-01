#pragma once

#include <bg/graph/variadic_parameter.hpp>
#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <string_view>

namespace bg {
namespace details {

template <typename Declaration, auto... Properties>
using bundle_input_field = In<typename bundle_field_projection<Declaration>::time_series_type, Properties...>;

class bundle_input_factory {
  public:
    explicit bundle_input_factory(const TimeSeriesInputData &data) noexcept : data_{&data} {}

    template <typename SchemaField> [[nodiscard]] auto make(std::size_t index) const {
        const auto item = data_->runtime_type().item(data_->data(), static_cast<item_id_t>(index));
        using Input = typename SchemaField::template rebind<bundle_input_field>;
        return Input{item.last_updated, item.value, item.runtime_type,
                     data_->modified() && item.last_updated == data_->last_updated()};
    }

  private:
    const TimeSeriesInputData *data_;
};

template <template <template <typename, auto...> typename> typename Schema>
[[nodiscard]] auto make_bundle_inputs(const TimeSeriesInputData &data) {
    auto factory = bundle_input_factory{data};
    return make_bundle_fields<Schema, bundle_input_field>(factory);
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema, auto... TimeSeriesProperties,
          auto... Properties>
    requires(!details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
class In<TSB<Schema, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);

  public:
    using time_series_type = TSB<Schema, TimeSeriesProperties...>;
    using fields_type = Schema<details::bundle_input_field>;

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified},
          fields_{details::make_bundle_inputs<Schema>(data_)} {}

    bool valid() const noexcept { return data_.valid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const fields_type *operator->() const noexcept { return &fields_; }
    const fields_type &fields() const noexcept { return fields_; }
    bool item_updated(std::size_t index) const noexcept {
        return data_.runtime_type().item_updated(data_.last_updated_ref(), data_.data(), static_cast<item_id_t>(index));
    }

    template <typename Function> void for_each_updated(Function &&function) const {
        data_.runtime_type().for_each_updated_item(
            data_.last_updated_ref(), data_.data(), std::addressof(function),
            [](void *context, item_id_t id) { (*static_cast<std::remove_reference_t<Function> *>(context))(id); });
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
    }

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    details::TimeSeriesInputData data_;
    fields_type fields_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... TimeSeriesProperties,
          auto... Properties>
    requires(details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
class In<TSB<Schema, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::basic_input_properties<true, Properties...>::dynamic_activity> {
    using ParsedProperties = details::basic_input_properties<true, Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);

  public:
    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    bool valid() const noexcept { return data_.valid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    std::size_t size() const noexcept { return data_.runtime_type().item_count(data_.data()); }
    util::symbol item_name(std::size_t index) const {
        return static_cast<const TimeSeriesBundleType &>(data_.runtime_type().type()).item_name(index);
    }
    In<TS_T> operator[](std::size_t index) const {
        const auto item = data_.runtime_type().item(data_.data(), static_cast<item_id_t>(index));
        return {item.last_updated, item.value, item.runtime_type,
                data_.modified() && item.last_updated == data_.last_updated()};
    }
    bool item_updated(std::size_t index) const noexcept {
        return data_.runtime_type().item_updated(data_.last_updated_ref(), data_.data(), static_cast<item_id_t>(index));
    }

    template <typename Function> void for_each_updated(Function &&function) const {
        data_.runtime_type().for_each_updated_item(
            data_.last_updated_ref(), data_.data(), std::addressof(function),
            [](void *context, item_id_t id) { (*static_cast<std::remove_reference_t<Function> *>(context))(id); });
    }

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TSB<Schema, TimeSeriesProperties...>;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;
    static constexpr auto variadic_mode = details::variadic_mode<Properties...>();

  private:
    details::TimeSeriesInputData data_;
};

} // namespace bg
