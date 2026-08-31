#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_view_proxy.hpp>
#include <bg/time_series/tss/tss_output_view.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/util/polymorphic_value.hpp>

namespace bg {

template <typename K, auto... Properties> class Out<TSS<K, Properties...>> {
  public:
    using storage_type = details::time_series_set_storage<K>;

  private:
    friend details::output_factory;

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : view_(View::copy(prototype().get(), context)) {
        view_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype,
        EvaluationContext &context)
        : view_(View::copy(static_cast<const TimeSeriesSetOutputView &>(prototype), context)) {
        view_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesOutputView &prototype, void *output_path)
        : view_(View::copy(static_cast<const TimeSeriesSetOutputView &>(prototype), output_path)) {
        view_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }
    bool insert(const Scalar<K> &item) const { return view_->insert(item.input_view()); }
    bool erase(const Scalar<K> &item) const { return view_->erase(item.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    bool insert(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return view_->insert(view.get());
    }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    bool erase(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return view_->erase(view.get());
    }

    void clear() const { view_->clear(); }

    [[nodiscard]] const TimeSeriesSetOutputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSS<K, Properties...>;
    using item_type = K;

  private:
    using View = details::output_view_proxy<TimeSeriesSetOutputView>;

    [[nodiscard]] static const auto &prototype() noexcept {
        static const auto value =
            util::polymorphic_value<TimeSeriesSetOutputView>::template make<TypedTimeSeriesSetOutputView<K>>();
        return value;
    }

    View view_;
};

} // namespace bg
