#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/node/aggregate_node_template.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/util/buffer.hpp>
#include <bg/util/symbol.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace bg {

namespace details {

struct FeedbackBuffer {
    FeedbackBuffer(util::buffer buffer, allocator memory, const TimeSeriesType &type)
        : writer{buffer, memory}, reader{std::move(buffer), type} {}

    TimeSeriesTickWriter writer;
    TimeSeriesTickReader reader;
};

template <typename SERIES> struct FeedbackSource {
    static constexpr bool deduplicate_wiring = false;
    static constexpr bool uses_timed_scheduling = true;

    Out<SERIES> output;
    State<FeedbackBuffer *> buffer;

    void start(EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        *buffer = memory.new_object<FeedbackBuffer>(util::buffer::chain(256, memory), memory, SERIES::type());
    }

    void evaluate() const {
        if ((*buffer)->reader.has_next()) {
            (*buffer)->reader.replay(output);
        }
    }

    void stop(EvaluationContext *context) const noexcept {
        auto memory = context->runtime_graph().allocation();
        memory.delete_object(*buffer);
        *buffer = nullptr;
    }
};

template <typename SERIES> struct FeedbackSink {
    static constexpr bool deduplicate_wiring = false;
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool uses_input_node_indices = true;

    In<SERIES, optional> input;
    In<SERIES, optional, passive> feedback_output;
    State<TimeSeriesTickWriter *> writer;
    State<std::size_t> source_index;

    void start(EvaluationContext *context) const {
        const auto &compiled_node = context->runtime_graph().compiled_node(context->node_index());
        *source_index = compiled_node.input_node_index(1).value();
        auto source_node = context->runtime_graph().node(*source_index);
        auto *source_buffer = static_cast<FeedbackBuffer **>(source_node.state());
        *writer = &(*source_buffer)->writer;
    }

    void evaluate(EvaluationContext *context) const {
        if ((*writer)->write(input, context->evaluation_time())) {
            context->runtime_graph().schedule_at(*source_index,
                                                 context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

struct FeedbackSourceLabel {
    util::symbol value;

    util::symbol operator()() const { return value; }
};

struct FeedbackSinkLabel {
    util::symbol value;

    util::symbol operator()(WiringPoint, WiringPoint) const { return value; }
};

} // namespace details

template <typename SERIES> class Feedback {
  public:
    explicit Feedback(std::string_view name)
        : label_{name.empty() ? util::symbol{"feedback"} : util::symbol{name}} {
        if (SERIES::type().contains_reference()) {
            throw WiringError{"feedback does not support time series types containing references"};
        }

        static_assert([] {
            if constexpr (requires { SERIES::has_explicit_guarantee; SERIES::guarantee; }) {
                return !SERIES::has_explicit_guarantee || SERIES::guarantee != TimeSeriesGuarantee::const_;
            }
            return true;
        }(), "feedback output cannot have the const guarantee");

        auto &wiring = WiringContext::current().wiring();
        using SourceTemplate = AggregateNodeTemplate<details::FeedbackSource<SERIES>, details::FeedbackSourceLabel>;
        const auto &source_template =
            wiring.own_node_template(std::make_unique<SourceTemplate>(details::FeedbackSourceLabel{label_}));
        source_ = *source_template();
    }

    Feedback(const Feedback &) = delete;
    Feedback &operator=(const Feedback &) = delete;
    Feedback(Feedback &&) = delete;
    Feedback &operator=(Feedback &&) = delete;

    operator WiringPoint() const noexcept { return source_; }
    operator wp<SERIES>() const { return wp<SERIES>{source_}; }

    template <typename SOURCE>
        requires details::is_typed_wiring_point<SOURCE>
    Feedback &operator=(SOURCE &&source) {
        bind(std::forward<SOURCE>(source));
        return *this;
    }

    Feedback &operator=(WiringPoint source) {
        bind(source);
        return *this;
    }

  private:
    template <typename SOURCE> void bind(SOURCE &&input) {
        if (assigned_) {
            throw std::logic_error{"a feedback sink can be assigned only once"};
        }

        auto &wiring = WiringContext::current().wiring();
        if (&wiring != &source_.graph()) {
            throw WiringError{"a feedback source and sink must be wired in the same GraphWiring"};
        }

        using SinkTemplate = AggregateNodeTemplate<details::FeedbackSink<SERIES>, details::FeedbackSinkLabel>;
        const auto &sink_template =
            wiring.own_node_template(std::make_unique<SinkTemplate>(details::FeedbackSinkLabel{label_}));
        (void)sink_template(std::forward<SOURCE>(input), source_);
        assigned_ = true;
    }

    util::symbol label_;
    WiringPoint source_;
    bool assigned_{};
};

template <typename SERIES> [[nodiscard]] auto feedback(std::string_view name = {}) {
    return Feedback<SERIES>{name};
}

} // namespace bg
