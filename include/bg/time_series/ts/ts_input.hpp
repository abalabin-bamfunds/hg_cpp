#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <string_view>

namespace bg {

template <details::scalar_variable Variable, auto... TimeSeriesProperties, auto... Properties>
class In<TS<Variable, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using view_type = util::polymorphic_value<TimeSeriesValueInputView>;

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesInputView &prototype) noexcept
        : view_(view_type::copy(static_cast<const TimeSeriesValueInputView &>(prototype))) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] Scalar<Variable> value_view() const { return view_->template value_view<Variable>(); }
    [[nodiscard]] const TimeSeriesValueInputView *operator->() const noexcept { return view_.operator->(); }
    [[nodiscard]] operator const TimeSeriesValueInputView &() const noexcept { return view_.get(); }

    using time_series_type = TS<Variable, TimeSeriesProperties...>;
    using value_type = Variable;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    view_type view_;
};

template <typename T, auto... TimeSeriesProperties, auto... Properties>
class In<TS<T, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using Traits = details::scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const Stored *value) noexcept : In() { view_->bind(last_updated, value); }

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesInputView &prototype) noexcept
        : view_(View::copy(static_cast<const TimeSeriesValueInputView &>(prototype))) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] details::scalar_value_t<T> value() const noexcept {
        return static_cast<const TypedTimeSeriesValueInputView<T> &>(view_.get()).value();
    }

    [[nodiscard]] Scalar<T> value_view() const {
        return static_cast<const TypedTimeSeriesValueInputView<T> &>(view_.get()).value_view();
    }

    [[nodiscard]] const TypedTimeSeriesValueInputView<T> *operator->() const noexcept {
        return &static_cast<const TypedTimeSeriesValueInputView<T> &>(view_.get());
    }

    [[nodiscard]] operator const TimeSeriesValueInputView &() const noexcept { return view_.get(); }

    using time_series_type = TS<T, TimeSeriesProperties...>;
    using value_type = T;
    using storage_type = Stored;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesValueInputView>;

    inline static const View prototype_ = View::template make<TypedTimeSeriesValueInputView<T>>();
    View view_;
};

} // namespace bg
