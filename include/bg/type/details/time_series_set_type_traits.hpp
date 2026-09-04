#pragma once

#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_set_type.hpp>

namespace bg::details {

template <typename Item, auto... Properties> struct time_series_type_traits<TSS<Item, Properties...>> {
    static constexpr bool generic = generic_scalar<Item>;
    static constexpr bool supported = generic_scalar<Item> || has_cpp_scalar_type<Item>;

    template <typename Scope, std::size_t Index, bool Input>
    [[nodiscard]] static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &item = scalar_type_for<Scope, Item>();

        if constexpr (Input || generic_scalar<Item>) {
            static const auto type = GenericTimeSeriesSetType{item, guarantee};
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesSetType{item, guarantee};
            return type;
        }

        return time_series_set_type(item, *guarantee.value());
    }
};

} // namespace bg::details
