#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/time_series/tsb/tsb_output_view.hpp>
#include <bg/time_series/tsb/tsb_storage_representation.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

namespace bg {
namespace details {

template <typename TimeSeries> using bundle_output_field = Out<TimeSeries>;

class bundle_output_factory {
  public:
    explicit bundle_output_factory(const TimeSeriesBundleOutputView &view) noexcept : view_{&view} {}

    template <typename Item> [[nodiscard]] auto make(std::size_t index) const {
        const auto item = view_->item(index);
        return output_factory::make<Item>(item.last_updated, item.value, item.prototype, item.output_path);
    }

  private:
    const TimeSeriesBundleOutputView *view_;
};

template <template <template <typename> typename> typename Schema>
[[nodiscard]] auto make_bundle_outputs(const TimeSeriesBundleOutputView &view) {
    auto factory = bundle_output_factory{view};
    return make_bundle_fields<Schema, bundle_output_field>(factory);
}

} // namespace details

template <template <template <typename> typename> typename Schema, auto... Properties>
class Out<TSB<Schema, Properties...>> {
  public:
    using time_series_type = TSB<Schema, Properties...>;
    using fields_type = Schema<details::bundle_output_field>;

  private:
    friend details::output_factory;

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, EvaluationContext &context)
        : view_(View::copy(static_cast<const TimeSeriesBundleOutputView &>(prototype), context)),
          fields_{bind(last_updated, value, context)} {}

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(View::copy(static_cast<const TimeSeriesBundleOutputView &>(prototype), output_path)),
          fields_{bind(last_updated, value, output_path)} {}

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] const fields_type *operator->() const noexcept { return &fields_; }
    [[nodiscard]] const fields_type &fields() const noexcept { return fields_; }
    void invalidate() const { view_->invalidate(); }

  private:
    using View = details::output_view_proxy<TimeSeriesBundleOutputView>;

    [[nodiscard]] fields_type bind(EvaluationTime &last_updated, void *value, EvaluationContext &context) {
        view_->bind(last_updated, value, context);
        return details::make_bundle_outputs<Schema>(view_.get());
    }

    [[nodiscard]] fields_type bind(EvaluationTime &last_updated, void *value, void *output_path) {
        view_->bind(last_updated, value, output_path);
        return details::make_bundle_outputs<Schema>(view_.get());
    }

    View view_;
    fields_type fields_;
};

} // namespace bg
