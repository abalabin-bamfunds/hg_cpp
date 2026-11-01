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
#include <memory>
#include <string_view>

namespace bg {

template <typename K, auto... TimeSeriesProperties, auto... Properties>
class In<TSS<K, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);
    static_assert(!ParsedProperties::valid_items, "valid_items applies only to TSD inputs");

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
    bool all_valid() const noexcept { return valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    const TimeSeriesState &last_updated_state() const noexcept { return data_.last_updated_ref(); }
    std::size_t size() const noexcept {
        if (!valid()) {
            return 0;
        }
        return sampled() ? set_runtime_type().set_size(data()) : storage().size();
    }
    bool contains(const Scalar<K> &item) const { return contains(details::scalar_data(item)); }

    bool was_added(const Scalar<K> &item) const {
        if (!modified() || !valid()) {
            return false;
        }
        if (sampled()) {
            return set_runtime_type().set_was_added(last_updated_state(), data(), details::scalar_data(item));
        }
        return changed(item, storage().added());
    }
    bool was_removed(const Scalar<K> &item) const {
        if (!modified() || data() == nullptr) {
            return false;
        }
        if (sampled()) {
            return set_runtime_type().set_was_removed(last_updated_state(), data(), details::scalar_data(item));
        }
        return changed(item, storage().removed());
    }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool contains(Source &&item) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(item));
        return contains(details::make_scalar_source<K>(value));
    }

    template <typename Function> void for_each(Function &&function) const {
        if (valid()) {
            if (sampled()) {
                apply_sampled(details::TimeSeriesSetRuntimeType::Range::current, function);
            } else {
                apply(storage().live(), function);
            }
        }
    }

    template <typename Function> void for_each_added(Function &&function) const {
        if (!modified() || !valid()) {
            return;
        }
        if (sampled()) {
            apply_sampled(details::TimeSeriesSetRuntimeType::Range::added, function);
        } else if (storage().change_time() == last_updated()) {
            apply(storage().added(), function);
        }
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        if (!modified()) {
            return;
        }
        if (sampled()) {
            apply_sampled(details::TimeSeriesSetRuntimeType::Range::removed, function);
        } else if (data() != nullptr && storage().change_time() == last_updated()) {
            apply(storage().removed(), function);
        }
    }

    Scalar<K> item_at(item_id_t position) const {
        if (sampled()) {
            const auto key = set_runtime_type().set_key_data(data(), position);
            return Scalar<K>::from(key.runtime_type(), key.data());
        }
        return Scalar<K>::from(key_runtime_type(), storage().key(position, key_layout()));
    }

    item_id_t next_item_id(item_id_t start) const {
        if (!valid()) {
            return util::occupancy_index::npos;
        }
        return sampled() ? set_runtime_type().set_next_item_id(last_updated_state(), data(),
                                                               details::TimeSeriesSetRuntimeType::Range::current, start)
                         : next(storage().live(), start);
    }

    item_id_t next_added_id(item_id_t start) const {
        if (!modified() || !valid() || (!sampled() && storage().change_time() != last_updated())) {
            return util::occupancy_index::npos;
        }
        return sampled() ? set_runtime_type().set_next_item_id(last_updated_state(), data(),
                                                               details::TimeSeriesSetRuntimeType::Range::added, start)
                         : next(storage().added(), start);
    }

    item_id_t next_removed_id(item_id_t start) const {
        if (!modified()) {
            return util::occupancy_index::npos;
        }
        if (sampled()) {
            return set_runtime_type().set_next_item_id(last_updated_state(), data(),
                                                       details::TimeSeriesSetRuntimeType::Range::removed, start);
        }
        if (data() == nullptr || storage().change_time() != last_updated()) {
            return util::occupancy_index::npos;
        }
        return next(storage().removed(), start);
    }

    // A sampled reference reports whether any common item has a different physical ID.
    bool has_item_id_remapping() const {
        return modified() && data_.data() != nullptr && set_runtime_type().has_item_id_remapping(data_.data());
    }

    template <typename Function> void for_each_item_id_remapping(Function &&function) const {
        if (modified() && data_.data() != nullptr) {
            set_runtime_type().for_each_item_id_remapping(data_.data(), std::addressof(function),
                                                         invoke_with_id_remapping<std::remove_reference_t<Function>>);
        }
    }

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_}.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSS<K, TimeSeriesProperties...>;
    using item_type = K;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    bool sampled() const noexcept { return set_runtime_type().is_reference_sample(); }

    const details::TimeSeriesSetRuntimeType &set_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesSetRuntimeType &>(data_.runtime_type());
    }

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
        return valid() && (sampled() ? set_runtime_type().set_contains(data(), item)
                                     : storage().contains(item, key_runtime_type(), key_layout()));
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

    template <typename Function>
    static void invoke_with_id_remapping(void *context, item_id_t old_id, item_id_t new_id) {
        std::invoke(*static_cast<Function *>(context), old_id, new_id);
    }

    template <typename Function> void apply(const util::index<> &positions, Function &function) const {
        for (const auto position : positions) {
            std::invoke(function, Scalar<K>::from(key_runtime_type(), storage().key(position, key_layout())));
        }
    }

    template <typename Function>
    void apply_sampled(details::TimeSeriesSetRuntimeType::Range range, Function &function) const {
        auto id = set_runtime_type().set_next_item_id(last_updated_state(), data(), range, 0);
        while (id != util::occupancy_index::npos) {
            const auto key = set_runtime_type().set_key_data(data(), id);
            std::invoke(function, Scalar<K>::from(key.runtime_type(), key.data()));
            id = set_runtime_type().set_next_item_id(last_updated_state(), data(), range, id + 1);
        }
    }

    details::TimeSeriesInputData data_;
};

} // namespace bg
