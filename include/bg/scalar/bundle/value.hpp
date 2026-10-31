#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/bundle/details/value_factory.hpp>
#include <bg/scalar/details/source_construction.hpp>
#include <bg/schema/arguments.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Source>
inline constexpr bool hierarchical_bundle_scalar_source = [] {
    using Value = std::remove_cvref_t<Source>;
    if constexpr (!scalar_source_construction_supported<Value>) {
        return false;
    } else if constexpr (scalar_source<Value>) {
        return scalar_type_mapped<canonical_scalar_t<Value>>;
    } else {
        return true;
    }
}();

template <typename Argument>
inline constexpr bool hierarchical_bundle_scalar_argument =
    hierarchical_bundle_scalar_source<schema_argument_value_t<Argument>>;

template <template <template <typename, auto...> typename> typename Schema>
class HierarchicalBundleConstruction {
    using Reflection = hierarchical_scalar_schema_reflection<Schema>;

  public:
    template <typename Argument, typename Source>
    void add(ScalarSourceConstructionContext<Source> &source) {
        const auto index = assignment_.add(schema_argument_name<Argument>(), [&](util::symbol name) {
            const auto fields = type().fields();
            const auto field = std::ranges::find(fields, name, &SchemaField::name);
            if (field == fields.end()) {
                return std::optional<std::size_t>{};
            }
            return std::optional<std::size_t>{static_cast<std::size_t>(field - fields.begin())};
        });
        fields_[index] = ScalarConstruction{&source, &ScalarSourceConstructionContext<Source>::callback};
    }

    ScalarValue<Bundle<Schema>> finish(allocator *memory) {
        const auto semantic_fields = type().fields();
        for (auto index = std::size_t{}; index < semantic_fields.size(); ++index) {
            if (!assignment_.assigned(index) && !semantic_fields[index].optional) {
                throw std::invalid_argument{"a required bundle field was not supplied"};
            }
        }
        return HierarchicalScalarValueFactory::make<Schema>(
            ScalarFieldConstruction{this, &HierarchicalBundleConstruction::construct_field}, memory);
    }

  private:
    static const ResolvedHierarchicalScalarType &type() {
        return static_cast<const ResolvedHierarchicalScalarType &>(cpp_scalar_type<Bundle<Schema>>());
    }

    static bool construct_field(void *context, std::size_t index, const ScalarRuntimeType &runtime_type,
                                void *destination, allocator &memory) {
        const auto &fields = static_cast<const HierarchicalBundleConstruction *>(context)->fields_;
        if (!fields[index]) {
            return false;
        }

        const auto &construction = *fields[index];
        construction.construct(construction.context, runtime_type, destination, memory);
        return true;
    }

    SchemaArgumentAssignment<Reflection::field_count> assignment_;
    std::array<std::optional<ScalarConstruction>, Reflection::field_count> fields_{};
};

template <template <template <typename, auto...> typename> typename Schema, typename... Arguments>
ScalarValue<Bundle<Schema>> make_hierarchical_bundle(allocator *memory, Arguments &&...arguments) {
    auto contexts = std::tuple{ScalarSourceConstructionContext<schema_argument_value_t<Arguments>>{
        schema_argument_value(std::forward<Arguments>(arguments))}...};
    auto construction = HierarchicalBundleConstruction<Schema>{};
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        (construction.template add<Arguments>(std::get<Index>(contexts)), ...);
    }(std::index_sequence_for<Arguments...>{});
    return construction.finish(memory);
}

} // namespace bg::details

namespace bg {

template <template <template <typename, auto...> typename> typename Schema, typename... Arguments>
    requires(!details::schema_variable_template<Schema>) &&
             details::hierarchical_scalar_schema_supported<Schema> &&
             (details::hierarchical_bundle_scalar_argument<Arguments> && ...)
[[nodiscard]] ScalarValue<Bundle<Schema>> bundle(Arguments &&...arguments) {
    return details::make_hierarchical_bundle<Schema>(nullptr, std::forward<Arguments>(arguments)...);
}

template <template <template <typename, auto...> typename> typename Schema, typename... Arguments>
    requires(!details::schema_variable_template<Schema>) &&
             details::hierarchical_scalar_schema_supported<Schema> &&
             (details::hierarchical_bundle_scalar_argument<Arguments> && ...)
[[nodiscard]] ScalarValue<Bundle<Schema>> bundle(allocator &memory, Arguments &&...arguments) {
    return details::make_hierarchical_bundle<Schema>(&memory, std::forward<Arguments>(arguments)...);
}

[[nodiscard]] inline ScalarValue<Bundle<SCHEMA>> bundle(
    const Schema &schema, std::span<const std::optional<Scalar<>>> fields) {
    return details::HierarchicalScalarValueFactory::make(schema, fields);
}

[[nodiscard]] inline ScalarValue<Bundle<SCHEMA>> bundle(
    allocator &memory, const Schema &schema, std::span<const std::optional<Scalar<>>> fields) {
    return details::HierarchicalScalarValueFactory::make(schema, fields, &memory);
}

} // namespace bg
