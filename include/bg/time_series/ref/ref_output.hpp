#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_type.hpp>

namespace bg {

template <typename Target, auto... Properties> class Out<REF<Target, Properties...>> {
  public:
    using storage_type = details::ReferenceAddress;

  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : state_(
              Proxy::copy(details::OutputState{cpp_time_series_runtime_type<REF<Target, Properties...>>()}, context)) {
        state_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{runtime_type}, context)) {
        state_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_path)
        : state_(Proxy::copy(details::OutputState{cpp_time_series_runtime_type<REF<Target, Properties...>>()},
                             output_path)) {
        state_->bind(last_updated, &value, output_path);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }

    template <auto... ReferenceProperties, auto... InputProperties>
    void set(const In<REF<Target, ReferenceProperties...>, InputProperties...> &source) const {
        if (!source.valid()) {
            invalidate();
            return;
        }
        set_target(source.target());
    }

    void set_target(details::ReferenceAddress target) const {
        if (!target) {
            invalidate();
            return;
        }
        if (state_->suppresses_equal_updates() && valid() && reference() == target) {
            return;
        }

        reference() = target;
        state_->publish();
    }

    void invalidate() const {
        reference() = nullptr;
        state_->invalidate_value();
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = REF<Target, Properties...>;
    using target_type = Target;

  private:
    details::ReferenceAddress &reference() const noexcept {
        return *static_cast<details::ReferenceAddress *>(state_->mutable_data());
    }

    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

} // namespace bg
