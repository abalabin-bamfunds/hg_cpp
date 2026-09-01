#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/tsd/tsd_input_view.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <functional>
#include <string_view>

namespace bg {

template <typename K, typename Item, auto... TimeSeriesProperties, auto... Properties>
class In<TSD<K, Item, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_dict_storage<K, Item>;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept : In() {
        view_->bind(last_updated, value);
    }

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesDictionaryInputView &prototype) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] std::size_t size() const noexcept { return view_->size(); }

    [[nodiscard]] bool contains(const Scalar<K> &key) const { return view_->contains(key.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool contains(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return view_->contains(view.get());
    }

    [[nodiscard]] auto operator[](const Scalar<K> &key) const { return make_item(view_->at(key.input_view())); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] auto operator[](Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return make_item(view_->at(view.get()));
    }

    template <typename Function> void for_each(Function &&function) const {
        view_->for_each(std::addressof(function), [](void *context, TimeSeriesDictionaryInputView::Entry entry) {
            auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
            std::invoke(callback, Scalar<K>::from(entry.key.prototype, entry.key.data), make_item(entry.item));
        });
    }

    template <typename Function> void for_each_added(Function &&function) const {
        apply(&TimeSeriesDictionaryInputView::for_each_added, function);
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        apply(&TimeSeriesDictionaryInputView::for_each_removed, function);
    }

    template <typename Function> void for_each_modified(Function &&function) const {
        apply(&TimeSeriesDictionaryInputView::for_each_modified, function);
    }

    [[nodiscard]] const TimeSeriesDictionaryInputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSD<K, Item, TimeSeriesProperties...>;
    using key_type = K;
    using item_type = Item;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesDictionaryInputView>;
    using Apply = void (TimeSeriesDictionaryInputView::*)(void *, TimeSeriesDictionaryInputView::KeyFunction) const;

    [[nodiscard]] static auto make_item(TimeSeriesDictionaryInputView::Item item) {
        if constexpr (details::time_series_variable<Item>) {
            return In<Item>{item.last_updated, item.value, item.prototype};
        } else if constexpr (details::is_time_series_dict<Item>) {
            return In<Item>{item.last_updated, item.value,
                            static_cast<const TimeSeriesDictionaryInputView &>(item.prototype)};
        } else if constexpr (details::is_time_series_set<Item>) {
            return In<Item>{item.last_updated, item.value, static_cast<const TimeSeriesSetInputView &>(item.prototype)};
        } else if constexpr (details::is_time_series_list<Item>) {
            return In<Item>{item.last_updated, item.value,
                            static_cast<const TimeSeriesListInputView &>(item.prototype)};
        } else if constexpr (details::scalar_variable<details::time_series_value_t<Item>>) {
            return In<Item>{item.last_updated, item.value,
                            static_cast<const TimeSeriesValueInputView &>(item.prototype)};
        } else {
            using ItemInput = In<Item>;
            return ItemInput{item.last_updated, static_cast<const typename ItemInput::storage_type *>(item.value)};
        }
    }

    template <typename Function> void apply(Apply operation, Function &function) const {
        (view_.get().*operation)(std::addressof(function), [](void *context, TimeSeriesDictionaryInputView::Key key) {
            auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
            std::invoke(callback, Scalar<K>::from(key.prototype, key.data));
        });
    }

    inline static const View prototype_ = View::template make<TypedTimeSeriesDictionaryInputView<K, Item>>();
    View view_;
};

} // namespace bg
