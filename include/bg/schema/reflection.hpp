#pragma once

#include <bg/util/compile_time_string.hpp>
#include <bg/util/reflection.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::details {

template <template <template <typename> typename> typename Schema> struct schema_template {
    template <template <typename> typename Field> using apply = Schema<Field>;
};

struct no_schema_base {};

template <template <template <typename> typename> typename Base> struct schema_base_marker {};

template <template <template <typename> typename> typename Base>
schema_template<Base> schema_base(const schema_base_marker<Base> *);

no_schema_base schema_base(...);

inline constexpr auto missing_schema_field = static_cast<std::size_t>(-1);

template <template <template <typename> typename> typename Schema, template <typename> typename ReflectionField>
class schema_reflection {
    using Reflected = Schema<ReflectionField>;

    template <std::size_t Index> static consteval auto direct_field_pointer() {
        auto fields = util::detail::aggregate_tie<direct_field_count>::apply(
            util::detail::reflected_aggregate<Reflected>.value);
        using Field = std::remove_reference_t<decltype(std::get<Index>(fields))>;
        return util::detail::pointer_value<Field>{std::addressof(std::get<Index>(fields))};
    }

    template <std::size_t... Index> static consteval auto direct_field_names(std::index_sequence<Index...>) {
        return std::array<std::string_view, sizeof...(Index)>{direct_field_name<Index>()...};
    }

    static consteval std::size_t field_count_of() {
        if constexpr (has_base) {
            using BaseReflection = schema_reflection<base_schema::template apply, ReflectionField>;
            auto count = BaseReflection::field_count;
            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                ((count += static_cast<std::size_t>(
                      BaseReflection::field_index(direct_field_name<Index>()) == missing_schema_field)),
                 ...);
            }(std::make_index_sequence<direct_field_count>{});
            return count;
        }
        return direct_field_count;
    }

    template <util::compile_time_string Name> static consteval auto field_type_identity() {
        constexpr auto Index = direct_field_index(Name.view());
        if constexpr (Index != missing_schema_field) {
            return std::type_identity<direct_field<Index>>{};
        } else if constexpr (has_base) {
            using BaseReflection = schema_reflection<base_schema::template apply, ReflectionField>;
            return std::type_identity<typename BaseReflection::template field_type<Name>>{};
        } else {
            static_assert(Index != missing_schema_field, "schema has no field with this name");
        }
    }

    template <template <template <typename> typename> typename Root,
              template <template <typename> typename> typename Current, template <typename> typename Field,
              typename Construction, typename Factory, std::size_t... Index>
    static Current<Field> construct(Factory &factory, std::index_sequence<Index...>) {
        using CurrentReflection = schema_reflection<Current, ReflectionField>;
        if constexpr (CurrentReflection::has_base) {
            using Base = typename CurrentReflection::base_schema;
            auto base = construct<Root, Base::template apply, Field, Construction>(
                factory, std::make_index_sequence<schema_reflection<Base::template apply, ReflectionField>::
                                                      direct_field_count>{});
            return Current<Field>{Construction::template make_base<Base::template apply, Field>(std::move(base)),
                                  factory.template make<typename CurrentReflection::template direct_field<Index>>(
                                      schema_reflection<Root, ReflectionField>::field_index(
                                          CurrentReflection::template direct_field_name<Index>()))...};
        } else {
            return Current<Field>{
                factory.template make<typename CurrentReflection::template direct_field<Index>>(
                    schema_reflection<Root, ReflectionField>::field_index(
                        CurrentReflection::template direct_field_name<Index>()))...};
        }
    }

  public:
    using base_schema = decltype(schema_base(static_cast<const Reflected *>(nullptr)));
    static constexpr bool has_base = !std::same_as<base_schema, no_schema_base>;

    static constexpr std::size_t direct_field_count = [] {
        constexpr auto initializer_count = util::detail::count_aggregate_members<Reflected>();
        constexpr auto base_count = static_cast<std::size_t>(has_base);
        static_assert(initializer_count >= base_count);
        return initializer_count - base_count;
    }();

    template <std::size_t Index>
    using direct_field = std::remove_reference_t<
        std::tuple_element_t<Index, decltype(util::detail::aggregate_tie<direct_field_count>::apply(
                                        std::declval<Reflected &>()))>>;

    template <std::size_t Index> static consteval std::string_view direct_field_name() {
        return util::detail::reflected_member_name<direct_field_pointer<Index>()>();
    }

    static consteval std::size_t direct_field_index(std::string_view name) {
        constexpr auto names = direct_field_names(std::make_index_sequence<direct_field_count>{});
        const auto field = std::ranges::find(names, name);
        return field == names.end() ? missing_schema_field : static_cast<std::size_t>(field - names.begin());
    }

    static consteval std::size_t direct_field_logical_index(std::size_t index) {
        if constexpr (!has_base) {
            return index;
        } else {
            using BaseReflection = schema_reflection<base_schema::template apply, ReflectionField>;
            constexpr auto names = direct_field_names(std::make_index_sequence<direct_field_count>{});
            const auto inherited = BaseReflection::field_index(names[index]);
            if (inherited != missing_schema_field) {
                return inherited;
            }

            auto preceding_new_fields = std::size_t{};
            for (auto previous = std::size_t{}; previous < index; ++previous) {
                preceding_new_fields += static_cast<std::size_t>(
                    BaseReflection::field_index(names[previous]) == missing_schema_field);
            }
            return BaseReflection::field_count + preceding_new_fields;
        }
    }

    static consteval std::size_t field_index(std::string_view name) {
        const auto direct = direct_field_index(name);
        if (direct != missing_schema_field) {
            return direct_field_logical_index(direct);
        }
        if constexpr (has_base) {
            using BaseReflection = schema_reflection<base_schema::template apply, ReflectionField>;
            return BaseReflection::field_index(name);
        }
        return missing_schema_field;
    }

    static constexpr std::size_t field_count = field_count_of();

    template <util::compile_time_string Name>
    using field_type = typename decltype(field_type_identity<Name>())::type;

    template <template <typename> typename Field, typename Construction, typename Factory>
    [[nodiscard]] static Schema<Field> make(Factory &factory) {
        return construct<Schema, Schema, Field, Construction>(
            factory, std::make_index_sequence<direct_field_count>{});
    }
};

} // namespace bg::details
