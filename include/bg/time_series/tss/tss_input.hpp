#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <functional>
#include <string_view>

namespace bg {

template <typename K, auto... TimeSeriesProperties, auto... Properties>
class In<TSS<K, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_set_storage;

    In() noexcept : data_{cpp_time_series_runtime_type<TSS<K, TimeSeriesProperties...>>(), MIN_DT, nullptr} {}

    In(const EvaluationTime &last_updated, const storage_type *value, bool modified = false) noexcept
        : data_{cpp_time_series_runtime_type<TSS<K, TimeSeriesProperties...>>(), last_updated, value, modified} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false) noexcept
        : data_{runtime_type, last_updated, value, modified} {}

    bool valid() const noexcept { return data_.valid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    std::size_t size() const noexcept { return valid() ? storage().size() : 0; }
    bool contains(const Scalar<K> &item) const { return contains(details::scalar_data(item)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool contains(Source &&item) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(item));
        return contains(details::make_scalar_source<K>(value));
    }

    template <typename Function> void for_each(Function &&function) const {
        if (valid()) {
            apply(storage().live(), function);
        }
    }

    template <typename Function> void for_each_added(Function &&function) const {
        if (valid() && storage().change_time() == last_updated()) {
            apply(storage().added(), function);
        }
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        if (!valid() || storage().change_time() == last_updated()) {
            apply(storage().removed(), function);
        }
    }

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TSS<K, TimeSeriesProperties...>;
    using item_type = K;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    const details::time_series_set_storage &storage() const noexcept {
        return *static_cast<const details::time_series_set_storage *>(data_.data());
    }

    const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesSetRuntimeType &>(data_.runtime_type()).key_runtime_type();
    }

    util::runtime_item_layout key_layout() const noexcept {
        return {key_runtime_type().size(), key_runtime_type().alignment()};
    }

    bool contains(const details::ScalarData &item) const {
        return valid() && storage().contains(item, key_runtime_type(), key_layout());
    }

    template <typename Function> void apply(const util::index<> &positions, Function &function) const {
        for (const auto position : positions) {
            std::invoke(function, Scalar<K>::from(key_runtime_type(), storage().key(position, key_layout())));
        }
    }

    details::TimeSeriesInputData data_;
};

} // namespace bg
