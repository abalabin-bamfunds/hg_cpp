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
#include <utility>

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

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool item_modified(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        return item_modified(details::make_scalar_source<K>(value));
    }

    template <typename Function> void for_each(Function &&function) const {
        if (!valid()) {
            return;
        }

        auto callback = [&](TimeSeriesItemId id) {
            std::invoke(function, Scalar<K>::from(key_runtime_type(), dictionary_runtime_type().key(data_.data(), id)),
                        make_item(id));
        };
        data_.runtime_type().for_each_item_id(data_.data(), &callback, invoke_with_id<decltype(callback)>);
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
    [[nodiscard]] const details::TimeSeriesDictionaryRuntimeType &dictionary_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(data_.runtime_type());
    }

    [[nodiscard]] const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesDictionaryRuntimeType &>(data_.runtime_type()).key_runtime_type();
    }

    [[nodiscard]] bool contains(const details::ScalarData &key) const {
        return valid() && dictionary_runtime_type().find_item(data_.data(), key) != util::occupancy_index::npos;
    }

    [[nodiscard]] std::uint32_t require_position(const details::ScalarData &key) const {
        if (!valid()) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        const auto id = dictionary_runtime_type().find_item(data_.data(), key);
        if (id == util::occupancy_index::npos) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        return id;
    }

    [[nodiscard]] In<Item> item(const details::ScalarData &key) const { return make_item(require_position(key)); }

    [[nodiscard]] In<Item> make_item(std::uint32_t position) const {
        const auto item = data_.runtime_type().item(data_.data(), position);
        return {item.last_updated, item.value, item.runtime_type};
    }

    [[nodiscard]] bool item_modified(const details::ScalarData &key) const {
        if (!valid()) {
            return false;
        }
        const auto id = dictionary_runtime_type().find_item(data_.data(), key);
        return id != util::occupancy_index::npos &&
               dictionary_runtime_type().item_modified(last_updated(), data_.data(), id);
    }

    template <typename Function> static void invoke_with_id(void *context, TimeSeriesItemId id) {
        std::invoke(*static_cast<Function *>(context), id);
    }

    using ChangeFunction = void (details::TimeSeriesDictionaryRuntimeType::*)(
        const EvaluationTime &, const void *, void *, TimeSeriesRuntimeType::ItemIdFunction) const;

    template <typename Function> void apply_changes(ChangeFunction changes, Function &function) const {
        auto callback = [&](TimeSeriesItemId id) {
            std::invoke(function,
                        Scalar<K>::from(key_runtime_type(), dictionary_runtime_type().key(data_.data(), id)));
        };
        (dictionary_runtime_type().*changes)(last_updated(), data_.data(), &callback,
                                             invoke_with_id<decltype(callback)>);
    }

    details::TimeSeriesInputData data_;
};

} // namespace bg
