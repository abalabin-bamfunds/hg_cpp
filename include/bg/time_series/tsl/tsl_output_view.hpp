#pragma once

#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <cstddef>
#include <stdexcept>

namespace bg {

class TimeSeriesListOutputView : public TimeSeriesOutputView {
  public:
    struct Item {
        const TimeSeriesOutputView &prototype;
        EvaluationTime &last_updated;
        void *value;
        void *output_path;
    };

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual Item item(std::size_t index) const = 0;

  protected:
    TimeSeriesListOutputView() = default;
    explicit TimeSeriesListOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesOutputView{representation} {}
};

class RuntimeTimeSeriesListOutputView final : public TimeSeriesListOutputView {
  public:
    explicit RuntimeTimeSeriesListOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesListOutputView{representation} {}

    [[nodiscard]] std::size_t size() const noexcept override {
        return static_cast<const TimeSeriesListType &>(storage_representation().type()).item_count();
    }

    [[nodiscard]] TimeSeriesListOutputView::Item item(std::size_t index) const override {
        const auto field = storage_representation().fixed_item(index);
        auto *bytes = static_cast<std::byte *>(mutable_value());
        const auto output_path = add_output_path(storage_representation(), index);
        const auto *prototype = field.representation.output_view_prototype();
        if (prototype == nullptr) {
            throw std::logic_error{"time series list item storage does not provide a mutable view"};
        }

        return {*prototype, *reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset),
                bytes + field.value_offset, output_path};
    }

    void invalidate() const override {
        for (auto index = std::size_t{}; index < size(); ++index) {
            const auto child = item(index);
            auto view = util::polymorphic_value<TimeSeriesOutputView>::copy(child.prototype);
            view->bind(child.last_updated, child.value, child.output_path);
            view->invalidate();
            view->release_output_path();
        }
    }
};

static_assert(sizeof(TimeSeriesListOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
