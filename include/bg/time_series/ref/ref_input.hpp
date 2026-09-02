#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/ref/ref_input_view.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <string_view>

namespace bg {

template <typename Target, auto... TimeSeriesProperties, auto... Properties>
class In<REF<Target, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::ReferenceStorage *;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept : In() {
        view_->bind(last_updated, value);
    }

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesInputView &prototype) noexcept
        : view_(View::copy(static_cast<const TimeSeriesReferenceInputView &>(prototype))) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] const TimeSeriesReferenceInputView *operator->() const noexcept { return view_.operator->(); }
    [[nodiscard]] operator const TimeSeriesReferenceInputView &() const noexcept { return view_.get(); }

    using time_series_type = REF<Target, TimeSeriesProperties...>;
    using target_type = Target;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesReferenceInputView>;

    inline static const View prototype_ = View::template make<TimeSeriesReferenceInputView>();
    View view_;
};

} // namespace bg
