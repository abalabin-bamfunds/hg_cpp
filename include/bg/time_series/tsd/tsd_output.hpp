#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <functional>
#include <stdexcept>

namespace bg {

template <typename K, typename Item, auto... Properties> class Out<TSD<K, Item, Properties...>> {
  public:
    using storage_type = details::time_series_dict_storage;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(!details::generic_time_series<TSD<K, Item, Properties...>>)
        : state_{last_updated,
                 &value,
                 cpp_time_series_runtime_type<TSD<K, Item, Properties...>, TimeSeriesTypeRole::output>(),
                 context} {}

    Out(TimeSeriesState &last_updated, storage_type &value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        requires(!details::generic_time_series<TSD<K, Item, Properties...>>)
        : state_{last_updated, &value, runtime_type, context} {}

    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    std::size_t size() const noexcept {
        return valid() ? state_->runtime_type().item_count(state_->data()) : 0;
    }
    bool empty() const noexcept { return size() == 0; }
    bool contains(const Scalar<K> &key) const { return valid() && item_id(key) != util::occupancy_index::npos; }

    bool contains(const Scalar<> &key) const {
        return valid() && item_id(details::scalar_data(key)) != util::occupancy_index::npos;
    }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool contains(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return valid() && item_id(details::make_scalar_source<K>(value)) != util::occupancy_index::npos;
    }

    bool contains(const literal::ScalarLiteral &key) const {
        const auto value = ScalarValue<>::from_literal(key_runtime_type(), key);
        return valid() && item_id(details::scalar_data(value)) != util::occupancy_index::npos;
    }
    void invalidate() const { state_->invalidate(); }

    // Visits borrowed keys of current entries. The callback may erase the key it receives, but must not insert.
    template <typename Function> void for_each_key(Function &&function) const {
        const In<TSD<K, Item, Properties...>> input{state_->mutable_last_updated(), state_->data(),
                                                    state_->runtime_type()};
        for (auto id = input.next_item_id(0); id != util::occupancy_index::npos;) {
            const auto next_id = input.next_item_id(id + 1);
            const auto key = input.key_at(id);
            std::invoke(function, key);
            id = next_id;
        }
    }

    void apply(const literal::TimeSeriesLiteral &literal) const {
        if (const auto sentinel = literal::Sentinel::from(literal); sentinel && *sentinel == literal::SKIP) {
            return;
        }
        apply_values(details::scalar_literal::sequence(literal));
    }

    Out<Item> operator[](const Scalar<K> &key) const { return item(details::scalar_data(key)); }

    Out<Item> operator[](const Scalar<> &key) const
        requires details::scalar_variable<K>
    {
        return item(details::scalar_data(key));
    }

    Out<Item> operator[](const literal::ScalarLiteral &key) const {
        const auto value = ScalarValue<>::from_literal(key_runtime_type(), key);
        return item(details::scalar_data(value));
    }

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

    bool erase(const Scalar<> &key) const
        requires details::scalar_variable<K>
    {
        return erase(details::scalar_data(key));
    }

    bool erase(const literal::ScalarLiteral &key) const {
        const auto value = ScalarValue<>::from_literal(key_runtime_type(), key);
        return erase(details::scalar_data(value));
    }

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
            state_->publish_item(key_set_item_id, value.key_storage().last_updated);
        }
    }

    template <typename InputItem, auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy_tick(const In<TSD<K, InputItem, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() || !input.modified()) {
            return;
        }
        if (!input.valid()) {
            if (valid()) {
                input.for_each_removed([&](const auto &key) { erase(key); });
            }
            return;
        }
        if (!valid()) {
            clear();
        }

        input.for_each_added([&](const auto &key) { (*this)[key].copy(input[key]); });
        input.for_each_removed([&](const auto &key) { erase(key); });
        input.for_each_updated([&](const auto &key) { (*this)[key].copy_tick(input[key]); });
    }

    template <typename InputItem, auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy(const In<TSD<K, InputItem, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            if (valid()) {
                clear();
            }
            return;
        }

        if (!valid()) {
            clear();
        }

        for_each_key([&](const auto &key) {
            if (!input.contains(key)) {
                erase(key);
            }
        });

        input.for_each_key_value([&](const auto &key, const auto &item) { (*this)[key].copy(item); });
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSD<K, Item, Properties...>;
    using key_type = K;
    using item_type = Item;

  private:
    void apply_values(std::span<const literal::ScalarLiteral> values) const {
        for (const auto &value : values) {
            (void)details::scalar_literal::pair(value, "a TSD update entry requires a key and a value");
        }

        if (values.empty()) {
            if (!valid()) {
                clear();
            }
            return;
        }

        for (const auto &value : values) {
            const auto current = details::scalar_literal::pair(value, "a TSD update entry requires a key and a value");
            auto skip = false;
            auto remove = false;
            if (const auto sentinel = literal::Sentinel::from(*current.second)) {
                if (*sentinel == literal::SKIP) {
                    skip = true;
                } else if (*sentinel == literal::REMOVE) {
                    remove = true;
                }
            }
            details::scalar_literal::with_scalar_data(key_runtime_type(), *current.first,
                                                      [&](const details::ScalarData &key) {
                if (skip) {
                    return;
                }
                if (remove) {
                    (void)erase(key);
                    return;
                }
                details::apply_time_series_literal(item(key), *current.second);
            });
        }
    }

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
            state_->publish_item(key_set_item_id, value.key_storage().last_updated);
        } else if (result.change == details::time_series_set_change::restored) {
            value.mark_item_resurrected(result.position, state_->context().evaluation_time(), memory);
            state_->publish();
        }

        auto *path = state_->add_path(state_->runtime_type(), result.position);
        const auto item = state_->runtime_type().item(state_->mutable_data(), result.position);
        return details::output_factory::make<Out<Item>>(item.last_updated, item.value, item.runtime_type, path);
    }

    bool erase(const details::ScalarData &key) const {
        auto memory = details::current_scalar_allocator();
        auto &value = storage();
        reset_change_tracking(value, memory);
        const auto result = value.erase(key, key_runtime_type(), key_layout(), item_runtime_type(), item_layout(),
                                        item_value_offset(), memory);

        if (result.change == details::time_series_set_change::removed) {
            state_->publish_item(key_set_item_id, value.key_storage().last_updated);
        }
        return result.change != details::time_series_set_change::none;
    }

    TimeSeriesOutputBinding state_;
};

} // namespace bg
