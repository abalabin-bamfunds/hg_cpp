#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

#include <cstdint>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto DEDUPED = guarantee_variable<"DEDUPED", TimeSeriesGuarantee::deduplicated>;
inline constexpr auto G = guarantee_variable<"G">;

struct stream_overloads {
    // Validity queries.
    overload<valid> valid_impl{
        eval = [](In<TS_T, "value"_n, optional> value, Out<TS<bool, TimeSeriesGuarantee::deduplicated>> output) {
            output.set(value.valid());
        },
        schedule_on_start = true};

    overload<all_valid> all_valid_impl{
        eval = [](In<TS_T, "value"_n, optional> value, Out<TS<bool, TimeSeriesGuarantee::deduplicated>> output) {
            output.set(value.all_valid());
        },
        schedule_on_start = true};

    struct type_impl {
        In<TS<SCALAR>, optional> value;
        Out<TS<const Type *, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const {
            if (!value.valid()) {
                output.invalidate();
                return;
            }

            output.set(std::addressof(value.value_view().concrete_type()));
        }
    };

    overload<type_, type_impl> type_impl;

    // Tick counting.
    struct count_impl {
        In<TS_T::with<G>> value;
        Out<TS<i64, G>> output;

        void evaluate() const {
            const auto next = output.valid()
                                  ? static_cast<i64>(static_cast<std::uint64_t>(output.value()) + std::uint64_t{1})
                                  : i64{1};
            output.set(next);
        }
    };

    overload<count, count_impl> count_impl;

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

    overload<sample, sample_impl> sample_impl;

    overload<dedup> dedup_identity{graph([](wp<TS_T::with<DEDUPED>> value) { return value; })};

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

    overload<dedup, dedup_impl> dedup_impl;

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

    overload<filter_, filter_impl> filter_impl;

    struct take_impl {
        In<TS_T, optional, dynamic_activity> value;
        Scalar<i64> n;
        State<i64> remaining;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto count = scalar_cast<i64>(*arguments.scalar("n")).value();
            return count == 0 ? &value_type.with_guarantee_limit(TimeSeriesGuarantee::final) : &value_type;
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto count = scalar_cast<i64>(*arguments.scalar("n")).value();
            return count < 0 ? reject("n must be nonnegative") : accept;
        }

        void start() const {
            *remaining = n.value();
            if (*remaining == 0) {
                value.make_passive();
            }
        }

        void evaluate() const {
            if (*remaining == 0) {
                return;
            }

            if (!value.valid()) {
                output.copy(value);
                return;
            }

            output.copy_tick(value);

            if (--*remaining == 0) {
                value.make_passive();
            }
        }
    };

    overload<take, take_impl> take_impl;

    struct drop_impl {
        In<TS_T, optional> value;
        Scalar<i64> n;
        State<i64> remaining;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto limit = weakest(*value_type.guarantee().value(), TimeSeriesGuarantee::final);
            return &value_type.with_guarantee_limit(limit);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto count = scalar_cast<i64>(*arguments.scalar("n")).value();
            return count < 0 ? reject("n must be nonnegative") : accept;
        }

        void start() const { *remaining = n.value(); }

        void evaluate() const {
            if (!value.valid()) {
                output.copy(value);
                return;
            }
            if (*remaining > 0) {
                --*remaining;
                return;
            }
            if (!output.valid()) {
                output.copy(value);
                return;
            }

            output.copy_tick(value);
        }
    };

    overload<drop, drop_impl> drop_impl;

    struct step_impl {
        In<TS_T, optional> value;
        Scalar<i64> n;
        State<i64> until_next;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto limit = weakest(*value_type.guarantee().value(), TimeSeriesGuarantee::final);
            return &value_type.with_guarantee_limit(limit);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto count = scalar_cast<i64>(*arguments.scalar("n")).value();
            return count < 1 ? reject("n must be positive") : accept;
        }

        void evaluate() const {
            if (!value.valid()) {
                output.copy(value);
                return;
            }
            if (*until_next > 0) {
                --*until_next;
                return;
            }

            output.copy(value);
            *until_next = n.value() - 1;
        }
    };

    overload<step, step_impl> step_impl;

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

    overload<if_then_else, if_then_else_impl> if_then_else_impl;

    // Stateful operators.
    struct lag_impl {
        In<TS<SCALAR>, optional> value;
        State<SCALAR> previous;
        State<bool> previous_valid;
        Out<TS<SCALAR>> output;

        void evaluate() const {
            if (*previous_valid) {
                output.set_value(previous.value());
            } else {
                output.invalidate();
            }

            if (!value.valid()) {
                previous.reset();
                *previous_valid = false;
                return;
            }

            // A failed copy must not mark an older snapshot as the current event's value.
            *previous_valid = false;
            previous.set(value.value_view());
            *previous_valid = true;
        }
    };

    overload<lag, lag_impl> lag_impl;
};

} // namespace

void register_stream_operators() {
    register_overloads<stream_overloads>();
}

} // namespace bg::operators::details
