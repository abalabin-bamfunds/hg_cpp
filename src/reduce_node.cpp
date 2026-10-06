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
#include <bg/time_series/tsd/tsd_key_subscription.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

struct ReduceRuntime;

struct ReduceNodeState {
    ReduceRuntime *runtime{};
};

using ReduceInvocation = NodeInvocation<CompiledReduceGraph, ReduceNodeState>;

struct CompactReducePair {
    std::array<std::size_t, 2> inputs;
    CompiledGraph::ScheduleWord input_validity{};
    bool initialized{};
};

struct CompactReduceInput {
    TimeSeriesState last_updated;
    const void *value{};
    const TimeSeriesRuntimeType *runtime_type{};
};

class ReduceRuntime {
  public:
    ReduceRuntime(allocator memory, EvaluationContext &context)
        : memory_{memory},
          selected_keys_{&memory_.memory_resource()},
          selected_references_{&memory_.memory_resource()},
          selected_values_{&memory_.memory_resource()},
          candidate_keys_{&memory_.memory_resource()},
          candidate_references_{&memory_.memory_resource()},
          candidate_values_{&memory_.memory_resource()},
          current_level_{&memory_.memory_resource()},
          next_level_{&memory_.memory_resource()},
          compact_pairs_{&memory_.memory_resource()},
          compact_current_level_{&memory_.memory_resource()},
          compact_next_level_{&memory_.memory_resource()},
          graphs_{&memory_.memory_resource()},
          owned_graphs_{&memory_.memory_resource()} {
        evaluator_.bind_owner(context.runtime_graph(), context.node_index());
    }

    ReduceRuntime(const ReduceRuntime &) = delete;
    ReduceRuntime &operator=(const ReduceRuntime &) = delete;

    bool evaluate(const CompiledReduceGraph &compiled, ReduceInvocation invocation) {
        const auto values = invocation.template input<In<TSD<SCALAR, TS_T>>>(compiled.dictionary_parameter, 0);
        if (!values.valid()) {
            selected_keys_.clear();
            selected_references_.clear();
            selected_values_.clear();
            retire_graphs(invocation.context());
            dictionary_valid_ = false;
            invocation.template output<TS_T>().invalidate();
            using_default_ = false;
            return true;
        }
        dictionary_valid_ = true;

        if (rebuilding_) {
            if (!continue_rebuild(compiled, invocation)) {
                return false;
            }
            return publish_result(compiled, invocation, true);
        }

        collect_valid_items(values);

        const auto use_compact_graph = compact_graph_is_usable(compiled, candidate_values_);
        if (!use_compact_graph) {
            collect_item_references(invocation.bound_reference(compiled.dictionary_parameter));
        }

        const auto membership_changed = !same_membership(!use_compact_graph);
        const auto compact_mode_changed = use_compact_graph != compact_active_;
        if (!membership_changed) {
            selected_values_.swap(candidate_values_);
        }
        if (membership_changed || compact_mode_changed) {
            if (membership_changed) {
                selected_keys_.swap(candidate_keys_);
                selected_references_.swap(candidate_references_);
                selected_values_.swap(candidate_values_);
            } else {
                selected_references_.swap(candidate_references_);
            }

            retire_graphs(invocation.context());
            compact_active_ = use_compact_graph;

            if (compact_active_) {
                build_compact_graph(compiled);
                if (!evaluate_compact_graph(compiled, invocation)) {
                    return false;
                }
                return publish_compact_result(compiled, invocation, true);
            }

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

        if (compact_active_) {
            if (!evaluate_compact_graph(compiled, invocation)) {
                return false;
            }
            return publish_compact_result(compiled, invocation, false);
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
    template <typename ValueDictionary> void collect_valid_items(const ValueDictionary &values) {
        candidate_keys_.clear();
        candidate_references_.clear();
        candidate_values_.clear();

        values.for_each_id_value([&](const auto id, const auto &item) {
            if (!item.valid()) {
                return;
            }

            const auto key = values.key_at(id);
            candidate_keys_.emplace_back(Scalar<>::from(key), memory_);
            const auto value = values.runtime_type().item(values.data(), id);
            candidate_values_.push_back({value.last_updated, value.value, &value.runtime_type});
        });
    }

    void collect_item_references(Reference dictionary_reference) {
        candidate_references_.clear();
        for (const auto &key : candidate_keys_) {
            const auto key_value = key.view();
            const auto scalar_key = Scalar<>::from(key_value);
            const auto item = tsd_item_reference_input(dictionary_reference, scalar_key);
            candidate_references_.push_back(item.binding.value());
        }
    }

    bool same_membership(bool compare_references) const noexcept {
        if (selected_keys_.size() != candidate_keys_.size() ||
            (compare_references && selected_references_.size() != candidate_references_.size())) {
            return false;
        }

        for (auto index = std::size_t{}; index < selected_keys_.size(); ++index) {
            if (!selected_keys_[index].equals(candidate_keys_[index].view()) ||
                (compare_references && selected_references_[index] != candidate_references_[index])) {
                return false;
            }
        }
        return true;
    }

    bool compact_graph_is_usable(const CompiledReduceGraph &compiled,
                                 std::span<const CompactReduceInput> values) const {
        if (!compiled.compact_graph) {
            return false;
        }

        const auto &compact = *compiled.compact_graph;
        if (!compact.output_node) {
            return compact.output_input.has_value();
        }

        const auto inputs = compact.graph->runtime_inputs();
        for (const auto &value : values) {
            if (!value.last_updated.valid() || value.value == nullptr) {
                return false;
            }
            for (const auto input_index : compact.input_indices) {
                if (input_index >= inputs.size() || value.runtime_type != inputs[input_index].runtime_type) {
                    return false;
                }
            }
        }
        return true;
    }

    void build_compact_graph(const CompiledReduceGraph &compiled) {
        compact_pairs_.clear();
        compact_current_level_.clear();
        compact_next_level_.clear();
        compact_root_ = std::numeric_limits<std::size_t>::max();

        const auto item_count = selected_values_.size();
        compact_current_level_.reserve(item_count);
        for (auto index = std::size_t{}; index < item_count; ++index) {
            compact_current_level_.push_back(index);
        }

        const auto &compact = *compiled.compact_graph;
        if (compact.output_input) {
            while (compact_current_level_.size() > 1) {
                const auto pair_count = compact_current_level_.size() / 2;
                compact_next_level_.clear();
                compact_next_level_.reserve(pair_count + (compact_current_level_.size() & 1U));
                for (auto pair = std::size_t{}; pair < pair_count; ++pair) {
                    compact_next_level_.push_back(compact_current_level_[pair * 2 + *compact.output_input]);
                }
                if ((compact_current_level_.size() & 1U) != 0) {
                    compact_next_level_.push_back(compact_current_level_.back());
                }
                compact_current_level_.swap(compact_next_level_);
            }
            if (!compact_current_level_.empty()) {
                compact_root_ = compact_current_level_.front();
            }
            return;
        }

        while (compact_current_level_.size() > 1) {
            const auto pair_count = compact_current_level_.size() / 2;
            compact_next_level_.clear();
            compact_next_level_.reserve(pair_count + (compact_current_level_.size() & 1U));
            for (auto pair = std::size_t{}; pair < pair_count; ++pair) {
                const auto pair_index = compact_pairs_.size();
                compact_pairs_.push_back(CompactReducePair{
                    .inputs = {compact_current_level_[pair * 2], compact_current_level_[pair * 2 + 1]},
                });
                compact_next_level_.push_back(item_count + pair_index);
            }
            if ((compact_current_level_.size() & 1U) != 0) {
                compact_next_level_.push_back(compact_current_level_.back());
            }
            compact_current_level_.swap(compact_next_level_);
        }

        if (!compact_current_level_.empty()) {
            compact_root_ = compact_current_level_.front();
        }
        allocate_compact_pair_storage(compact);
    }

    void allocate_compact_pair_storage(const CompiledReduceGraph::CompactGraph &compact) {
        if (compact_pairs_.empty()) {
            return;
        }

        compact_storage_size_ = RuntimeGraph::borrowed_storage_size(*compact.graph);
        compact_storage_alignment_ = RuntimeGraph::borrowed_storage_alignment(*compact.graph);
        if (compact_storage_size_ > std::numeric_limits<std::size_t>::max() - (compact_storage_alignment_ - 1)) {
            throw std::length_error{"reduce compact graph storage is too large"};
        }
        compact_storage_stride_ = (compact_storage_size_ + compact_storage_alignment_ - 1) &
                                  ~(compact_storage_alignment_ - 1);
        if (compact_storage_stride_ == 0 ||
            compact_pairs_.size() > std::numeric_limits<std::size_t>::max() / compact_storage_stride_) {
            throw std::length_error{"reduce compact graph storage is too large"};
        }
        compact_storage_bytes_ = compact_pairs_.size() * compact_storage_stride_;
        compact_storage_ = static_cast<std::byte *>(
            memory_.allocate_bytes(compact_storage_bytes_, compact_storage_alignment_));

        for (auto index = std::size_t{}; index < compact_pairs_.size(); ++index) {
            const auto storage = std::span{compact_storage_ + index * compact_storage_stride_, compact_storage_size_};
            RuntimeGraph::initialize_borrowed_storage(*compact.graph, storage);
        }
    }

    void clear_compact_graph() noexcept {
        if (compact_storage_ != nullptr) {
            memory_.deallocate_bytes(compact_storage_, compact_storage_bytes_, compact_storage_alignment_);
            compact_storage_ = nullptr;
        }
        compact_storage_size_ = 0;
        compact_storage_alignment_ = 0;
        compact_storage_stride_ = 0;
        compact_storage_bytes_ = 0;
        compact_pairs_.clear();
        compact_current_level_.clear();
        compact_next_level_.clear();
        compact_root_ = std::numeric_limits<std::size_t>::max();
    }

    std::byte *compact_pair_storage(std::size_t index) const noexcept {
        return compact_storage_ + index * compact_storage_stride_;
    }

    CompactReduceInput compact_input(const CompiledReduceGraph::CompactGraph &compact,
                                     std::size_t input_index) const noexcept {
        const auto item_count = selected_values_.size();
        if (input_index < item_count) {
            return selected_values_[input_index];
        }

        const auto pair_index = input_index - item_count;
        const auto &node = compact.graph->node(*compact.output_node);
        const auto runtime_node = Node{node, compact_pair_storage(pair_index)};
        const auto &state = runtime_node.output_last_updated();
        return {state, state.valid() ? runtime_node.output_value() : nullptr,
                &node.node_invocation_output_runtime_type()};
    }

    static bool same_state(const TimeSeriesState &left, const TimeSeriesState &right) noexcept {
        return left.valid() == right.valid() && (!left.valid() || left.time() == right.time());
    }

    bool evaluate_compact_graph(const CompiledReduceGraph &compiled, ReduceInvocation invocation) {
        const auto &compact = *compiled.compact_graph;
        if (!compact.output_node) {
            return true;
        }

        const auto runtime_inputs = compact.graph->runtime_inputs();
        for (auto index = std::size_t{}; index < compact_pairs_.size(); ++index) {
            auto &pair = compact_pairs_[index];
            auto storage = std::span{compact_pair_storage(index), compact_storage_size_};
            const auto left = compact_input(compact, pair.inputs[0]);
            const auto right = compact_input(compact, pair.inputs[1]);
            const auto left_index = compact.input_indices[0];
            const auto right_index = compact.input_indices[1];
            const auto &left_binding = runtime_inputs[left_index];
            const auto &right_binding = runtime_inputs[right_index];
            const auto &left_previous = *reinterpret_cast<const RuntimeInputBinding *>(
                storage.data() + left_binding.slot_offset);
            const auto &right_previous = *reinterpret_cast<const RuntimeInputBinding *>(
                storage.data() + right_binding.slot_offset);
            const auto left_changed = !same_state(left_previous.last_updated, left.last_updated);
            const auto right_changed = !same_state(right_previous.last_updated, right.last_updated);
            if (pair.initialized && !left_changed && !right_changed) {
                continue;
            }

            auto runtime = RuntimeGraph{*compact.graph, memory_, storage, invocation.context().global_state(),
                                        pair.initialized ? std::optional{pair.input_validity} : std::nullopt};
            auto bind_input = [&](std::size_t binding_index, const CompactReduceInput &input, bool changed) {
                if (!changed) {
                    return;
                }
                if (input.last_updated.valid()) {
                    runtime.bind_runtime_input(binding_index, *input.runtime_type, input.last_updated, input.value,
                                               invocation.context().evaluation_time());
                } else if (!input.last_updated.never_valid()) {
                    runtime.clear_runtime_input(binding_index, invocation.context().evaluation_time());
                }
            };
            bind_input(left_index, left, left_changed);
            bind_input(right_index, right, right_changed);
            if (!runtime.evaluate(invocation.context())) {
                throw std::logic_error{"a borrowed reduce graph cannot pend evaluation"};
            }
            pair.input_validity = runtime.input_validity();
            pair.initialized = true;
        }
        return true;
    }

    bool publish_compact_result(const CompiledReduceGraph &compiled, ReduceInvocation invocation, bool force) {
        auto output = invocation.template output<TS_T>();
        if (selected_values_.empty()) {
            return publish_empty_result(compiled, invocation, output);
        }

        const auto sample = compact_input(*compiled.compact_graph, compact_root_);
        if (!sample.last_updated.valid()) {
            output.invalidate();
            using_default_ = false;
            return true;
        }
        if (!force && sample.last_updated.time() != invocation.context().evaluation_time()) {
            return true;
        }

        const auto &value_type = static_cast<const TimeSeriesValueRuntimeType &>(*sample.runtime_type);
        const auto value = Scalar<>::from(value_type.scalar_runtime_type(), sample.value);
        output.visit([&](Out<TS<SCALAR>> *target) { target->set_value(value); }, ddv::noop);
        using_default_ = false;
        return true;
    }

    template <typename Output>
    bool publish_empty_result(const CompiledReduceGraph &compiled, ReduceInvocation invocation, Output &output) {
        if (!dictionary_valid_) {
            output.invalidate();
            using_default_ = false;
            return true;
        }
        if (compiled.default_parameter) {
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
        clear_compact_graph();
        compact_active_ = false;
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
        clear_compact_graph();
        compact_active_ = false;
    }

    bool publish_result(const CompiledReduceGraph &compiled, ReduceInvocation invocation, bool force) {
        auto output = invocation.template output<TS_T>();
        if (current_level_.empty()) {
            return publish_empty_result(compiled, invocation, output);
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
    std::pmr::vector<CompactReduceInput> selected_values_;
    std::pmr::vector<ScalarValue<>> candidate_keys_;
    std::pmr::vector<Reference> candidate_references_;
    std::pmr::vector<CompactReduceInput> candidate_values_;
    std::pmr::vector<Reference> current_level_;
    std::pmr::vector<Reference> next_level_;
    std::pmr::vector<CompactReducePair> compact_pairs_;
    std::pmr::vector<std::size_t> compact_current_level_;
    std::pmr::vector<std::size_t> compact_next_level_;
    std::pmr::vector<NestedGraph *> graphs_;
    std::pmr::vector<NestedGraph *> owned_graphs_;
    std::byte *compact_storage_{};
    std::size_t compact_storage_size_{};
    std::size_t compact_storage_alignment_{};
    std::size_t compact_storage_stride_{};
    std::size_t compact_storage_bytes_{};
    std::size_t compact_root_{std::numeric_limits<std::size_t>::max()};
    std::size_t next_pair_{};
    std::size_t evaluation_graph_index_{};
    std::size_t retired_graph_count_{};
    bool rebuilding_{};
    bool building_pair_{};
    bool dictionary_valid_{};
    bool using_default_{};
    bool compact_active_{};
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
