#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/fixed_item_selection.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_key_subscription.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>

#include <stdexcept>
#include <string_view>

namespace bg {

namespace details {

template <typename T> struct tsd_item_reference_type;

template <typename K, typename Item, auto... Properties> struct tsd_item_reference_type<TSD<K, Item, Properties...>> {
    using type = REF<Item, Properties...>;
    using key_type = K;
};

template <typename K, typename Item, auto... ItemProperties, auto... Properties>
struct tsd_item_reference_type<TSD<K, REF<Item, ItemProperties...>, Properties...>> {
    using type = REF<Item, ItemProperties...>;
    using key_type = K;
};

template <typename T> struct tsl_item_reference_type;

template <typename Item, auto Size, auto... Properties> struct tsl_item_reference_type<TSL<Item, Size, Properties...>> {
    using type = REF<Item, Properties...>;
};

template <typename Item, auto... ItemProperties, auto Size, auto... Properties>
struct tsl_item_reference_type<TSL<REF<Item, ItemProperties...>, Size, Properties...>> {
    using type = REF<Item, ItemProperties...>;
};

} // namespace details

template <typename Target, auto... TimeSeriesProperties, auto... Properties>
class In<REF<Target, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::input_properties<Properties...>::dynamic_activity> {
    using ParsedProperties = details::input_properties<Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);
    static_assert(!ParsedProperties::valid_items, "valid_items applies only to TSD inputs");

  public:
    using storage_type = Reference;

    In() noexcept
        : activity_control{},
          data_{cpp_time_series_runtime_type<REF<Target, TimeSeriesProperties...>>(), initial_time_series_state, nullptr} {}

    In(const TimeSeriesState &last_updated, const storage_type *value, bool modified = false,
       bool *activity = nullptr) noexcept
        : activity_control{activity},
          data_{cpp_time_series_runtime_type<REF<Target, TimeSeriesProperties...>>(), last_updated, value, modified} {}

    In(const TimeSeriesState &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{data} {}

    In(const TimeSeriesState &last_updated, details::ReferenceBinding binding, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, &binding.value(), modified}, binding_{binding} {
    }

    In(details::DynamicReferenceInputStorage &storage, const TimeSeriesRuntimeType &runtime_type,
       EvaluationContext &context, std::size_t storage_offset) noexcept
        requires ParsedProperties::dynamic_binding
        : activity_control{},
          data_{runtime_type, storage.last_updated(), &storage.value(), storage.modified(context.evaluation_time())},
          dynamic_{&storage, &context, storage_offset} {}

    bool valid() const noexcept { return data_.valid(); }
    bool all_valid() const noexcept { return valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    Reference value() const noexcept { return data_.runtime_type().reference(data_.data()); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename T = Target>
        requires details::is_time_series_dict<T>
    auto operator[](const Scalar<typename details::tsd_item_reference_type<T>::key_type> &key) const {
        using Result = typename details::tsd_item_reference_type<T>::type;
        if (!valid()) {
            const auto &reference_type = static_cast<const TimeSeriesReferenceType &>(runtime_type().type());
            const auto &tsd = resolve_time_series_runtime_type(reference_type.target_type());
            return In<Result>{initial_time_series_state, nullptr, details::tsd_item_reference_runtime_type(tsd)};
        }

        const auto input = details::tsd_item_reference_input(value(), Scalar<>::from(key));
        if (!input.binding) {
            return In<Result>{*input.last_updated, nullptr, *input.runtime_type, input.modified};
        }
        return In<Result>{*input.last_updated, input.binding, *input.runtime_type, input.modified};
    }

    template <typename Source, typename T = Target>
        requires(details::is_time_series_dict<T> &&
                 !details::scalar_variable<typename details::tsd_item_reference_type<T>::key_type> &&
                 scalar_source_for<typename details::tsd_item_reference_type<T>::key_type, Source>)
    auto operator[](Source &&key) const {
        using Key = typename details::tsd_item_reference_type<T>::key_type;
        const auto value = scalar_source_value<Key>(std::forward<Source>(key));
        return (*this)[Scalar<Key>::from(details::make_scalar_source<Key>(value))];
    }

    template <typename T = Target>
        requires details::is_time_series_list<T>
    auto operator[](std::size_t index) const {
        using Result = typename details::tsl_item_reference_type<T>::type;
        const auto &reference_type = type_cast<TimeSeriesReferenceType>(runtime_type().type());
        const auto &tsl_type = type_cast<TimeSeriesListType>(reference_type.target_type());
        if (!valid()) {
            const auto &result_runtime_type = details::fixed_item_reference_runtime_type(tsl_type.item_type());
            return In<Result>{initial_time_series_state, nullptr, result_runtime_type};
        }

        const auto input = details::fixed_item_reference_input(value(), tsl_type.item_id(index));
        const auto *result_runtime_type = input.runtime_type;
        if (result_runtime_type == nullptr) {
            result_runtime_type = &details::fixed_item_reference_runtime_type(tsl_type.item_type());
        }
        if (!input.binding) {
            return In<Result>{*input.last_updated, nullptr, *result_runtime_type, input.modified};
        }
        return In<Result>{*input.last_updated, input.binding, *result_runtime_type, input.modified};
    }

    template <typename T = Target>
        requires details::is_time_series_bundle<T>
    In<REF<TS_T>> operator[](std::size_t index) const {
        const auto &reference_type = type_cast<TimeSeriesReferenceType>(runtime_type().type());
        const auto &bundle_type = type_cast<TimeSeriesBundleType>(reference_type.target_type());
        if (index >= bundle_type.item_count()) {
            throw std::out_of_range{"time series bundle field index is out of range"};
        }

        const auto fallback_runtime_type = [&]() -> const TimeSeriesRuntimeType & {
            return details::fixed_item_reference_runtime_type(bundle_type.item_type(index));
        };
        if (!valid()) {
            return {initial_time_series_state, nullptr, fallback_runtime_type()};
        }

        const auto input = details::fixed_item_reference_input(value(), bundle_type.item_id(index));
        const auto &result_runtime_type = input.runtime_type == nullptr ? fallback_runtime_type() : *input.runtime_type;
        if (!input.binding) {
            return {*input.last_updated, nullptr, result_runtime_type, input.modified};
        }
        return {*input.last_updated, input.binding, result_runtime_type, input.modified};
    }

    template <typename T = Target>
        requires details::is_time_series_bundle<T>
    In<REF<TS_T>> operator[](util::symbol name) const {
        const auto &reference_type = type_cast<TimeSeriesReferenceType>(runtime_type().type());
        const auto &bundle_type = type_cast<TimeSeriesBundleType>(reference_type.target_type());
        const auto index = bundle_type.find_item_index(name);
        if (!index) {
            throw std::out_of_range{"time series bundle has no field with the requested name"};
        }
        return (*this)[*index];
    }

    template <typename InputTarget, auto... InputTimeSeriesProperties, auto... InputProperties>
    void bind(const In<REF<InputTarget, InputTimeSeriesProperties...>, InputProperties...> &input) const
        requires(ParsedProperties::dynamic_binding &&
                 (std::same_as<InputTarget, Target> || details::time_series_variable<InputTarget>))
    {
        const auto *declared_runtime_type = &data_.runtime_type();
        if (!input.binding_) {
            if (!input.valid()) {
                dynamic_.storage->unbind(dynamic_.context->runtime_graph(), dynamic_.storage_offset);
                data_ = details::TimeSeriesInputData{*declared_runtime_type, dynamic_.storage->last_updated(),
                                                     &dynamic_.storage->value(),
                                                     dynamic_.storage->modified(dynamic_.context->evaluation_time())};
                return;
            }
            throw std::invalid_argument{"a dynamic reference input requires an addressable input"};
        }

        dynamic_.storage->bind(input.binding_, *dynamic_.context, dynamic_.storage_offset);
        data_ = details::TimeSeriesInputData{*declared_runtime_type, dynamic_.storage->last_updated(),
                                             &dynamic_.storage->value(),
                                             dynamic_.storage->modified(dynamic_.context->evaluation_time())};
    }

    void unbind() const noexcept
        requires ParsedProperties::dynamic_binding
    {
        const auto *declared_runtime_type = &data_.runtime_type();
        dynamic_.storage->unbind(dynamic_.context->runtime_graph(), dynamic_.storage_offset);
        data_ = details::TimeSeriesInputData{*declared_runtime_type, dynamic_.storage->last_updated(),
                                             &dynamic_.storage->value(), false};
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = REF<Target, TimeSeriesProperties...>;
    using target_type = Target;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    template <typename, auto...> friend class In;

    struct EmptyDynamicBinding {};

    struct DynamicBindingData {
        details::DynamicReferenceInputStorage *storage{};
        EvaluationContext *context{};
        std::size_t storage_offset{};
    };

    mutable details::TimeSeriesInputData data_;
    details::ReferenceBinding binding_{};
    [[no_unique_address]] std::conditional_t<ParsedProperties::dynamic_binding, DynamicBindingData, EmptyDynamicBinding>
        dynamic_;
};

} // namespace bg
