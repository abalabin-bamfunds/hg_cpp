#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/tss/tss_input_view.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <functional>
#include <string_view>

namespace bg {

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

} // namespace bg
