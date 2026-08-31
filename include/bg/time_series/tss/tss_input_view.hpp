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
};

template <typename K> class TypedTimeSeriesSetInputView final : public TimeSeriesSetInputView {
    using Storage = details::time_series_set_storage<K>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return valid() ? storage().size() : 0; }

    [[nodiscard]] bool contains(const ScalarValueInputView &item) const override {
        return valid() && storage().contains(item);
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

        apply(storage().revalidated() ? storage().live() : storage().added(), context, function);
    }

    void for_each_removed(void *context, ItemFunction function) const override {
        if (!valid()) {
            apply(storage().live(), context, function);
            return;
        }
        if (storage().change_time() == last_updated() && !storage().revalidated()) {
            apply(storage().removed(), context, function);
        }
    }

  private:
    [[nodiscard]] const Storage &storage() const noexcept { return *static_cast<const Storage *>(value_); }

    void apply(const util::index<> &positions, void *context, ItemFunction function) const {
        const auto &representation = cpp_scalar_storage_representation<K>();
        const auto &prototype = *representation.scalar_input_view_prototype();
        for (const auto position : positions) {
            function(context, {prototype, storage().key(position)});
        }
    }
};

static_assert(sizeof(TimeSeriesSetInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
