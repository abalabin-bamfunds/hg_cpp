#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <functional>
#include <string_view>

namespace bg {

template <typename K, typename Item, auto... TimeSeriesProperties, auto... Properties>
class In<TSD<K, Item, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_dict_storage;

    In() noexcept : data_{cpp_time_series_runtime_type<TSD<K, Item, TimeSeriesProperties...>>(), MIN_DT, nullptr} {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept
        : data_{cpp_time_series_runtime_type<TSD<K, Item, TimeSeriesProperties...>>(), last_updated, value} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type) noexcept
        : data_{runtime_type, last_updated, value} {}

    [[nodiscard]] bool valid() const noexcept { return data_.valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    [[nodiscard]] std::size_t size() const noexcept {
        return valid() ? data_.runtime_type().item_count(data_.data()) : 0;
    }
    [[nodiscard]] bool contains(const Scalar<K> &key) const { return contains(key.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool contains(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        return contains(details::make_scalar_source<K>(value));
    }

    [[nodiscard]] In<Item> operator[](const Scalar<K> &key) const { return item(key.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] In<Item> operator[](Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        return item(details::make_scalar_source<K>(value));
    }

    [[nodiscard]] bool item_modified(const Scalar<K> &key) const { return item_modified(key.input_view()); }

    template <typename Function> void for_each(Function &&function) const {
        if (!valid()) {
            return;
        }

        const auto &keys = storage().key_storage().value;
        for (const auto position : keys.live()) {
            std::invoke(function, Scalar<K>::from(key_runtime_type(), keys.key(position, key_layout())),
                        make_item(position));
        }
    }

    template <typename Function> void for_each_added(Function &&function) const {
        if (valid() && storage().change_time() == last_updated()) {
            apply(storage().key_storage().value.added(), function);
        }
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        const auto &keys = storage().key_storage().value;
        if (!valid() || storage().change_time() == last_updated()) {
            apply(keys.removed(), function);
        }
    }

    template <typename Function> void for_each_modified(Function &&function) const {
        if (!valid() || storage().change_time() != last_updated()) {
            return;
        }

        const auto &keys = storage().key_storage().value;
        for (const auto position : storage().modified()) {
            if (!keys.added().contains(position)) {
                std::invoke(function, Scalar<K>::from(key_runtime_type(), keys.key(position, key_layout())));
            }
        }
    }

    [[nodiscard]] const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    [[nodiscard]] const void *data() const noexcept { return data_.data(); }
    [[nodiscard]] const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TSD<K, Item, TimeSeriesProperties...>;
    using key_type = K;
    using item_type = Item;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    [[nodiscard]] const details::time_series_dict_storage &storage() const noexcept {
        return *static_cast<const details::time_series_dict_storage *>(data_.data());
    }

    [[nodiscard]] const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(data_.runtime_type()).key_runtime_type();
    }

    [[nodiscard]] util::runtime_item_layout key_layout() const noexcept {
        return {key_runtime_type().size(), key_runtime_type().alignment()};
    }

    [[nodiscard]] bool contains(const details::ScalarData &key) const {
        return valid() && storage().key_storage().value.contains(key, key_runtime_type(), key_layout());
    }

    [[nodiscard]] std::uint32_t require_position(const details::ScalarData &key) const {
        const auto position = storage().key_storage().value.find_position(key, key_runtime_type(), key_layout());
        if (!valid() || position == util::occupancy_index::npos ||
            !storage().key_storage().value.live().contains(position)) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        return position;
    }

    [[nodiscard]] In<Item> item(const details::ScalarData &key) const { return make_item(require_position(key)); }

    [[nodiscard]] In<Item> make_item(std::uint32_t position) const {
        const auto item = data_.runtime_type().item(data_.data(), position);
        return {item.last_updated, item.value, item.runtime_type};
    }

    [[nodiscard]] bool item_modified(const details::ScalarData &key) const {
        if (!valid() || storage().change_time() != last_updated()) {
            return false;
        }
        const auto position = storage().key_storage().value.find_position(key, key_runtime_type(), key_layout());
        return position != util::occupancy_index::npos && storage().modified().contains(position);
    }

    template <typename Function> void apply(const util::index<> &positions, Function &function) const {
        const auto &keys = storage().key_storage().value;
        for (const auto position : positions) {
            std::invoke(function, Scalar<K>::from(key_runtime_type(), keys.key(position, key_layout())));
        }
    }

    details::TimeSeriesInputData data_;
};

} // namespace bg
