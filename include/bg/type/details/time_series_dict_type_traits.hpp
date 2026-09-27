#pragma once

#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_set_type.hpp>

namespace bg::details {

template <typename Key, typename Item, auto... Properties>
struct time_series_type_traits<TSD<Key, Item, Properties...>> {
    using semantic_type = TimeSeriesDictionaryType;

    static constexpr bool generic = generic_scalar<Key> || generic_time_series<Item>;
    static constexpr bool supported =
        (generic_scalar<Key> ||
         (scalar_declaration_traits<Key>::supports_time_series && has_cpp_scalar_type<Key>)) &&
        supported_time_series<Item>;

    template <typename Scope, std::size_t Index, bool Input>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &key = scalar_type_for<Key>();
        const auto &value = time_series_type_for<Scope, Index, Input, Item>(guarantee);
        static const auto key_set = GenericTimeSeriesSetType{key, guarantee};

        if constexpr (Input || generic_scalar<Key>) {
            static const auto type = GenericTimeSeriesDictionaryType{key, value, key_set, guarantee};
            return type;
        }
        if (!value.is_resolved() || !guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesDictionaryType{key, value, key_set, guarantee};
            return type;
        }

        return time_series_dictionary_type(key, value, *guarantee.value());
    }
};

template <typename Key, typename Item, auto... Properties>
struct declaration_variable_traits<TSD<Key, Item, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Key>(visitor);
        for_each_declaration_variable<Item>(visitor);
        (for_each_property_variable<Properties>(visitor), ...);
    }
};

} // namespace bg::details
