#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>

namespace bg::operators::details {
namespace {

using SAMPLE_RESULT = TimeSeriesVariable<"SAMPLE_RESULT">;
using DEDUP_RESULT = TimeSeriesVariable<"DEDUP_RESULT">;
inline constexpr auto G = guarantee_variable<"G", TimeSeriesGuarantee::deduplicated>;

struct sample_series {
    static constexpr auto on_invalidation = preserve_output;

    In<SIGNAL> signal;
    In<TS_T, passive> value;
    Out<SAMPLE_RESULT> output;

    static const TimeSeriesType *resolve(SAMPLE_RESULT, const WiringArguments &arguments) {
        const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
        return &value_type.with_guarantee_limit(TimeSeriesGuarantee::ordinary);
    }

    void evaluate() const { output.copy(value); }
};

struct deduplicate_series {
    In<TS_T> value;
    Out<DEDUP_RESULT> output;

    static const TimeSeriesType *resolve(DEDUP_RESULT, const WiringArguments &arguments) {
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

    dedup.overload(graph("name"_n = "deduplicated_identity", [](wp<TS_T::with<G>> value) { return value; }));
    dedup.overload(node<deduplicate_series>());
}

} // namespace bg::operators::details
