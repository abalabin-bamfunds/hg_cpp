#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

namespace bg {

template <typename Item, auto Size, OutputSpecification Specification, auto... Properties>
class Out<TSL<Item, Size, Properties...>, Specification> {
  public:
    using storage_type = details::time_series_list_storage;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(Specification == OutputSpecification::default_ &&
                 !details::generic_time_series<TSL<Item, Size, Properties...>> &&
                 !details::size_variable_declaration<decltype(Size)> && Size != std::dynamic_extent)
        : state_{last_updated, &value, cpp_time_series_runtime_type<TSL<Item, Size, Properties...>>(), context} {}

    Out(TimeSeriesState &last_updated, storage_type &value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        requires(Specification == OutputSpecification::default_ &&
                 !details::generic_time_series<TSL<Item, Size, Properties...>> &&
                 !details::size_variable_declaration<decltype(Size)> && Size != std::dynamic_extent)
        : state_{last_updated, &value, runtime_type, context} {}

    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    std::size_t size() const noexcept { return state_->runtime_type().item_count(state_->data()); }

    Out<Item, Specification> operator[](std::size_t index) const {
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return details::output_factory::make<Item, Specification>(item.last_updated, item.value, item.runtime_type, path);
    }

    void invalidate() const { state_->invalidate(); }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T, Specification>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSL<Item, Size, Properties...>;
    using item_type = Item;

  private:
    TimeSeriesOutputBinding state_;
};

} // namespace bg
