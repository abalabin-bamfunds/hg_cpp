#pragma once

#include <bg/time_series/time_series_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

template <typename Target, auto... Properties> struct time_series_type_traits<REF<Target, Properties...>> {
    static constexpr bool generic = generic_time_series<Target>::value;
    static constexpr bool supported = supported_time_series<Target>::value;

    template <typename Scope, std::size_t Index, bool Input>
    [[nodiscard]] static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &target = time_series_type_for<Scope, Index, Input, Target>(TimeSeriesGuarantee::ordinary);

        if constexpr (Input || generic_time_series<Target>::value) {
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

} // namespace bg::details
