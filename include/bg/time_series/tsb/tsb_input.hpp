#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/tsb/tsb_input_view.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <string_view>
#include <type_traits>

namespace bg {
namespace details {

template <typename TimeSeries> using bundle_input_field = In<TimeSeries>;

class bundle_input_factory {
  public:
    explicit bundle_input_factory(const TimeSeriesBundleInputView &view) noexcept : view_{&view} {}

    template <typename Item> [[nodiscard]] auto make(std::size_t index) const {
        const auto item = view_->item(index);
        return In<Item>{item.last_updated, item.value, item.prototype};
    }

  private:
    const TimeSeriesBundleInputView *view_;
};

template <template <template <typename> typename> typename Schema>
[[nodiscard]] auto make_bundle_inputs(const TimeSeriesBundleInputView &view) {
    auto factory = bundle_input_factory{view};
    return make_bundle_fields<Schema, bundle_input_field>(factory);
}

} // namespace details

template <template <template <typename> typename> typename Schema, auto... TimeSeriesProperties, auto... Properties>
class In<TSB<Schema, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using time_series_type = TSB<Schema, TimeSeriesProperties...>;
    using fields_type = Schema<details::bundle_input_field>;

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesInputView &prototype) noexcept
        : view_(View::copy(static_cast<const TimeSeriesBundleInputView &>(prototype))),
          fields_{bind(last_updated, value)} {}

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] const fields_type *operator->() const noexcept { return &fields_; }
    [[nodiscard]] const fields_type &fields() const noexcept { return fields_; }
    [[nodiscard]] bool item_updated(std::size_t index) const noexcept { return view_->item_updated(index); }

    template <typename Function> void for_each_updated(Function &&function) const {
        auto callback = [](void *context, std::size_t index) {
            (*static_cast<std::remove_reference_t<Function> *>(context))(index);
        };
        view_->for_each_updated(&function, callback);
    }

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesBundleInputView>;

    [[nodiscard]] fields_type bind(const EvaluationTime &last_updated, const void *value) {
        view_->bind(last_updated, value);
        return details::make_bundle_inputs<Schema>(view_.get());
    }

    View view_;
    fields_type fields_;
};

} // namespace bg
