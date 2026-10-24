#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/bundle/scalar.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/set/scalar.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/hash_set.hpp>
#include <bg/util/scope_exit.hpp>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"CONVERSION_G">;
using KEY = ScalarVariable<"CONVERSION_KEY", Hashable>;
using SIZE_DECLARATION = std::remove_cv_t<decltype(SIZE)>;
template <template <typename, auto...> typename Field>
using COMBINE_SCHEMA = BundleSchemaVariableDeclaration<"COMBINE_SCHEMA", Field>;
enum class BundleConversionDirection { bundle_to_tsb, tsb_to_bundle };

template <template <typename, auto...> typename Field>
using SCALAR_SCHEMA = BundleSchemaVariableDeclaration<"CONVERSION_SCALAR_SCHEMA", Field>;
template <template <typename, auto...> typename Field>
using SERIES_SCHEMA = BundleSchemaVariableDeclaration<"CONVERSION_SERIES_SCHEMA", Field>;
struct conversion_overloads {
    static const TimeSeriesType *requested_result(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(::bg::details::type_parameter<CONVERT_RESULT>());
        const auto *requested_mapping = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        return requested_mapping == nullptr || *requested_mapping == nullptr
                   ? nullptr
                   : type_cast<TimeSeriesType>(*requested_mapping);
    }

    static RequirementResult require_combine_tsl(const WiringArguments &arguments) {
        const auto &requested = *arguments.resolved_output_type();
        const auto &captured = TSL<TS_T, SIZE, G>::type().resolve(arguments.type_map());
        if (!requested.accepts(captured)) {
            return reject("combine requires a compatible explicit TSL result type");
        }
        return accept;
    }

    static RequirementResult require_combine_tsb(const WiringArguments &arguments) {
        const auto &requested = *arguments.resolved_output_type();
        const auto &captured = TSB<COMBINE_SCHEMA, G>::type().resolve(arguments.type_map());
        if (!requested.accepts(captured)) {
            return reject("combine requires a compatible explicit TSB result type");
        }
        return accept;
    }

    static RequirementResult require_convert_result(const WiringArguments &arguments) {
        const auto *requested = requested_result(arguments);
        if (requested == nullptr || !requested->accepts(*arguments.resolved_output_type())) {
            return reject("convert requires a compatible explicit time-series result type");
        }
        return accept;
    }

    static RequirementResult require_matching_bundle_fields(const ResolvedHierarchicalScalarType &scalar_schema,
                                                            const TimeSeriesBundleType &series_schema,
                                                            BundleConversionDirection direction) {
        const auto scalar_fields = scalar_schema.fields();
        const auto series_fields = series_schema.schema().fields();
        if (scalar_fields.size() != series_fields.size()) {
            return reject("convert between Bundle and TSB requires matching field names");
        }

        for (const auto &scalar_field : scalar_fields) {
            const auto series_field = std::ranges::find(series_fields, scalar_field.name, &SchemaField::name);
            if (series_field == series_fields.end()) {
                return reject("convert between Bundle and TSB requires matching field names");
            }

            const auto *series_value = type_cast<TimeSeriesValueType>(series_field->type);
            if (series_value == nullptr) {
                return reject("convert between Bundle and TSB requires compatible scalar-valued fields");
            }
            const auto &scalar_value = type_cast<ScalarType>(*scalar_field.type);

            const auto &source_type = direction == BundleConversionDirection::bundle_to_tsb
                                          ? static_cast<const Type &>(scalar_value)
                                          : static_cast<const Type &>(series_value->value_type());
            const auto &destination_type = direction == BundleConversionDirection::bundle_to_tsb
                                               ? static_cast<const Type &>(series_value->value_type())
                                               : static_cast<const Type &>(scalar_value);
            if (!destination_type.accepts(source_type)) {
                return reject("convert between Bundle and TSB requires compatible scalar-valued fields");
            }
        }
        return accept;
    }

    // Copy a scalar list into a TSL snapshot.
    struct list_to_tsl {
        In<TS<List<SCALAR>, G>> value;
        Out<TSL<TS<SCALAR, G>, SIZE, G>> output;

        static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &arguments) {
            const auto *requested = requested_result(arguments);
            const auto *list = requested == nullptr ? nullptr : type_cast<TimeSeriesListType>(&requested->target_type());
            return list == nullptr ? std::dynamic_extent : list->item_count();
        }

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate() const {
            const auto values = value.value_view();
            if (values.size() != output.size()) {
                throw std::invalid_argument{"convert input list size does not match the fixed TSL result size"};
            }
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                output[index].set_value(values[index]);
            }
        }
    };

    overload<convert, list_to_tsl> convert_list_to_tsl;

    // Copy a fixed TSL snapshot into a scalar list.
    struct tsl_to_list {
        In<TSL<TS<SCALAR, G>, SIZE, G>, InputValidity::all_valid> value;
        Out<TS<List<SCALAR>, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate() const {
            auto items = std::views::iota(std::size_t{}, value.size()) |
                         std::views::transform([&](std::size_t index) { return value[index].value_view(); });
            output.set(items);
        }
    };

    overload<convert, tsl_to_list> convert_tsl_to_list;

    // Copy a TSS snapshot into a scalar set.
    struct tss_to_set {
        In<TSS<KEY, G>> value;
        Out<TS<Set<KEY>, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            auto items = std::pmr::vector<Scalar<KEY>>{&memory.memory_resource()};
            items.reserve(value.size());
            value.for_each([&](const auto &item) { items.push_back(item); });
            output.set(items);
        }
    };

    overload<convert, tss_to_set> convert_tss_to_set;

    // Copy a TSD snapshot into a scalar dictionary.
    struct tsd_to_dict {
        In<TSD<KEY, TS<SCALAR, G>, G>, InputValidity::all_valid> value;
        Out<TS<Dict<KEY, SCALAR>, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            auto entries = std::pmr::vector<std::pair<Scalar<KEY>, Scalar<SCALAR>>>{&memory.memory_resource()};
            entries.reserve(value.size());
            value.for_each_key_value([&](const auto &key, const auto &item) {
                entries.emplace_back(key, item.value_view());
            });
            output.set(entries);
        }
    };

    overload<convert, tsd_to_dict> convert_tsd_to_dict;

    // Copy scalar collection members into a TSS snapshot.
    struct list_to_tss {
        In<TS<List<KEY>, G>> value;
        Out<TSS<KEY, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            const auto values = value.value_view();
            if (!output.valid()) {
                output.clear();
            }

            auto memory = context->runtime_graph().allocation();
            auto current = util::hash_set<Scalar<KEY>, ScalarHash, ScalarEqual>{};
            auto release = util::scope_exit{[&]() noexcept { current.release(memory); }};
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                current.insert(values[index], memory);
            }

            output.for_each([&](const auto &existing) {
                if (!current.contains(existing)) {
                    output.erase(existing);
                }
            });
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                output.insert(values[index]);
            }
        }
    };

    overload<convert, list_to_tss> convert_list_to_tss;

    struct set_to_tss {
        In<TS<Set<KEY>, G>> value;
        Out<TSS<KEY, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate() const {
            const auto values = value.value_view();
            if (!output.valid()) {
                output.clear();
            }
            output.for_each([&](const auto &existing) {
                if (!values.contains(existing)) {
                    output.erase(existing);
                }
            });
            values.for_each([&](const auto &item) { output.insert(item); });
        }
    };

    overload<convert, set_to_tss> convert_set_to_tss;

    // Copy scalar dictionary members into a TSD snapshot.
    struct dict_to_tsd {
        In<TS<Dict<KEY, SCALAR>, G>> value;
        Out<TSD<KEY, TS<SCALAR, G>, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate() const {
            const auto values = value.value_view();
            if (!output.valid()) {
                output.clear();
            }

            output.for_each_key([&](const auto &existing) {
                if (!values.contains(existing)) {
                    output.erase(existing);
                }
            });
            values.for_each([&](const auto &key, const auto &item) { output[key].set_value(item); });
        }
    };

    overload<convert, dict_to_tsd> convert_dict_to_tsd;

    // Copy scalar Bundle fields into TSB fields using a wiring-time field-index map.
    struct bundle_to_tsb_impl {
        In<TS<Bundle<SCALAR_SCHEMA>, G>> value;
        Scalar<List<i64>> field_mapping;
        Out<TSB<SERIES_SCHEMA, G>> output;

        void evaluate() const {
            const auto fields = value.value_view();
            for (auto series_index = std::size_t{}; series_index < output.size(); ++series_index) {
                const auto scalar_index = static_cast<std::size_t>(field_mapping[series_index].value());
                auto field = output[series_index];
                if (!fields.present(scalar_index)) {
                    field.invalidate();
                    continue;
                }
                field.apply(literal::ScalarLiteral(fields[scalar_index]));
            }
        }
    };

    static const SchemaType *bundle_to_tsb_schema(const WiringArguments &arguments) {
        const auto *requested = requested_result(arguments);
        if (requested == nullptr) {
            return nullptr;
        }

        return requested->target_type()
            .visit([](const TimeSeriesBundleType *bundle) { return &bundle->schema_type(); }, ddv::noop)
            .value_or(nullptr);
    }

    static RequirementResult require_bundle_to_tsb(const WiringArguments &arguments) {
        const auto result = require_convert_result(arguments);
        if (!result.accepted()) {
            return result;
        }

        const auto &source_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
        const auto &source_value = type_cast<TimeSeriesValueType>(source_type.target_type());
        const auto &source_schema = static_cast<const ResolvedHierarchicalScalarType &>(
            type_cast<HierarchicalScalarType>(source_value.value_type()));
        const auto &resolved_schema =
            type_cast<TimeSeriesBundleType>(arguments.resolved_output_type()->target_type());
        return require_matching_bundle_fields(source_schema, resolved_schema,
                                              BundleConversionDirection::bundle_to_tsb);
    }

    overload<convert> convert_bundle_to_tsb{graph<CONVERT_RESULT>(
        resolve<schema_type<SERIES_SCHEMA>> = bundle_to_tsb_schema,
        require = require_bundle_to_tsb,
        [](const WiringArguments &arguments, wp<TS<Bundle<SCALAR_SCHEMA>, G>, "value"_n> value)
            -> wp<TSB<SERIES_SCHEMA, G>> {
            static const auto copy_fields = node<bundle_to_tsb_impl>();

            const auto &source_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto &source_value = type_cast<TimeSeriesValueType>(source_type.target_type());
            const auto &source_schema = static_cast<const ResolvedHierarchicalScalarType &>(
                type_cast<HierarchicalScalarType>(source_value.value_type()));
            const auto &series_schema =
                type_cast<TimeSeriesBundleType>(arguments.resolved_output_type()->target_type()).schema();

            auto field_indices = std::vector<i64>{};
            field_indices.reserve(series_schema.fields().size());
            for (const auto &series_field : series_schema.fields()) {
                const auto source_field = std::ranges::find(source_schema.fields(), series_field.name,
                                                            &SchemaField::name);
                field_indices.push_back(static_cast<i64>(
                    std::distance(source_schema.fields().begin(), source_field)));
            }

            return copy_fields(arguments.type_map(), value, ScalarValue<List<i64>>{field_indices});
        })};

    // Copy same-named scalar-valued TSB fields into a scalar Bundle.
    struct tsb_to_bundle {
        In<TSB<SERIES_SCHEMA, G>, InputValidity::all_valid> value;
        Out<TS<Bundle<SCALAR_SCHEMA>, G>> output;

        static const SchemaType *resolve(schema_type<SCALAR_SCHEMA>, const WiringArguments &arguments) {
            const auto *requested = requested_result(arguments);
            if (requested == nullptr) {
                return nullptr;
            }

            return requested->target_type()
                .visit([](const TimeSeriesValueType *value) -> const SchemaType * {
                    const auto *schema = type_cast<HierarchicalScalarType>(&value->value_type());
                    return schema == nullptr ? nullptr : &schema->schema_type();
                },
                      ddv::noop)
                .value_or(nullptr);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto result = require_convert_result(arguments);
            if (!result.accepted()) {
                return result;
            }

            const auto &source_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto &source_schema = type_cast<TimeSeriesBundleType>(source_type.target_type());
            const auto &result_value =
                type_cast<TimeSeriesValueType>(arguments.resolved_output_type()->target_type());
            const auto &result_schema = static_cast<const ResolvedHierarchicalScalarType &>(
                type_cast<HierarchicalScalarType>(result_value.value_type()));
            return require_matching_bundle_fields(result_schema, source_schema,
                                                  BundleConversionDirection::tsb_to_bundle);
        }

        void evaluate() const {
            auto names = std::vector<std::string_view>{};
            auto fields = std::vector<Scalar<>>{};
            names.reserve(value.size());
            fields.reserve(value.size());
            for (auto index = std::size_t{}; index < value.size(); ++index) {
                names.emplace_back(value.item_name(index).text());
                const auto field = value[index].as<TS<SCALAR>>();
                const auto scalar = field.value_view();
                fields.push_back(Scalar<>::from(scalar));
            }

            auto pairs = std::vector<std::array<literal::ScalarLiteral, 2>>{};
            pairs.reserve(value.size());
            for (auto index = std::size_t{}; index < value.size(); ++index) {
                pairs.push_back({literal::ScalarLiteral(names[index]), literal::ScalarLiteral(fields[index])});
            }

            auto updates = std::vector<literal::ScalarLiteral>{};
            updates.reserve(pairs.size());
            for (const auto &pair : pairs) {
                updates.emplace_back(std::span<const literal::ScalarLiteral>{pair});
            }
            output.apply(literal::TimeSeriesLiteral(std::span<const literal::TimeSeriesLiteral>{updates}));
        }
    };

    overload<convert, tsb_to_bundle> convert_tsb_to_bundle;

    overload<combine> combine_tsl{graph<OUT>(
        require = require_combine_tsl,
        [](wp<TSL<TS_T, SIZE, G>, "values"_n, var_args, optional> values) { return values.as<OUT>(); })};

    overload<combine> combine_tsb{graph<OUT>(
        require = require_combine_tsb,
        [](wp<TSB<COMBINE_SCHEMA, G>, "values"_n, var_kwargs, optional> values) { return values.as<OUT>(); })};
};

} // namespace

void register_conversion_operators() { register_overloads<conversion_overloads>(); }

} // namespace bg::operators::details
