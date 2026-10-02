#include <bg/graph/nested_graph.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <stdexcept>

namespace bg {

void NestedGraph::start(const CompiledNestedGraph &compiled, EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs) {
    start(compiled, context, inputs, {}, nullptr);
}

void NestedGraph::start(const CompiledNestedGraph &compiled, EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs, RuntimeGraphOwner owner) {
    if (owner.callbacks == nullptr || owner.callbacks->inner_scheduled == nullptr ||
        owner.callbacks->timed_schedule_changed == nullptr) {
        throw std::invalid_argument{"a runtime graph owner requires scheduling callbacks"};
    }

    start(compiled, context, inputs, {}, &owner);
}

void NestedGraph::start(const CompiledNestedGraph &compiled, EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants,
                        RuntimeGraphOwner owner) {
    if (owner.callbacks == nullptr || owner.callbacks->inner_scheduled == nullptr ||
        owner.callbacks->timed_schedule_changed == nullptr) {
        throw std::invalid_argument{"a runtime graph owner requires scheduling callbacks"};
    }

    start(compiled, context, inputs, constants, &owner);
}

void NestedGraph::start(const CompiledNestedGraph &compiled, EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants,
                        const RuntimeGraphOwner *owner) {
    if (active_ != nullptr) {
        throw std::logic_error{"the nested graph is already running"};
    }
    if (inputs.size() != compiled.input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of inputs"};
    }
    if (constants.size() != compiled.constant_input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of constant inputs"};
    }

    auto &parent = context.runtime_graph();
    active_ = parent.allocation().new_object<RuntimeGraph>(*compiled.graph, parent.allocation(), parent.global_state());
    auto rollback = util::scope_exit{[&]() noexcept {
        active_->clear_owner();
        destroy(active_);
        active_ = nullptr;
    }};
    if (owner == nullptr) {
        active_->set_owner(parent, context.node_index());
    } else {
        active_->set_owner(parent, context.node_index(), *owner);
    }
    active_->set_owner_scheduling_suppressed(true);

    update_inputs(compiled, inputs, context);
    for (auto i = std::size_t{}; i < constants.size(); ++i) {
        active_->initialize_constant_output(compiled.constant_input_nodes[i], constants[i].value,
                                            context.evaluation_time());
    }
    active_->start(context);
    active_->set_owner_scheduling_suppressed(false);
    rollback.release();
}

void NestedGraph::update_input(const CompiledNestedGraph &compiled, std::size_t input_index, NestedGraphInput input,
                               EvaluationContext &context) {
    if (input_index >= compiled.input_nodes.size()) {
        throw std::out_of_range{"the nested graph input index is out of range"};
    }

    const auto previous_suppression = active_->set_owner_scheduling_suppressed(true);
    auto restore = util::scope_exit{
        [&]() noexcept { active_->set_owner_scheduling_suppressed(previous_suppression); }};
    active_->update_reference_output(compiled.input_nodes[input_index], input.reference, context.evaluation_time());
}

void NestedGraph::update_inputs(const CompiledNestedGraph &compiled, std::span<const NestedGraphInput> inputs,
                                EvaluationContext &context) {
    if (inputs.size() != compiled.input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of inputs"};
    }

    const auto previous_suppression = active_->set_owner_scheduling_suppressed(true);
    auto restore = util::scope_exit{
        [&]() noexcept { active_->set_owner_scheduling_suppressed(previous_suppression); }};

    for (auto i = std::size_t{}; i < inputs.size(); ++i) {
        active_->update_reference_output(compiled.input_nodes[i], inputs[i].reference, context.evaluation_time());
    }
}

bool NestedGraph::evaluate(EvaluationContext &context) {
    active_->set_owner_scheduling_suppressed(true);
    auto restore = util::scope_exit{[&]() noexcept { active_->set_owner_scheduling_suppressed(false); }};
    const auto complete = active_->evaluate(context);
    if (!complete) {
        restore.release();
    }
    return complete;
}

bool NestedGraph::evaluation_pending() const noexcept {
    return active_ != nullptr && active_->evaluation_pending();
}

void NestedGraph::stop(EvaluationContext &context) {
    if (active_ == nullptr) {
        return;
    }

    context.add_cycle_callback(this, destroy_retired_callback);

    active_->set_owner_scheduling_suppressed(true);
    active_->stop(context);
    active_->set_owner_scheduling_suppressed(false);
    active_->clear_owner();
    retired_ = active_;
    active_ = nullptr;
}

Reference NestedGraph::output(const CompiledNestedGraph &compiled) const noexcept {
    if (active_ == nullptr || !compiled.output_node) {
        return nullptr;
    }

    const auto node = active_->node(*compiled.output_node);
    if (!node.output_last_updated().valid()) {
        return nullptr;
    }

    return *static_cast<Reference *>(node.output_value());
}

void NestedGraph::destroy_retired_callback(void *receiver) noexcept {
    static_cast<NestedGraph *>(receiver)->destroy_retired();
}

void NestedGraph::destroy(RuntimeGraph *graph) noexcept {
    if (graph == nullptr) {
        return;
    }

    graph->allocation().delete_object(graph);
}

void NestedGraph::destroy_retired() noexcept {
    destroy(retired_);
    retired_ = nullptr;
}

void NestedGraph::stop_and_destroy(EvaluationContext &context) noexcept {
    if (active_ != nullptr) {
        active_->set_owner_scheduling_suppressed(true);
        active_->stop(context);
        active_->clear_owner();
        destroy(active_);
        active_ = nullptr;
    }

    destroy_retired();
}

} // namespace bg
