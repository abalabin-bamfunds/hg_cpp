#pragma once

#include <bg/util/reflection.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::details {

template <template <template <typename> typename> typename Schema> struct bundle_template {
    template <template <typename> typename Field> using apply = Schema<Field>;
};

struct no_bundle_base {};

template <template <template <typename> typename> typename Base>
bundle_template<Base> bundle_base(const bundle_base_marker<Base> *);

no_bundle_base bundle_base(...);

template <template <template <typename> typename> typename Schema>
using bundle_base_t = decltype(bundle_base(static_cast<const Schema<bundle_schema_field> *>(nullptr)));

template <template <template <typename> typename> typename Schema>
inline constexpr bool bundle_has_base = !std::same_as<bundle_base_t<Schema>, no_bundle_base>;

template <template <template <typename> typename> typename Schema>
inline constexpr std::size_t direct_bundle_field_count = [] {
    using Reflected = Schema<bundle_schema_field>;
    constexpr auto initializer_count = util::detail::count_aggregate_members<Reflected>();
    constexpr auto base_count = static_cast<std::size_t>(bundle_has_base<Schema>);
    static_assert(initializer_count >= base_count);
    return initializer_count - base_count;
}();

template <std::size_t Index, template <template <typename> typename> typename Schema>
using direct_bundle_field = std::remove_reference_t<
    std::tuple_element_t<Index, decltype(util::detail::aggregate_tie<direct_bundle_field_count<Schema>>::apply(
                                    std::declval<Schema<bundle_schema_field> &>()))>>;

template <std::size_t Index, template <template <typename> typename> typename Schema>
using direct_bundle_field_time_series = typename direct_bundle_field<Index, Schema>::time_series_type;

template <std::size_t Index, template <template <typename> typename> typename Schema>
consteval auto direct_bundle_field_pointer() {
    using Reflected = Schema<bundle_schema_field>;
    auto fields = util::detail::aggregate_tie<direct_bundle_field_count<Schema>>::apply(
        util::detail::reflected_aggregate<Reflected>);
    using Field = std::remove_reference_t<decltype(std::get<Index>(fields))>;
    return util::detail::pointer_value<Field>{std::addressof(std::get<Index>(fields))};
}

template <std::size_t Index, template <template <typename> typename> typename Schema>
consteval std::string_view direct_bundle_field_name() {
    constexpr auto signature = util::detail::value_signature<direct_bundle_field_pointer<Index, Schema>()>();
    constexpr auto marker = std::string_view{"reflected_aggregate"};
    constexpr auto marker_position = signature.find(marker);
    constexpr auto member_access = signature.find('.', marker_position);

#if defined(_MSC_VER)
    auto end = signature.rfind(">(void)");
#else
    auto end = signature.find(';', member_access);
    if (end == std::string_view::npos) {
        end = signature.rfind(']');
    }
#endif

    while (end > member_access && !util::detail::identifier_character(signature[end - 1])) {
        --end;
    }
    auto begin = end;
    while (begin > member_access && util::detail::identifier_character(signature[begin - 1])) {
        --begin;
    }
    return signature.substr(begin, end - begin);
}

template <typename T> struct time_series_bundle : std::false_type {};

template <template <template <typename> typename> typename Schema, auto... Properties>
struct time_series_bundle<TSB<Schema, Properties...>> : std::true_type {
    template <template <typename> typename Field> using schema_type = Schema<Field>;
};

template <typename T> inline constexpr bool is_time_series_bundle = time_series_bundle<std::remove_cv_t<T>>::value;

inline constexpr auto missing_bundle_field = static_cast<std::size_t>(-1);

template <template <template <typename> typename> typename Schema>
consteval std::size_t direct_bundle_field_index_of(std::string_view name) {
    constexpr auto names = []<std::size_t... Index>(std::index_sequence<Index...>) {
        return std::array<std::string_view, sizeof...(Index)>{direct_bundle_field_name<Index, Schema>()...};
    }(std::make_index_sequence<direct_bundle_field_count<Schema>>{});
    const auto field = std::ranges::find(names, name);
    return field == names.end() ? missing_bundle_field : static_cast<std::size_t>(field - names.begin());
}

template <template <template <typename> typename> typename Schema> consteval std::size_t bundle_field_count_of();

template <template <template <typename> typename> typename Schema>
consteval std::size_t bundle_field_index_of(std::string_view name);

template <template <template <typename> typename> typename Schema>
consteval std::size_t direct_bundle_field_logical_index_of(std::size_t index);

template <template <template <typename> typename> typename Schema> consteval std::size_t bundle_field_count_of() {
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        auto count = bundle_field_count_of<Base::template apply>();
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((count +=
              static_cast<std::size_t>(bundle_field_index_of<Base::template apply>(
                                           direct_bundle_field_name<Index, Schema>()) == missing_bundle_field)),
             ...);
        }(std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        return count;
    }
    return direct_bundle_field_count<Schema>;
}

template <template <template <typename> typename> typename Schema>
consteval std::size_t direct_bundle_field_logical_index_of(std::size_t index) {
    if constexpr (!bundle_has_base<Schema>) {
        return index;
    } else {
        using Base = bundle_base_t<Schema>;
        constexpr auto names = []<std::size_t... Index>(std::index_sequence<Index...>) {
            return std::array<std::string_view, sizeof...(Index)>{direct_bundle_field_name<Index, Schema>()...};
        }(std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        const auto inherited = bundle_field_index_of<Base::template apply>(names[index]);
        if (inherited != missing_bundle_field) {
            return inherited;
        }

        auto preceding_new_fields = std::size_t{};
        for (auto previous = std::size_t{}; previous < index; ++previous) {
            preceding_new_fields += static_cast<std::size_t>(
                bundle_field_index_of<Base::template apply>(names[previous]) == missing_bundle_field);
        }
        return bundle_field_count_of<Base::template apply>() + preceding_new_fields;
    }
}

template <template <template <typename> typename> typename Schema>
consteval std::size_t bundle_field_index_of(std::string_view name) {
    const auto direct = direct_bundle_field_index_of<Schema>(name);
    if (direct != missing_bundle_field) {
        return direct_bundle_field_logical_index_of<Schema>(direct);
    }
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        return bundle_field_index_of<Base::template apply>(name);
    }
    return missing_bundle_field;
}

template <template <template <typename> typename> typename Schema>
inline constexpr std::size_t bundle_field_count = bundle_field_count_of<Schema>();

template <std::size_t Index, template <template <typename> typename> typename Schema>
inline constexpr std::size_t direct_bundle_field_logical_index = direct_bundle_field_logical_index_of<Schema>(Index);

template <util::compile_time_string Name, template <template <typename> typename> typename Schema>
inline constexpr std::size_t direct_bundle_field_index = direct_bundle_field_index_of<Schema>(Name.view());

template <util::compile_time_string Name, template <template <typename> typename> typename Schema>
inline constexpr std::size_t bundle_field_index = bundle_field_index_of<Schema>(Name.view());

template <util::compile_time_string Name, template <template <typename> typename> typename Schema>
consteval auto bundle_field_type_identity() {
    constexpr auto Index = direct_bundle_field_index<Name, Schema>;
    if constexpr (Index != missing_bundle_field) {
        return std::type_identity<direct_bundle_field_time_series<Index, Schema>>{};
    } else if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        return bundle_field_type_identity<Name, Base::template apply>();
    } else {
        static_assert(Index != missing_bundle_field, "bundle schema has no field with this name");
    }
}

template <util::compile_time_string Name, template <template <typename> typename> typename Schema>
using bundle_field_time_series = typename decltype(bundle_field_type_identity<Name, Schema>())::type;

template <template <template <typename> typename> typename Root,
          template <template <typename> typename> typename Schema, template <typename> typename Field, typename Factory,
          std::size_t... Index>
[[nodiscard]] Schema<Field> make_bundle_fields(Factory &factory, std::index_sequence<Index...>) {
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        auto base = make_bundle_fields<Root, Base::template apply, Field>(
            factory, std::make_index_sequence<direct_bundle_field_count<Base::template apply>>{});
        return Schema<Field>{bundle_extends<Base::template apply, Field>{std::move(base)},
                             factory.template make<direct_bundle_field_time_series<Index, Schema>>(
                                 bundle_field_index_of<Root>(direct_bundle_field_name<Index, Schema>()))...};
    } else {
        return Schema<Field>{factory.template make<direct_bundle_field_time_series<Index, Schema>>(
            bundle_field_index_of<Root>(direct_bundle_field_name<Index, Schema>()))...};
    }
}

template <template <template <typename> typename> typename Schema, template <typename> typename Field, typename Factory>
[[nodiscard]] Schema<Field> make_bundle_fields(Factory &factory) {
    return make_bundle_fields<Schema, Schema, Field>(factory,
                                                     std::make_index_sequence<direct_bundle_field_count<Schema>>{});
}

} // namespace bg::details
