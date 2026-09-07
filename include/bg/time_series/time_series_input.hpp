#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <ddv/serial_visitor.h>
#include <ddv/visitable.h>

#include <string_view>
#include <type_traits>
#include <utility>

namespace bg {

template <details::time_series_variable Variable, auto... Properties> class In<Variable, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type) noexcept
        : data_{runtime_type, last_updated, value} {}

    bool valid() const noexcept { return data_.valid(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

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

  private:
    details::TimeSeriesInputData data_;
};

static_assert(sizeof(In<TS_T>) == 3 * sizeof(void *));

} // namespace bg

#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>

namespace bg {

using TimeSeriesInputTypes = ddv::mux<In<TS<SCALAR>>, In<TSL<TS_T, SIZE>>, In<TSB<SCHEMA>>, In<TSS<SCALAR>>,
                                      In<TSD<SCALAR, TS_T>>, In<REF<TS_T>>>;

template <details::time_series_variable Variable, auto... Properties>
template <typename Visitor, typename... Arguments>
    requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
auto In<Variable, Properties...>::visit(Visitor &&visitor, Arguments &&...arguments) const {
    auto dispatch = ddv::make_visitable_visitor<TimeSeriesInputTypes>(
        std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
    auto &types = static_cast<TimeSeriesInputTypes &>(dispatch);
    runtime_type().type().visit(
        [&](const TimeSeriesValueType *) {
            auto input = In<TS<SCALAR>>{data_.last_updated_ref(), data(), runtime_type()};
            types.visit(&input);
        },
        [&](const TimeSeriesListType *) {
            auto input = In<TSL<TS_T, SIZE>>{data_.last_updated_ref(), data(), runtime_type()};
            types.visit(&input);
        },
        [&](const TimeSeriesBundleType *) {
            auto input = In<TSB<SCHEMA>>{data_.last_updated_ref(), data(), runtime_type()};
            types.visit(&input);
        },
        [&](const TimeSeriesSetType *) {
            auto input = In<TSS<SCALAR>>{data_.last_updated_ref(), data(), runtime_type()};
            types.visit(&input);
        },
        [&](const TimeSeriesDictionaryType *) {
            auto input = In<TSD<SCALAR, TS_T>>{data_.last_updated_ref(), data(), runtime_type()};
            types.visit(&input);
        },
        [&](const TimeSeriesReferenceType *) {
            auto input = In<REF<TS_T>>{data_.last_updated_ref(), data(), runtime_type()};
            types.visit(&input);
        },
        ddv::noop);

    if constexpr (!decltype(dispatch)::is_result_void) {
        return *dispatch;
    }
}

} // namespace bg
