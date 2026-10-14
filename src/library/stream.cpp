#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>

namespace bg::operators::details {
namespace {

inline constexpr auto DEDUPED = guarantee_variable<"DEDUPED", TimeSeriesGuarantee::deduplicated>;

struct sample_series {
    In<SIGNAL> signal;
    In<TS_T, passive> value;
    Out<OUT> output;

    static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
        const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
        return &value_type.with_guarantee_limit(TimeSeriesGuarantee::ordinary);
    }

    void evaluate() const { output.copy(value); }

    static constexpr auto on_invalidation = preserve_output;
};

struct deduplicate_series {
    In<TS_T> value;
    Out<OUT> output;

    static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
        const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
        const auto guarantee = strongest(*value_type.guarantee().value(), TimeSeriesGuarantee::deduplicated);
        return &value_type.with_guarantee(guarantee);
    }

    void evaluate() const { output.copy_tick(value); }
};

} // namespace

void register_stream_operators() {
    using namespace bg::literals;

    valid.overload(node(
        eval = [](In<TS_T, "value"_n, optional> value,
                 Out<TS<bool, TimeSeriesGuarantee::deduplicated>> output) { output.set(value.valid()); },
        schedule_on_start = true));
    all_valid.overload(node(
        eval = [](In<TS_T, "value"_n, optional> value,
                 Out<TS<bool, TimeSeriesGuarantee::deduplicated>> output) { output.set(value.all_valid()); },
        schedule_on_start = true));

    sample.overload(node<sample_series>());

    dedup.overload(graph("name"_n = "deduplicated_identity", [](wp<TS_T::with<DEDUPED>> value) { return value; }));
    dedup.overload(node<deduplicate_series>());
}

} // namespace bg::operators::details
