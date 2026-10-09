#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/guarantee_variable_traits.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <ddv/serial_visitor.h>
#include <ddv/visitable.h>

#include <string_view>
#include <typeinfo>
#include <type_traits>
#include <utility>

namespace bg {

template <details::time_series_variable Variable, auto... Properties>
class In<Variable, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);
    static_assert(!ParsedProperties::valid_items, "valid_items applies only to TSD inputs");

  public:
    In(const TimeSeriesState &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    // Binds this view to caller-owned input metadata and storage.
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool all_valid() const noexcept {
        return data_.valid() && data_.runtime_type().all_valid(data_.last_updated_ref(), data_.data());
    }
    // The declared type must accept this input's runtime type.
    bool all_valid(const TimeSeriesType &declared_type) const noexcept {
        return data_.valid() && data_.runtime_type().all_valid(data_.last_updated_ref(), data_.data(), declared_type);
    }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    // Returns a provider-defined view when its semantic type accepts this input's type.
    template <typename Target> In<Target> as() const {
        const auto &target_type = Target::type();
        const auto &actual_type = runtime_type().type();
        if (!target_type.is_resolved() || (&target_type != &actual_type && !target_type.accepts(actual_type))) {
            throw std::bad_cast{};
        }

        return In<Target>{TimeSeriesInputData{runtime_type(), data_.last_updated_ref(), data(), data_.modified()}};
    }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit(Visitor &&visitor, Arguments &&...arguments) const;

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

    using time_series_type = Variable;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    details::TimeSeriesInputData data_;
};

static_assert(sizeof(In<TS_T>) == 3 * sizeof(void *));
static_assert(sizeof(In<TS_T, dynamic_activity>) == 4 * sizeof(void *));

} // namespace bg

#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>

namespace bg::details {

template <typename Input>
concept direct_time_series_input = requires(const TimeSeriesState &last_updated,
                                              const typename Input::storage_type *value, bool *activity) {
    Input{last_updated, value, false, activity};
};

template <typename Input>
inline constexpr bool input_requires_runtime_type = true;

template <typename TimeSeries, auto... Properties>
inline constexpr bool input_requires_runtime_type<In<TimeSeries, Properties...>> =
    !direct_time_series_input<In<TimeSeries, Properties...>> || generic_time_series<TimeSeries> ||
    contains_guarantee_variable_v<TimeSeries> || is_time_series_set<TimeSeries>;

} // namespace bg::details

namespace bg {

using TimeSeriesInputTypes = ddv::mux<In<TS<SCALAR>>, In<TSL<TS_T, SIZE>>, In<TSB<SCHEMA>>, In<TSS<SCALAR>>,
                                      In<TSD<SCALAR, TS_T>>, In<REF<TS_T>>, In<TS_T>>;

template <details::time_series_variable Variable, auto... Properties>
template <typename Visitor, typename... Arguments>
    requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
auto In<Variable, Properties...>::visit(Visitor &&visitor, Arguments &&...arguments) const {
    auto dispatch = ddv::make_visitable_visitor<TimeSeriesInputTypes>(
        std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
    auto &types = static_cast<TimeSeriesInputTypes &>(dispatch);
    runtime_type().type().visit(
        [&](const TimeSeriesValueType *) {
            auto input = In<TS<SCALAR>>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        },
        [&](const TimeSeriesListType *) {
            auto input = In<TSL<TS_T, SIZE>>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        },
        [&](const TimeSeriesBundleType *) {
            auto input = In<TSB<SCHEMA>>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        },
        [&](const TimeSeriesSetType *) {
            auto input = In<TSS<SCALAR>>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        },
        [&](const TimeSeriesDictionaryType *) {
            auto input = In<TSD<SCALAR, TS_T>>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        },
        [&](const TimeSeriesReferenceType *) {
            auto input = In<REF<TS_T>>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        },
        [&] {
            auto input = In<TS_T>{data_.last_updated_ref(), data(), runtime_type(), data_.modified()};
            types.visit(&input);
        });

    if constexpr (!decltype(dispatch)::is_result_void) {
        return *dispatch;
    }
}

} // namespace bg
