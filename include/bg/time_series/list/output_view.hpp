#pragma once

#include <bg/time_series/list/storage.hpp>
#include <bg/time_series/output_view.hpp>
#include <bg/time_series/storage_representation.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <cstddef>

namespace bg {

class TimeSeriesListOutputView : public TimeSeriesOutputView {
  public:
    struct Item {
        const TimeSeriesOutputView &prototype;
        EvaluationTime &last_updated;
        void *value;
        void *output_mutation;
    };

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual Item item(std::size_t index) const = 0;
};

template <typename ItemTimeSeries, std::size_t Size>
class TypedTimeSeriesListOutputView final : public TimeSeriesListOutputView {
    using TimeSeries = TSL<ItemTimeSeries, Size>;
    using Storage = details::fixed_time_series_list_storage<ItemTimeSeries, Size>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return Size; }

    [[nodiscard]] TimeSeriesListOutputView::Item item(std::size_t index) const override {
        const auto &item_representation = cpp_time_series_storage_representation<ItemTimeSeries>();
        auto &item = static_cast<Storage *>(mutable_value())->items.at(index);
        const auto output_mutation = add_output_mutation(cpp_time_series_storage_representation<TimeSeries>(), index);

        return {item_representation.output_view_prototype(), item.last_updated, &item.value.value, output_mutation};
    }

    void invalidate() const override {
        for (auto index = std::size_t{}; index < Size; ++index) {
            const auto child = item(index);
            auto view = util::polymorphic_value<TimeSeriesOutputView>::copy(child.prototype);
            view->bind(child.last_updated, child.value, child.output_mutation);
            view->invalidate();
        }
    }
};

static_assert(sizeof(TimeSeriesListOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
