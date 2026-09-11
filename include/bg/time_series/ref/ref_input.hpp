#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <string_view>

namespace bg {

template <typename Target, auto... TimeSeriesProperties, auto... Properties>
class In<REF<Target, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = Reference;

    In() noexcept : data_{cpp_time_series_runtime_type<REF<Target, TimeSeriesProperties...>>(), MIN_DT, nullptr} {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept
        : data_{cpp_time_series_runtime_type<REF<Target, TimeSeriesProperties...>>(), last_updated, value} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type) noexcept
        : data_{runtime_type, last_updated, value} {}

    bool valid() const noexcept { return data_.valid(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    Reference value() const noexcept { return data_.runtime_type().reference(data_.data()); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = REF<Target, TimeSeriesProperties...>;
    using target_type = Target;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    details::TimeSeriesInputData data_;
};

} // namespace bg
