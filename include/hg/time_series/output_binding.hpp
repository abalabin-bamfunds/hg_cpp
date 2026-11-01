#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/time_series/output_state.hpp>

namespace bg {

// Owns or borrows the output state used by an Out view. Values constructed from storage retain the existing
// scratch-backed ownership until the view releases them.
class TimeSeriesOutputBinding {
  public:
    explicit TimeSeriesOutputBinding(OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    TimeSeriesOutputBinding(TimeSeriesState &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
                            EvaluationContext &context)
        : state_(Proxy::copy(OutputState{runtime_type}, context)) {
        state_->bind(last_updated, value, context);
    }

    TimeSeriesOutputBinding(TimeSeriesState &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
                            void *output_path)
        : state_(Proxy::copy(OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

    TimeSeriesOutputBinding(TimeSeriesOutputBinding &&) noexcept = default;
    TimeSeriesOutputBinding(const TimeSeriesOutputBinding &) = delete;

    OutputState &get() const noexcept { return state_.get(); }
    OutputState *operator->() const noexcept { return state_.operator->(); }

  private:
    using Proxy = details::output_proxy<OutputState>;

    Proxy state_;
};

} // namespace bg
