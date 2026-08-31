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
};

template <typename K> class TypedTimeSeriesSetOutputView final : public TimeSeriesSetOutputView {
    using Storage = details::time_series_set_storage<K>;

  public:
    bool insert(const ScalarValueInputView &item) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), last_updated(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result = value.insert(item, memory, [](std::uint32_t) {}, [](std::uint32_t) {});
        refresh_last_updated();
        if (value.changed()) {
            publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    bool erase(const ScalarValueInputView &item) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), last_updated(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result = value.erase(item, memory, [](std::uint32_t) noexcept {});
        refresh_last_updated();
        if (value.changed()) {
            publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    void clear() const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), last_updated(), memory,
                                    [](std::uint32_t) noexcept {});
        value.clear(memory, [](std::uint32_t) noexcept {});

        if (value.previous_last_updated() <= MIN_DT || value.changed()) {
            publish();
        } else {
            refresh_last_updated();
        }
    }

  private:
    [[nodiscard]] Storage &storage() const noexcept { return *static_cast<Storage *>(mutable_value()); }

    void refresh_last_updated() const noexcept {
        auto &value = storage();
        *mutable_last_updated() = value.changed() ? context().evaluation_time() : value.previous_last_updated();
    }
};

static_assert(sizeof(TimeSeriesSetOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
