#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/time_series/tsd/tsd_output_view.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/util/polymorphic_value.hpp>

namespace bg {

template <typename K, typename Item, auto... Properties> class Out<TSD<K, Item, Properties...>> {
  public:
    using storage_type = details::time_series_dict_storage<K, Item>;

  private:
    friend details::output_factory;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : view_(View::copy(prototype().get(), context)) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context)
        : view_(View::copy(static_cast<const TimeSeriesDictionaryOutputView &>(prototype), context)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(View::copy(static_cast<const TimeSeriesDictionaryOutputView &>(prototype), output_path)) {
        view_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

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
    using View = details::output_view_proxy<TimeSeriesDictionaryOutputView>;

    [[nodiscard]] static auto make_item(TimeSeriesDictionaryOutputView::Item item) {
        return details::output_factory::make<Item>(item.last_updated, item.value, item.prototype, item.output_path);
    }

    [[nodiscard]] static const auto &prototype() noexcept {
        static const auto value = util::polymorphic_value<TimeSeriesDictionaryOutputView>::template make<
            TypedTimeSeriesDictionaryOutputView<K, Item>>();
        return value;
    }

    View view_;
};

} // namespace bg
