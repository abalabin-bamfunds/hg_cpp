#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto DEDUPED = guarantee_variable<"DEDUPED", TimeSeriesGuarantee::deduplicated>;

struct stream_overloads {
    // Validity queries.
    overload<valid> validity{
        eval = [](In<TS_T, "value"_n, optional> value, Out<TS<bool, TimeSeriesGuarantee::deduplicated>> output) {
            output.set(value.valid());
        },
        schedule_on_start = true};
    overload<all_valid> immediate_validity{
        eval = [](In<TS_T, "value"_n, optional> value, Out<TS<bool, TimeSeriesGuarantee::deduplicated>> output) {
            output.set(value.all_valid());
        },
        schedule_on_start = true};

    // Tick selection.
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

    overload<sample, sample_series> sampling;

    overload<dedup> deduplicate_identity{
        graph("name"_n = "deduplicated_identity", [](wp<TS_T::with<DEDUPED>> value) { return value; })};

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

    overload<dedup, deduplicate_series> deduplicate_node;
};

} // namespace

void register_stream_operators() {
    (void)stream_overloads{};
}

} // namespace bg::operators::details
