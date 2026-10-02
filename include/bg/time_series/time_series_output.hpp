#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <ddv/serial_visitor.h>
#include <ddv/visitable.h>

namespace bg {

template <details::time_series_variable Variable> class Out<Variable> {
  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(TimeSeriesState &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{runtime_type}, context)) {
        state_->bind(last_updated, value, context);
    }

    Out(TimeSeriesState &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    const void *data() const noexcept { return state_->data(); }
    const Out *operator->() const noexcept { return this; }

    void invalidate() const { state_->invalidate(); }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit(Visitor &&visitor, Arguments &&...arguments) const;

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

    using time_series_type = Variable;

  private:
    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

static_assert(sizeof(Out<TS_T>) == sizeof(void *));

} // namespace bg

#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>
#include <bg/time_series/tss/tss_output.hpp>

namespace bg {

using TimeSeriesOutputTypes = ddv::mux<Out<TS<SCALAR>>, Out<TSL<TS_T, SIZE>>, Out<TSB<SCHEMA>>, Out<TSS<SCALAR>>,
                                       Out<TSD<SCALAR, TS_T>>, Out<REF<TS_T>>>;

template <details::time_series_variable Variable>
template <typename Visitor, typename... Arguments>
    requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
auto Out<Variable>::visit(Visitor &&visitor, Arguments &&...arguments) const {
    auto dispatch = ddv::make_visitable_visitor<TimeSeriesOutputTypes>(
        std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
    auto &types = static_cast<TimeSeriesOutputTypes &>(dispatch);
    runtime_type().type().visit(
        [&](const TimeSeriesValueType *) {
            auto output = details::output_factory::make<TS<SCALAR>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesListType *) {
            auto output = details::output_factory::make<TSL<TS_T, SIZE>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesBundleType *) {
            auto output = details::output_factory::make<TSB<SCHEMA>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesSetType *) {
            auto output = details::output_factory::make<TSS<SCALAR>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesDictionaryType *) {
            auto output = details::output_factory::make<TSD<SCALAR, TS_T>>(state_.get());
            types.visit(&output);
        },
        [&](const TimeSeriesReferenceType *) {
            auto output = details::output_factory::make<REF<TS_T>>(state_.get());
            types.visit(&output);
        },
        ddv::noop);

    if constexpr (!decltype(dispatch)::is_result_void) {
        return *dispatch;
    }
}

} // namespace bg
