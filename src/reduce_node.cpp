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
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
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

using CompactReduceNodeIndex = util::occupancy_index::size_type;
inline constexpr auto invalid_compact_reduce_node = util::occupancy_index::npos;
inline constexpr auto compact_pair_storage_block_size = std::size_t{4096};

struct CompactReduceTreeNode {
    CompactReduceNodeIndex parent{invalid_compact_reduce_node};
    CompactReduceNodeIndex left{invalid_compact_reduce_node};
    CompactReduceNodeIndex right{invalid_compact_reduce_node};
    union {
        item_id_t item_id{};
        CompactReduceNodeIndex pair_index;
    };
    std::uint32_t flags{leaf_flag};

    static constexpr std::uint32_t leaf_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t left_changed_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t right_changed_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t left_replaced_flag = std::uint32_t{1} << 28;
    static constexpr std::uint32_t right_replaced_flag = std::uint32_t{1} << 27;
    static constexpr std::uint32_t split_bit_mask = 0x1f;

    bool is_leaf() const noexcept { return (flags & leaf_flag) != 0; }
    std::uint8_t split_bit() const noexcept { return static_cast<std::uint8_t>(flags & split_bit_mask); }
    void mark_input_changed(std::size_t side) noexcept {
        flags |= side == 0 ? left_changed_flag : right_changed_flag;
    }
    bool input_replaced(std::size_t side) const noexcept {
        return (flags & (side == 0 ? left_replaced_flag : right_replaced_flag)) != 0;
    }
    void mark_input_replaced(std::size_t side) noexcept {
        flags |= side == 0 ? left_replaced_flag : right_replaced_flag;
        mark_input_changed(side);
    }
    void clear_input_changes() noexcept { flags &= ~(left_changed_flag | right_changed_flag | left_replaced_flag |
                                                       right_replaced_flag); }
};

struct CompactReduceInput {
    item_id_t id{};
    TimeSeriesState last_updated;
    const void *value{};
    const TimeSeriesRuntimeType *runtime_type{};
};

struct CompactReducePairState {
    CompiledGraph::ScheduleWord input_validity{};
    bool initialized{};
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
          compact_pair_blocks_{&memory_.memory_resource()},
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

        auto compact_fallback_required = false;
        if (compact_active_) {
            if (update_compact_graph(compiled, values, invocation)) {
                if (!evaluate_compact_graph(compiled, values, invocation)) {
                    return false;
                }
                return publish_compact_result(compiled, values, invocation, false);
            }
            clear_compact_graph();
            compact_active_ = false;
            compact_fallback_required = true;
        }

        collect_valid_items(values);

        const auto use_compact_graph = compact_graph_is_usable(compiled, candidate_values_);
        const auto needs_item_references = !use_compact_graph && candidate_values_.size() > 1;
        if (needs_item_references) {
            collect_item_references(invocation.bound_reference(compiled.dictionary_parameter));
        }

        const auto membership_changed = !same_membership(needs_item_references);
        const auto compact_mode_changed = use_compact_graph != compact_active_;
        if (!membership_changed && !compact_fallback_required) {
            selected_values_.swap(candidate_values_);
        }
        if (membership_changed || compact_mode_changed || compact_fallback_required) {
            selected_keys_.swap(candidate_keys_);
            selected_references_.swap(candidate_references_);
            selected_values_.swap(candidate_values_);

            retire_graphs(invocation.context());
            compact_active_ = use_compact_graph;

            if (compact_active_) {
                build_compact_graph(compiled);
                if (!evaluate_compact_graph(compiled, values, invocation)) {
                    return false;
                }
                return publish_compact_result(compiled, values, invocation, true);
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
            candidate_values_.push_back({id, value.last_updated, value.value, &value.runtime_type});
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

    bool compact_input_accepts(const CompiledReduceGraph::CompactGraph &compact,
                               const CompactReduceInput &value) const {
        if (!value.last_updated.valid() || value.value == nullptr) {
            return false;
        }

        const auto inputs = compact.graph->runtime_inputs();
        for (const auto input_index : compact.input_indices) {
            if (input_index >= inputs.size() || value.runtime_type != inputs[input_index].runtime_type) {
                return false;
            }
        }
        return true;
    }

    template <typename Values> CompactReduceInput compact_item(const Values &values, item_id_t id) const {
        const auto &runtime_type =
            static_cast<const TimeSeriesDictionaryRuntimeType &>(values.runtime_type());
        const auto item = runtime_type.item(values.data(), id);
        return {id, item.last_updated, item.last_updated.valid() ? item.value : nullptr, &item.runtime_type};
    }

    CompactReduceNodeIndex allocate_compact_node() {
        auto &memory = memory_.memory_resource();
        const auto index = compact_node_positions_.acquire(memory);
        try {
            compact_nodes_.emplace_at(index, memory);
        } catch (...) {
            compact_node_positions_.reset(index);
            throw;
        }
        return index;
    }

    void release_compact_node(CompactReduceNodeIndex index) noexcept {
        compact_nodes_.erase_at(index, memory_.memory_resource());
        compact_node_positions_.reset(index);
    }

    CompactReduceNodeIndex allocate_compact_pair() {
        auto &memory = memory_.memory_resource();
        const auto index = compact_pair_positions_.acquire(memory);
        try {
            compact_pair_states_.emplace_at(index, memory);
            initialize_compact_pair_storage(index);
        } catch (...) {
            if (compact_pair_states_.get(index, compact_pair_positions_) != nullptr) {
                compact_pair_states_.erase_at(index, memory);
            }
            compact_pair_positions_.reset(index);
            throw;
        }
        return index;
    }

    void release_compact_pair(CompactReduceNodeIndex index) noexcept {
        compact_pair_states_.erase_at(index, memory_.memory_resource());
        compact_pair_positions_.reset(index);
    }

    std::byte *compact_pair_storage(CompactReduceNodeIndex index) const noexcept {
        const auto block_index = index / compact_storage_slots_per_block_;
        const auto block_offset = index % compact_storage_slots_per_block_;
        assert(block_index < compact_pair_blocks_.size());
        assert(compact_pair_blocks_[block_index] != nullptr);
        return compact_pair_blocks_[block_index] + block_offset * compact_storage_stride_;
    }

    void initialize_compact_pair_storage(CompactReduceNodeIndex index) {
        const auto block_index = index / compact_storage_slots_per_block_;
        while (compact_pair_blocks_.size() <= block_index) {
            compact_pair_blocks_.push_back(nullptr);
        }
        if (compact_pair_blocks_[block_index] == nullptr) {
            compact_pair_blocks_[block_index] = static_cast<std::byte *>(
                memory_.allocate_bytes(compact_storage_block_bytes_, compact_storage_alignment_));
        }

        const auto storage = std::span{compact_pair_storage(index), compact_storage_size_};
        RuntimeGraph::initialize_borrowed_storage(*compact_graph_->graph, storage);
    }

    CompactReduceNodeIndex allocate_compact_branch(std::uint8_t split_bit) {
        const auto index = allocate_compact_node();
        auto release_node = util::scope_exit{[&]() noexcept { release_compact_node(index); }};
        auto pair_index = invalid_compact_reduce_node;
        if (compact_has_output_node_) {
            pair_index = allocate_compact_pair();
        }

        auto &branch = compact_nodes_[index];
        branch.flags = split_bit;
        branch.pair_index = pair_index;
        release_node.release();
        return index;
    }

    bool compact_bit(item_id_t id, std::uint8_t bit) const noexcept { return ((id >> bit) & 1U) != 0; }

    void mark_compact_path_changed(CompactReduceNodeIndex child) noexcept {
        if (!compact_has_output_node_) {
            return;
        }
        auto current = child;
        auto parent = compact_nodes_[current].parent;
        while (parent != invalid_compact_reduce_node) {
            auto &branch = compact_nodes_[parent];
            branch.mark_input_changed(branch.left == current ? 0 : 1);
            current = parent;
            parent = branch.parent;
        }
    }

    bool insert_compact_leaf(item_id_t id) {
        if (compact_leaf_by_id_.contains(id)) {
            return false;
        }

        const auto leaf_index = allocate_compact_node();
        auto release_leaf = util::scope_exit{[&]() noexcept { release_compact_node(leaf_index); }};
        auto &leaf = compact_nodes_[leaf_index];
        leaf.item_id = id;
        const auto inserted = compact_leaf_by_id_.try_emplace(id, memory_.memory_resource(), leaf_index).second;
        if (!inserted) {
            return false;
        }
        auto remove_leaf_record = util::scope_exit{[&]() noexcept {
            compact_leaf_by_id_.erase(id, memory_.memory_resource());
        }};

        if (compact_root_ == invalid_compact_reduce_node) {
            compact_root_ = leaf_index;
            compact_root_changed_ = true;
            release_leaf.release();
            remove_leaf_record.release();
            return true;
        }

        auto existing_index = compact_root_;
        while (!compact_nodes_[existing_index].is_leaf()) {
            const auto &branch = compact_nodes_[existing_index];
            existing_index = compact_bit(id, branch.split_bit()) ? branch.right : branch.left;
        }
        const auto existing_id = compact_nodes_[existing_index].item_id;
        const auto differing_bits = id ^ existing_id;
        const auto split_bit = static_cast<std::uint8_t>(std::bit_width(differing_bits) - 1);

        auto parent = invalid_compact_reduce_node;
        auto subtree = compact_root_;
        while (!compact_nodes_[subtree].is_leaf() && compact_nodes_[subtree].split_bit() > split_bit) {
            parent = subtree;
            const auto &branch = compact_nodes_[subtree];
            subtree = compact_bit(id, branch.split_bit()) ? branch.right : branch.left;
        }

        const auto branch_index = allocate_compact_branch(split_bit);
        auto release_branch = util::scope_exit{[&]() noexcept {
            const auto pair_index = compact_nodes_[branch_index].pair_index;
            if (pair_index != invalid_compact_reduce_node) {
                release_compact_pair(pair_index);
            }
            release_compact_node(branch_index);
        }};
        auto &branch = compact_nodes_[branch_index];
        branch.parent = parent;
        branch.mark_input_changed(0);
        branch.mark_input_changed(1);
        if (compact_bit(id, split_bit)) {
            branch.left = subtree;
            branch.right = leaf_index;
        } else {
            branch.left = leaf_index;
            branch.right = subtree;
        }
        compact_nodes_[subtree].parent = branch_index;
        leaf.parent = branch_index;

        if (parent == invalid_compact_reduce_node) {
            compact_root_ = branch_index;
            compact_root_changed_ = true;
        } else {
            auto &old_parent = compact_nodes_[parent];
            if (old_parent.left == subtree) {
                old_parent.left = branch_index;
                old_parent.mark_input_replaced(0);
            } else {
                old_parent.right = branch_index;
                old_parent.mark_input_replaced(1);
            }
        }

        mark_compact_path_changed(branch_index);
        release_branch.release();
        release_leaf.release();
        remove_leaf_record.release();
        return true;
    }

    bool remove_compact_leaf(item_id_t id) noexcept {
        const auto *found = compact_leaf_by_id_.find(id);
        if (found == nullptr) {
            return false;
        }
        const auto leaf_index = *found;
        compact_leaf_by_id_.erase(id, memory_.memory_resource());
        const auto parent = compact_nodes_[leaf_index].parent;
        if (parent == invalid_compact_reduce_node) {
            compact_root_ = invalid_compact_reduce_node;
            release_compact_node(leaf_index);
            compact_root_changed_ = true;
            return true;
        }

        auto &branch = compact_nodes_[parent];
        const auto sibling = branch.left == leaf_index ? branch.right : branch.left;
        const auto grandparent = branch.parent;
        compact_nodes_[sibling].parent = grandparent;
        if (grandparent == invalid_compact_reduce_node) {
            compact_root_ = sibling;
            compact_root_changed_ = true;
        } else {
            auto &ancestor = compact_nodes_[grandparent];
            if (ancestor.left == parent) {
                ancestor.left = sibling;
                ancestor.mark_input_replaced(0);
            } else {
                ancestor.right = sibling;
                ancestor.mark_input_replaced(1);
            }
            mark_compact_path_changed(sibling);
        }

        const auto pair_index = branch.pair_index;
        if (pair_index != invalid_compact_reduce_node) {
            release_compact_pair(pair_index);
        }
        release_compact_node(parent);
        release_compact_node(leaf_index);
        return true;
    }

    void build_compact_graph(const CompiledReduceGraph &compiled) {
        clear_compact_graph();
        compact_graph_ = &*compiled.compact_graph;
        compact_has_output_node_ = compact_graph_->output_node.has_value();
        compact_root_changed_ = true;
        if (compact_has_output_node_) {
            compact_storage_size_ = RuntimeGraph::borrowed_storage_size(*compact_graph_->graph);
            compact_storage_alignment_ = RuntimeGraph::borrowed_storage_alignment(*compact_graph_->graph);
        }
        if (compact_has_output_node_) {
            if (compact_storage_size_ > std::numeric_limits<std::size_t>::max() - (compact_storage_alignment_ - 1)) {
                throw std::length_error{"reduce compact graph storage is too large"};
            }
            compact_storage_stride_ = (compact_storage_size_ + compact_storage_alignment_ - 1) &
                                      ~(compact_storage_alignment_ - 1);
            if (compact_storage_stride_ == 0) {
                throw std::length_error{"reduce compact graph storage is too large"};
            }
            compact_storage_slots_per_block_ =
                std::max<std::size_t>(1, compact_pair_storage_block_size / compact_storage_stride_);
            if (compact_storage_slots_per_block_ >
                std::numeric_limits<std::size_t>::max() / compact_storage_stride_) {
                throw std::length_error{"reduce compact graph storage is too large"};
            }
            compact_storage_block_bytes_ = compact_storage_slots_per_block_ * compact_storage_stride_;
        }
        for (const auto &value : selected_values_) {
            insert_compact_leaf(value.id);
        }
    }

    void clear_compact_graph() noexcept {
        auto &memory = memory_.memory_resource();
        compact_leaf_by_id_.release(memory);
        compact_nodes_.release(compact_node_positions_, memory);
        compact_node_positions_.release(memory);
        compact_pair_states_.release(compact_pair_positions_, memory);
        compact_pair_positions_.release(memory);
        for (auto *block : compact_pair_blocks_) {
            if (block != nullptr) {
                memory_.deallocate_bytes(block, compact_storage_block_bytes_, compact_storage_alignment_);
            }
        }
        compact_pair_blocks_.clear();
        compact_storage_size_ = 0;
        compact_storage_alignment_ = 0;
        compact_storage_stride_ = 0;
        compact_storage_slots_per_block_ = 0;
        compact_storage_block_bytes_ = 0;
        compact_graph_ = nullptr;
        compact_has_output_node_ = false;
        compact_root_changed_ = false;
        compact_root_ = invalid_compact_reduce_node;
    }

    template <typename Values>
    CompactReduceInput compact_tree_output(CompactReduceNodeIndex index, const Values &values,
                                           const CompiledReduceGraph::CompactGraph &compact) const {
        auto current = index;
        while (!compact_nodes_[current].is_leaf() && !compact.output_node) {
            const auto &branch = compact_nodes_[current];
            current = *compact.output_input == 0 ? branch.left : branch.right;
        }
        const auto &tree_node = compact_nodes_[current];
        if (tree_node.is_leaf()) {
            return compact_item(values, tree_node.item_id);
        }

        const auto &compiled_node = compact.graph->node(*compact.output_node);
        const auto runtime_node = Node{compiled_node, compact_pair_storage(tree_node.pair_index)};
        const auto &state = runtime_node.output_last_updated();
        return {invalid_compact_reduce_node, state, state.valid() ? runtime_node.output_value() : nullptr,
                &compiled_node.node_invocation_output_runtime_type()};
    }

    bool evaluate_compact_branch(CompactReduceNodeIndex index, const CompiledReduceGraph &compiled,
                                 const auto &values, ReduceInvocation invocation) {
        auto &branch = compact_nodes_[index];
        const auto evaluation_time = invocation.context().evaluation_time();
        if (branch.is_leaf()) {
            return compact_item(values, branch.item_id).last_updated.time() == evaluation_time;
        }

        const auto dirty = (branch.flags & (CompactReduceTreeNode::left_changed_flag |
                                            CompactReduceTreeNode::right_changed_flag)) != 0;
        if (!dirty) {
            const auto &compiled_node = compiled.compact_graph->graph->node(*compiled.compact_graph->output_node);
            const auto runtime_node = Node{compiled_node, compact_pair_storage(branch.pair_index)};
            return runtime_node.output_last_updated().time() == evaluation_time;
        }

        const auto left_changed = evaluate_compact_branch(branch.left, compiled, values, invocation);
        const auto right_changed = evaluate_compact_branch(branch.right, compiled, values, invocation);
        const auto &compact = *compiled.compact_graph;
        auto &pair_state = compact_pair_states_[branch.pair_index];
        const auto left = compact_tree_output(branch.left, values, compact);
        const auto right = compact_tree_output(branch.right, values, compact);
        const auto runtime_inputs = compact.graph->runtime_inputs();
        const auto left_index = compact.input_indices[0];
        const auto right_index = compact.input_indices[1];
        const auto storage = std::span{compact_pair_storage(branch.pair_index), compact_storage_size_};
        const auto left_binding = *reinterpret_cast<const RuntimeInputBinding *>(
            storage.data() + runtime_inputs[left_index].slot_offset);
        const auto right_binding = *reinterpret_cast<const RuntimeInputBinding *>(
            storage.data() + runtime_inputs[right_index].slot_offset);
        auto runtime = RuntimeGraph{*compact.graph, memory_, storage, invocation.context().global_state(),
                                   pair_state.initialized ? std::optional{pair_state.input_validity} : std::nullopt};

        const auto bind_input = [&](std::size_t binding_index, const CompactReduceInput &input,
                                    const RuntimeInputBinding &previous, bool child_replaced,
                                    bool child_changed) {
            if (!input.last_updated.valid()) {
                if (previous.last_updated.valid()) {
                    runtime.clear_runtime_input(binding_index, evaluation_time);
                }
                return;
            }
            if (child_replaced || !pair_state.initialized || child_changed) {
                runtime.bind_runtime_input(binding_index, *input.runtime_type, input.last_updated, input.value,
                                           evaluation_time);
                return;
            }
            runtime.refresh_runtime_input(binding_index, *input.runtime_type, input.value);
        };
        bind_input(left_index, left, left_binding, branch.input_replaced(0), left_changed);
        bind_input(right_index, right, right_binding, branch.input_replaced(1), right_changed);
        if (!runtime.evaluate(invocation.context())) {
            throw std::logic_error{"a borrowed reduce graph cannot pend evaluation"};
        }
        const auto &compiled_node = compact.graph->node(*compact.output_node);
        const auto runtime_node = Node{compiled_node, storage.data()};
        pair_state.input_validity = runtime.input_validity();
        pair_state.initialized = true;
        branch.clear_input_changes();
        return runtime_node.output_last_updated().time() == evaluation_time;
    }

    bool update_compact_graph(const CompiledReduceGraph &compiled, const auto &values,
                              ReduceInvocation invocation) {
        compact_root_changed_ = false;
        if (!values.modified()) {
            return true;
        }
        const auto &compact = *compiled.compact_graph;
        auto compatible = true;
        const auto check = [&](item_id_t id) {
            if (!compatible) {
                return;
            }
            const auto item = compact_item(values, id);
            if (item.last_updated.valid() && !compact_input_accepts(compact, item)) {
                compatible = false;
            }
        };
        values.for_each_item_id_remapping([&](item_id_t, item_id_t new_id) { check(new_id); });
        for (auto id = values.next_added_id(0); id != util::occupancy_index::npos; id = values.next_added_id(id + 1)) {
            check(id);
        }
        values.for_each_modified([&](const auto &key) { check(values.find_item_id(key)); });
        if (!compatible) {
            return false;
        }

        values.for_each_removed_physical_id([&](item_id_t id) { remove_compact_leaf(id); });
        values.for_each_item_id_remapping([&](item_id_t old_id, item_id_t) { remove_compact_leaf(old_id); });

        values.for_each_item_id_remapping([&](item_id_t, item_id_t new_id) {
            const auto item = compact_item(values, new_id);
            if (item.last_updated.valid()) {
                insert_compact_leaf(new_id);
            }
        });
        for (auto id = values.next_added_id(0); id != util::occupancy_index::npos; id = values.next_added_id(id + 1)) {
            if (compact_item(values, id).last_updated.valid()) {
                insert_compact_leaf(id);
            }
        }
        values.for_each_modified([&](const auto &key) {
            const auto id = values.find_item_id(key);
            const auto item = compact_item(values, id);
            if (!item.last_updated.valid()) {
                remove_compact_leaf(id);
                return;
            }
            if (const auto *leaf = compact_leaf_by_id_.find(id)) {
                mark_compact_path_changed(*leaf);
            } else {
                insert_compact_leaf(id);
            }
        });

        return true;
    }

    bool evaluate_compact_graph(const CompiledReduceGraph &compiled, const auto &values,
                                ReduceInvocation invocation) {
        if (compact_root_ == invalid_compact_reduce_node || !compact_has_output_node_) {
            return true;
        }
        evaluate_compact_branch(compact_root_, compiled, values, invocation);
        return true;
    }

    bool publish_compact_result(const CompiledReduceGraph &compiled, const auto &values,
                                ReduceInvocation invocation, bool force) {
        auto output = invocation.template output<TS_T>();
        if (compact_root_ == invalid_compact_reduce_node) {
            compact_root_changed_ = false;
            return publish_empty_result(compiled, invocation, output);
        }

        const auto sample = compact_tree_output(compact_root_, values, *compiled.compact_graph);
        if (!sample.last_updated.valid()) {
            output.invalidate();
            using_default_ = false;
            compact_root_changed_ = false;
            return true;
        }
        if (!force && !compact_root_changed_ &&
            sample.last_updated.time() != invocation.context().evaluation_time()) {
            return true;
        }

        const auto &value_type = static_cast<const TimeSeriesValueRuntimeType &>(*sample.runtime_type);
        const auto value = Scalar<>::from(value_type.scalar_runtime_type(), sample.value);
        output.visit([&](Out<TS<SCALAR>> *target) { target->set_value(value); }, ddv::noop);
        using_default_ = false;
        compact_root_changed_ = false;
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
        if (selected_values_.size() == 1) {
            const auto &value = selected_values_.front();
            if (!value.last_updated.valid()) {
                output.invalidate();
                using_default_ = false;
                return true;
            }
            if (!force && value.last_updated.time() != invocation.context().evaluation_time()) {
                return true;
            }

            const auto &value_type = static_cast<const TimeSeriesValueRuntimeType &>(*value.runtime_type);
            const auto scalar = Scalar<>::from(value_type.scalar_runtime_type(), value.value);
            output.visit([&](Out<TS<SCALAR>> *target) { target->set_value(scalar); }, ddv::noop);
            using_default_ = false;
            return true;
        }
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
    util::hash_dictionary<item_id_t, CompactReduceNodeIndex> compact_leaf_by_id_;
    util::occupancy_index compact_node_positions_;
    util::sparse_bunch<CompactReduceTreeNode> compact_nodes_;
    util::occupancy_index compact_pair_positions_;
    util::sparse_bunch<CompactReducePairState> compact_pair_states_;
    std::pmr::vector<std::byte *> compact_pair_blocks_;
    std::pmr::vector<NestedGraph *> graphs_;
    std::pmr::vector<NestedGraph *> owned_graphs_;
    const CompiledReduceGraph::CompactGraph *compact_graph_{};
    std::size_t compact_storage_size_{};
    std::size_t compact_storage_alignment_{};
    std::size_t compact_storage_stride_{};
    std::size_t compact_storage_slots_per_block_{};
    std::size_t compact_storage_block_bytes_{};
    CompactReduceNodeIndex compact_root_{invalid_compact_reduce_node};
    std::size_t next_pair_{};
    std::size_t evaluation_graph_index_{};
    std::size_t retired_graph_count_{};
    bool rebuilding_{};
    bool building_pair_{};
    bool dictionary_valid_{};
    bool using_default_{};
    bool compact_active_{};
    bool compact_has_output_node_{};
    bool compact_root_changed_{};
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
