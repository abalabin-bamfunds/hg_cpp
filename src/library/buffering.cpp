#include "registration.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/util/buffer.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace bg::operators::details {
namespace {

struct buffering_overloads {
    struct retained_ticks {
        retained_ticks(util::buffer buffer, allocator memory, const TimeSeriesType &type)
            : writer{buffer, memory}, reader{std::move(buffer), type} {}

        TimeSeriesTickWriter writer;
        TimeSeriesTickReader reader;
    };

    struct gate_impl {
        static constexpr bool uses_timed_scheduling = true;

        In<TS<bool>, optional> condition;
        In<TS_T, optional> value;
        Scalar<i64> buffer_length;
        In<SIGNAL, optional> reset;
        State<retained_ticks *> retained;
        State<i64> pending_count;
        State<bool> was_open;
        State<ScheduleHandle> drain_timer;
        Out<OUT> output;

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            return &value_type.dereferenced_type().with_guarantee_limit(TimeSeriesGuarantee::final);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto buffer_length = scalar_cast<i64>(*arguments.scalar("buffer_length")).value();
            return buffer_length <= 0 ? reject("gate buffer_length must be positive") : accept;
        }

        void evaluate(EvaluationContext *context) const {
            const auto now = context->evaluation_time();
            const auto has_current_tick = value.modified() && value.last_updated() == now;
            if (reset.modified() && reset.valid()) {
                release(*context);
            }

            const auto open = condition.valid() && *condition;
            if (!open) {
                close(*context);
                if (has_current_tick) {
                    enqueue(*context, now);
                }
                return;
            }

            const auto opening = !*was_open;
            *was_open = true;
            const auto timer_fired = *drain_timer && context->timer_fired(*drain_timer);
            if (timer_fired) {
                *drain_timer = {};
            }

            const auto published = *pending_count > 0 && (opening || timer_fired);
            if (published) {
                replay_one();
            }

            if (has_current_tick) {
                if (!published && *pending_count == 0) {
                    output.copy_tick(value);
                } else {
                    enqueue(*context, now);
                }
            }

            if (*pending_count > 0) {
                schedule_drain(*context, now);
            } else {
                cancel_drain(*context);
            }
        }

        void stop(EvaluationContext *context) const noexcept { release(*context); }
        void finalize(EvaluationContext *context) const noexcept { release(*context); }

      private:
        void close(EvaluationContext &context) const noexcept {
            *was_open = false;
            cancel_drain(context);
        }

        void enqueue(EvaluationContext &context, EvaluationTime now) const {
            if (*pending_count >= buffer_length.value()) {
                throw std::runtime_error{"gate buffer overflow"};
            }
            if (*retained == nullptr) {
                auto memory = context.runtime_graph().allocation();
                auto buffer = util::buffer::chain(256, memory);
                *retained = memory.new_object<retained_ticks>(std::move(buffer), memory, value.runtime_type().type());
            }

            auto &ticks = **retained;
            if (ticks.writer.write(value, now)) {
                ++*pending_count;
            }
        }

        void replay_one() const {
            (*retained)->reader.replay(output);
            --*pending_count;
        }

        void schedule_drain(EvaluationContext &context, EvaluationTime now) const {
            if (*drain_timer) {
                return;
            }
            *drain_timer = context.schedule_at(now + timedelta_t{1});
        }

        void cancel_drain(EvaluationContext &context) const noexcept {
            if (!*drain_timer) {
                return;
            }
            context.cancel(*drain_timer);
            *drain_timer = {};
        }

        void release(EvaluationContext &context) const noexcept {
            cancel_drain(context);
            if (*retained == nullptr) {
                *pending_count = 0;
                return;
            }
            auto memory = context.runtime_graph().allocation();
            memory.delete_object(*retained);
            *retained = nullptr;
            *pending_count = 0;
        }
    };

    overload<gate> gate_without_reset_overload{graph([](wp<TS<bool>> condition, wp<TS_T> value, i64 buffer_length) {
        static const auto gate = node<gate_impl>();
        return gate(condition, value, buffer_length, nothing[type<TS<bool>>]());
    })};
    overload<gate, gate_impl> gate_with_reset_overload;

    struct batch_impl {
        static constexpr bool uses_timed_scheduling = true;

        In<TS<bool>, optional> condition;
        In<TS<SCALAR>, optional> value;
        Scalar<timedelta_t> delay;
        Scalar<i64> buffer_length;
        State<List<SCALAR>> queued;
        State<bool> was_open;
        State<ScheduleHandle> flush_timer;
        Out<TS<List<SCALAR>>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            const auto delay = scalar_cast<timedelta_t>(*arguments.scalar("delay")).value();
            if (delay <= timedelta_t{}) {
                return reject("batch delay must be positive");
            }

            const auto buffer_length = scalar_cast<i64>(*arguments.scalar("buffer_length")).value();
            return buffer_length <= 0 ? reject("batch buffer_length must be positive") : accept;
        }

        void evaluate(EvaluationContext *context) const {
            const auto now = context->evaluation_time();
            const auto has_current_value = value.valid() && value.modified() && value.last_updated() == now;
            const auto open = condition.valid() && *condition;
            if (!open) {
                *was_open = false;
                cancel_flush(*context);
                if (has_current_value) {
                    append();
                }
                return;
            }

            const auto opening = !*was_open;
            *was_open = true;
            const auto timer_fired = *flush_timer && context->timer_fired(*flush_timer);
            if (timer_fired) {
                *flush_timer = {};
            }

            if (has_current_value) {
                append();
            }

            if (queued.empty()) {
                return;
            }

            if (opening || timer_fired) {
                flush();
                return;
            }

            if (has_current_value && !*flush_timer) {
                *flush_timer = context->schedule_at(now + delay.value());
            }
        }

      private:
        void append() const {
            if (queued.size() >= static_cast<std::size_t>(buffer_length.value())) {
                throw std::runtime_error{"batch buffer overflow"};
            }
            queued.append(value.value_view());
        }

        void flush() const {
            output.set_value(queued.value());
            queued.clear();
        }

        void cancel_flush(EvaluationContext &context) const noexcept {
            if (!*flush_timer) {
                return;
            }
            context.cancel(*flush_timer);
            *flush_timer = {};
        }
    };

    overload<batch, batch_impl> batch_impl;
};

} // namespace

void register_buffering_operators() { register_overloads<buffering_overloads>(); }

} // namespace bg::operators::details
