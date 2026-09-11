#include <bg/graph/nested_graph.hpp>

#include <bg/allocator.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/util/scope_exit.hpp>

#include <memory>
#include <stdexcept>

namespace bg {

void NestedGraph::start(const CompiledNestedGraph &compiled, EvaluationContext &context,
                        std::span<const NestedGraphInput> inputs) {
    if (active_ != nullptr) {
        throw std::logic_error{"the nested graph is already running"};
    }

    auto &parent = context.runtime_graph();
    active_ = allocator{*parent.memory_}.new_object<RuntimeGraph>(*compiled.graph, *parent.memory_,
                                                                   parent.global_state());
    active_->parent_ = &parent;
    active_->owner_node_index_ = context.node_index();
    active_->suppress_parent_scheduling_ = true;
    active_->evaluation_time_ = context.evaluation_time();

    update_inputs(compiled, inputs);
    active_->start(context.evaluation_time(), *parent.scratch_);
    active_->suppress_parent_scheduling_ = false;
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
    active_->propagate_output_change(node, change);
}

void NestedGraph::evaluate(EvaluationContext &context) {
    active_->suppress_parent_scheduling_ = true;
    auto restore = util::scope_exit{[&]() noexcept { active_->suppress_parent_scheduling_ = false; }};
    active_->evaluate(context.evaluation_time(), *context.runtime_graph().scratch_);
}

void NestedGraph::stop(EvaluationContext &context) {
    if (active_ == nullptr) {
        return;
    }

    active_->suppress_parent_scheduling_ = true;
    active_->stop(*context.runtime_graph().scratch_);
    active_->suppress_parent_scheduling_ = false;
    active_->parent_ = nullptr;
    retired_ = active_;
    active_ = nullptr;
    if (!context.runtime_graph().stopped_) {
        context.runtime_graph().add_cycle_callback(this, destroy_retired_callback);
    }
}

Reference NestedGraph::output(const CompiledNestedGraph &compiled) const noexcept {
    if (active_ == nullptr) {
        return nullptr;
    }

    const auto node = active_->node(compiled.output_node);
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

    allocator{*graph->memory_}.delete_object(graph);
}

void NestedGraph::destroy_retired() noexcept {
    destroy(retired_);
    retired_ = nullptr;
}

void NestedGraph::stop_and_destroy(EvaluationContext &context) noexcept {
    if (active_ != nullptr) {
        active_->suppress_parent_scheduling_ = true;
        active_->stop(*context.runtime_graph().scratch_);
        destroy(active_);
        active_ = nullptr;
    }

    destroy_retired();
}

} // namespace bg
