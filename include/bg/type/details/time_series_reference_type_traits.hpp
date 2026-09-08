#pragma once

#include <bg/time_series/time_series_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

template <typename Target, auto... Properties> struct time_series_type_traits<REF<Target, Properties...>> {
    using semantic_type = TimeSeriesReferenceType;

    static constexpr bool generic = generic_time_series<Target>;
    static constexpr bool supported = supported_time_series<Target>;

    template <typename Scope, std::size_t Index, bool Input>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &target = time_series_type_for<Scope, Index, Input, Target>(TimeSeriesGuarantee::ordinary);

        if constexpr (!Input && sizeof...(Properties) == 0) {
            const auto inherited = guarantee.value();
            guarantee = inherited ? strongest(*inherited, TimeSeriesGuarantee::deduplicated)
                                  : TimeSeriesGuarantee::deduplicated;
        }

        if constexpr (Input || generic_time_series<Target>) {
            static const auto type = GenericTimeSeriesReferenceType{target, guarantee};
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesReferenceType{target, guarantee};
            return type;
        }

        return time_series_reference_type(target, *guarantee.value());
    }
};

template <typename Target, auto... Properties> struct declaration_variable_traits<REF<Target, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Target>(visitor);
        (for_each_property_variable<Properties>(visitor), ...);
    }
};

} // namespace bg::details
