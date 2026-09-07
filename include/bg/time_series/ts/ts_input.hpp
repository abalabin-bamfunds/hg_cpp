#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/ts/details/value_access.hpp>

#include <string_view>
#include <utility>

namespace bg {

template <details::scalar_variable Variable, auto... TimeSeriesProperties, auto... Properties>
class In<TS<Variable, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type) noexcept
        : data_{runtime_type, last_updated, value} {}

    bool valid() const noexcept { return data_.valid(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    Scalar<Variable> value_view() const {
        return Scalar<Variable>::from(
            static_cast<const TimeSeriesValueRuntimeType &>(data_.runtime_type()).scalar_runtime_type(), data_.data());
    }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<Variable, TimeSeriesProperties...>;
    using value_type = Variable;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    details::TimeSeriesInputData data_;
};

template <typename T, auto... TimeSeriesProperties, auto... Properties>
class In<TS<T, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using Traits = details::scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    In() noexcept : data_{cpp_time_series_runtime_type<TS<T, TimeSeriesProperties...>>(), MIN_DT, nullptr} {}

    In(const EvaluationTime &last_updated, const Stored *value) noexcept
        : data_{cpp_time_series_runtime_type<TS<T, TimeSeriesProperties...>>(), last_updated, value} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type) noexcept
        : data_{runtime_type, last_updated, value} {}

    bool valid() const noexcept { return data_.valid(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }

    details::scalar_value_t<T> value() const noexcept
        requires(!details::scalar_type_traits<T>::is_collection)
    {
        return Traits::read(*static_cast<const Stored *>(data_.data()));
    }

    details::scalar_value_t<T> operator*() const noexcept
        requires(!details::scalar_type_traits<T>::is_collection)
    {
        return value();
    }

    auto operator->() const noexcept
        requires(!details::scalar_type_traits<T>::is_collection && details::scalar_value_supports_arrow<T>)
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
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<T, TimeSeriesProperties...>;
    using value_type = T;
    using storage_type = Stored;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    details::TimeSeriesInputData data_;
};

} // namespace bg
