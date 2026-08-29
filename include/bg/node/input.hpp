#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/dict/input_view.hpp>
#include <bg/time_series/dict/type.hpp>
#include <bg/time_series/input_view.hpp>
#include <bg/time_series/list/input_view.hpp>
#include <bg/time_series/list/storage.hpp>
#include <bg/time_series/list/type.hpp>
#include <bg/time_series/set/input_view.hpp>
#include <bg/time_series/set/type.hpp>
#include <bg/time_series/type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <string_view>

namespace bg {

template <typename TimeSeries, auto... Properties> class In;

template <auto... Properties> class In<TS_T, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using view_type = util::polymorphic_value<TimeSeriesInputView>;

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesInputView &prototype) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] const TimeSeriesInputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS_T;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    view_type view_;
};

template <details::scalar_variable Variable, auto... TimeSeriesProperties, auto... Properties>
class In<TS<Variable, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using view_type = util::polymorphic_value<TimeSeriesValueInputView>;

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesValueInputView &prototype) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] Scalar<Variable> value_view() const { return view_->template value_view<Variable>(); }

    [[nodiscard]] const TimeSeriesValueInputView *operator->() const noexcept { return view_.operator->(); }

    [[nodiscard]] operator const TimeSeriesValueInputView &() const noexcept { return view_.get(); }

    using time_series_type = TS<Variable, TimeSeriesProperties...>;
    using value_type = Variable;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    view_type view_;
};

template <typename T, auto... TimeSeriesProperties, auto... Properties>
class In<TS<T, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using Traits = details::scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const Stored *value) noexcept : In() { view_->bind(last_updated, value); }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] details::scalar_value_t<T> value() const noexcept {
        return static_cast<const TypedTimeSeriesValueInputView<T> &>(view_.get()).value();
    }

    [[nodiscard]] Scalar<T> value_view() const {
        return static_cast<const TypedTimeSeriesValueInputView<T> &>(view_.get()).value_view();
    }

    [[nodiscard]] const TypedTimeSeriesValueInputView<T> *operator->() const noexcept {
        return &static_cast<const TypedTimeSeriesValueInputView<T> &>(view_.get());
    }

    [[nodiscard]] operator const TimeSeriesValueInputView &() const noexcept { return view_.get(); }

    using time_series_type = TS<T, TimeSeriesProperties...>;
    using value_type = T;
    using storage_type = Stored;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesValueInputView>;

    inline static const View prototype_ = View::template make<TypedTimeSeriesValueInputView<T>>();

    View view_;
};

template <typename Item, std::size_t Size, auto... TimeSeriesProperties, auto... Properties>
class In<TSL<Item, Size, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);
    static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");

  public:
    using storage_type = details::fixed_time_series_list_storage<Item, Size>;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept : In() {
        view_->bind(last_updated, value);
    }

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesListInputView &prototype) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] auto operator[](std::size_t index) const {
        const auto item = view_->item(index);

        if constexpr (std::same_as<Item, TS_T>) {
            return In<Item>{item.last_updated, item.value, item.prototype};
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

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept { return view_->item_updated(index); }

    template <typename Function> void for_each_updated(Function &&function) const {
        auto callback = [](void *context, std::size_t index) {
            (*static_cast<std::remove_reference_t<Function> *>(context))(index);
        };
        view_->for_each_updated(&function, callback);
    }

    [[nodiscard]] const TimeSeriesListInputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSL<Item, Size, TimeSeriesProperties...>;
    using item_type = Item;

    static constexpr std::size_t size = Size;
    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesListInputView>;

    inline static const View prototype_ = View::template make<TypedTimeSeriesListInputView<Item, Size>>();

    View view_;
};

template <typename K, auto... TimeSeriesProperties, auto... Properties>
class In<TSS<K, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_set_storage<K>;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept : In() {
        view_->bind(last_updated, value);
    }

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesSetInputView &prototype) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] std::size_t size() const noexcept { return view_->size(); }

    [[nodiscard]] bool contains(const Scalar<K> &item) const { return view_->contains(item.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool contains(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return view_->contains(view.get());
    }

    template <typename Function> void for_each(Function &&function) const {
        apply(&TimeSeriesSetInputView::for_each, function);
    }
    template <typename Function> void for_each_added(Function &&function) const {
        apply(&TimeSeriesSetInputView::for_each_added, function);
    }
    template <typename Function> void for_each_removed(Function &&function) const {
        apply(&TimeSeriesSetInputView::for_each_removed, function);
    }

    [[nodiscard]] const TimeSeriesSetInputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSS<K, TimeSeriesProperties...>;
    using item_type = K;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesSetInputView>;
    using Apply = void (TimeSeriesSetInputView::*)(void *, TimeSeriesSetInputView::ItemFunction) const;

    template <typename Function> void apply(Apply operation, Function &function) const {
        (view_.get().*operation)(std::addressof(function), [](void *context, TimeSeriesSetInputView::Item item) {
            auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
            std::invoke(callback, Scalar<K>::from(item.prototype, item.data));
        });
    }

    inline static const View prototype_ = View::template make<TypedTimeSeriesSetInputView<K>>();
    View view_;
};

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
        if constexpr (std::same_as<Item, TS_T>) {
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
