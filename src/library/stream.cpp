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
    struct sample_impl {
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

    overload<sample, sample_impl> sampling;

    overload<dedup> deduplicate_identity{
        graph("name"_n = "deduplicated_identity", [](wp<TS_T::with<DEDUPED>> value) { return value; })};

    struct dedup_impl {
        In<TS_T> value;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto guarantee = strongest(*value_type.guarantee().value(), TimeSeriesGuarantee::deduplicated);
            return &value_type.with_guarantee(guarantee);
        }

        void evaluate() const { output.copy_tick(value); }
    };

    overload<dedup, dedup_impl> deduplicate_node;

    struct filter_impl {
        In<TS<bool>, optional> condition;
        In<TS_T, optional> value;
        Out<OUT> output;
        State<bool> was_open;
        State<EvaluationTime> last_forwarded;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto limit = weakest(*value_type.guarantee().value(), TimeSeriesGuarantee::final);
            return &value_type.with_guarantee_limit(limit);
        }

        void evaluate() const {
            if (!condition.valid() || !*condition) {
                *was_open = false;
                return;
            }

            if (!*was_open) {
                if (!output.valid() || value.last_updated() != *last_forwarded) {
                    output.copy(value);
                }
            } else {
                output.copy_tick(value);
            }

            *was_open = true;
            *last_forwarded = value.last_updated();
        }
    };

    overload<filter_, filter_impl> filter_node;

    struct if_then_else_impl {
        In<TS<bool>> condition;
        In<REF<TRUE_TS>, optional> true_value;
        In<REF<FALSE_TS>, optional> false_value;
        Out<REF<OUT>> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &true_value = type_cast<TimeSeriesType>(arguments.resolved_type("true_value"));
            const auto &false_value = type_cast<TimeSeriesType>(arguments.resolved_type("false_value"));
            return true_value.target_type().common_type(false_value.target_type());
        }

        void evaluate() const {
            if (*condition) {
                output.copy(true_value);
            } else {
                output.copy(false_value);
            }
        }
    };

    overload<if_then_else, if_then_else_impl> if_then_else_node;
};

} // namespace

void register_stream_operators() {
    (void)stream_overloads{};
}

} // namespace bg::operators::details
