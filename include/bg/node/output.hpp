#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/output_view.hpp>
#include <bg/time_series/type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <utility>

namespace bg {

template <typename TimeSeries> class Out;

template <> class Out<TS_T> {
  public:
    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        const EvaluationTime &evaluation_time) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, evaluation_time);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context) noexcept
        : Out(last_updated, value, prototype, context.evaluation_time()) {}

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const noexcept { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS_T;

  private:
    using view_type = util::polymorphic_value<TimeSeriesOutputView>;

    view_type view_;
};

template <details::scalar_variable Variable, auto... Properties> class Out<TS<Variable, Properties...>> {
  public:
    Out(EvaluationTime &last_updated, void *value, const TimeSeriesValueOutputView &prototype,
        const EvaluationTime &evaluation_time) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, evaluation_time);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesValueOutputView &prototype,
        EvaluationContext &context) noexcept
        : Out(last_updated, value, prototype, context.evaluation_time()) {}

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const noexcept { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesValueOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS<Variable, Properties...>;

  private:
    using view_type = util::polymorphic_value<TimeSeriesValueOutputView>;

    view_type view_;
};

template <typename T, auto... Properties> class Out<TS<T, Properties...>> {
    using Traits = details::scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

    Out(EvaluationTime &last_updated, storage_type &value, const EvaluationTime &evaluation_time) noexcept
        : view_(prototype_) {
        view_->bind(last_updated, &value, evaluation_time);
    }

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context) noexcept
        : Out(last_updated, value, context.evaluation_time()) {}

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] value_type value() const noexcept { return view_->value(); }

    template <typename Value>
        requires requires(storage_type &target, Value &&source) { target = std::forward<Value>(source); }
    void set(Value &&value) const noexcept(noexcept(view_->set(std::forward<Value>(value)))) {
        view_->set(std::forward<Value>(value));
    }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const noexcept { view_->invalidate(); }

    [[nodiscard]] const TypedTimeSeriesValueOutputView<T> *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS<T, Properties...>;

  private:
    using View = util::polymorphic_value<TypedTimeSeriesValueOutputView<T>>;

    inline static const View prototype_ = View::template make<TypedTimeSeriesValueOutputView<T>>();

    View view_;
};

static_assert(sizeof(Out<TS<int>>) == 4 * sizeof(void *));

} // namespace bg
