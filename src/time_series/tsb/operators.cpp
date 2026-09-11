#include <bg/time_series/tsb/operators.hpp>

#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <memory>
#include <vector>

namespace bg::details {
namespace {

class TimeSeriesBundleFieldWiringRecord final : public WiringRecord {
  public:
    TimeSeriesBundleFieldWiringRecord(WiringPoint bundle, item_id_t field, const TimeSeriesType &type)
        : WiringRecord{type}, bundle_{bundle}, field_{field} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto bundle = materialize_wiring_point(bundle_, bundle_.type());
        const auto field = wiring_point_for_item(bundle, field_, type());
        return materialize_wiring_point(field, requested_type);
    }

    WiringPoint bundle_;
    item_id_t field_;
};

const TimeSeriesBundleType &bundle_type(const Type &type) {
    const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&type);
    if (bundle == nullptr) {
        throw WiringError{"getattr requires a TSB"};
    }
    return *bundle;
}

std::size_t field_index(const TimeSeriesBundleType &type, util::symbol name) {
    for (auto index = std::size_t{}; index < type.item_count(); ++index) {
        if (type.item_name(index) == name) {
            return index;
        }
    }

    throw WiringError{"a TSB has no field with the requested name"};
}

struct TimeSeriesBundleGetattr {
    wp<TS_T> operator()(wp<TSB<SCHEMA>> bundle, util::symbol name) const {
        const auto &type = bundle_type(bundle.type());
        const auto index = field_index(type, name);

        const auto &record = wiring_record(bundle);
        if (const auto *free_bundle = dynamic_cast<const FreeTimeSeriesBundleWiringRecord *>(&record)) {
            return wp<TS_T>{free_bundle->field(index)};
        }

        const auto field = type.item_id(index);
        auto selected = std::make_unique<TimeSeriesBundleFieldWiringRecord>(bundle, field, type.item_type(index));
        return wp<TS_T>{add_item_wiring_record(bundle, field, std::move(selected))};
    }
};

Signature getattr_signature() {
    auto resolvers = std::vector<VariableResolver>{};
    resolvers.emplace_back(&TimeSeriesVariableInstance<TS_T>::get(),
                           [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
                               const auto &type = bundle_type(arguments.resolved_type("bundle"));
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
