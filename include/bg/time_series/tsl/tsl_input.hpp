#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/tsl/details/wiring.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <string_view>

namespace bg {

template <typename Item, auto Size, auto... TimeSeriesProperties, auto... Properties>
class In<TSL<Item, Size, TimeSeriesProperties...>, Properties...>
    : public details::input_activity_control<details::basic_input_properties<true, Properties...>::dynamic_activity> {
    using ParsedProperties = details::basic_input_properties<true, Properties...>;
    using activity_control = details::input_activity_control<ParsedProperties::dynamic_activity>;
    static_assert(ParsedProperties::valid);

  public:
    using storage_type = details::time_series_list_storage;

    In() noexcept
        requires(!details::size_variable_declaration<decltype(Size)>)
        : activity_control{},
          data_{cpp_time_series_runtime_type<TSL<Item, Size, TimeSeriesProperties...>>(), MIN_DT, nullptr} {}

    In(const EvaluationTime &last_updated, const storage_type *value, bool modified = false,
       bool *activity = nullptr) noexcept
        requires(!details::size_variable_declaration<decltype(Size)>)
        : activity_control{activity}, data_{cpp_time_series_runtime_type<TSL<Item, Size, TimeSeriesProperties...>>(),
                                            last_updated, value, modified} {}

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesRuntimeType &runtime_type,
       bool modified = false, bool *activity = nullptr) noexcept
        : activity_control{activity}, data_{runtime_type, last_updated, value, modified} {}

    bool valid() const noexcept { return data_.valid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    std::size_t size() const noexcept { return data_.runtime_type().item_count(data_.data()); }

    In<Item> operator[](std::size_t index) const {
        const auto item = data_.runtime_type().item(data_.data(), static_cast<item_id_t>(index));
        return {item.last_updated, item.value, item.runtime_type,
                data_.modified() && item.last_updated == data_.last_updated()};
    }

    bool item_updated(std::size_t index) const noexcept {
        return data_.runtime_type().item_updated(data_.last_updated_ref(), data_.data(), static_cast<item_id_t>(index));
    }

    template <typename Function> void for_each_updated(Function &&function) const {
        data_.runtime_type().for_each_updated_item(
            data_.last_updated_ref(), data_.data(), std::addressof(function),
            [](void *context, item_id_t id) { (*static_cast<std::remove_reference_t<Function> *>(context))(id); });
    }

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }
    const void *data() const noexcept { return data_.data(); }
    const In *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_.last_updated_ref(), data_.data(), data_.runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
    }

    using time_series_type = TSL<Item, Size, TimeSeriesProperties...>;
    using item_type = Item;

    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;
    static constexpr bool dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr bool dynamic_activity = ParsedProperties::dynamic_activity;
    static constexpr auto variadic_mode = details::variadic_mode<Properties...>();

  private:
    details::TimeSeriesInputData data_;
};

} // namespace bg
