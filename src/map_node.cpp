#include <bg/map/details/map_node.hpp>

#include <bg/allocator.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/map/details/map_node_definition.hpp>
#include <bg/map/details/map_runtime.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>

#include <optional>
#include <utility>

namespace bg::details {
namespace {

class MapRuntimeState;

struct MapNodeState {
    MapRuntimeState *runtime{};
};

using MapInvocation = NodeInvocation<CompiledMapGraph, MapNodeState>;

class MapRuntimeState {
    using MapOutput = std::optional<Out<TSD<SCALAR, REF<TS_T>>>>;

  public:
    MapRuntimeState(allocator memory, EvaluationContext &context) : branches_{memory} {
        branches_.entries().bind_evaluator(context);
    }

    MapRuntimeState(const MapRuntimeState &) = delete;
    MapRuntimeState &operator=(const MapRuntimeState &) = delete;

    bool evaluate(const CompiledMapGraph &compiled, MapInvocation invocation) {
        auto output = MapOutput{};
        if (paused_branch_) {
            if (!evaluate_entry(invocation.context(), *paused_branch_)) {
                return false;
            }
            update_entry_output(compiled, invocation, *paused_branch_, output);
            paused_branch_.reset();
            return evaluate_ready(compiled, invocation, output);
        }

        branches_.update(compiled, invocation, [&](MapEntries::Entry &entry) {
            const auto should_run = entry.presence_count != 0;
            if (should_run == entry.inner_graph.running()) {
                return;
            }

            if (should_run) {
                branches_.start(entry, compiled, invocation);
                return;
            }

            branches_.stop(entry, invocation.context());
            if (compiled.nested.output_node) {
                auto output = invocation.template output<TSD<SCALAR, REF<TS_T>>>();
                const auto key = branches_.entries().key(entry);
                output.erase(Scalar<SCALAR>::from(key));
            }
        });

        return evaluate_ready(compiled, invocation, output);
    }

    bool evaluate_ready(const CompiledMapGraph &compiled, MapInvocation invocation, MapOutput &output) {
        auto &entries = branches_.entries();
        auto &ready = entries.ready();
        for (auto ready_entry = ready.begin(); ready_entry != ready.end();) {
            const auto branch_id = *ready_entry;
            ++ready_entry;
            ready.reset(branch_id);

            auto &entry = entries.table().value_at(branch_id);
            if (!entry.inner_graph.running()) {
                continue;
            }

            if (!evaluate_entry(invocation.context(), branch_id)) {
                paused_branch_ = branch_id;
                return false;
            }
            update_entry_output(compiled, invocation, branch_id, output);
        }
        return true;
    }

    void stop(EvaluationContext &context) noexcept {
        branches_.stop_all(context);
        paused_branch_.reset();
    }

    void destroy(EvaluationContext &context) noexcept {
        branches_.destroy_all(context);

        auto memory = context.runtime_graph().allocation();
        memory.delete_object(this);
    }

  private:
    bool evaluate_entry(EvaluationContext &context, branch_id_t branch_id) {
        auto &entries = branches_.entries();
        auto &entry = entries.table().value_at(branch_id);
        return entry.inner_graph.evaluate(context);
    }

    void update_entry_output(const CompiledMapGraph &compiled, MapInvocation invocation, branch_id_t branch_id,
                             MapOutput &output) {
        if (!compiled.nested.output_node) {
            return;
        }
        if (!output) {
            output.emplace(invocation.template output<TSD<SCALAR, REF<TS_T>>>());
        }

        auto &entries = branches_.entries();
        auto &entry = entries.table().value_at(branch_id);
        const auto key = entries.key(entry);
        auto item = (*output)[Scalar<SCALAR>::from(key)];
        const auto result = entry.inner_graph.output(compiled.nested);
        if (result) {
            item.set_value(result);
        } else {
            item.invalidate();
        }
    }

    MapRuntime branches_;
    std::optional<branch_id_t> paused_branch_;
};

class DefaultMapNodeDefinition : public MapNodeDefinition {
  public:
    using state_type = MapNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    using MapNodeDefinition::MapNodeDefinition;

    static bool evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        if (state.runtime == nullptr) {
            auto memory = invocation.context().runtime_graph().allocation();
            state.runtime = memory.new_object<MapRuntimeState>(memory, invocation.context());
        }
        return state.runtime->evaluate(invocation.compiled_data(), invocation);
    }

    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->stop(invocation.context());
        }
    }

    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->destroy(invocation.context());
            invocation.state().runtime = nullptr;
        }
    }
};

} // namespace

std::unique_ptr<NodeTemplate> make_map_node(MapGraphWiring wiring, const ResolvedSignature &outer_signature) {
    return std::make_unique<RuntimeSignatureNodeTemplate<DefaultMapNodeDefinition>>(
        DefaultMapNodeDefinition{std::move(wiring), outer_signature});
}

} // namespace bg::details
