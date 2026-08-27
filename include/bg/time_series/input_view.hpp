#pragma once

#include <bg/scalar/value.hpp>
#include <bg/time_series/view.hpp>

#include <ddv/visitable.h>

namespace bg {

class TimeSeriesValueInputView;

using TimeSeriesInputViewTypes = ddv::mux<TimeSeriesValueInputView>;

class TimeSeriesInputView : public TimeSeriesView,
                            public ddv::visitable<TimeSeriesInputView, TimeSeriesInputViewTypes> {
  public:
    void bind(const EvaluationTime &last_updated, const void *value) noexcept {
        TimeSeriesView::bind(last_updated, value);
    }

    [[nodiscard]] const void *data() const noexcept { return value_; }

    [[nodiscard]] Scalar<> value_view() const { return Scalar<>::from(scalar_view()); }

  protected:
    [[nodiscard]] virtual details::ErasedScalarView scalar_view() const = 0;
};

class TimeSeriesValueInputView : public TimeSeriesInputView {
  public:
    VISITOR_SUPPORT(final)

    [[nodiscard]] Scalar<SCALAR> value_view() const { return Scalar<SCALAR>::from(scalar_view()); }

    template <details::scalar_variable Variable> [[nodiscard]] Scalar<Variable> value_view() const {
        return Scalar<Variable>::from(scalar_view());
    }
};

template <typename T> class TypedTimeSeriesValueInputView final : public TimeSeriesValueInputView {
    using Traits = details::scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

  public:
    [[nodiscard]] details::scalar_value_t<T> value() const noexcept {
        return Traits::read(*static_cast<const Stored *>(value_));
    }

    [[nodiscard]] Scalar<T> value_view() const { return Scalar<T>::from(*static_cast<const Stored *>(value_)); }

    using value_type = T;

  protected:
    [[nodiscard]] details::ErasedScalarView scalar_view() const override {
        return details::make_borrowed_scalar<ScalarValueInputView, T>(static_cast<const Stored *>(value_));
    }
};

static_assert(sizeof(TimeSeriesInputView) == 3 * sizeof(void *));
static_assert(sizeof(TimeSeriesValueInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
