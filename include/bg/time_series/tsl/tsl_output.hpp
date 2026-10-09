#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

namespace bg {

template <typename Item, auto Size, auto... Properties> class Out<TSL<Item, Size, Properties...>> {
  public:
    using storage_type = details::time_series_list_storage;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(!details::generic_time_series<TSL<Item, Size, Properties...>> &&
                 !details::size_variable_declaration<decltype(Size)> && Size != std::dynamic_extent)
        : state_{last_updated, &value, cpp_time_series_runtime_type<TSL<Item, Size, Properties...>>(), context} {}

    Out(TimeSeriesState &last_updated, storage_type &value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        requires(!details::generic_time_series<TSL<Item, Size, Properties...>> &&
                 !details::size_variable_declaration<decltype(Size)> && Size != std::dynamic_extent)
        : state_{last_updated, &value, runtime_type, context} {}

    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    std::size_t size() const noexcept { return state_->runtime_type().item_count(state_->data()); }

    Out<Item> operator[](std::size_t index) const {
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return details::output_factory::make<Out<Item>>(item.last_updated, item.value, item.runtime_type, path);
    }

    void invalidate() const { state_->invalidate(); }

    template <typename InputItem, auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy_tick(const In<TSL<InputItem, Size, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() ||
            (!input.modified() && !input.became_invalid())) {
            return;
        }
        if (!input.valid()) {
            invalidate();
            return;
        }

        input.for_each_updated([&](item_id_t id) { (*this)[id].copy_tick(input[id]); });
    }

    template <typename InputItem, auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy(const In<TSL<InputItem, Size, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            invalidate();
            return;
        }

        for (auto index = std::size_t{}; index < size(); ++index) {
            (*this)[index].copy(input[index]);
        }
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSL<Item, Size, Properties...>;
    using item_type = Item;

  private:
    TimeSeriesOutputBinding state_;
};

} // namespace bg
