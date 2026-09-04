#pragma once

#include <bg/time_series/time_series_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_value_type.hpp>

namespace bg::details {

template <typename T, auto... Properties> struct time_series_type_traits<TS<T, Properties...>> {
    static constexpr bool generic = generic_scalar<T>;
    static constexpr bool supported =
        generic_scalar<T> || (scalar_type_traits<T>::supports_time_series && has_cpp_scalar_type<T>);

    template <typename Scope, std::size_t Index, bool Input>
    [[nodiscard]] static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &value = scalar_type_for<Scope, T>();

        if constexpr (Input || generic_scalar<T>) {
            static const auto type = GenericTimeSeriesValueType{value, guarantee};
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesValueType{value, guarantee};
            return type;
        }

        return time_series_value_type(value, *guarantee.value());
    }
};

} // namespace bg::details
