#include <bg/time_series/tsb/operators.hpp>

#include <bg/graph.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <optional>

namespace bg::details {
namespace {

using namespace bg::literals;

using V = TimeSeriesVariable<"V">;
template <template <typename, auto...> typename Field>
using SCALAR_SCHEMA = BundleSchemaVariableDeclaration<"GETATTR_SCALAR_SCHEMA", Field>;
using SELECTED_SCALAR = ScalarVariable<"GETATTR_SELECTED_SCALAR">;
inline constexpr auto BUNDLE_G = guarantee_variable<"GETATTR_BUNDLE_G">;
inline constexpr auto GC = guarantee_variable<"GC">;
inline constexpr auto GR = guarantee_variable<"GR">;
inline constexpr auto GF = guarantee_variable<"GF">;
using GFDeclaration = std::remove_cv_t<decltype(GF)>;

struct tsb_reference_getattr {
    In<REF<TSB<SCHEMA, GC>, GR>, optional> bundle;
    util::symbol name;
    In<REF<V, GF>, dynamic_binding, optional> field;
    Out<REF<V, GF>> output;

    static const TimeSeriesType *resolve(V, const WiringArguments &arguments) {
        const auto *selected = selected_field(arguments);
        if (selected == nullptr) {
            return nullptr;
        }

        const auto *reference = type_cast<TimeSeriesReferenceType>(selected);
        return reference == nullptr ? selected : &reference->target_type();
    }

    static TimeSeriesGuarantee resolve(GFDeclaration, const WiringArguments &arguments) {
        const auto &reference = type_cast<TimeSeriesReferenceType>(arguments.resolved_type("bundle"));
        const auto *selected = selected_field(arguments);
        if (selected == nullptr) {
            return TimeSeriesGuarantee::ordinary;
        }

        return weakest(*reference.guarantee().value(), *selected->guarantee().value());
    }

    void evaluate() const {
        if (!bundle.valid()) {
            field.unbind();
            output.invalidate();
            return;
        }

        field.bind(bundle[name]);
        output.copy(field);
    }

  private:
    static const TimeSeriesType *selected_field(const WiringArguments &arguments) {
        const auto &reference = type_cast<TimeSeriesReferenceType>(arguments.resolved_type("bundle"));
        const auto &bundle = type_cast<TimeSeriesBundleType>(reference.target_type());
        const auto value = arguments.scalar("name");
        if (!value) {
            return nullptr;
        }

        const auto name = scalar_cast<util::symbol>(*value).value();
        const auto index = bundle.find_item_index(name);
        return index ? &bundle.item_type(*index) : nullptr;
    }
};

struct tsb_getattr {
    wp<TSB<SCHEMA>> bundle;
    util::symbol name;

    static const TimeSeriesType *resolve(TS_T, const WiringArguments &arguments) {
        const auto &type = type_cast<TimeSeriesBundleType>(arguments.resolved_type("bundle"));
        const auto value = arguments.scalar("name");
        const auto field_name = scalar_cast<util::symbol>(*value).value();
        const auto *field = type.schema().find(field_name);
        return field == nullptr ? nullptr : static_cast<const TimeSeriesType *>(field->type);
    }

    wp<TS_T> wire() const {
        return wp<TS_T>{time_series_bundle_field(bundle, name)};
    }
};

} // namespace

void register_time_series_bundle_operators(Operator &getattr) {
    getattr.overload(bg::node<tsb_reference_getattr>());
    getattr.overload(bg::graph<tsb_getattr>());

    struct SelectedField {
        const SchemaField *field;
        std::size_t index;
    };
    const auto selected_field = [](const Type &input_type, util::symbol name) -> std::optional<SelectedField> {
        const auto &input = type_cast<TimeSeriesType>(input_type);
        const auto &value = type_cast<TimeSeriesValueType>(input.target_type());
        const auto &hierarchical = type_cast<HierarchicalScalarType>(value.value_type());
        const auto &schema = static_cast<const ResolvedHierarchicalScalarType &>(hierarchical);
        const auto *field = schema.find(name);
        if (field == nullptr) {
            return std::nullopt;
        }

        return SelectedField{field, static_cast<std::size_t>(field - schema.fields().data())};
    };

    getattr.overload(bg::graph<SELECTED_SCALAR>({"bundle"_n, "name"_n},
        bg::resolve<SELECTED_SCALAR> = [selected_field](const WiringArguments &arguments) -> const ScalarType & {
            const auto name = scalar_cast<util::symbol>(arguments.scalar("name").value()).value();
            const auto selected = selected_field(arguments.resolved_type("bundle"), name);
            if (!selected) {
                throw WiringError{"getattr requires a field present in the scalar Bundle schema"};
            }

            return type_cast<ScalarType>(*selected->field->type);
        },
        [selected_field](const WiringArguments &arguments, wp<TS<Bundle<SCALAR_SCHEMA>, BUNDLE_G>> bundle,
                         util::symbol name) {
            const auto field_index = selected_field(bundle.type(), name)->index;
            static const auto select = node<SELECTED_SCALAR>(eval = [](
                In<TS<Bundle<SCALAR_SCHEMA>, BUNDLE_G>, optional> input, i64 field_index,
                Out<TS<SELECTED_SCALAR, BUNDLE_G>> output) {
                if (!input.valid()) {
                    output.invalidate();
                    return;
                }

                const auto value = input.value_view();
                const auto index = static_cast<std::size_t>(field_index);
                if (!value.present(index)) {
                    output.invalidate();
                    return;
                }

                output.set_value(value[index]);
            });

            return select(arguments.type_map(), bundle, static_cast<i64>(field_index));
        }));
}

} // namespace bg::details
