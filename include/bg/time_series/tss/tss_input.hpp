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
class In<TSS<K, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_set_storage;

    In() noexcept
        : activity_control{},
          data_{cpp_time_series_runtime_type<TSS<K, TimeSeriesProperties...>>(), initial_time_series_state, nullptr} {}

    In(const TimeSeriesState &last_updated, const storage_type *value, bool modified = false,
       bool *activity = nullptr) noexcept
        : activity_control{activity},
          data_{cpp_time_series_runtime_type<TSS<K, TimeSeriesProperties...>>(), last_updated, value, modified} {}

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
    std::size_t size() const noexcept { return valid() ? storage().size() : 0; }
    bool contains(const Scalar<K> &item) const { return contains(details::scalar_data(item)); }

    bool was_added(const Scalar<K> &item) const { return valid() && changed(item, storage().added()); }
    bool was_removed(const Scalar<K> &item) const { return data() != nullptr && changed(item, storage().removed()); }

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

    Scalar<K> item_at(item_id_t position) const {
        return Scalar<K>::from(key_runtime_type(), storage().key(position, key_layout()));
    }

    item_id_t next_item_id(item_id_t start) const noexcept {
        return valid() ? next(storage().live(), start) : util::occupancy_index::npos;
    }

    item_id_t next_added_id(item_id_t start) const noexcept {
        return valid() && storage().change_time() == last_updated() ? next(storage().added(), start)
                                                                    : util::occupancy_index::npos;
    }

    item_id_t next_removed_id(item_id_t start) const noexcept {
        return data() != nullptr && (!valid() || storage().change_time() == last_updated())
                   ? next(storage().removed(), start)
                   : util::occupancy_index::npos;
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
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

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

    bool changed(const Scalar<K> &item, const util::index<> &positions) const {
        return changed(details::scalar_data(item), positions);
    }

    bool changed(const details::ScalarData &item, const util::index<> &positions) const {
        if (data() == nullptr || storage().change_time() != last_updated()) {
            return false;
        }
        const auto position = storage().find_position(item, key_runtime_type(), key_layout());
        return position != util::occupancy_index::npos && positions.contains(position);
    }

    static item_id_t next(const util::index<> &positions, item_id_t start) noexcept {
        const auto position = positions.first_set(start);
        return position == positions.size() ? util::occupancy_index::npos : position;
    }

    template <typename Function> void apply(const util::index<> &positions, Function &function) const {
        for (const auto position : positions) {
            std::invoke(function, Scalar<K>::from(key_runtime_type(), storage().key(position, key_layout())));
        }
    }

    details::TimeSeriesInputData data_;
};

} // namespace bg
