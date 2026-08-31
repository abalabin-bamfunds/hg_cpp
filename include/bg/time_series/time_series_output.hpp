#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/time_series_type.hpp>

namespace bg {

template <> class Out<TS_T> {
  private:
    friend details::output_factory;

    explicit Out(TimeSeriesOutputView &view) noexcept : view_(view) {}

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context)
        : view_(view_type::copy(prototype, context)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(view_type::copy(prototype, output_path)) {
        view_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const {
        static_cast<const TimeSeriesValueOutputView &>(view_.get()).set_value(value.input_view());
    }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS_T;

  private:
    using view_type = details::output_view_proxy<TimeSeriesOutputView>;

    view_type view_;
};

} // namespace bg
