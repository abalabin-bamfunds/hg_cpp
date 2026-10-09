#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <ddv/serial_visitor.h>
#include <ddv/visitable.h>

#include <typeinfo>

namespace bg {

template <details::time_series_variable Variable> class Out<Variable> {
  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    const void *data() const noexcept { return state_->data(); }
    // Lets engine adapters rebuild a typed output while preserving its containing output path.
    OutputState &output_state() const noexcept { return state_.get(); }
    const Out *operator->() const noexcept { return this; }

    template <typename Output> Output as_output() const {
        using Target = typename output_declaration_traits<Output>::time_series_type;
        const auto &target_type = Target::type();
        const auto &actual_type = runtime_type().type();
        if (!target_type.is_resolved() || (&target_type != &actual_type && !target_type.accepts(actual_type))) {
            throw std::bad_cast{};
        }

        return details::output_factory::make<Output>(TimeSeriesOutputBinding{state_.get()});
    }

    template <typename Target> Out<Target> as() const {
        return as_output<Out<Target>>();
    }

    void invalidate() const { state_->invalidate(); }

    template <typename Input, auto... Properties>
    void copy_tick(const In<Input, Properties...> &input) const {
        state_->runtime_type().copy_tick(input.input_data(), state_.get());
    }

    template <typename Input, auto... Properties>
    void copy(const In<Input, Properties...> &input) const {
        state_->runtime_type().copy(input.input_data(), state_.get());
    }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit(Visitor &&visitor, Arguments &&...arguments) const;

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

    using time_series_type = Variable;

  private:
    TimeSeriesOutputBinding state_;
};

static_assert(sizeof(Out<TS_T>) == sizeof(void *));

} // namespace bg

#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>
#include <bg/time_series/tss/tss_output.hpp>
#include <bg/time_series/details/guarantee_variable_traits.hpp>

namespace bg::details {

template <typename Output>
concept direct_time_series_output = requires(TimeSeriesState &last_updated, typename Output::storage_type &value,
                                              EvaluationContext &context) {
    Output{last_updated, value, context};
};

template <typename Output>
inline constexpr bool output_requires_runtime_type = true;

template <typename TimeSeries>
inline constexpr bool output_requires_runtime_type<Out<TimeSeries>> =
    !direct_time_series_output<Out<TimeSeries>> || contains_guarantee_variable_v<TimeSeries>;

} // namespace bg::details

namespace bg {

using TimeSeriesOutputTypes = ddv::mux<Out<TS<SCALAR>>, Out<TSL<TS_T, SIZE>>, Out<TSB<SCHEMA>>, Out<TSS<SCALAR>>,
                                       Out<TSD<SCALAR, TS_T>>, Out<REF<TS_T>>, Out<TS_T>>;

template <details::time_series_variable Variable>
template <typename Visitor, typename... Arguments>
    requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
auto Out<Variable>::visit(Visitor &&visitor, Arguments &&...arguments) const {
    auto dispatch = ddv::make_visitable_visitor<TimeSeriesOutputTypes>(
        std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
    auto &types = static_cast<TimeSeriesOutputTypes &>(dispatch);
    runtime_type().type().visit(
        [&](const TimeSeriesValueType *) {
            auto output = details::output_factory::make<Out<TS<SCALAR>>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesListType *) {
            auto output = details::output_factory::make<Out<TSL<TS_T, SIZE>>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesBundleType *) {
            auto output = details::output_factory::make<Out<TSB<SCHEMA>>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesSetType *) {
            auto output = details::output_factory::make<Out<TSS<SCALAR>>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesDictionaryType *) {
            auto output = details::output_factory::make<Out<TSD<SCALAR, TS_T>>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesReferenceType *) {
            auto output = details::output_factory::make<Out<REF<TS_T>>>(state_.get());
            types.visit(&output);
        },
        [&] {
            auto output = details::output_factory::make<Out<TS_T>>(state_.get());
            types.visit(&output);
        });

    if constexpr (!decltype(dispatch)::is_result_void) {
        return *dispatch;
    }
}

} // namespace bg
