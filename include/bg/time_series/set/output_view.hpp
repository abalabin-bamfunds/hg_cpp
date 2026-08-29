#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/output_view.hpp>
#include <bg/time_series/set/storage.hpp>

namespace bg {

class TimeSeriesSetOutputView : public TimeSeriesOutputView {
  public:
    [[nodiscard]] virtual bool insert(const ScalarValueInputView &item) const = 0;
    [[nodiscard]] virtual bool erase(const ScalarValueInputView &item) const = 0;
};

template <typename K> class TypedTimeSeriesSetOutputView final : public TimeSeriesSetOutputView {
    using Storage = details::time_series_set_storage<K>;

  public:
    [[nodiscard]] bool insert(const ScalarValueInputView &item) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.prepare(context().evaluation_time(), last_updated(), memory, [](std::uint32_t) {});
        const auto result = value.insert(item, memory, [](std::uint32_t) {}, [](std::uint32_t) {});
        refresh_last_updated();
        if (value.changed()) {
            publish();
        }
        return result.change != details::time_series_set_change::none &&
               result.change != details::time_series_set_change::revalidated;
    }

    [[nodiscard]] bool erase(const ScalarValueInputView &item) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.prepare(context().evaluation_time(), last_updated(), memory, [](std::uint32_t) {});
        const auto result = value.erase(item, memory);
        refresh_last_updated();
        if (value.changed()) {
            publish();
        }
        return result.change != details::time_series_set_change::none;
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
