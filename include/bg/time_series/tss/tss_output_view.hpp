#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/tss/tss_storage.hpp>

namespace bg {

class TimeSeriesSetOutputView : public TimeSeriesOutputView {
  public:
    virtual bool insert(const ScalarValueInputView &item) const = 0;
    virtual bool erase(const ScalarValueInputView &item) const = 0;
    virtual void clear() const = 0;

  private:
    void invalidate() const final {}

  protected:
    TimeSeriesSetOutputView() = default;
    explicit TimeSeriesSetOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesOutputView{representation} {}
};

class RuntimeTimeSeriesSetOutputView final : public TimeSeriesSetOutputView {
  public:
    explicit RuntimeTimeSeriesSetOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesSetOutputView{representation} {}

    bool insert(const ScalarValueInputView &item) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), key_representation(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result = value.insert(item, key_representation(), key_layout(), memory, [](std::uint32_t) {},
                                         [](std::uint32_t) {});
        if (result.change != details::time_series_set_change::none) {
            publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    bool erase(const ScalarValueInputView &item) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), key_representation(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result = value.erase(item, key_representation(), key_layout(), memory,
                                        [](std::uint32_t) noexcept {});
        if (result.change != details::time_series_set_change::none) {
            publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    void clear() const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        const auto should_publish = !valid() || value.size() != 0;
        value.reset_change_tracking(context().evaluation_time(), key_representation(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        value.clear(key_representation(), key_layout(), memory, [](std::uint32_t) noexcept {});

        if (should_publish) {
            publish();
        }
    }

  private:
    [[nodiscard]] details::time_series_set_storage &storage() const noexcept {
        return *static_cast<details::time_series_set_storage *>(mutable_value());
    }

    [[nodiscard]] const ScalarStorageRepresentation &key_representation() const noexcept {
        return *storage_representation().key_storage_representation();
    }

    [[nodiscard]] util::runtime_item_layout key_layout() const noexcept {
        return {key_representation().size(), key_representation().alignment()};
    }
};

static_assert(sizeof(TimeSeriesSetOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
