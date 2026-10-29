#include "registration.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph.hpp>
#include <bg/library/key_value.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/set/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/time_series_state.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bg::operators::details {
namespace {

inline constexpr auto INPUT_G = guarantee_variable<"EMIT_INPUT_G">;
inline constexpr auto ITEM_G = guarantee_variable<"EMIT_ITEM_G">;
using KEY = KEY_VALUE_KEY;
using VALUE = KEY_VALUE_VALUE;

template <typename Item> struct emission_queue {
    explicit emission_queue(allocator &memory) : values{&memory.memory_resource()} {}

    template <typename... Arguments> void append(Arguments &&...arguments) {
        values.emplace_back(std::forward<Arguments>(arguments)...);
    }

    template <typename Publish> void publish(EvaluationContext &context, Publish &&publish_value) {
        if (values.empty()) {
            return;
        }

        const auto now = context.evaluation_time();
        std::forward<Publish>(publish_value)(values.front());
        values.pop_front();
        if (values.empty()) {
            return;
        }

        context.schedule_at(due_time(now));
    }

  private:
    static EvaluationTime due_time(EvaluationTime now) {
        const auto ticks = now.time_since_epoch().count();
        if (ticks == std::numeric_limits<std::int64_t>::max()) {
            throw std::overflow_error{"emit evaluation time is outside the supported range"};
        }

        const auto due = now + EvaluationTime::duration{1};
        if (!TimeSeriesState::can_represent(due)) {
            throw std::overflow_error{"emit evaluation time is outside the supported range"};
        }
        return due;
    }

    std::pmr::deque<Item> values;
};

using scalar_queue = emission_queue<ScalarValue<>>;

struct dictionary_value {
    dictionary_value(const Scalar<KEY> &key, const Scalar<VALUE> &value, allocator &memory)
        : key{key, memory}, value{value, memory} {}

    ScalarValue<> key;
    ScalarValue<> value;
};

using dictionary_queue = emission_queue<dictionary_value>;

struct emit_overloads {
    static const TimeSeriesType *requested_output(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(::bg::details::type_parameter<OUT>());
        const auto *requested = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        return requested == nullptr || *requested == nullptr ? nullptr : type_cast<TimeSeriesType>(*requested);
    }

    static RequirementResult require_output(const WiringArguments &arguments) {
        const auto *requested = requested_output(arguments);
        const auto *resolved = arguments.resolved_output_type();
        if (requested != nullptr && !requested->accepts(*resolved)) {
            return reject("emit requires an output type compatible with the emitted values");
        }
        if (requested != nullptr && requested->guarantee().value() &&
            *requested->guarantee().value() != TimeSeriesGuarantee::ordinary) {
            return reject("emit requires an ordinary output guarantee");
        }
        return accept;
    }

    template <typename Queue, typename... Arguments>
    static void append(Queue *&queue, allocator &memory, Arguments &&...arguments) {
        if (queue == nullptr) {
            queue = memory.new_object<Queue>(memory);
        }
        queue->append(std::forward<Arguments>(arguments)...);
    }

    template <typename Queue> static void release(Queue *&queue, EvaluationContext &context) noexcept {
        if (queue == nullptr) {
            return;
        }
        auto memory = context.runtime_graph().allocation();
        memory.delete_object(queue);
        queue = nullptr;
    }

    // Scalar collection values emit every item in their current value.
    struct list_to_value {
        static constexpr bool uses_timed_scheduling = true;

        In<TS<List<VALUE>, INPUT_G>, optional> value;
        State<scalar_queue *> pending;
        Out<TS<VALUE>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return emit_overloads::require_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            if (value.valid() && value.modified()) {
                const auto values = value.value_view();
                for (auto index = std::size_t{}; index < values.size(); ++index) {
                    emit_overloads::append(*pending, memory, values[index], memory);
                }
            }
            if (*pending != nullptr) {
                (*pending)->publish(*context, [&](ScalarValue<> &item) { output.set_value(std::move(item)); });
            }
        }

        void stop(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
        void finalize(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
    };

    overload<emit, list_to_value> emit_list_to_value;

    struct set_to_value {
        static constexpr bool uses_timed_scheduling = true;

        In<TS<Set<VALUE>, INPUT_G>, optional> value;
        State<scalar_queue *> pending;
        Out<TS<VALUE>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return emit_overloads::require_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            if (value.valid() && value.modified()) {
                value.value_view().for_each([&](const auto &item) {
                    emit_overloads::append(*pending, memory, item, memory);
                });
            }
            if (*pending != nullptr) {
                (*pending)->publish(*context, [&](ScalarValue<> &item) { output.set_value(std::move(item)); });
            }
        }

        void stop(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
        void finalize(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
    };

    overload<emit, set_to_value> emit_set_to_value;

    struct dict_to_bundle {
        static constexpr bool uses_timed_scheduling = true;

        In<TS<Dict<KEY, VALUE>, INPUT_G>, optional> value;
        State<dictionary_queue *> pending;
        Out<TSB<key_value>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return emit_overloads::require_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            if (value.valid() && value.modified()) {
                value.value_view().for_each([&](const auto &key, const auto &item) {
                    emit_overloads::append(*pending, memory, key, item, memory);
                });
            }
            if (*pending != nullptr) {
                (*pending)->publish(*context, [&](dictionary_value &item) {
                    output->key.set_value(std::move(item.key));
                    output->value.set_value(std::move(item.value));
                });
            }
        }

        void stop(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
        void finalize(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
    };

    overload<emit, dict_to_bundle> emit_dict_to_bundle;

    struct set_to_value_changes {
        static constexpr bool uses_timed_scheduling = true;

        In<TSS<VALUE, INPUT_G>, optional> value;
        State<scalar_queue *> pending;
        Out<TS<VALUE>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return emit_overloads::require_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            if (value.valid() && value.modified()) {
                value.for_each_added([&](const auto &item) {
                    emit_overloads::append(*pending, memory, item, memory);
                });
            }
            if (*pending != nullptr) {
                (*pending)->publish(*context, [&](ScalarValue<> &item) { output.set_value(std::move(item)); });
            }
        }

        void stop(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
        void finalize(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
    };

    overload<emit, set_to_value_changes> emit_set_to_value_changes;

    struct fixed_list_to_value_changes {
        static constexpr bool uses_timed_scheduling = true;

        In<TSL<TS<VALUE, ITEM_G>, SIZE, INPUT_G>, optional> value;
        State<scalar_queue *> pending;
        Out<TS<VALUE>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return emit_overloads::require_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            if (value.valid() && value.modified()) {
                value.for_each_updated([&](item_id_t index) {
                    const auto item = value[index];
                    if (item.valid()) {
                        emit_overloads::append(*pending, memory, item.value_view(), memory);
                    }
                });
            }
            if (*pending != nullptr) {
                (*pending)->publish(*context, [&](ScalarValue<> &item) { output.set_value(std::move(item)); });
            }
        }

        void stop(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
        void finalize(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
    };

    overload<emit, fixed_list_to_value_changes> emit_fixed_list_to_value_changes;

    struct dict_to_bundle_changes {
        static constexpr bool uses_timed_scheduling = true;

        In<TSD<KEY, TS<VALUE, ITEM_G>, INPUT_G>, optional> value;
        State<dictionary_queue *> pending;
        Out<TSB<key_value>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return emit_overloads::require_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            auto memory = context->runtime_graph().allocation();
            if (value.valid() && value.modified()) {
                const auto append_item = [&](const auto &key) {
                    const auto item = value[key];
                    if (item.valid()) {
                        emit_overloads::append(*pending, memory, key, item.value_view(), memory);
                    }
                };
                value.for_each_added(append_item);
                value.for_each_updated(append_item);
            }
            if (*pending != nullptr) {
                (*pending)->publish(*context, [&](dictionary_value &item) {
                    output->key.set_value(std::move(item.key));
                    output->value.set_value(std::move(item.value));
                });
            }
        }

        void stop(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
        void finalize(EvaluationContext *context) const noexcept { emit_overloads::release(*pending, *context); }
    };

    overload<emit, dict_to_bundle_changes> emit_dict_to_bundle_changes;
};

} // namespace

void register_emit_operators() { register_overloads<emit_overloads>(); }

} // namespace bg::operators::details
