#pragma once

#include <hg/time_series/tsl/tsl_type.hpp>
#include <hg/type/details/time_series_type_traits.hpp>
#include <hg/type/size_variable_instance.hpp>
#include <hg/type/time_series_list_type.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/util/static_storage.hpp>

namespace hg::details {

template <typename Item, auto Size, auto... Properties> struct time_series_type_traits<TSL<Item, Size, Properties...>> {
    using semantic_type = TimeSeriesListType;

    static constexpr bool generic = generic_time_series<Item> || size_variable_declaration<decltype(Size)>;
    static constexpr bool supported = supported_time_series<Item>;

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        using Declaration = time_series_declaration_identity<time_series_type_traits, Scope, Index, Role>;
        if constexpr (!size_variable_declaration<decltype(Size)>) {
            static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");
        }
        const auto &item = time_series_type_for<Scope, Index, Role, Item>(guarantee);
        const auto size = size_parameter<Size>();

        if constexpr (Role == TimeSeriesTypeRole::input) {
            static const auto &type = TypeRegistry::instance().get_or_create_static<
                Declaration, GenericTimeSeriesListType>([&]() -> const auto & {
                    static const auto type = util::static_storage<GenericTimeSeriesListType>{item, size, guarantee};
                    return type.get();
                });
            return type;
        }
        if (!item.is_resolved() || !size.is_resolved() || !guarantee.is_resolved()) {
            static const auto &type = TypeRegistry::instance().get_or_create_static<
                Declaration, GenericTimeSeriesListType>([&]() -> const auto & {
                    static const auto type = util::static_storage<GenericTimeSeriesListType>{item, size, guarantee};
                    return type.get();
                });
            return type;
        }

        return time_series_list_type(item, *size.value(), *guarantee.value());
    }
};

template <typename Item, auto Size, auto... Properties>
struct declaration_variable_traits<TSL<Item, Size, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Item>(visitor);
        if constexpr (size_variable_declaration<decltype(Size)>) {
            using Declaration = std::remove_cv_t<decltype(Size)>;
            visitor.template operator()<Declaration>(size_variable_instance<Size>());
        }
        time_series_properties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

} // namespace hg::details
