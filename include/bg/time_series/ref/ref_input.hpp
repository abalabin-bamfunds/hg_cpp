#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_key_subscription.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <stdexcept>
#include <string_view>

namespace bg {

namespace details {

template <typename T> struct tsd_item_reference_type;

template <typename K, typename Item, auto... Properties>
struct tsd_item_reference_type<TSD<K, Item, Properties...>> {
    using type = REF<Item, Properties...>;
    using key_type = K;
};

template <typename K, typename Item, auto... ItemProperties, auto... Properties>
struct tsd_item_reference_type<TSD<K, REF<Item, ItemProperties...>, Properties...>> {
    using type = REF<Item, ItemProperties...>;
    using key_type = K;
};

} // namespace details

template <typename Target, auto... TimeSeriesProperties, auto... Properties>
class In<REF<Target, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = Reference;

    In() noexcept : data_{cpp_time_series_runtime_type<REF<Target, TimeSeriesProperties...>>(), MIN_DT, nullptr} {}

    In(const EvaluationTime &last_updated, const storage_type *value, bool modified = false) noexcept
        : data_{cpp_time_series_runtime_type<REF<Target, TimeSeriesProperties...>>(), last_updated, value, modified} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false) noexcept
        : data_{runtime_type, last_updated, value, modified} {}

    In(details::DynamicReferenceInputStorage &storage, EvaluationContext &context, std::size_t storage_offset) noexcept
        requires ParsedProperties::dynamic_binding
        : data_{storage.runtime_type(), storage.last_updated(), &storage.value(),
                storage.modified(context.evaluation_time())},
          dynamic_{&storage, &context, storage_offset} {}

    bool valid() const noexcept { return data_.valid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
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
            return In<Result>{MIN_DT, nullptr, details::tsd_item_reference_runtime_type(tsd)};
        }

        const auto input = details::tsd_item_reference_input(value(), Scalar<>::from(key));
        return In<Result>{*input.last_updated, input.value, *input.runtime_type, input.modified};
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

    template <auto... InputTimeSeriesProperties, auto... InputProperties>
    void bind(const In<REF<Target, InputTimeSeriesProperties...>, InputProperties...> &input) const
        requires ParsedProperties::dynamic_binding
    {
        const auto &runtime_type = static_cast<const TimeSeriesReferenceRuntimeType &>(input.runtime_type());
        const auto reference = runtime_type.binding_reference(input.data());
        if (!reference) {
            if (!input.valid()) {
                dynamic_.storage->bind({}, runtime_type, *dynamic_.context, dynamic_.storage_offset);
                data_ = details::TimeSeriesInputData{dynamic_.storage->runtime_type(),
                                                     dynamic_.storage->last_updated(), &dynamic_.storage->value(),
                                                     dynamic_.storage->modified(
                                                         dynamic_.context->evaluation_time())};
                return;
            }
            throw std::invalid_argument{"a dynamic reference input requires an addressable input"};
        }

        dynamic_.storage->bind(reference, runtime_type, *dynamic_.context, dynamic_.storage_offset);
        data_ = details::TimeSeriesInputData{dynamic_.storage->runtime_type(), dynamic_.storage->last_updated(),
                                             &dynamic_.storage->value(),
                                             dynamic_.storage->modified(dynamic_.context->evaluation_time())};
    }

    void unbind() const noexcept
        requires ParsedProperties::dynamic_binding
    {
        dynamic_.storage->unbind(dynamic_.context->runtime_graph(), dynamic_.storage_offset);
        data_ = details::TimeSeriesInputData{dynamic_.storage->runtime_type(), dynamic_.storage->last_updated(),
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

  private:
    struct EmptyDynamicBinding {};

    struct DynamicBindingData {
        details::DynamicReferenceInputStorage *storage{};
        EvaluationContext *context{};
        std::size_t storage_offset{};
    };

    mutable details::TimeSeriesInputData data_;
    [[no_unique_address]] std::conditional_t<ParsedProperties::dynamic_binding, DynamicBindingData,
                                             EmptyDynamicBinding>
        dynamic_;
};

} // namespace bg
