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
#include <bg/type/time_series_variable.hpp>
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

struct FeedbackSourceLabel {
    util::symbol value;

    util::symbol operator()() const { return value; }
};

struct FeedbackSinkLabel {
    util::symbol value;

    util::symbol operator()(WiringPoint, WiringPoint) const { return value; }
};

struct FeedbackTickStream {
    FeedbackTickStream(util::buffer buffer, allocator memory, const TimeSeriesType &type)
        : writer{buffer, memory}, reader{std::move(buffer), type} {}

    TimeSeriesTickWriter writer;
    TimeSeriesTickReader reader;
};

using FEEDBACK_INPUT = TimeSeriesVariable<"FEEDBACK_INPUT">;

template <typename SERIES> struct FeedbackSource {
    static constexpr bool deduplicate_wiring = false;
    static constexpr bool uses_timed_scheduling = true;

    Out<SERIES> output;
    State<FeedbackTickStream *> stream;

    void start(EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        auto storage = util::buffer::chain(256, memory);
        *stream = memory.new_object<FeedbackTickStream>(std::move(storage), memory, output.runtime_type().type());
    }

    void evaluate() const {
        if ((*stream)->reader.has_next()) {
            (*stream)->reader.replay(output);
        }
    }

    void stop(EvaluationContext *context) const noexcept {
        auto memory = context->runtime_graph().allocation();
        memory.delete_object(*stream);
        *stream = nullptr;
    }
};

template <typename SERIES> struct FeedbackSink {
    static constexpr bool deduplicate_wiring = false;
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool uses_input_node_indices = true;

    In<FEEDBACK_INPUT, optional> input;
    In<SERIES, optional, passive> feedback_output;
    State<TimeSeriesTickWriter *> writer;
    State<std::size_t> source_index;

    static RequirementResult require(const WiringArguments &arguments) {
        return arguments.resolved_type(1).accepts(arguments.resolved_type(0))
                   ? accept
                   : reject("feedback input does not satisfy its output type");
    }

    void start(EvaluationContext *context) const {
        const auto &compiled_node = context->runtime_graph().compiled_node(context->node_index());
        *source_index = compiled_node.input_node_index(1).value();
        auto source_node = context->runtime_graph().node(*source_index);
        auto *source_stream = static_cast<FeedbackTickStream **>(source_node.state());
        *writer = &(*source_stream)->writer;
    }

    void evaluate(EvaluationContext *context) const {
        if ((*writer)->write(input, context->evaluation_time())) {
            context->schedule_at(*source_index, context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
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

        initialize_source();
    }

    explicit Feedback(const TimeSeriesType &series_type, std::string_view name)
        requires details::time_series_variable<SERIES>
        : label_{name.empty() ? util::symbol{"feedback"} : util::symbol{name}} {
        if (!series_type.is_resolved()) {
            throw WiringError{"feedback requires a resolved time series type"};
        }
        if (series_type.contains_reference()) {
            throw WiringError{"feedback does not support time series types containing references"};
        }
        if (series_type.guarantee().value() == TimeSeriesGuarantee::const_) {
            throw WiringError{"feedback output cannot have the const guarantee"};
        }

        type_map_.set(details::TimeSeriesVariableInstance<SERIES>::get(), &series_type);
        initialize_source();
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
    void initialize_source() {
        auto &wiring = WiringContext::current().wiring();
        using SourceTemplate = AggregateNodeTemplate<details::FeedbackSource<SERIES>, details::FeedbackSourceLabel>;
        const auto &source_template =
            wiring.own_node_template(std::make_unique<SourceTemplate>(details::FeedbackSourceLabel{label_}));
        source_ = *source_template(type_map_);
    }

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
        (void)sink_template(type_map_, std::forward<SOURCE>(input), source_);
        assigned_ = true;
    }

    util::symbol label_;
    TypeMap type_map_;
    WiringPoint source_;
    bool assigned_{};
};

template <typename SERIES> [[nodiscard]] auto feedback(std::string_view name = {}) {
    return Feedback<SERIES>{name};
}

[[nodiscard]] inline auto feedback(const TimeSeriesType &series_type, std::string_view name = {}) {
    return Feedback<TS_T>{series_type, name};
}

[[nodiscard]] inline WiringPoint feedback(WiringPoint input, const TimeSeriesType &output_type,
                                          std::string_view name = {}) {
    auto delayed = Feedback<TS_T>{output_type, name};
    delayed = input;
    return static_cast<WiringPoint>(delayed);
}

[[nodiscard]] inline WiringPoint feedback(WiringPoint input, std::string_view name = {}) {
    const auto &value_type = input.type().reference_free_type();
    const auto &output_type = value_type.guarantee().value() == TimeSeriesGuarantee::const_
                                  ? value_type.with_guarantee(TimeSeriesGuarantee::final)
                                  : value_type;
    return feedback(input, output_type, name);
}

} // namespace bg
