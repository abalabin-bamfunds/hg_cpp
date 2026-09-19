#include <bg/time_series/tsb/operators.hpp>

#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <vector>

namespace bg::details {
namespace {

struct TimeSeriesBundleGetattr {
    wp<TS_T> operator()(wp<TSB<SCHEMA>> bundle, util::symbol name) const {
        return wp<TS_T>{time_series_bundle_field(bundle, name)};
    }
};

Signature getattr_signature() {
    auto resolvers = std::vector<VariableResolver>{};
    resolvers.emplace_back(&TimeSeriesVariableInstance<TS_T>::get(),
                           [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
                               const auto &type =
                                   type_cast<TimeSeriesBundleType>(arguments.resolved_type("bundle"));
                               const auto value = arguments.scalar("name");
                               const auto name = scalar_cast<util::symbol>(*value).value();
                               const auto *field = type.schema().find(name);
                               return static_cast<const Type *>(field == nullptr ? nullptr : field->type);
                           });

    auto parameters = std::vector<Parameter>{
        {util::symbol{"bundle"}, &TSB<SCHEMA>::type()},
        {util::symbol{"name"}, &symbol_type()},
    };
    return {util::symbol{"tsb_getattr"}, std::move(parameters), TS_T::type(), std::move(resolvers)};
}

} // namespace

void register_time_series_bundle_operators(Operator &getattr) {
    getattr.overload(GraphTemplate{getattr_signature(), TimeSeriesBundleGetattr{}});
}

} // namespace bg::details
