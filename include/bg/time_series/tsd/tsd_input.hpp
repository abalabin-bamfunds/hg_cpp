#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_input.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg {

template <typename K, typename Item, auto... TimeSeriesProperties, auto... Properties>
class In<TSD<K, Item, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_dict_storage;

    In() noexcept
        : activity_control{},
          data_{cpp_time_series_runtime_type<TSD<K, Item, TimeSeriesProperties...>>(), initial_time_series_state, nullptr} {}

    In(const TimeSeriesState &last_updated, const storage_type *value, bool modified = false,
       bool *activity = nullptr) noexcept
        : activity_control{activity},
          data_{cpp_time_series_runtime_type<TSD<K, Item, TimeSeriesProperties...>>(), last_updated, value, modified} {}

    In(const TimeSeriesState &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesState &last_updated_state() const noexcept { return data_.last_updated_ref(); }
    std::size_t size() const noexcept { return valid() ? data_.runtime_type().item_count(data_.data()) : 0; }
    bool contains(const Scalar<K> &key) const { return contains(details::scalar_data(key)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool contains(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return contains(details::make_scalar_source<K>(value));
    }

    In<Item> operator[](const Scalar<K> &key) const { return item(details::scalar_data(key)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    In<Item> operator[](Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return item(details::make_scalar_source<K>(value));
    }

    bool item_modified(const Scalar<K> &key) const { return item_modified(details::scalar_data(key)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool item_modified(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return item_modified(details::make_scalar_source<K>(value));
    }

    template <typename Function> void for_each_key(Function &&function) const {
        for_each_live_id([&](item_id_t id) { std::invoke(function, make_key(id)); });
    }

    template <typename Function> void for_each_id(Function &&function) const {
        for_each_live_id(std::forward<Function>(function));
    }

    template <typename Function> void for_each_value(Function &&function) const {
        for_each_live_id([&](item_id_t id) { std::invoke(function, make_item(id)); });
    }

    template <typename Function> void for_each_key_value(Function &&function) const {
        for_each_live_id([&](item_id_t id) { std::invoke(function, make_key(id), make_item(id)); });
    }

    template <typename Function> void for_each_id_value(Function &&function) const {
        for_each_live_id([&](item_id_t id) { std::invoke(function, id, make_item(id)); });
    }

    Scalar<K> key_at(item_id_t id) const { return make_key(id); }
    In<Item> item_at(item_id_t id) const { return make_item(id); }

    In<TSS<K>> key_set() const {
        if (!valid()) {
            return In<TSS<K>>{TimeSeriesInputData{dictionary_runtime_type().key_set_runtime_type(),
                                                  initial_time_series_state, nullptr}};
        }
        const auto item = data_.runtime_type().item(data_.data(), key_set_item_id);
        return In<TSS<K>>{TimeSeriesInputData{item.runtime_type, item.last_updated, item.value,
                                              data_.modified() && item.last_updated.valid() &&
                                                  item.last_updated.time() == data_.last_updated()}};
    }

    item_id_t next_item_id(item_id_t start) const noexcept { return next(ItemRange::current, start); }
    item_id_t next_valid_id(item_id_t start) const {
        for (auto id = next_item_id(start); id != util::occupancy_index::npos; id = next_item_id(id + 1)) {
            if (make_item(id).valid()) {
                return id;
            }
        }
        return util::occupancy_index::npos;
    }
    item_id_t next_added_id(item_id_t start) const noexcept { return next(ItemRange::added, start); }
    item_id_t next_removed_id(item_id_t start) const noexcept { return next(ItemRange::removed, start); }
    item_id_t next_modified_id(item_id_t start) const noexcept { return next(ItemRange::modified, start); }

    item_id_t find_item_id(const Scalar<K> &key) const {
        return data_.data() == nullptr ? util::occupancy_index::npos
                                       : dictionary_runtime_type().find_item(data_.data(), details::scalar_data(key));
    }

    item_id_t find_added_id(const Scalar<K> &key) const {
        return dictionary_runtime_type().find_added(last_updated_state(), data_.data(), details::scalar_data(key));
    }

    item_id_t find_removed_id(const Scalar<K> &key) const {
        return dictionary_runtime_type().find_removed(last_updated_state(), data_.data(), details::scalar_data(key));
    }

    item_id_t find_modified_id(const Scalar<K> &key) const {
        return dictionary_runtime_type().find_modified(last_updated_state(), data_.data(), details::scalar_data(key));
    }

    template <typename Function> void for_each(Function &&function) const {
        for_each_key_value(std::forward<Function>(function));
    }

    template <typename Function> void for_each_added(Function &&function) const {
        if (valid()) {
            apply_changes(&details::TimeSeriesDictionaryRuntimeType::for_each_added, function);
        }
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        apply_changes(&details::TimeSeriesDictionaryRuntimeType::for_each_removed, function);
    }

    template <typename Function> void for_each_modified(Function &&function) const {
        apply_changes(&details::TimeSeriesDictionaryRuntimeType::for_each_modified, function);
    }

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TSD<K, Item, TimeSeriesProperties...>;
    using key_type = K;
    using item_type = Item;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    using ItemRange = details::TimeSeriesDictionaryRuntimeType::ItemRange;

    const details::TimeSeriesDictionaryRuntimeType &dictionary_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(data_.runtime_type());
    }

    const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(data_.runtime_type()).key_runtime_type();
    }

    bool contains(const details::ScalarData &key) const {
        return valid() && dictionary_runtime_type().find_item(data_.data(), key) != util::occupancy_index::npos;
    }

    std::uint32_t require_position(const details::ScalarData &key) const {
        if (!valid()) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        const auto id = dictionary_runtime_type().find_item(data_.data(), key);
        if (id == util::occupancy_index::npos) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        return id;
    }

    In<Item> item(const details::ScalarData &key) const { return make_item(require_position(key)); }

    [[nodiscard]] In<Item> make_item(std::uint32_t position) const {
        const auto item = data_.runtime_type().item(data_.data(), position);
        return In<Item>{TimeSeriesInputData{item.runtime_type, item.last_updated, item.value,
                                            data_.modified() && item.last_updated.valid() &&
                                                item.last_updated.time() == data_.last_updated()}};
    }

    Scalar<K> make_key(item_id_t id) const {
        return Scalar<K>::from(key_runtime_type(), dictionary_runtime_type().key(data_.data(), id));
    }

    bool item_modified(const details::ScalarData &key) const {
        if (!valid()) {
            return false;
        }
        const auto id = dictionary_runtime_type().find_item(data_.data(), key);
        return id != util::occupancy_index::npos &&
               dictionary_runtime_type().item_modified(last_updated_state(), data_.data(), id);
    }

    template <typename Function> static void invoke_with_id(void *context, item_id_t id) {
        std::invoke(*static_cast<Function *>(context), id);
    }

    template <typename Function> void for_each_live_id(Function &&function) const {
        if (!valid()) {
            return;
        }

        data_.runtime_type().for_each_item_id(data_.data(), std::addressof(function),
                                              invoke_with_id<std::remove_reference_t<Function>>);
    }

    using ChangeFunction = void (details::TimeSeriesDictionaryRuntimeType::*)(
        const TimeSeriesState &, const void *, void *, TimeSeriesRuntimeType::ItemIdFunction) const;

    item_id_t next(ItemRange range, item_id_t start) const noexcept {
        return dictionary_runtime_type().next_item_id(last_updated_state(), data_.data(), range, start);
    }

    template <typename Function> void apply_changes(ChangeFunction changes, Function &function) const {
        auto callback = [&](item_id_t id) { std::invoke(function, make_key(id)); };
        (dictionary_runtime_type().*changes)(last_updated_state(), data_.data(), &callback,
                                             invoke_with_id<decltype(callback)>);
    }

    details::TimeSeriesInputData data_;
};

} // namespace bg
