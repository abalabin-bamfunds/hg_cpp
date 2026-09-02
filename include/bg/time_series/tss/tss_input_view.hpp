#pragma once

#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/tss/tss_storage.hpp>

#include <cstddef>
#include <functional>

namespace bg {

class TimeSeriesSetInputView : public TimeSeriesInputView {
  public:
    VISITOR_SUPPORT(final)

    struct Item {
        const ScalarValueInputView &prototype;
        const void *data;
    };

    using ItemFunction = void (*)(void *, Item);

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool contains(const ScalarValueInputView &item) const = 0;
    virtual void for_each(void *context, ItemFunction function) const = 0;
    virtual void for_each_added(void *context, ItemFunction function) const = 0;
    virtual void for_each_removed(void *context, ItemFunction function) const = 0;

  protected:
    TimeSeriesSetInputView() = default;
    explicit TimeSeriesSetInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesInputView{representation} {}
};

class RuntimeTimeSeriesSetInputView final : public TimeSeriesSetInputView {
  public:
    explicit RuntimeTimeSeriesSetInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesSetInputView{representation} {}

    [[nodiscard]] std::size_t size() const noexcept override { return valid() ? storage().size() : 0; }

    [[nodiscard]] bool contains(const ScalarValueInputView &item) const override {
        return valid() && storage().contains(item, key_representation(), key_layout());
    }

    void for_each(void *context, ItemFunction function) const override {
        if (valid()) {
            apply(storage().live(), context, function);
        }
    }

    void for_each_added(void *context, ItemFunction function) const override {
        if (!valid() || storage().change_time() != last_updated()) {
            return;
        }

        apply(storage().added(), context, function);
    }

    void for_each_removed(void *context, ItemFunction function) const override {
        if (!valid()) {
            apply(storage().removed(), context, function);
            return;
        }
        if (storage().change_time() == last_updated()) {
            apply(storage().removed(), context, function);
        }
    }

  private:
    [[nodiscard]] const details::time_series_set_storage &storage() const noexcept {
        return *static_cast<const details::time_series_set_storage *>(value_);
    }

    [[nodiscard]] const ScalarStorageRepresentation &key_representation() const noexcept {
        return *storage_representation().key_storage_representation();
    }

    [[nodiscard]] util::runtime_item_layout key_layout() const noexcept {
        return {key_representation().size(), key_representation().alignment()};
    }

    void apply(const util::index<> &positions, void *context, ItemFunction function) const {
        const auto &prototype = *key_representation().scalar_input_view_prototype();
        for (const auto position : positions) {
            function(context, {prototype, storage().key(position, key_layout())});
        }
    }
};

static_assert(sizeof(TimeSeriesSetInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
