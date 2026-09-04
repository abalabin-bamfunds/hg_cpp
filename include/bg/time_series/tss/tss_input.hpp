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

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept
        : data_{cpp_time_series_runtime_type<TSS<K, TimeSeriesProperties...>>(), last_updated, value} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type) noexcept
        : data_{runtime_type, last_updated, value} {}

    [[nodiscard]] bool valid() const noexcept { return data_.valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    [[nodiscard]] std::size_t size() const noexcept { return valid() ? storage().size() : 0; }
    [[nodiscard]] bool contains(const Scalar<K> &item) const { return contains(item.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool contains(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
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

    [[nodiscard]] const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    [[nodiscard]] const void *data() const noexcept { return data_.data(); }
    [[nodiscard]] const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TSS<K, TimeSeriesProperties...>;
    using item_type = K;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    [[nodiscard]] const details::time_series_set_storage &storage() const noexcept {
        return *static_cast<const details::time_series_set_storage *>(data_.data());
    }

    [[nodiscard]] const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesSetRuntimeType &>(data_.runtime_type()).key_runtime_type();
    }

    [[nodiscard]] util::runtime_item_layout key_layout() const noexcept {
        return {key_runtime_type().size(), key_runtime_type().alignment()};
    }

    [[nodiscard]] bool contains(const details::ScalarData &item) const {
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
