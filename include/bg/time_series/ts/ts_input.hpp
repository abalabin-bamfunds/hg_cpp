#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <string_view>
#include <utility>

namespace bg {

template <details::scalar_variable Variable, auto... TimeSeriesProperties, auto... Properties>
class In<TS<Variable, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);
    static_assert(!ParsedProperties::valid_items, "valid_items applies only to TSD inputs");

  public:
    In(const TimeSeriesState &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool all_valid() const noexcept { return valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    Scalar<Variable> value_view() const {
        return Scalar<Variable>::from(
            static_cast<const TimeSeriesValueRuntimeType &>(data_.runtime_type()).scalar_runtime_type(), data_.data());
    }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_}.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<Variable, TimeSeriesProperties...>;
    using value_type = Variable;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    details::TimeSeriesInputData data_;
};

template <typename T, auto... TimeSeriesProperties, auto... Properties>
class In<TS<T, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);
    static_assert(!ParsedProperties::valid_items, "valid_items applies only to TSD inputs");

  public:
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    In() noexcept
        : activity_control{},
          data_{cpp_time_series_runtime_type<TS<T, TimeSeriesProperties...>>(), initial_time_series_state, nullptr} {}

    In(const TimeSeriesState &last_updated, const Stored *value, bool modified = false,
       bool *activity = nullptr) noexcept
        requires(!scalar_declaration_traits<T>::is_collection && !details::hierarchical_scalar_declaration<T>)
        : activity_control{activity},
          data_{cpp_time_series_runtime_type<TS<T, TimeSeriesProperties...>>(), last_updated, value, modified} {}

    In(const TimeSeriesState &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool all_valid() const noexcept { return valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }

    typename scalar_storage_traits<T>::value_type value() const
        requires(!scalar_declaration_traits<T>::is_collection && !details::hierarchical_scalar_declaration<T>)
    {
        return Traits::read(*static_cast<const Stored *>(data_.data()));
    }

    Scalar<T> value() const
        requires details::hierarchical_scalar_declaration<T>
    {
        return value_view();
    }

    typename scalar_storage_traits<T>::value_type operator*() const
        requires(!scalar_declaration_traits<T>::is_collection && !details::hierarchical_scalar_declaration<T>)
    {
        return value();
    }

    Scalar<T> operator*() const
        requires details::hierarchical_scalar_declaration<T>
    {
        return value();
    }

    auto operator->() const
        requires(!scalar_declaration_traits<T>::is_collection && !details::hierarchical_scalar_declaration<T> &&
                 details::scalar_value_supports_arrow<T>)
    {
        return details::scalar_value_arrow<T>(value());
    }

    Scalar<T> value_view() const {
        return Scalar<T>::from(
            static_cast<const TimeSeriesValueRuntimeType &>(data_.runtime_type()).scalar_runtime_type(), data_.data());
    }

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_}.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<T, TimeSeriesProperties...>;
    using value_type = T;
    using storage_type = Stored;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    details::TimeSeriesInputData data_;
};

static_assert(sizeof(In<TS<int>>) == 3 * sizeof(void *));
static_assert(sizeof(In<TS<int>, dynamic_activity>) == 4 * sizeof(void *));

} // namespace bg
