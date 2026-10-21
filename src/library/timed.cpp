#include "registration.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/util/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bg::operators::details {
namespace {

EvaluationTime due_time(EvaluationTime now, timedelta_t duration) {
    const auto now_count = now.time_since_epoch().count();
    const auto duration_count = duration.count();
    if (now_count > std::numeric_limits<std::int64_t>::max() - duration_count) {
        throw std::overflow_error{"timed operator evaluation time is outside the supported range"};
    }

    const auto result = EvaluationTime{EvaluationTime::duration{now_count + duration_count}};
    if (!TimeSeriesState::can_represent(result)) {
        throw std::overflow_error{"timed operator evaluation time is outside the supported range"};
    }
    return result;
}

struct timed_overloads {
    // Delay retains each changed input tick until its due evaluation time.
    struct delay_node {
        struct retained_ticks {
            retained_ticks(util::buffer buffer, allocator memory, const TimeSeriesType &type)
                : writer{buffer, memory}, reader{std::move(buffer), type}, due_times{&memory.memory_resource()} {}

            TimeSeriesTickWriter writer;
            TimeSeriesTickReader reader;
            std::pmr::deque<EvaluationTime> due_times;
        };

        static constexpr bool uses_timed_scheduling = true;

        In<TS_T, optional> value;
        Scalar<timedelta_t> duration;
        State<retained_ticks *> retained;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            return &value_type.dereferenced_type().with_guarantee_limit(TimeSeriesGuarantee::final);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto &stored_type = value_type.dereferenced_type();
            const auto contains_sparse_list = [](const auto &self, const TimeSeriesType &type) -> bool {
                if (const auto *list = type_cast<TimeSeriesListType>(&type); list != nullptr) {
                    return list->item_count() == std::dynamic_extent || self(self, list->item_type());
                }
                if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type); dictionary != nullptr) {
                    return self(self, dictionary->value_type());
                }
                if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type); bundle != nullptr) {
                    for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
                        if (self(self, bundle->item_type(index))) {
                            return true;
                        }
                    }
                }
                return false;
            };
            if (contains_sparse_list(contains_sparse_list, stored_type)) {
                return reject("delay does not support sparse time series lists");
            }
            return accept;
        }

        void evaluate(EvaluationContext *context) const {
            const auto now = context->evaluation_time();
            const auto has_current_tick = value.modified();

            if (*retained == nullptr) {
                if (!has_current_tick) {
                    return;
                }
                auto memory = context->runtime_graph().allocation();
                auto buffer = util::buffer::chain(256, memory);
                *retained = memory.new_object<retained_ticks>(std::move(buffer), memory, value.runtime_type().type());
            }

            auto &ticks = **retained;
            auto replayed = false;
            while (!ticks.due_times.empty() && ticks.due_times.front() <= now) {
                ticks.reader.replay(output);
                ticks.due_times.pop_front();
                replayed = true;
            }

            const auto had_retained_ticks = !ticks.due_times.empty();
            if (has_current_tick) {
                const auto due = due_time(now, duration.value());
                if (ticks.writer.write(value, now)) {
                    ticks.due_times.push_back(due);
                }
            }

            if (replayed || !had_retained_ticks) {
                if (!ticks.due_times.empty()) {
                    context->schedule_at(ticks.due_times.front());
                }
            }
        }

        void stop(EvaluationContext *context) const noexcept {
            if (*retained == nullptr) {
                return;
            }
            auto memory = context->runtime_graph().allocation();
            memory.delete_object(*retained);
            *retained = nullptr;
        }
    };

    struct delay_graph {
        wp<TS_T> value;
        timedelta_t duration;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto duration = scalar_cast<timedelta_t>(*arguments.scalar("duration")).value();
            if (duration == timedelta_t{}) {
                return &value_type;
            }
            return &value_type.dereferenced_type().with_guarantee_limit(TimeSeriesGuarantee::final);
        }

        wp<OUT> wire() const {
            if (duration < timedelta_t{}) {
                throw WiringError{"delay duration must be nonnegative"};
            }
            if (duration == timedelta_t{}) {
                return wp<OUT>{value};
            }
            if (value.type().target_type().contains_reference()) {
                throw WiringError{"delay does not support time series types containing references"};
            }

            static const auto delay = node<delay_node>();
            return delay(value, duration);
        }
    };

    overload<delay> delay_impl{graph<delay_graph>()};

    // Schedule emits a bounded sequence of true ticks.
    struct schedule_node {
        static constexpr bool uses_timed_scheduling = true;

        Scalar<timedelta_t> period;
        Scalar<bool> initial_delay;
        Scalar<i64> max_ticks;
        State<i64> emitted;
        Out<TS<bool>> output;

        void start(EvaluationContext *context) const {
            if (max_ticks.value() == 0) {
                return;
            }
            if (!initial_delay.value()) {
                output.set(true);
                ++*emitted;
            }
            schedule_next(*context);
        }

        void evaluate(EvaluationContext *context) const {
            output.set(true);
            ++*emitted;
            schedule_next(*context);
        }

      private:
        void schedule_next(EvaluationContext &context) const {
            if (*emitted >= max_ticks.value()) {
                return;
            }
            context.schedule_at(due_time(context.evaluation_time(), period.value()));
        }
    };

    overload<schedule> schedule_impl{graph([](timedelta_t period, std::optional<bool> initial_delay,
                                              std::optional<i64> max_ticks) -> wp<TS<bool>> {
        if (period <= timedelta_t{}) {
            throw WiringError{"period must be positive"};
        }
        if (max_ticks && *max_ticks < 0) {
            throw WiringError{"max_ticks must be nonnegative"};
        }
        static const auto schedule = node<schedule_node>();
        return schedule(period, initial_delay.value_or(true),
                        max_ticks.value_or(std::numeric_limits<i64>::max()));
    })};

    // Throttle emits the latest input state at each active period boundary.
    struct throttle_node {
        static constexpr bool uses_timed_scheduling = true;

        In<TS_T, optional> value;
        Scalar<timedelta_t> period;
        Scalar<bool> delay_first_tick;
        State<bool> pending;
        State<EvaluationTime> deadline;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            return &value_type.dereferenced_type().with_guarantee_limit(TimeSeriesGuarantee::final);
        }

        void evaluate(EvaluationContext *context) const {
            const auto now = context->evaluation_time();
            const auto input_changed = value.modified();
            const auto active_period = *deadline != MIN_DT;
            const auto at_deadline = active_period && *deadline <= now;

            if (input_changed) {
                if (!active_period) {
                    if (delay_first_tick.value()) {
                        *pending = true;
                        start_period(*context, now);
                        return;
                    }
                    output.copy(value);
                    start_period(*context, now);
                    return;
                }
                *pending = true;
            }

            if (!at_deadline) {
                return;
            }

            *deadline = MIN_DT;
            if (!*pending) {
                return;
            }

            output.copy(value);
            *pending = false;
            start_period(*context, now);
        }

      private:
        void start_period(EvaluationContext &context, EvaluationTime now) const {
            *deadline = due_time(now, period.value());
            context.schedule_at(*deadline);
        }
    };

    struct throttle_graph {
        wp<TS_T> value;
        timedelta_t period;
        std::optional<bool> delay_first_tick;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            return throttle_node::resolve(OUT{}, arguments);
        }

        wp<OUT> wire() const {
            if (period <= timedelta_t{}) {
                throw WiringError{"period must be positive"};
            }
            static const auto throttle = node<throttle_node>();
            return throttle(value, period, delay_first_tick.value_or(false));
        }
    };

    overload<throttle> throttle_impl{graph<throttle_graph>()};

    struct resample_graph {
        wp<TS_T> value;
        timedelta_t period;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            return &value_type.with_guarantee_limit(TimeSeriesGuarantee::ordinary);
        }

        wp<OUT> wire() const { return wp<OUT>{operators::sample(operators::schedule(period), value)}; }
    };

    overload<resample> resample_impl{graph<resample_graph>()};
};

} // namespace

void register_timed_operators() { register_overloads<timed_overloads>(); }

} // namespace bg::operators::details
