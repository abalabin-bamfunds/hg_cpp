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

template <template <template <typename, auto...> typename> typename Schema> struct schema_template {
    template <template <typename, auto...> typename Field> using apply = Schema<Field>;
};

template <typename, auto...> struct schema_identity_field {};

struct no_schema_base {};

// Field wrappers can record their address while schema_reflection captures a
// schema's field offsets.
struct FieldProbe {
    const void *address{};

    void record(const void *field_address) noexcept { address = field_address; }
};

template <template <template <typename, auto...> typename> typename Base> struct schema_base_marker {};

template <template <typename, auto...> typename Field>
inline constexpr bool is_schema_reflection_field = false;

template <template <template <typename, auto...> typename> typename Base>
schema_template<Base> schema_base(const schema_base_marker<Base> *);

no_schema_base schema_base(...);

} // namespace bg::details

namespace bg {

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field,
          bool Reflection = details::is_schema_reflection_field<Field>>
struct schema_extends;

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field>
struct schema_extends<Base, Field, false> : Base<Field> {};

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field>
struct schema_extends<Base, Field, true> : details::schema_base_marker<Base> {};

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field>
using bundle_extends = schema_extends<Base, Field>;

} // namespace bg

namespace bg::details {

inline constexpr auto missing_schema_field = static_cast<std::size_t>(-1);

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename ReflectionField>
class schema_reflection;

template <typename... Values> struct schema_initializer_list {};

template <typename Left, typename Right> struct schema_initializer_list_concat;

template <typename... Left, typename... Right>
struct schema_initializer_list_concat<schema_initializer_list<Left...>, schema_initializer_list<Right...>> {
    using type = schema_initializer_list<Left..., Right...>;
};

template <typename ReflectedField, std::size_t ProbeIndex, std::size_t LogicalIndex>
struct schema_field_initializer {
    using reflected_field = ReflectedField;
    static constexpr std::size_t probe_index = ProbeIndex;
    static constexpr std::size_t logical_index = LogicalIndex;
    static constexpr bool is_field = true;
};

template <template <template <typename, auto...> typename> typename Base> struct schema_empty_base_initializer {
    template <template <typename, auto...> typename Field> using schema = Base<Field>;
    static constexpr bool is_field = false;
};

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename ReflectionField, typename RootReflection,
          std::size_t ProbeOffset, typename IndexSequence>
struct schema_direct_initializers;

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename ReflectionField, typename RootReflection,
          std::size_t ProbeOffset, std::size_t... Index>
struct schema_direct_initializers<Schema, ReflectionField, RootReflection, ProbeOffset,
                                  std::index_sequence<Index...>> {
    using Reflection = schema_reflection<Schema, ReflectionField>;

    template <std::size_t FieldIndex>
    using field_initializer = schema_field_initializer<
        typename Reflection::template direct_field<FieldIndex>, ProbeOffset + FieldIndex,
        RootReflection::field_index(Reflection::template direct_field_name<FieldIndex>())>;

    using type = schema_initializer_list<field_initializer<Index>...>;
};

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename ReflectionField, typename RootReflection>
struct schema_initializers {
    using Reflection = schema_reflection<Schema, ReflectionField>;

    static constexpr std::size_t base_probe_count = [] {
        if constexpr (Reflection::has_base) {
            using Base = typename Reflection::base_schema;
            using BaseReflection = schema_reflection<Base::template apply, ReflectionField>;
            if constexpr (BaseReflection::field_count == 0) {
                return std::size_t{};
            } else {
                return schema_initializers<Base::template apply, ReflectionField, RootReflection>::probe_count;
            }
        }
        return std::size_t{};
    }();

    using base_initializers = decltype([] {
        if constexpr (!Reflection::has_base) {
            return schema_initializer_list<>{};
        } else {
            using Base = typename Reflection::base_schema;
            using BaseReflection = schema_reflection<Base::template apply, ReflectionField>;
            if constexpr (BaseReflection::field_count == 0) {
                return schema_initializer_list<schema_empty_base_initializer<Base::template apply>>{};
            } else {
                return typename schema_initializers<Base::template apply, ReflectionField,
                                                    RootReflection>::type{};
            }
        }
    }());

    using direct_initializers = typename schema_direct_initializers<
        Schema, ReflectionField, RootReflection, base_probe_count,
        std::make_index_sequence<Reflection::direct_field_count>>::type;
    using type = typename schema_initializer_list_concat<base_initializers, direct_initializers>::type;
    static constexpr std::size_t probe_count = base_probe_count + Reflection::direct_field_count;
};

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename ReflectionField>
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

    using initializers = schema_initializers<Schema, ReflectionField, schema_reflection>;

    template <typename Descriptor, template <typename, auto...> typename Field, typename Factory>
    static decltype(auto) make_initializer(Factory &factory) {
        if constexpr (Descriptor::is_field) {
            return factory.template make<typename Descriptor::reflected_field>(Descriptor::logical_index);
        } else {
            return typename Descriptor::template schema<Field>{};
        }
    }

    template <template <typename, auto...> typename Field, typename Factory, typename... Descriptor>
    static Schema<Field> make_fields(Factory &factory, schema_initializer_list<Descriptor...>) {
        return Schema<Field>{make_initializer<Descriptor, Field>(factory)...};
    }

    template <typename Descriptor, template <typename, auto...> typename Field, std::size_t Size>
    static decltype(auto) probe_initializer(std::array<FieldProbe, Size> &probes) {
        if constexpr (Descriptor::is_field) {
            return (probes[Descriptor::probe_index]);
        } else {
            return typename Descriptor::template schema<Field>{};
        }
    }

    template <typename Descriptor, typename Sample, typename Probes, typename Offsets>
    static void record_offset(const Sample &sample, const Probes &probes, Offsets &offsets) {
        if constexpr (Descriptor::is_field) {
            const auto *object = reinterpret_cast<const std::byte *>(std::addressof(sample));
            const auto *field = reinterpret_cast<const std::byte *>(probes[Descriptor::probe_index].address);
            offsets[Descriptor::logical_index] = static_cast<std::size_t>(field - object);
        }
    }

    template <template <typename, auto...> typename Field, typename... Descriptor>
    static auto capture_offsets(schema_initializer_list<Descriptor...>) {
        std::array<FieldProbe, initializers::probe_count> probes{};
        Schema<Field> sample{probe_initializer<Descriptor, Field>(probes)...};
        std::array<std::size_t, field_count> offsets{};
        (record_offset<Descriptor>(sample, probes, offsets), ...);
        return offsets;
    }

    template <template <typename, auto...> typename Field, std::size_t... Index>
    static auto fields(Schema<Field> &schema, std::index_sequence<Index...>) {
        auto *object = reinterpret_cast<std::byte *>(std::addressof(schema));
        const auto &offsets = field_offsets<Field>();
        return std::tuple<typename field<Index>::template rebind<Field> &...>{
            *reinterpret_cast<typename field<Index>::template rebind<Field> *>(object + offsets[Index])...};
    }

    template <template <typename, auto...> typename Field, std::size_t... Index>
    static auto fields(const Schema<Field> &schema, std::index_sequence<Index...>) {
        const auto *object = reinterpret_cast<const std::byte *>(std::addressof(schema));
        const auto &offsets = field_offsets<Field>();
        return std::tuple<const typename field<Index>::template rebind<Field> &...>{
            *reinterpret_cast<const typename field<Index>::template rebind<Field> *>(object + offsets[Index])...};
    }

    template <std::size_t LogicalIndex, std::size_t DirectIndex = 0>
    static consteval std::string_view field_name_identity() {
        if constexpr (DirectIndex < direct_field_count) {
            if constexpr (direct_field_logical_index(DirectIndex) == LogicalIndex) {
                return direct_field_name<DirectIndex>();
            } else {
                return field_name_identity<LogicalIndex, DirectIndex + 1>();
            }
        } else if constexpr (has_base) {
            using BaseReflection = schema_reflection<base_schema::template apply, ReflectionField>;
            return BaseReflection::template field_name<LogicalIndex>();
        } else {
            static_assert(LogicalIndex == missing_schema_field, "schema has no field with this index");
        }
    }

    template <std::size_t LogicalIndex, std::size_t DirectIndex = 0>
    static consteval auto field_type_identity() {
        if constexpr (DirectIndex < direct_field_count) {
            if constexpr (direct_field_logical_index(DirectIndex) == LogicalIndex) {
                return std::type_identity<direct_field<DirectIndex>>{};
            } else {
                return field_type_identity<LogicalIndex, DirectIndex + 1>();
            }
        } else if constexpr (has_base) {
            using BaseReflection = schema_reflection<base_schema::template apply, ReflectionField>;
            return std::type_identity<typename BaseReflection::template field<LogicalIndex>>{};
        } else {
            static_assert(LogicalIndex == missing_schema_field, "schema has no field with this index");
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

    template <std::size_t Index>
    using field = typename decltype(field_type_identity<Index>())::type;

    template <std::size_t Index> static consteval std::string_view field_name() {
        static_assert(Index < field_count, "schema has no field with this index");
        return field_name_identity<Index>();
    }

    template <template <typename, auto...> typename Field> static const auto &field_offsets() {
        static const auto offsets = capture_offsets<Field>(typename initializers::type{});
        return offsets;
    }

    template <template <typename, auto...> typename Field> static auto fields(Schema<Field> &schema) {
        return fields(schema, std::make_index_sequence<field_count>{});
    }

    template <template <typename, auto...> typename Field> static auto fields(const Schema<Field> &schema) {
        return fields(schema, std::make_index_sequence<field_count>{});
    }

    template <template <typename, auto...> typename Field, typename Factory>
    [[nodiscard]] static Schema<Field> make(Factory &factory) {
        return make_fields<Field>(factory, typename initializers::type{});
    }
};

} // namespace bg::details
