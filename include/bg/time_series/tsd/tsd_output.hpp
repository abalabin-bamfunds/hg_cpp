#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <stdexcept>

namespace bg {

template <typename K, typename Item, auto... Properties> class Out<TSD<K, Item, Properties...>> {
  public:
    using storage_type = details::time_series_dict_storage;

  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        : state_(
              Proxy::copy(details::OutputState{cpp_time_series_runtime_type<TSD<K, Item, Properties...>>()}, context)) {
        state_->bind(last_updated, &value, context);
    }

    Out(TimeSeriesState &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{runtime_type}, context)) {
        state_->bind(last_updated, value, context);
    }

    Out(TimeSeriesState &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }

    Out<Item> operator[](const Scalar<K> &key) const { return item(details::scalar_data(key)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    Out<Item> operator[](Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return item(details::make_scalar_source<K>(value));
    }

    item_id_t item_id(const Scalar<K> &key) const { return item_id(details::scalar_data(key)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    item_id_t item_id(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return item_id(details::make_scalar_source<K>(value));
    }

    Scalar<K> key(item_id_t id) const {
        if (!dictionary_runtime_type().key_live(state_->data(), id)) {
            throw std::out_of_range{"time series dictionary item id was not found"};
        }
        return Scalar<K>::from(key_runtime_type(), dictionary_runtime_type().key(state_->data(), id));
    }

    bool erase(const Scalar<K> &key) const { return erase(details::scalar_data(key)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool erase(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return erase(details::make_scalar_source<K>(value));
    }

    void clear() const {
        auto memory = details::current_scalar_allocator();
        auto &value = storage();
        const auto should_publish = !valid() || value.key_storage().value.size() != 0;
        reset_change_tracking(value, memory);
        value.clear(key_runtime_type(), key_layout(), item_runtime_type(), item_layout(), item_value_offset(), memory);

        if (should_publish) {
            value.key_storage().last_updated.publish(state_->context().evaluation_time());
            state_->publish_item(state_->runtime_type(), key_set_item_id);
        }
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSD<K, Item, Properties...>;
    using key_type = K;
    using item_type = Item;

  private:
    details::time_series_dict_storage &storage() const noexcept {
        return *static_cast<details::time_series_dict_storage *>(state_->mutable_data());
    }

    const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(state_->runtime_type()).key_runtime_type();
    }

    const TimeSeriesRuntimeType &item_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(state_->runtime_type())
            .item_runtime_type();
    }

    const details::TimeSeriesDictionaryRuntimeType &dictionary_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(state_->runtime_type());
    }

    item_id_t item_id(const details::ScalarData &key) const {
        return dictionary_runtime_type().find_key(state_->data(), key);
    }

    util::runtime_item_layout key_layout() const noexcept {
        return {key_runtime_type().size(), key_runtime_type().alignment()};
    }

    util::runtime_item_layout item_layout() const noexcept {
        return details::contained_time_series_layout(item_runtime_type());
    }

    std::size_t item_value_offset() const noexcept {
        return details::contained_time_series_value_offset(item_runtime_type());
    }

    void reset_change_tracking(details::time_series_dict_storage &value, allocator &memory) const {
        value.reset_change_tracking(state_->context().evaluation_time(), key_runtime_type(), key_layout(),
                                    item_runtime_type(), item_layout(), item_value_offset(), memory);
    }

    Out<Item> item(const details::ScalarData &key) const {
        auto memory = details::current_scalar_allocator();
        auto &value = storage();
        reset_change_tracking(value, memory);
        const auto result = value.item(key, key_runtime_type(), key_layout(), item_runtime_type(), item_layout(),
                                       item_value_offset(), memory);

        if (result.change == details::time_series_set_change::added) {
            value.key_storage().last_updated.publish(state_->context().evaluation_time());
            state_->publish_item(state_->runtime_type(), key_set_item_id);
        } else if (result.change == details::time_series_set_change::restored) {
            value.mark_item_resurrected(result.position, state_->context().evaluation_time(), memory);
            state_->publish();
        }

        auto *path = state_->add_path(state_->runtime_type(), result.position);
        const auto item = state_->runtime_type().item(state_->mutable_data(), result.position);
        return details::output_factory::make<Item>(item.last_updated, item.value, item.runtime_type, path);
    }

    bool erase(const details::ScalarData &key) const {
        auto memory = details::current_scalar_allocator();
        auto &value = storage();
        reset_change_tracking(value, memory);
        const auto result = value.erase(key, key_runtime_type(), key_layout(), item_runtime_type(), item_layout(),
                                        item_value_offset(), memory);

        if (result.change == details::time_series_set_change::removed) {
            value.key_storage().last_updated.publish(state_->context().evaluation_time());
            state_->publish_item(state_->runtime_type(), key_set_item_id);
        }
        return result.change != details::time_series_set_change::none;
    }

    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

} // namespace bg
