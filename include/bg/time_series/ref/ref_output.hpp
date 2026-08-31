#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/time_series/ref/ref_output_view.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/polymorphic_value.hpp>

namespace bg {

template <typename Target, auto... Properties> class Out<REF<Target, Properties...>> {
  public:
    using storage_type = details::ReferenceStorage *;

  private:
    friend details::output_factory;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : view_(View::copy(prototype().get(), context)) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context)
        : view_(View::copy(static_cast<const TimeSeriesReferenceOutputView &>(prototype), context)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_path)
        : view_(View::copy(prototype().get(), output_path)) {
        view_->bind(last_updated, &value, output_path);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(View::copy(static_cast<const TimeSeriesReferenceOutputView &>(prototype), output_path)) {
        view_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <auto... ReferenceProperties, auto... InputProperties>
    void set(const In<REF<Target, ReferenceProperties...>, InputProperties...> &source) const {
        view_->set_reference(*source.operator->());
    }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesReferenceOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = REF<Target, Properties...>;
    using target_type = Target;

  private:
    using View = details::output_view_proxy<TimeSeriesReferenceOutputView>;

    [[nodiscard]] static const auto &prototype() noexcept {
        static const auto value =
            util::polymorphic_value<TimeSeriesReferenceOutputView>::template make<TimeSeriesReferenceOutputView>();
        return value;
    }

    View view_;
};

} // namespace bg
