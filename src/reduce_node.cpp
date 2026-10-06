#include <bg/reduce/details/reduce_node.hpp>

#include <bg/allocator.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/details/nested_graph_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/reduce/details/reduce_node_definition.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <array>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

struct ReduceRuntime;

struct ReduceNodeState {
    ReduceRuntime *runtime{};
};

using ReduceInvocation = NodeInvocation<CompiledReduceGraph, ReduceNodeState>;

class ReduceRuntime {
  public:
    ReduceRuntime(allocator memory, EvaluationContext &context)
        : memory_{memory},
          selected_keys_{&memory_.memory_resource()},
          selected_references_{&memory_.memory_resource()},
          candidate_keys_{&memory_.memory_resource()},
          candidate_references_{&memory_.memory_resource()},
          current_level_{&memory_.memory_resource()},
          next_level_{&memory_.memory_resource()},
          graphs_{&memory_.memory_resource()},
          owned_graphs_{&memory_.memory_resource()} {
        evaluator_.bind_owner(context.runtime_graph(), context.node_index());
    }

    ReduceRuntime(const ReduceRuntime &) = delete;
    ReduceRuntime &operator=(const ReduceRuntime &) = delete;

    bool evaluate(const CompiledReduceGraph &compiled, ReduceInvocation invocation) {
        const auto dictionary = invocation.template input<In<TSD<SCALAR, REF<TS_T>>>>(
            compiled.dictionary_parameter, 0);
        if (!dictionary.valid()) {
            selected_keys_.clear();
            selected_references_.clear();
            retire_graphs(invocation.context());
            dictionary_valid_ = false;
            invocation.template output<TS_T>().invalidate();
            using_default_ = false;
            return true;
        }
        dictionary_valid_ = true;
        dictionary_empty_ = dictionary.size() == 0;

        if (rebuilding_) {
            if (!continue_rebuild(compiled, invocation)) {
                return false;
            }
            return publish_result(compiled, invocation, true);
        }

        collect_valid_items(dictionary);

        const auto membership_changed = !same_membership();
        if (membership_changed) {
            selected_keys_.swap(candidate_keys_);
            selected_references_.swap(candidate_references_);
            retire_graphs(invocation.context());
            current_level_.assign(selected_references_.begin(), selected_references_.end());
            next_level_.clear();
            next_pair_ = 0;
            building_pair_ = false;
            rebuilding_ = current_level_.size() > 1;
            const auto graph_count = current_level_.empty() ? std::size_t{} : current_level_.size() - 1;
            graphs_.reserve(graph_count);
            owned_graphs_.reserve(owned_graphs_.size() + graph_count);

            if (rebuilding_ && !continue_rebuild(compiled, invocation)) {
                return false;
            }
            return publish_result(compiled, invocation, true);
        }

        while (evaluation_graph_index_ < graphs_.size()) {
            if (!graphs_[evaluation_graph_index_]->evaluate(invocation.context())) {
                return false;
            }
            ++evaluation_graph_index_;
        }
        evaluation_graph_index_ = 0;

        return publish_result(compiled, invocation, false);
    }

    void stop(EvaluationContext &context) noexcept { retire_graphs(context); }

    void destroy(EvaluationContext &context) noexcept {
        context.runtime_graph().remove_cycle_callback(this);
        destroy_graphs(context);
        memory_.delete_object(this);
    }

  private:
    template <typename Dictionary> void collect_valid_items(const Dictionary &dictionary) {
        candidate_keys_.clear();
        candidate_references_.clear();
        if (!dictionary.valid()) {
            return;
        }

        dictionary.for_each_key_value([&](const auto &key, const auto &item) {
            if (!item.valid()) {
                return;
            }

            const auto reference = item.value();
            if (!ResolvedTimeSeries::resolve(reference).valid()) {
                return;
            }

            candidate_keys_.emplace_back(Scalar<>::from(key), memory_);
            candidate_references_.push_back(reference);
        });
    }

    bool same_membership() const noexcept {
        if (selected_keys_.size() != candidate_keys_.size()) {
            return false;
        }

        for (auto index = std::size_t{}; index < selected_keys_.size(); ++index) {
            if (!selected_keys_[index].equals(candidate_keys_[index].view()) ||
                selected_references_[index] != candidate_references_[index]) {
                return false;
            }
        }
        return true;
    }

    bool continue_rebuild(const CompiledReduceGraph &compiled, ReduceInvocation invocation) {
        auto &context = invocation.context();
        while (current_level_.size() > 1) {
            const auto pair_count = current_level_.size() / 2;
            while (next_pair_ < pair_count) {
                if (!building_pair_) {
                    auto *graph = memory_.new_object<NestedGraph>();
                    owned_graphs_.push_back(graph);
                    graphs_.push_back(graph);
                    const auto pair_inputs = std::array{
                        NestedGraphInput{current_level_[next_pair_ * 2]},
                        NestedGraphInput{current_level_[next_pair_ * 2 + 1]},
                    };
                    const auto inherited_count = compiled.nested.graph->inherited_time_series_count();
                    auto startup = NestedGraphStartupContext{
                        invocation, compiled.nested, invocation.parameter_count() - inherited_count, inherited_count};
                    auto rollback = util::scope_exit{[&]() noexcept {
                        graph->stop_and_destroy(context);
                        graphs_.pop_back();
                        owned_graphs_.pop_back();
                        memory_.delete_object(graph);
                    }};
                    graph->start(compiled.nested, evaluator_, startup.context(), pair_inputs, graphs_.size() - 1);
                    rollback.release();
                    building_pair_ = true;
                }

                auto &graph = *graphs_.back();
                if (!graph.evaluate(context)) {
                    return false;
                }
                next_level_.push_back(graph.output(compiled.nested));
                building_pair_ = false;
                ++next_pair_;
            }

            if ((current_level_.size() & 1U) != 0) {
                next_level_.push_back(current_level_.back());
            }
            current_level_.swap(next_level_);
            next_level_.clear();
            next_pair_ = 0;
        }

        rebuilding_ = false;
        evaluation_graph_index_ = 0;
        return true;
    }

    void retire_graphs(EvaluationContext &context) noexcept {
        if (!graphs_.empty()) {
            // NestedGraph callbacks are added below this callback and run first at cycle start.
            if (retired_graph_count_ == 0) {
                context.add_cycle_callback(this, release_retired_graphs_callback);
            }
            for (auto *graph : graphs_) {
                graph->stop(context);
            }
            graphs_.clear();
            retired_graph_count_ = owned_graphs_.size();
        }
        current_level_.clear();
        next_level_.clear();
        next_pair_ = 0;
        building_pair_ = false;
        rebuilding_ = false;
        evaluation_graph_index_ = 0;
    }

    static void release_retired_graphs_callback(void *receiver, EvaluationContext &context) noexcept {
        static_cast<ReduceRuntime *>(receiver)->release_retired_graphs(context);
    }

    void release_retired_graphs(EvaluationContext &context) noexcept {
        if (retired_graph_count_ == 0) {
            return;
        }

        for (auto index = std::size_t{}; index < retired_graph_count_; ++index) {
            auto *graph = owned_graphs_[index];
            graph->stop_and_destroy(context);
            memory_.delete_object(graph);
        }
        owned_graphs_.erase(owned_graphs_.begin(), owned_graphs_.begin() + retired_graph_count_);
        retired_graph_count_ = 0;
    }

    void destroy_graphs(EvaluationContext &context) noexcept {
        for (auto *graph : owned_graphs_) {
            context.runtime_graph().remove_cycle_callback(graph);
            graph->stop_and_destroy(context);
            memory_.delete_object(graph);
        }
        owned_graphs_.clear();
        graphs_.clear();
        retired_graph_count_ = 0;
        current_level_.clear();
        next_level_.clear();
        next_pair_ = 0;
        building_pair_ = false;
        rebuilding_ = false;
        evaluation_graph_index_ = 0;
    }

    bool publish_result(const CompiledReduceGraph &compiled, ReduceInvocation invocation, bool force) {
        auto output = invocation.template output<TS_T>();
        if (current_level_.empty()) {
            if (!dictionary_valid_) {
                output.invalidate();
                using_default_ = false;
                return true;
            }
            if (dictionary_empty_ && compiled.default_parameter) {
                const auto fallback = invocation.scalar(*compiled.default_parameter, 0);
                if (!fallback.has_value()) {
                    output.invalidate();
                    using_default_ = false;
                    return true;
                }
                if (!using_default_) {
                    output.visit([&](Out<TS<SCALAR>> *value) { value->set_value(fallback.value()); }, ddv::noop);
                }
                using_default_ = true;
            } else {
                output.invalidate();
                using_default_ = false;
            }
            return true;
        }

        const auto reference = current_level_.front();
        if (!reference) {
            output.invalidate();
            using_default_ = false;
            return true;
        }

        const auto resolved = ResolvedTimeSeries::resolve(reference);
        if (!resolved.valid()) {
            output.invalidate();
            using_default_ = false;
            return true;
        }
        if (!force && resolved.last_updated->time() != invocation.context().evaluation_time()) {
            return true;
        }

        const auto &value_type = static_cast<const TimeSeriesValueRuntimeType &>(*resolved.runtime_type);
        const auto value = Scalar<>::from(value_type.scalar_runtime_type(), resolved.storage);
        output.visit([&](Out<TS<SCALAR>> *target) { target->set_value(value); }, ddv::noop);
        using_default_ = false;
        return true;
    }

    allocator memory_;
    RuntimeGraphEvaluator evaluator_;
    std::pmr::vector<ScalarValue<>> selected_keys_;
    std::pmr::vector<Reference> selected_references_;
    std::pmr::vector<ScalarValue<>> candidate_keys_;
    std::pmr::vector<Reference> candidate_references_;
    std::pmr::vector<Reference> current_level_;
    std::pmr::vector<Reference> next_level_;
    std::pmr::vector<NestedGraph *> graphs_;
    std::pmr::vector<NestedGraph *> owned_graphs_;
    std::size_t next_pair_{};
    std::size_t evaluation_graph_index_{};
    std::size_t retired_graph_count_{};
    bool rebuilding_{};
    bool building_pair_{};
    bool dictionary_valid_{};
    bool dictionary_empty_{};
    bool using_default_{};
};

class DefaultReduceNodeDefinition : public ReduceNodeDefinition {
  public:
    using state_type = ReduceNodeState;
    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_timed_scheduling = true;

    using ReduceNodeDefinition::ReduceNodeDefinition;

    static bool evaluate(ReduceInvocation invocation) {
        auto &state = invocation.state();
        if (state.runtime == nullptr) {
            auto memory = invocation.context().runtime_graph().allocation();
            state.runtime = memory.new_object<ReduceRuntime>(memory, invocation.context());
        }
        return state.runtime->evaluate(invocation.compiled_data(), invocation);
    }

    static void stop(ReduceInvocation invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->stop(invocation.context());
        }
    }

    static void destroy_state(ReduceInvocation invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->destroy(invocation.context());
            invocation.state().runtime = nullptr;
        }
    }
};

} // namespace

std::unique_ptr<NodeTemplate> make_reduce_node(ReduceGraphWiring wiring, const ResolvedSignature &signature,
                                               std::uint32_t dictionary_parameter,
                                               std::optional<std::uint32_t> default_parameter) {
    return std::make_unique<RuntimeSignatureNodeTemplate<DefaultReduceNodeDefinition>>(
        DefaultReduceNodeDefinition{std::move(wiring), signature, dictionary_parameter, default_parameter});
}

} // namespace bg::details
