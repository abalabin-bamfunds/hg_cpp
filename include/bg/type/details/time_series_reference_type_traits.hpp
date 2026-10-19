#pragma once

#include <bg/time_series/time_series_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

namespace bg::details {

template <typename Target, auto... Properties> struct time_series_type_traits<REF<Target, Properties...>> {
    using semantic_type = TimeSeriesReferenceType;

    static constexpr bool generic = generic_time_series<Target>;
    static constexpr bool supported = supported_time_series<Target>;

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        constexpr auto target_role = Role == TimeSeriesTypeRole::input ? Role : TimeSeriesTypeRole::declaration;
        const auto &target = time_series_type_for<Scope, Index, target_role, Target>(TimeSeriesGuarantee::ordinary);

        if constexpr (Role == TimeSeriesTypeRole::output && sizeof...(Properties) == 0) {
            const auto inherited = guarantee.value();
            guarantee = inherited ? strongest(*inherited, TimeSeriesGuarantee::deduplicated)
                                  : TimeSeriesGuarantee::deduplicated;
        }

        if constexpr (Role == TimeSeriesTypeRole::input || generic_time_series<Target>) {
            static const auto type = util::static_storage<GenericTimeSeriesReferenceType>{target, guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
            return type.get();
        }
        if (!target.is_resolved() || !guarantee.is_resolved()) {
            static const auto type = util::static_storage<GenericTimeSeriesReferenceType>{target, guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
            return type.get();
        }

        return time_series_reference_type(target, *guarantee.value());
    }
};

template <typename Target, auto... Properties> struct declaration_variable_traits<REF<Target, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Target>(visitor);
        time_series_properties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
