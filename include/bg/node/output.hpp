#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/dict/output_view.hpp>
#include <bg/time_series/dict/storage.hpp>
#include <bg/time_series/dict/type.hpp>
#include <bg/time_series/list/output_view.hpp>
#include <bg/time_series/list/storage.hpp>
#include <bg/time_series/list/type.hpp>
#include <bg/time_series/output_view.hpp>
#include <bg/time_series/set/output_view.hpp>
#include <bg/time_series/set/storage.hpp>
#include <bg/time_series/set/type.hpp>
#include <bg/time_series/type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <utility>

namespace bg {

template <typename TimeSeries> class Out;

template <> class Out<TS_T> {
  public:
    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        const EvaluationTime &evaluation_time) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, evaluation_time);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        void *output_mutation) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, output_mutation);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const {
        static_cast<const TimeSeriesValueOutputView &>(view_.get()).set_value(value.input_view());
    }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS_T;

  private:
    using view_type = util::polymorphic_value<TimeSeriesOutputView>;

    view_type view_;
};

template <details::scalar_variable Variable, auto... Properties> class Out<TS<Variable, Properties...>> {
  public:
    Out(EvaluationTime &last_updated, void *value, const TimeSeriesValueOutputView &prototype,
        const EvaluationTime &evaluation_time) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, evaluation_time);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesValueOutputView &prototype,
        EvaluationContext &context) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesValueOutputView &prototype,
        void *output_mutation) noexcept
        : view_(view_type::copy(prototype)) {
        view_->bind(last_updated, value, output_mutation);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesValueOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS<Variable, Properties...>;

  private:
    using view_type = util::polymorphic_value<TimeSeriesValueOutputView>;

    view_type view_;
};

template <typename T, auto... Properties> class Out<TS<T, Properties...>> {
    using Traits = details::scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

    Out(EvaluationTime &last_updated, storage_type &value, const EvaluationTime &evaluation_time) noexcept
        : view_(prototype_) {
        view_->bind(last_updated, &value, evaluation_time);
    }

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context) noexcept : view_(prototype_) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_mutation) noexcept : view_(prototype_) {
        view_->bind(last_updated, &value, output_mutation);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] value_type value() const noexcept { return view_->value(); }

    template <typename Value>
        requires requires(storage_type &target, Value &&source) { target = std::forward<Value>(source); }
    void set(Value &&value) const noexcept(noexcept(view_->set(std::forward<Value>(value)))) {
        view_->set(std::forward<Value>(value));
    }

    template <typename Value> void set_value(const Scalar<Value> &value) const { view_->set_value(value.input_view()); }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TypedTimeSeriesValueOutputView<T> *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TS<T, Properties...>;

  private:
    using View = util::polymorphic_value<TypedTimeSeriesValueOutputView<T>>;

    inline static const View prototype_ = View::template make<TypedTimeSeriesValueOutputView<T>>();

    View view_;
};

template <typename Item, std::size_t Size, auto... Properties> class Out<TSL<Item, Size, Properties...>> {
    static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");

  public:
    using storage_type = details::fixed_time_series_list_storage<Item, Size>;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context) noexcept : view_(prototype_) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesListOutputView &prototype,
        EvaluationContext &context) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesListOutputView &prototype,
        void *output_mutation) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value, output_mutation);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_mutation) noexcept : view_(prototype_) {
        view_->bind(last_updated, &value, output_mutation);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] auto operator[](std::size_t index) const {
        const auto item = view_->item(index);

        if constexpr (std::same_as<Item, TS_T>) {
            return Out<Item>{item.last_updated, item.value, item.prototype, item.output_mutation};
        } else if constexpr (details::is_time_series_list<Item>) {
            return Out<Item>{item.last_updated, item.value,
                             static_cast<const TimeSeriesListOutputView &>(item.prototype), item.output_mutation};
        } else if constexpr (details::scalar_variable<details::time_series_value_t<Item>>) {
            return Out<Item>{item.last_updated, item.value,
                             static_cast<const TimeSeriesValueOutputView &>(item.prototype), item.output_mutation};
        } else {
            using ItemOutput = Out<Item>;
            return ItemOutput{item.last_updated, *static_cast<typename ItemOutput::storage_type *>(item.value),
                              item.output_mutation};
        }
    }

    void invalidate() const { view_->invalidate(); }

    [[nodiscard]] const TimeSeriesListOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSL<Item, Size, Properties...>;
    using item_type = Item;

    static constexpr std::size_t size = Size;

  private:
    using View = util::polymorphic_value<TimeSeriesListOutputView>;

    inline static const View prototype_ = View::template make<TypedTimeSeriesListOutputView<Item, Size>>();

    View view_;
};

template <typename K, auto... Properties> class Out<TSS<K, Properties...>> {
  public:
    using storage_type = details::time_series_set_storage<K>;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context) noexcept : view_(prototype_) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesSetOutputView &prototype,
        EvaluationContext &context) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesSetOutputView &prototype,
        void *output_mutation) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value, output_mutation);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    [[nodiscard]] bool insert(const Scalar<K> &item) const { return view_->insert(item.input_view()); }
    [[nodiscard]] bool erase(const Scalar<K> &item) const { return view_->erase(item.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool insert(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return view_->insert(view.get());
    }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool erase(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return view_->erase(view.get());
    }

    void invalidate() const { view_->invalidate(); }
    [[nodiscard]] const TimeSeriesSetOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSS<K, Properties...>;
    using item_type = K;

  private:
    using View = util::polymorphic_value<TimeSeriesSetOutputView>;
    inline static const View prototype_ = View::template make<TypedTimeSeriesSetOutputView<K>>();
    View view_;
};

template <typename K, typename Item, auto... Properties> class Out<TSD<K, Item, Properties...>> {
  public:
    using storage_type = details::time_series_dict_storage<K, Item>;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context) noexcept : view_(prototype_) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesDictionaryOutputView &prototype,
        EvaluationContext &context) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesDictionaryOutputView &prototype,
        void *output_mutation) noexcept
        : view_(View::copy(prototype)) {
        view_->bind(last_updated, value, output_mutation);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] auto operator[](const Scalar<K> &key) const { return make_item(view_->item(key.input_view())); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] auto operator[](Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto key_view = details::make_borrowed_scalar_source<K>(value);
        return make_item(view_->item(key_view.get()));
    }

    [[nodiscard]] bool erase(const Scalar<K> &key) const { return view_->erase(key.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool erase(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto key_view = details::make_borrowed_scalar_source<K>(value);
        return view_->erase(key_view.get());
    }

    void invalidate() const { view_->invalidate(); }
    [[nodiscard]] const TimeSeriesDictionaryOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSD<K, Item, Properties...>;
    using key_type = K;
    using item_type = Item;

  private:
    using View = util::polymorphic_value<TimeSeriesDictionaryOutputView>;

    [[nodiscard]] static auto make_item(TimeSeriesDictionaryOutputView::Item item) {
        if constexpr (std::same_as<Item, TS_T>) {
            return Out<Item>{item.last_updated, item.value, item.prototype, item.output_mutation};
        } else if constexpr (details::is_time_series_dict<Item>) {
            return Out<Item>{item.last_updated, item.value,
                             static_cast<const TimeSeriesDictionaryOutputView &>(item.prototype), item.output_mutation};
        } else if constexpr (details::is_time_series_set<Item>) {
            return Out<Item>{item.last_updated, item.value,
                             static_cast<const TimeSeriesSetOutputView &>(item.prototype), item.output_mutation};
        } else if constexpr (details::is_time_series_list<Item>) {
            return Out<Item>{item.last_updated, item.value,
                             static_cast<const TimeSeriesListOutputView &>(item.prototype), item.output_mutation};
        } else if constexpr (details::scalar_variable<details::time_series_value_t<Item>>) {
            return Out<Item>{item.last_updated, item.value,
                             static_cast<const TimeSeriesValueOutputView &>(item.prototype), item.output_mutation};
        } else {
            using ItemOutput = Out<Item>;
            return ItemOutput{item.last_updated, *static_cast<typename ItemOutput::storage_type *>(item.value),
                              item.output_mutation};
        }
    }

    inline static const View prototype_ = View::template make<TypedTimeSeriesDictionaryOutputView<K, Item>>();
    View view_;
};

static_assert(sizeof(Out<TS<int>>) == 4 * sizeof(void *));

} // namespace bg
