#include <bg/map/details/map_node.hpp>

#include <bg/allocator.hpp>
#include <bg/details/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/map/details/map_node_definition.hpp>
#include <bg/map/details/map_runtime.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>

#include <utility>

namespace bg::details {
namespace {

class MapRuntimeState {
  public:
    explicit MapRuntimeState(allocator memory) : branches_{memory} {}

    MapRuntimeState(const MapRuntimeState &) = delete;
    MapRuntimeState &operator=(const MapRuntimeState &) = delete;

    template <typename Invocation> void evaluate(const CompiledMapGraph &compiled, Invocation invocation) {
        if (paused_entry_) {
            if (!resume_entry(compiled, invocation, *paused_entry_)) {
                return;
            }
            paused_entry_.reset();
            evaluate_ready(compiled, invocation);
            return;
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
                auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
                const auto key = branches_.entries().key(entry);
                output.erase(Scalar<SCALAR>::from(key));
            }
        });

        evaluate_ready(compiled, invocation);
    }

    template <typename Invocation> void evaluate_ready(const CompiledMapGraph &compiled, Invocation invocation) {
        auto &entries = branches_.entries();
        auto &ready = entries.ready();
        for (auto ready_entry = ready.begin(); ready_entry != ready.end();) {
            const auto entry_index = *ready_entry;
            ++ready_entry;
            ready.reset(entry_index);

            auto &entry = entries.table().value_at(entry_index);
            if (!entry.inner_graph.running()) {
                continue;
            }

            evaluation_.reset();
            if (!resume_entry(compiled, invocation, entry_index)) {
                paused_entry_ = entry_index;
                return;
            }
        }
    }

    template <typename Invocation> void stop(Invocation invocation) noexcept {
        branches_.stop_all(invocation.context());
        evaluation_.reset();
        paused_entry_.reset();
    }

    template <typename Invocation> void destroy(Invocation invocation) noexcept {
        branches_.destroy_all(invocation.context());

        auto memory = invocation.context().runtime_graph().allocation();
        memory.delete_object(this);
    }

  private:
    template <typename Invocation>
    bool resume_entry(const CompiledMapGraph &compiled, Invocation invocation, MapEntries::size_type entry_index) {
        auto &entries = branches_.entries();
        auto &entry = entries.table().value_at(entry_index);
        if (const auto pending = entries.evaluate(entry, invocation.context(), evaluation_)) {
            invocation.context().set_pending(*pending);
            return false;
        }

        evaluation_.reset();
        if (!compiled.nested.output_node) {
            return true;
        }

        auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
        const auto key = entries.key(entry);
        auto item = output[Scalar<SCALAR>::from(key)];
        const auto result = entry.inner_graph.output(compiled.nested);
        if (result) {
            item.set_value(result);
        } else {
            item.invalidate();
        }
        return true;
    }

    MapRuntime branches_;
    RuntimeGraphEvaluation evaluation_;
    std::optional<MapEntries::size_type> paused_entry_;
};

struct MapNodeState {
    MapRuntimeState *runtime{};
};

class DefaultMapNodeDefinition : public MapNodeDefinition {
  public:
    using state_type = MapNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_timed_scheduling = true;

    using MapNodeDefinition::MapNodeDefinition;

    static void evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        if (state.runtime == nullptr) {
            auto memory = invocation.context().runtime_graph().allocation();
            state.runtime = memory.new_object<MapRuntimeState>(memory);
        }
        state.runtime->evaluate(invocation.compiled_data(), invocation);
    }

    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->stop(invocation);
        }
    }

    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->destroy(invocation);
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
