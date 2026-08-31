#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <utility>

namespace bg {

template <details::scalar_variable Variable, auto... Properties> class Out<TS<Variable, Properties...>> {
  private:
    friend details::output_factory;

    explicit Out(TimeSeriesValueOutputView &view) noexcept : view_(view) {}

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context)
        : view_(view_type::copy(static_cast<const TimeSeriesValueOutputView &>(prototype), context)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(view_type::copy(static_cast<const TimeSeriesValueOutputView &>(prototype), output_path)) {
        view_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesValueOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS<Variable, Properties...>;

  private:
    using view_type = details::output_view_proxy<TimeSeriesValueOutputView>;

    view_type view_;
};

template <typename T, auto... Properties> class Out<TS<T, Properties...>> {
    using Traits = details::scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

  private:
    friend details::output_factory;

    explicit Out(TypedTimeSeriesValueOutputView<T> &view) noexcept : view_(view) {}

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : view_(View::copy(prototype().get(), context)) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_path)
        : view_(View::copy(prototype().get(), output_path)) {
        view_->bind(last_updated, &value, output_path);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(View::copy(static_cast<const TypedTimeSeriesValueOutputView<T> &>(prototype), output_path)) {
        view_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] value_type value() const noexcept { return view_->value(); }

    template <typename Value>
        requires requires(storage_type &target, Value &&source) { target = std::forward<Value>(source); }
    void set(Value &&value) const noexcept(noexcept(view_->set(std::forward<Value>(value)))) {
        view_->set(std::forward<Value>(value));
    }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TypedTimeSeriesValueOutputView<T> *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS<T, Properties...>;

  private:
    using View = details::output_view_proxy<TypedTimeSeriesValueOutputView<T>>;

    [[nodiscard]] static const auto &prototype() noexcept {
        static const auto value = util::polymorphic_value<TypedTimeSeriesValueOutputView<T>>::template make<
            TypedTimeSeriesValueOutputView<T>>();
        return value;
    }

    View view_;
};

static_assert(sizeof(Out<TS<int>>) == sizeof(void *));

} // namespace bg
