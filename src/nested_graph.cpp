#include <bg/graph/nested_graph.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/util/scope_exit.hpp>

#include <stdexcept>

namespace bg {

void NestedGraph::start(const CompiledNestedGraph &compiled, RuntimeGraphEvaluator &evaluator,
                        EvaluationContext &context, std::span<const NestedGraphInput> inputs,
                        std::size_t child_token, std::optional<Scalar<>> label) {
    start(compiled, evaluator, context, inputs, {}, child_token, std::move(label));
}

void NestedGraph::start(const CompiledNestedGraph &compiled, RuntimeGraphEvaluator &evaluator,
                        EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants,
                        std::size_t child_token, std::optional<Scalar<>> label) {
    if (graph_ != nullptr && graph_->running()) {
        throw std::logic_error{"the nested graph is already running"};
    }
    if (inputs.size() != compiled.input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of inputs"};
    }
    if (constants.size() != compiled.constant_input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of constant inputs"};
    }

    if (graph_ != nullptr) {
        retired_ = graph_;
        graph_ = nullptr;
    }

    auto &parent = context.runtime_graph();
    graph_ = parent.allocation().new_object<RuntimeGraph>(*compiled.graph, parent.allocation(), evaluator,
                                                          child_token);
    auto rollback = util::scope_exit{[&]() noexcept {
        destroy(graph_, context);
        graph_ = nullptr;
    }};
    graph_->set_owner_scheduling_suppressed(true);

    update_inputs(compiled, inputs, context);
    for (auto i = std::size_t{}; i < constants.size(); ++i) {
        graph_->initialize_constant_output(compiled.constant_input_nodes[i], constants[i].value,
                                           context.evaluation_time());
    }
    graph_->start(context, nullptr, std::move(label));
    graph_->set_owner_scheduling_suppressed(false);
    rollback.release();
}

void NestedGraph::update_input(const CompiledNestedGraph &compiled, std::size_t input_index, NestedGraphInput input,
                               EvaluationContext &context) {
    if (input_index >= compiled.input_nodes.size()) {
        throw std::out_of_range{"the nested graph input index is out of range"};
    }

    const auto previous_suppression = graph_->set_owner_scheduling_suppressed(true);
    auto restore = util::scope_exit{
        [&]() noexcept { graph_->set_owner_scheduling_suppressed(previous_suppression); }};
    graph_->update_reference_output(compiled.input_nodes[input_index], input.reference, context.evaluation_time());
}

void NestedGraph::update_inputs(const CompiledNestedGraph &compiled, std::span<const NestedGraphInput> inputs,
                                EvaluationContext &context) {
    if (inputs.size() != compiled.input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of inputs"};
    }

    const auto previous_suppression = graph_->set_owner_scheduling_suppressed(true);
    auto restore = util::scope_exit{
        [&]() noexcept { graph_->set_owner_scheduling_suppressed(previous_suppression); }};

    for (auto i = std::size_t{}; i < inputs.size(); ++i) {
        graph_->update_reference_output(compiled.input_nodes[i], inputs[i].reference, context.evaluation_time());
    }
}

bool NestedGraph::evaluate(EvaluationContext &context) {
    graph_->set_owner_scheduling_suppressed(true);
    auto restore = util::scope_exit{[&]() noexcept { graph_->set_owner_scheduling_suppressed(false); }};
    const auto complete = graph_->evaluate(context);
    if (!complete) {
        restore.release();
    }
    return complete;
}

bool NestedGraph::evaluation_pending() const noexcept {
    return running() && graph_->evaluation_pending();
}

void NestedGraph::stop(EvaluationContext &context) {
    if (!running()) {
        return;
    }

    context.add_cycle_callback(this, destroy_stopped_callback);

    graph_->set_owner_scheduling_suppressed(true);
    graph_->stop(context);
    graph_->set_owner_scheduling_suppressed(false);
}

Reference NestedGraph::output(const CompiledNestedGraph &compiled) const noexcept {
    if (!running() || !compiled.output_node) {
        return nullptr;
    }

    const auto node = graph_->node(*compiled.output_node);
    if (!node.output_last_updated().valid()) {
        return nullptr;
    }

    return *static_cast<Reference *>(node.output_value());
}

void NestedGraph::destroy_stopped_callback(void *receiver, EvaluationContext &context) noexcept {
    static_cast<NestedGraph *>(receiver)->destroy_stopped(context);
}

void NestedGraph::destroy(RuntimeGraph *graph, EvaluationContext &context) noexcept {
    if (graph == nullptr) {
        return;
    }

    graph->finalize(context);
    context.runtime_graph().allocation().delete_object(graph);
}

void NestedGraph::destroy_stopped(EvaluationContext &context) noexcept {
    if (graph_ != nullptr && !graph_->running()) {
        destroy(graph_, context);
        graph_ = nullptr;
    }

    destroy(retired_, context);
    retired_ = nullptr;
}

void NestedGraph::stop_and_destroy(EvaluationContext &context) noexcept {
    if (graph_ != nullptr) {
        if (graph_->running()) {
            graph_->set_owner_scheduling_suppressed(true);
            graph_->stop(context);
        }
        destroy(graph_, context);
        graph_ = nullptr;
    }

    destroy_stopped(context);
}

} // namespace bg
