#include <bg/graph/nested_graph.hpp>

#include <bg/allocator.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <limits>
#include <memory>
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
                        std::span<const NestedGraphInput> inputs,
                        std::span<const NestedGraphConstantInput> constants, RuntimeGraphOwner owner) {
    if (owner.callbacks == nullptr || owner.callbacks->inner_scheduled == nullptr ||
        owner.callbacks->timed_schedule_changed == nullptr) {
        throw std::invalid_argument{"a runtime graph owner requires scheduling callbacks"};
    }

    start(compiled, context, inputs, constants, &owner);
}

void NestedGraph::start(const CompiledNestedGraph &compiled, EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs,
                        std::span<const NestedGraphConstantInput> constants, const RuntimeGraphOwner *owner) {
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
    active_ = parent.allocator_.new_object<RuntimeGraph>(*compiled.graph, parent.allocator_, parent.global_state());
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
    active_->suppress_parent_scheduling_ = true;
    active_->evaluation_time_ = context.evaluation_time();

    update_inputs(compiled, inputs);
    for (auto i = std::size_t{}; i < constants.size(); ++i) {
        initialize_constant(compiled.constant_input_nodes[i], constants[i], context.evaluation_time());
    }
    active_->start(context);
    active_->suppress_parent_scheduling_ = false;
    rollback.release();
}

void NestedGraph::update_input(const CompiledNestedGraph &compiled, std::size_t input_index,
                               NestedGraphInput input) {
    if (input_index >= compiled.input_nodes.size()) {
        throw std::out_of_range{"the nested graph input index is out of range"};
    }

    const auto previous_suppression = active_->suppress_parent_scheduling_;
    active_->suppress_parent_scheduling_ = true;
    auto restore = util::scope_exit{[&]() noexcept { active_->suppress_parent_scheduling_ = previous_suppression; }};
    bind_input(compiled.input_nodes[input_index], input);
}

void NestedGraph::update_inputs(const CompiledNestedGraph &compiled, std::span<const NestedGraphInput> inputs) {
    if (inputs.size() != compiled.input_nodes.size()) {
        throw std::invalid_argument{"the nested graph received the wrong number of inputs"};
    }

    const auto previous_suppression = active_->suppress_parent_scheduling_;
    active_->suppress_parent_scheduling_ = true;
    auto restore = util::scope_exit{[&]() noexcept { active_->suppress_parent_scheduling_ = previous_suppression; }};

    for (auto i = std::size_t{}; i < inputs.size(); ++i) {
        bind_input(compiled.input_nodes[i], inputs[i]);
    }
}

void NestedGraph::bind_input(std::size_t node_index, const NestedGraphInput &input) {
    const auto &node = active_->compiled_graph_->node(node_index);
    const auto runtime_node = active_->node(node_index);
    auto &last_updated = runtime_node.output_last_updated();
    auto &bound_reference = *static_cast<Reference *>(runtime_node.output_value());

    if (bound_reference == input.reference) {
        return;
    }

    const auto was_valid = last_updated > MIN_DT;
    bound_reference = input.reference;
    last_updated = input.reference ? input.last_updated : MIN_DT;
    const auto now_valid = last_updated > MIN_DT;
    const auto change =
        now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation) : OutputChange::invalidation;
    active_->publish_output(node, change);
}

void NestedGraph::initialize_constant(std::size_t node_index, const NestedGraphConstantInput &input,
                                      EvaluationTime start_time) {
    const auto &node = active_->compiled_graph_->node(node_index);
    const auto runtime_node = active_->node(node_index);
    const auto &runtime_type =
        static_cast<const TimeSeriesValueRuntimeType &>(node.node_invocation_output_runtime_type());
    runtime_type.scalar_runtime_type().assign_from_input(runtime_node.output_value(), input.value);
    runtime_node.output_last_updated() = start_time;
    active_->publish_output(node, OutputChange::revalidation);
}

bool NestedGraph::evaluate(EvaluationContext &context) {
    active_->suppress_parent_scheduling_ = true;
    auto restore = util::scope_exit{[&]() noexcept { active_->suppress_parent_scheduling_ = false; }};
    const auto complete = active_->evaluate(context);
    if (!complete) {
        restore.release();
    }
    return complete;
}

bool NestedGraph::evaluation_pending() const noexcept {
    return active_ != nullptr && active_->paused_node_index_ != std::numeric_limits<std::uint32_t>::max();
}

void NestedGraph::stop(EvaluationContext &context) {
    if (active_ == nullptr) {
        return;
    }

    context.add_cycle_callback(this, destroy_retired_callback);

    active_->suppress_parent_scheduling_ = true;
    active_->stop(context);
    active_->suppress_parent_scheduling_ = false;
    active_->clear_owner();
    retired_ = active_;
    active_ = nullptr;
}

Reference NestedGraph::output(const CompiledNestedGraph &compiled) const noexcept {
    if (active_ == nullptr || !compiled.output_node) {
        return nullptr;
    }

    const auto node = active_->node(*compiled.output_node);
    if (node.output_last_updated() <= MIN_DT) {
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

    graph->allocator_.delete_object(graph);
}

void NestedGraph::destroy_retired() noexcept {
    destroy(retired_);
    retired_ = nullptr;
}

void NestedGraph::stop_and_destroy(EvaluationContext &context) noexcept {
    if (active_ != nullptr) {
        active_->suppress_parent_scheduling_ = true;
        active_->stop(context);
        active_->clear_owner();
        destroy(active_);
        active_ = nullptr;
    }

    destroy_retired();
}

} // namespace bg
