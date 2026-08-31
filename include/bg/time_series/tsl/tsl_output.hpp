#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/time_series/tsl/tsl_output_view.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/util/polymorphic_value.hpp>

namespace bg {

template <typename Item, std::size_t Size, auto... Properties> class Out<TSL<Item, Size, Properties...>> {
    static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");

  public:
    using storage_type = details::fixed_time_series_list_storage<Item, Size>;

  private:
    friend details::output_factory;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : view_(View::copy(prototype().get(), context)) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context)
        : view_(View::copy(static_cast<const TimeSeriesListOutputView &>(prototype), context)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(View::copy(static_cast<const TimeSeriesListOutputView &>(prototype), output_path)) {
        view_->bind(last_updated, value, output_path);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_path)
        : view_(View::copy(prototype().get(), output_path)) {
        view_->bind(last_updated, &value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] auto operator[](std::size_t index) const {
        const auto item = view_->item(index);
        return details::output_factory::make<Item>(item.last_updated, item.value, item.prototype, item.output_path);
    }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesListOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSL<Item, Size, Properties...>;
    using item_type = Item;

    static constexpr std::size_t size = Size;

  private:
    using View = details::output_view_proxy<TimeSeriesListOutputView>;

    [[nodiscard]] static const auto &prototype() noexcept {
        static const auto value = util::polymorphic_value<TimeSeriesListOutputView>::template make<
            TypedTimeSeriesListOutputView<Item, Size>>();
        return value;
    }

    View view_;
};

} // namespace bg
