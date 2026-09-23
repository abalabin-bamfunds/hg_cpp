#include <bg/time_series/tsb/operators.hpp>

#include <bg/graph.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_variable.hpp>

namespace bg::details {
namespace {

using V = TimeSeriesVariable<"V">;
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
        return field == nullptr ? nullptr : field->type;
    }

    wp<TS_T> wire() const {
        return wp<TS_T>{time_series_bundle_field(bundle, name)};
    }
};

} // namespace

void register_time_series_bundle_operators(Operator &getattr) {
    getattr.overload(bg::node<tsb_reference_getattr>());
    getattr.overload(bg::graph<tsb_getattr>());
}

} // namespace bg::details
