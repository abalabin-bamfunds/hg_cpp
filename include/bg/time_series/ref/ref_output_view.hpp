#pragma once

#include <bg/time_series/ref/ref_input_view.hpp>
#include <bg/time_series/time_series_output_view.hpp>

namespace bg {

class TimeSeriesReferenceOutputView final : public TimeSeriesOutputView {
  public:
    void set_reference(const TimeSeriesReferenceInputView &source) const {
        if (!source.valid()) {
            invalidate();
            return;
        }

        reference() = source.target();
        publish();
    }

    void invalidate() const override {
        reference() = nullptr;
        TimeSeriesOutputView::invalidate();
    }

  private:
    [[nodiscard]] details::ReferenceStorage *&reference() const noexcept {
        return *static_cast<details::ReferenceStorage **>(mutable_value());
    }
};

} // namespace bg
