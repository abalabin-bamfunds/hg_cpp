#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

namespace bg {

template <typename Item, auto Size, auto... Properties> class Out<TSL<Item, Size, Properties...>> {
  public:
    using storage_type = details::time_series_list_storage;

  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        requires(!details::size_variable_declaration<decltype(Size)>)
        : state_(Proxy::copy(details::OutputState{cpp_time_series_runtime_type<TSL<Item, Size, Properties...>>()},
                             context)) {
        state_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_path)
        requires(!details::size_variable_declaration<decltype(Size)>)
        : state_(Proxy::copy(details::OutputState{cpp_time_series_runtime_type<TSL<Item, Size, Properties...>>()},
                             output_path)) {
        state_->bind(last_updated, &value, output_path);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{runtime_type}, context)) {
        state_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    std::size_t size() const noexcept { return state_->runtime_type().item_count(state_->data()); }

    Out<Item> operator[](std::size_t index) const {
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return details::output_factory::make<Item>(item.last_updated, item.value, item.runtime_type, path);
    }

    void invalidate() const { state_->invalidate(); }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSL<Item, Size, Properties...>;
    using item_type = Item;

  private:
    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

} // namespace bg
