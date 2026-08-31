#pragma once

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input_view.hpp>

namespace bg {

class TimeSeriesReferenceInputView final : public TimeSeriesInputView {
  public:
    VISITOR_SUPPORT(final)

    [[nodiscard]] details::ReferenceStorage *target() const noexcept {
        return *static_cast<details::ReferenceStorage *const *>(value_);
    }
};

} // namespace bg
