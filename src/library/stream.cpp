#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/util/buffer.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto DEDUPED = guarantee_variable<"DEDUPED", TimeSeriesGuarantee::deduplicated>;
inline constexpr auto G = guarantee_variable<"G">;
using SIZE_DECLARATION = std::remove_cv_t<decltype(SIZE)>;

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

    // Conditional and indexed routing.
    struct if_true_impl {
        In<TS<bool>, dynamic_activity> condition;
        Scalar<bool> tick_once_only;
        Out<TS<bool>> output;

        void evaluate() const {
            if (!condition.value()) {
                return;
            }

            output.set(true);
            if (tick_once_only.value()) {
                condition.make_passive();
            }
        }

        static constexpr auto on_invalidation = preserve_output;
    };

    overload<if_true, if_true_impl> if_true_impl;

    struct if_impl {
        In<TS<bool>, optional> condition;
        In<REF<TS_T>, optional> value;
        Out<TSB<if_result>> output;

        void evaluate() const {
            if (!condition.valid()) {
                output->true_value.invalidate();
                output->false_value.invalidate();
                return;
            }
            if (condition.value()) {
                output->true_value.copy(value);
                output->false_value.invalidate();
            } else {
                output->true_value.invalidate();
                output->false_value.copy(value);
            }
        }
    };

    overload<if_, if_impl> if_impl;

    struct race_selection_impl {
        In<TSL<TS<bool>, SIZE>> validities;
        In<TSL<REF<TS_T>, SIZE>, optional> candidates;
        Out<REF<TS_T>> output;

        void evaluate() const {
            auto selected = std::numeric_limits<std::size_t>::max();
            auto earliest_validity = EvaluationTime::max();
            for (auto index = std::size_t{}; index < validities.size(); ++index) {
                if (!validities[index].valid() || !validities[index].value()) {
                    continue;
                }

                const auto validity_started = validities[index].last_updated();
                if (selected == std::numeric_limits<std::size_t>::max() || validity_started < earliest_validity) {
                    selected = index;
                    earliest_validity = validity_started;
                }
            }

            if (selected == std::numeric_limits<std::size_t>::max()) {
                output.invalidate();
                return;
            }

            output.copy(candidates[selected]);
        }
    };

    struct race_graph_impl {
        wp<TSL<TS_T, SIZE>, "values"_n, var_args, optional> values;

        static RequirementResult require(const WiringArguments &arguments) {
            if (arguments.size() == 0) {
                return reject("race requires at least one input");
            }
            const auto &captured = type_cast<TimeSeriesType>(TSL<TS_T, SIZE>::type().resolve(arguments.type_map()));
            const auto &list = type_cast<TimeSeriesListType>(captured.target_type());
            if (list.item_count() == 0) {
                return reject("race requires at least one input");
            }
            return accept;
        }

        wp<REF<TS_T>> wire() const {
            if (values.size() == 1) {
                return wp<REF<TS_T>>{values[0].time_series()};
            }

            auto validities = std::vector<WiringPoint>{};
            auto candidates = std::vector<WiringPoint>{};
            validities.reserve(values.size());
            candidates.reserve(values.size());
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                auto candidate = values[index].time_series();
                validities.push_back(bg::operators::valid(candidate));
                candidates.push_back(candidate);
            }

            static const auto impl = node<race_selection_impl>();
            return wp<REF<TS_T>>::from_validated(impl(tsl(validities), tsl(candidates)));
        }
    };

    overload<race> race_impl{graph<race_graph_impl>()};

    struct route_by_index_impl {
        In<TS<i64>, optional> index;
        In<REF<TS_T>, optional> value;
        Out<TSL<REF<TS_T>, SIZE>> output;
        State<i64> selected_index;
        State<bool> has_selection;

        static const TimeSeriesType *requested_result(const WiringArguments &arguments) {
            const auto *mapping = arguments.type_map().find(bg::details::type_parameter<OUT>());
            const auto *requested_mapping = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
            return requested_mapping == nullptr || *requested_mapping == nullptr
                       ? nullptr
                       : type_cast<TimeSeriesType>(*requested_mapping);
        }

        static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &arguments) {
            const auto *result = requested_result(arguments);
            const auto *list = result == nullptr ? nullptr
                                                 : type_cast<TimeSeriesListType>(&result->target_type());
            return list == nullptr ? std::dynamic_extent : list->item_count();
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto *result = requested_result(arguments);
            const auto *list = result == nullptr ? nullptr
                                                 : type_cast<TimeSeriesListType>(&result->target_type());
            if (list == nullptr || list->item_count() == std::dynamic_extent) {
                return reject("route_by_index requires a fixed TSL result type");
            }

            const auto *resolved = arguments.resolved_output_type();
            if (resolved == nullptr || !result->accepts(*resolved)) {
                return reject("route_by_index requires a compatible fixed TSL result type");
            }

            return accept;
        }

        void evaluate() const {
            const auto next_index = index.valid() ? index.value() : i64{-1};
            if (next_index < 0 || static_cast<std::size_t>(next_index) >= output.size()) {
                if (*has_selection) {
                    output[static_cast<std::size_t>(*selected_index)].invalidate();
                    *has_selection = false;
                }
                return;
            }

            if (*has_selection && *selected_index != next_index) {
                output[static_cast<std::size_t>(*selected_index)].invalidate();
            }
            output[static_cast<std::size_t>(next_index)].copy(value);
            *selected_index = next_index;
            *has_selection = true;
        }
    };

    overload<route_by_index, route_by_index_impl> route_by_index_impl;

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

    struct take_duration_impl {
        In<TS_T, optional, dynamic_activity> value;
        Scalar<timedelta_t> n;
        State<std::optional<EvaluationTime>> started_at;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto duration = scalar_cast<timedelta_t>(*arguments.scalar("n")).value();
            return duration == timedelta_t{} ? &value_type.with_guarantee_limit(TimeSeriesGuarantee::final)
                                             : &value_type;
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto duration = scalar_cast<timedelta_t>(*arguments.scalar("n")).value();
            return duration < timedelta_t{} ? reject("duration must be nonnegative") : accept;
        }

        void start() const {
            if (n.value() == timedelta_t{}) {
                value.make_passive();
            }
        }

        void evaluate() const {
            const auto now = value.last_updated();
            if (value.valid() && !*started_at) {
                *started_at = now;
            }
            if (*started_at && now - **started_at >= n.value()) {
                value.make_passive();
                return;
            }

            output.copy_tick(value);
        }
    };

    overload<take, take_duration_impl> take_duration;

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

    struct drop_duration_impl {
        In<TS_T, optional> value;
        Scalar<timedelta_t> n;
        State<std::optional<EvaluationTime>> started_at;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto limit = weakest(*value_type.guarantee().value(), TimeSeriesGuarantee::final);
            return &value_type.with_guarantee_limit(limit);
        }

        void evaluate() const {
            const auto now = value.last_updated();
            if (value.valid() && !*started_at) {
                *started_at = now;
            }
            if (!*started_at || now - **started_at < n.value()) {
                return;
            }

            if (!value.valid() || !output.valid()) {
                output.copy(value);
                return;
            }

            output.copy_tick(value);
        }
    };

    overload<drop> drop_duration{graph(
        {"value"_n, "n"_n},
        require = [](const WiringArguments &arguments) {
            const auto duration = scalar_cast<timedelta_t>(*arguments.scalar("n")).value();
            return duration < timedelta_t{} ? reject("duration must be nonnegative") : accept;
        },
        [](wp<TS_T> value, timedelta_t n) -> wp<OUT> {
            if (n == timedelta_t{}) {
                return wp<OUT>{value};
            }

            static const auto drop = node<drop_duration_impl>();
            return wp<OUT>{drop(value, n)};
        })};

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

    overload<slice_> slice_impl{graph(
        {"value"_n, "start"_n, "stop"_n, "step"_n},
        require = [](const WiringArguments &arguments) {
            const auto start = scalar_cast<i64>(*arguments.scalar("start")).value();
            const auto stop = scalar_cast<i64>(*arguments.scalar("stop")).value();
            const auto step = scalar_cast<i64>(*arguments.scalar("step")).value();
            return start < 0 || stop < start || step <= 0
                       ? reject("slice requires 0 <= start <= stop and step > 0")
                       : accept;
        },
        [](wp<TS_T> value, i64 start, i64 stop, i64 step) -> wp<OUT> {
            auto selected = operators::take(value, stop);
            selected = operators::drop(selected, start);
            return wp<OUT>{operators::step(selected, step)};
        })};

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
    struct lag_one_impl {
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

    struct lag_buffered_impl {
        struct retained_ticks {
            retained_ticks(util::buffer buffer, allocator memory, const TimeSeriesType &type)
                : writer{buffer, memory}, reader{std::move(buffer), type} {}

            TimeSeriesTickWriter writer;
            TimeSeriesTickReader reader;
        };

        In<TS<SCALAR>, optional> value;
        Scalar<i64> n;
        State<retained_ticks *> retained;
        State<i64> pending_count;
        Out<TS<SCALAR>> output;

        void evaluate(EvaluationContext *context) const {
            const auto now = context->evaluation_time();
            if (*retained == nullptr) {
                auto memory = context->runtime_graph().allocation();
                auto buffer = util::buffer::chain(256, memory);
                *retained = memory.new_object<retained_ticks>(std::move(buffer), memory, value.runtime_type().type());
            }

            auto &ticks = **retained;
            if (!ticks.writer.write(value, now)) {
                return;
            }
            if (*pending_count < n.value()) {
                ++*pending_count;
                return;
            }

            ticks.reader.replay(output);
        }

        void stop(EvaluationContext *context) const noexcept { release(*context); }
        void finalize(EvaluationContext *context) const noexcept { release(*context); }

      private:
        void release(EvaluationContext &context) const noexcept {
            if (*retained == nullptr) {
                return;
            }
            auto memory = context.runtime_graph().allocation();
            memory.delete_object(*retained);
            *retained = nullptr;
            *pending_count = 0;
        }
    };

    overload<lag> lag_impl{graph(
        {"value"_n, "n"_n},
        require = [](const WiringArguments &arguments) {
            const auto count = scalar_cast<i64>(*arguments.scalar("n")).value();
            return count < 0 ? reject("n must be nonnegative") : accept;
        },
        [](wp<TS<SCALAR>> value, i64 n) -> wp<TS<SCALAR>> {
            if (n == 0) {
                return value;
            }
            if (n == 1) {
                static const auto lag_one = node<lag_one_impl>();
                return lag_one(value);
            }

            static const auto lag_buffered = node<lag_buffered_impl>();
            return lag_buffered(value, n);
        })};
};

} // namespace

void register_stream_operators() {
    register_overloads<stream_overloads>();
}

} // namespace bg::operators::details
