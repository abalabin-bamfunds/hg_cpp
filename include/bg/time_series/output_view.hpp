#pragma once

#include <bg/scalar/value.hpp>
#include <bg/time_series/view.hpp>

namespace bg {

class TimeSeriesOutputView : public TimeSeriesView {
  public:
    void bind(EvaluationTime &last_updated, void *value, const EvaluationTime &evaluation_time) noexcept {
        last_updated_ = &last_updated;
        value_ = value;
        evaluation_time_ = &evaluation_time;
    }

    void invalidate() const noexcept { *mutable_last_updated() = MIN_DT; }

    virtual void set_value(const ScalarInputView &value) const = 0;

  protected:
    [[nodiscard]] EvaluationTime *mutable_last_updated() const noexcept {
        return const_cast<EvaluationTime *>(last_updated_);
    }

    [[nodiscard]] void *mutable_value() const noexcept { return const_cast<void *>(value_); }

    void publish() const noexcept { *mutable_last_updated() = *evaluation_time_; }

  private:
    const EvaluationTime *evaluation_time_{&MIN_DT};
};

class TimeSeriesValueOutputView : public TimeSeriesOutputView {};

template <typename T> class TypedTimeSeriesValueOutputView final : public TimeSeriesValueOutputView {
    using Traits = details::scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

    [[nodiscard]] value_type value() const noexcept { return Traits::read(*static_cast<const storage_type *>(value_)); }

    template <typename Value>
        requires requires(storage_type &target, Value &&source) { target = std::forward<Value>(source); }
    void set(Value &&value) const
        noexcept(noexcept(*static_cast<storage_type *>(mutable_value()) = std::forward<Value>(value))) {
        *static_cast<storage_type *>(mutable_value()) = std::forward<Value>(value);
        publish();
    }

    void set_value(const ScalarInputView &value) const override;
};

static_assert(sizeof(TimeSeriesOutputView) == 4 * sizeof(void *));
static_assert(sizeof(TimeSeriesValueOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
