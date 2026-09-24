#include <bg/graph/details/mesh_node.hpp>

#include <bg/allocator.hpp>
#include <bg/details/evaluation_context.hpp>
#include <bg/graph/nested_graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map/details/map_node_definition.hpp>
#include <bg/map/details/map_runtime.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

class MeshRuntimeState;

inline constexpr unsigned char mesh_context_section_type{};

struct MeshContextSection : EvaluationContextSection {
    MeshRuntimeState *mesh;
    std::uint32_t branch;

    MeshContextSection(MeshRuntimeState &mesh, std::uint32_t branch) noexcept
        : EvaluationContextSection{nullptr, &mesh_context_section_type}, mesh{&mesh}, branch{branch} {}

    static const MeshContextSection &find(const EvaluationContext &context, std::size_t depth) {
        const auto *section = context.find_section(&mesh_context_section_type, depth);
        if (section == nullptr) {
            throw std::logic_error{"a mesh selection has no matching evaluation context"};
        }
        return *static_cast<const MeshContextSection *>(section);
    }
};

struct MeshDependencyData {
    std::uint32_t mesh_depth;
};

struct MeshDependencyState {
    static constexpr auto no_edge = std::numeric_limits<std::uint32_t>::max();

    MeshRuntimeState *mesh{};
    std::uint32_t edge{no_edge};
};

struct MeshDependencyIdentity {
    const Type *key_type;
    const TimeSeriesType *target_type;
    std::size_t mesh_depth;

    std::size_t hash() const noexcept {
        auto result = reinterpret_cast<std::size_t>(key_type);
        result ^= reinterpret_cast<std::size_t>(target_type) + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        result ^= mesh_depth + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        return result;
    }

    bool operator==(const MeshDependencyIdentity &) const = default;
};

class MeshDependencyDefinition {
  public:
    using compiled_data_type = MeshDependencyData;
    using state_type = MeshDependencyState;

    MeshDependencyDefinition(const Type &key_type, const TimeSeriesType &target_type, std::size_t mesh_depth)
        : target_type_{&target_type}, mesh_depth_{mesh_depth},
          signature_{util::symbol{"mesh_dependency"},
                     {{util::symbol{"key"}, &key_type, active, required}},
                     time_series_reference_type(target_type, TimeSeriesGuarantee::deduplicated)} {
        if (mesh_depth > std::numeric_limits<std::uint32_t>::max()) {
            throw WiringError{"mesh nesting exceeds the supported depth"};
        }
    }

    const Signature &signature() const noexcept { return signature_; }

    NodeTemplateIdentity definition_identity() const {
        return NodeTemplateIdentity::from_value(MeshDependencyIdentity{
            .key_type = signature_.parameter(0).type,
            .target_type = target_type_,
            .mesh_depth = mesh_depth_,
        });
    }

    compiled_data_type compile(const ResolvedSignature &, allocator &) const noexcept {
        return {static_cast<std::uint32_t>(mesh_depth_)};
    }

    static void start(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        invocation.context().schedule_now();
    }

    static bool evaluate(NodeInvocation<compiled_data_type, state_type> invocation);
    static void on_invalidation(NodeInvocation<compiled_data_type, state_type> invocation);
    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept;
    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept;

  private:
    template <typename State> static Scalar<> dependency_key(NodeInvocation<compiled_data_type, State> invocation) {
        const auto inputs = invocation.inputs();
        if (inputs.begin() != inputs.end()) {
            const auto input = (*inputs.begin()).value();
            return *input.visit(
                [](const In<TS<SCALAR>> *value) {
                    const auto key = value->value_view();
                    return Scalar<>::from(key);
                },
                ddv::noop);
        }
        return (*invocation.scalars().begin()).value();
    }

    const TimeSeriesType *target_type_;
    std::size_t mesh_depth_;
    Signature signature_;
};

struct CompiledMeshGraph {
    CompiledMapGraph map;
};

struct MeshNodeState;

class MeshRuntimeState {
    static constexpr auto no_dependency = std::numeric_limits<std::uint32_t>::max() >> 1;

    struct BranchState {
        std::uint32_t first_dependency{no_dependency};
        std::uint32_t next_queued{no_dependency};
        union {
            std::uint32_t first_incoming_dependency;
            std::uint32_t destruction_dependencies;
        };
        std::uint32_t incoming_dependencies : 31 {};
        std::uint32_t stop_queued : 1 {};

        BranchState() noexcept : first_incoming_dependency{no_dependency} {}
    };

    static_assert(sizeof(BranchState) == 4 * sizeof(std::uint32_t));

    struct DependencyEdge {
        std::uint32_t requester;
        std::uint32_t selected : 31 {no_dependency};
        std::uint32_t active : 1 {};
        std::uint32_t previous{no_dependency};
        std::uint32_t next{no_dependency};
        std::uint32_t previous_incoming{no_dependency};
        std::uint32_t next_incoming{no_dependency};
    };

    static_assert(sizeof(DependencyEdge) == 6 * sizeof(std::uint32_t));

    struct DependencySelection {
        Reference reference;
        bool pending{};
    };

    struct EvaluationFrame {
        std::uint32_t entry_index;
        std::uint32_t dependency_cursor{no_dependency};
    };

    struct CycleSearchFrame {
        std::uint32_t entry_index;
        std::uint32_t dependency_cursor;
    };

  public:
    explicit MeshRuntimeState(allocator memory)
        : memory_{memory}, branches_{memory}, edges_{&memory.memory_resource()},
          evaluation_stack_{&memory.memory_resource()}, cycle_stack_{&memory.memory_resource()},
          selections_to_release_{&memory.memory_resource()} {}

    MeshRuntimeState(const MeshRuntimeState &) = delete;
    MeshRuntimeState &operator=(const MeshRuntimeState &) = delete;

    ~MeshRuntimeState() {
        branch_states_.release_with(branch_state_positions_, memory_.memory_resource(),
                                    [&](BranchState *state) noexcept { std::destroy_at(state); });
        branch_state_positions_.release(memory_.memory_resource());
        active_evaluation_.release(memory_.memory_resource());
        cycle_visited_.release(memory_.memory_resource());
    }

    template <typename Invocation> bool evaluate(const CompiledMeshGraph &compiled, Invocation invocation) {
        active_compiled_ = &compiled;
        auto clear_active = util::scope_exit{[&]() noexcept { active_compiled_ = nullptr; }};

        if (!evaluation_stack_.empty()) {
            if (!evaluate_branches(compiled, invocation)) {
                return false;
            }

            process_stops(compiled, invocation);
            if (!evaluate_ready(compiled, invocation)) {
                return false;
            }
            process_stops(compiled, invocation);
            return true;
        }

        branches_.update(compiled.map, invocation, [&](MapEntries::Entry &entry) {
            auto &state = branch_state(entry.entry_index);
            const auto should_run = entry.presence_count != 0 || state.incoming_dependencies != 0;
            if (should_run == entry.inner_graph.running()) {
                return;
            }
            if (should_run) {
                start(entry, compiled, invocation);
            } else {
                queue_stop(entry.entry_index);
            }
        });

        process_stops(compiled, invocation);
        if (!evaluate_ready(compiled, invocation)) {
            return false;
        }
        process_stops(compiled, invocation);
        return true;
    }

    template <typename Invocation> void stop(Invocation invocation) noexcept {
        branches_.stop_all(invocation.context());
        evaluation_stack_.clear();
        active_evaluation_.clear();
        selections_to_release_.clear();
        pending_dependency_ = no_dependency;
        selection_release_pending_ = false;
        first_pending_stop_ = no_dependency;
        for (const auto entry_index : branch_state_positions_) {
            auto &state = branch_states_[entry_index];
            state.stop_queued = false;
            state.next_queued = no_dependency;
        }
    }

    template <typename Invocation> void destroy(Invocation invocation) noexcept {
        auto &entries = branches_.entries();
        branches_.stop_all(invocation.context());
        entries.clear_schedule_tracking(invocation.context());

        for (const auto item : entries.table()) {
            auto &state = existing_branch_state(item.value.entry_index);
            state.destruction_dependencies = 0;
            state.next_queued = no_dependency;
        }
        for (const auto &edge : edges_) {
            if (edge.selected != no_dependency) {
                ++existing_branch_state(edge.selected).destruction_dependencies;
            }
        }

        auto first = no_dependency;
        for (const auto item : entries.table()) {
            auto &state = existing_branch_state(item.value.entry_index);
            if (state.destruction_dependencies == 0) {
                state.next_queued = first;
                first = item.value.entry_index;
            }
        }

        destroying_ = true;
        auto destroyed = std::size_t{};
        while (first != no_dependency) {
            const auto entry_index = first;
            auto &state = existing_branch_state(entry_index);
            first = state.next_queued;
            entries.table().value_at(entry_index).inner_graph.stop_and_destroy(invocation.context());

            auto edge_index = state.first_dependency;
            while (edge_index != no_dependency) {
                auto &edge = edges_[edge_index];
                const auto next = edge.next;
                if (edge.selected != no_dependency) {
                    auto &selected = existing_branch_state(edge.selected);
                    --selected.destruction_dependencies;
                    if (selected.destruction_dependencies == 0) {
                        selected.next_queued = first;
                        first = edge.selected;
                    }
                }
                discard_edge(edge_index);
                edge_index = next;
            }
            state.first_dependency = no_dependency;
            ++destroyed;
        }
        destroying_ = false;
        assert(destroyed == entries.table().size());

        selections_to_release_.clear();
        selection_release_pending_ = false;

        auto memory = invocation.context().runtime_graph().allocation();
        memory.delete_object(this);
    }

  private:
    using Entry = MapEntries::Entry;
    using size_type = MapEntries::size_type;

    BranchState &branch_state(size_type entry_index) {
        if (auto *state = branch_states_.get(entry_index, branch_state_positions_)) {
            return *state;
        }

        branch_state_positions_.set(entry_index, memory_.memory_resource());
        auto reset_position = util::scope_exit{[&]() noexcept { branch_state_positions_.reset(entry_index); }};
        auto &result = branch_states_.emplace_at(entry_index, memory_.memory_resource());
        reset_position.release();
        return result;
    }

    BranchState &existing_branch_state(size_type entry_index) noexcept {
        auto *state = branch_states_.get(entry_index, branch_state_positions_);
        assert(state != nullptr);
        return *state;
    }

    template <typename Invocation> void start(Entry &entry, const CompiledMeshGraph &compiled, Invocation invocation) {
        const auto owner = RuntimeGraphOwner{
            .receiver = this,
            .token = entry.entry_index,
            .callbacks = &owner_callbacks,
        };
        auto section = MeshContextSection{*this, entry.entry_index};
        auto branch_context = invocation.context().child(section);
        branches_.start(entry, compiled.map, invocation, owner, branch_context);
    }

    void queue_stop(size_type entry_index) {
        auto &state = branch_state(entry_index);
        if (state.stop_queued) {
            return;
        }
        state.stop_queued = true;
        state.next_queued = first_pending_stop_;
        first_pending_stop_ = entry_index;
    }

    void release_dependency(size_type selected_index) {
        auto &selected = branch_state(selected_index);
        if (selected.incoming_dependencies == 0) {
            throw std::logic_error{"mesh dependency count underflow"};
        }

        --selected.incoming_dependencies;
        const auto &entry = branches_.entries().table().value_at(selected_index);
        if (selected.incoming_dependencies == 0 && entry.presence_count == 0) {
            queue_stop(selected_index);
        }
    }

    void queue_selection_release(size_type entry_index, EvaluationContext &context) {
        selections_to_release_.push_back(entry_index);
        if (selection_release_pending_) {
            return;
        }

        context.add_cycle_callback(this, release_stopped_selections_callback);
        selection_release_pending_ = true;
    }

    static void release_stopped_selections_callback(void *receiver) noexcept {
        static_cast<MeshRuntimeState *>(receiver)->release_stopped_selections();
    }

    void release_stopped_selections() noexcept {
        selection_release_pending_ = false;
        for (const auto entry_index : selections_to_release_) {
            auto edge_index = existing_branch_state(entry_index).first_incoming_dependency;
            while (edge_index != no_dependency) {
                const auto next = edges_[edge_index].next_incoming;
                if (!edges_[edge_index].active) {
                    unlink_selection(edge_index);
                }
                edge_index = next;
            }
        }
        selections_to_release_.clear();
    }

    template <typename Invocation> void process_stops(const CompiledMeshGraph &compiled, Invocation invocation) {
        auto stops = std::pmr::vector<size_type>{&memory_.memory_resource()};
        while (first_pending_stop_ != no_dependency) {
            const auto entry_index = first_pending_stop_;
            auto &state = branch_state(entry_index);
            first_pending_stop_ = std::exchange(state.next_queued, no_dependency);
            state.stop_queued = false;

            auto &entry = branches_.entries().table().value_at(entry_index);
            if (entry.presence_count != 0 || state.incoming_dependencies != 0 || !entry.inner_graph.running()) {
                continue;
            }

            for (auto edge_index = state.first_dependency; edge_index != no_dependency;) {
                auto &edge = edges_[edge_index];
                edge_index = edge.next;
                if (!edge.active) {
                    continue;
                }
                edge.active = false;
                release_dependency(edge.selected);
            }

            stops.push_back(entry_index);
        }
        for (auto position = stops.size(); position != 0; --position) {
            auto &entry = branches_.entries().table().value_at(stops[position - 1]);
            branches_.stop(entry, invocation.context());
            queue_selection_release(entry.entry_index, invocation.context());
            erase_output(compiled, invocation, entry);
        }
    }

    template <typename Invocation>
    bool evaluate_ready(const CompiledMeshGraph &compiled, Invocation invocation) {
        auto &ready = branches_.entries().ready();
        while (ready.begin() != ready.end()) {
            const auto entry_index = *ready.begin();
            ready.reset(entry_index);

            auto &entry = branches_.entries().table().value_at(entry_index);
            if (!entry.inner_graph.running()) {
                continue;
            }

            evaluation_stack_.clear();
            active_evaluation_.clear();
            push_evaluation(entry_index);
            if (!evaluate_branches(compiled, invocation)) {
                return false;
            }
            process_stops(compiled, invocation);
        }
        return true;
    }

    template <typename Invocation>
    bool evaluate_branches(const CompiledMeshGraph &compiled, Invocation invocation) {
        while (!evaluation_stack_.empty()) {
            auto &frame = evaluation_stack_.back();
            auto &state = branch_state(frame.entry_index);

            auto pushed_dependency = false;
            if (frame.dependency_cursor == no_dependency) {
                frame.dependency_cursor = state.first_dependency;
            }
            while (frame.dependency_cursor != no_dependency) {
                const auto edge_index = std::exchange(frame.dependency_cursor, edges_[frame.dependency_cursor].next);
                const auto &edge = edges_[edge_index];
                const auto dependency = edge.selected;
                if (!edge.active || dependency == no_dependency || !branches_.entries().ready().contains(dependency)) {
                    continue;
                }

                ensure_running(compiled, invocation, dependency);
                push_evaluation(dependency);
                pushed_dependency = true;
                break;
            }
            if (pushed_dependency) {
                continue;
            }

            auto &entry = branches_.entries().table().value_at(frame.entry_index);
            auto section = MeshContextSection{*this, frame.entry_index};
            auto branch_context = invocation.context().child(section);
            if (!entry.inner_graph.evaluate(branch_context)) {
                if (pending_dependency_ == no_dependency) {
                    return false;
                }
                const auto selected = std::exchange(pending_dependency_, no_dependency);
                ensure_running(compiled, invocation, selected);
                push_evaluation(selected);
                continue;
            }

            publish_output(compiled, invocation, entry);
            branches_.entries().ready().reset(frame.entry_index);
            active_evaluation_.reset(frame.entry_index);
            evaluation_stack_.pop_back();
        }
        return true;
    }

    template <typename Invocation>
    void ensure_running(const CompiledMeshGraph &compiled, Invocation invocation, size_type entry_index) {
        auto &entry = branches_.entries().table().value_at(entry_index);
        if (!entry.inner_graph.running()) {
            start(entry, compiled, invocation);
        }
    }

    void push_evaluation(size_type entry_index) {
        if (active_evaluation_.contains(entry_index)) {
            throw std::runtime_error{"mesh branch dependency cycle"};
        }

        active_evaluation_.set(entry_index, memory_.memory_resource());
        branches_.entries().ready().reset(entry_index);
        evaluation_stack_.push_back({entry_index, no_dependency});
    }

    DependencySelection select_dependency(size_type requester_index, std::uint32_t &edge_index, const Scalar<> &key) {
        if (active_compiled_ == nullptr) {
            throw std::logic_error{"a mesh dependency request is outside mesh evaluation"};
        }

        if (edge_index == MeshDependencyState::no_edge) {
            edge_index = add_edge(requester_index);
        } else if (edges_[edge_index].requester != requester_index) {
            throw std::logic_error{"a mesh dependency moved between requesting branches"};
        }

        auto &selected_entry = branches_.entries().require(key);
        if (selected_entry.entry_index >= no_dependency) {
            throw std::length_error{"a mesh has too many branches for runtime dependencies"};
        }
        auto &selected_branch = branch_state(selected_entry.entry_index);
        auto &edge = edges_[edge_index];
        if (edge.selected != selected_entry.entry_index || !edge.active) {
            require_acyclic(requester_index, selected_entry.entry_index);

            if (edge.selected != selected_entry.entry_index) {
                if (edge.active) {
                    edge.active = false;
                    release_dependency(edge.selected);
                }
                unlink_selection(edge_index);
                link_selection(edge_index, selected_entry.entry_index);
            }

            ++selected_branch.incoming_dependencies;
            edge.active = true;
        }

        const auto evaluation_required =
            !selected_entry.inner_graph.running() || branches_.entries().ready().contains(selected_entry.entry_index);
        if (evaluation_required) {
            pending_dependency_ = selected_entry.entry_index;
            return {
                .reference = {},
                .pending = true,
            };
        }

        const auto reference = selected_entry.inner_graph.output(active_compiled_->map.nested);
        return {.reference = reference};
    }

    void clear_dependency(std::uint32_t edge_index) {
        if (edge_index == MeshDependencyState::no_edge) {
            return;
        }
        auto &edge = edges_[edge_index];
        if (edge.active) {
            edge.active = false;
            release_dependency(edge.selected);
        }
        unlink_selection(edge_index);
    }

    void deactivate_dependency(std::uint32_t edge_index) {
        if (edge_index == MeshDependencyState::no_edge || !edges_[edge_index].active) {
            return;
        }
        auto &edge = edges_[edge_index];
        edge.active = false;
        release_dependency(edge.selected);
    }

    void remove_dependency(std::uint32_t edge_index) noexcept {
        if (edge_index == MeshDependencyState::no_edge) {
            return;
        }

        if (destroying_) {
            return;
        }

        auto &edge = edges_[edge_index];
        if (edge.active) {
            edge.active = false;
            release_dependency(edge.selected);
        }
        discard_edge(edge_index);
    }

    void link_selection(std::uint32_t edge_index, size_type selected_index) {
        auto &selected = branch_state(selected_index);
        auto &edge = edges_[edge_index];
        edge.selected = selected_index;
        edge.previous_incoming = no_dependency;
        edge.next_incoming = selected.first_incoming_dependency;
        if (edge.next_incoming != no_dependency) {
            edges_[edge.next_incoming].previous_incoming = edge_index;
        }
        selected.first_incoming_dependency = edge_index;
    }

    void unlink_selection(std::uint32_t edge_index) noexcept {
        auto &edge = edges_[edge_index];
        if (edge.selected == no_dependency) {
            return;
        }

        auto &selected = existing_branch_state(edge.selected);
        if (edge.previous_incoming == no_dependency) {
            selected.first_incoming_dependency = edge.next_incoming;
        } else {
            edges_[edge.previous_incoming].next_incoming = edge.next_incoming;
        }
        if (edge.next_incoming != no_dependency) {
            edges_[edge.next_incoming].previous_incoming = edge.previous_incoming;
        }

        edge.selected = no_dependency;
        edge.previous_incoming = no_dependency;
        edge.next_incoming = no_dependency;
    }

    void discard_edge(std::uint32_t edge_index) noexcept {
        auto &edge = edges_[edge_index];
        if (!destroying_) {
            unlink_selection(edge_index);
        }
        auto &branch = existing_branch_state(edge.requester);
        if (edge.previous == no_dependency) {
            branch.first_dependency = edge.next;
        } else {
            edges_[edge.previous].next = edge.next;
        }
        if (edge.next != no_dependency) {
            edges_[edge.next].previous = edge.previous;
        }

        edge.requester = free_edge_;
        edge.selected = no_dependency;
        edge.active = false;
        free_edge_ = edge_index;
    }

    std::uint32_t add_edge(size_type requester) {
        const auto index = [&] {
            if (free_edge_ == no_dependency) {
                if (edges_.size() >= no_dependency) {
                    throw std::length_error{"a mesh has too many runtime dependencies"};
                }
                const auto result = static_cast<std::uint32_t>(edges_.size());
                edges_.push_back({});
                return result;
            }
            const auto result = free_edge_;
            free_edge_ = edges_[result].requester;
            return result;
        }();

        auto &branch = branch_state(requester);
        auto &edge = edges_[index];
        edge = {
            .requester = requester,
            .selected = no_dependency,
            .active = false,
            .previous = no_dependency,
            .next = branch.first_dependency,
            .previous_incoming = no_dependency,
            .next_incoming = no_dependency,
        };
        if (branch.first_dependency != no_dependency) {
            edges_[branch.first_dependency].previous = index;
        }
        branch.first_dependency = index;
        return index;
    }

    void require_acyclic(size_type requester, size_type selected) {
        if (requester == selected) {
            throw std::runtime_error{"mesh branch dependency cycle"};
        }

        cycle_stack_.clear();
        cycle_visited_.clear();
        cycle_visited_.set(selected, memory_.memory_resource());
        cycle_stack_.push_back({selected, branch_state(selected).first_dependency});

        while (!cycle_stack_.empty()) {
            auto &frame = cycle_stack_.back();
            if (frame.dependency_cursor == no_dependency) {
                cycle_stack_.pop_back();
                continue;
            }

            const auto &edge = edges_[frame.dependency_cursor];
            frame.dependency_cursor = edge.next;
            const auto dependency = edge.selected;
            if (!edge.active || cycle_visited_.contains(dependency)) {
                continue;
            }
            if (dependency == requester) {
                throw std::runtime_error{"mesh branch dependency cycle"};
            }

            cycle_visited_.set(dependency, memory_.memory_resource());
            cycle_stack_.push_back({dependency, branch_state(dependency).first_dependency});
        }
    }

    template <typename Invocation>
    void publish_output(const CompiledMeshGraph &compiled, Invocation invocation, Entry &entry) {
        if (!compiled.map.nested.output_node) {
            return;
        }

        auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
        const auto key = branches_.entries().key(entry);
        auto item = output[Scalar<SCALAR>::from(key)];
        const auto result = entry.inner_graph.output(compiled.map.nested);
        if (result) {
            item.set_value(result);
        } else {
            item.invalidate();
        }
    }

    template <typename Invocation>
    void erase_output(const CompiledMeshGraph &compiled, Invocation invocation, Entry &entry) {
        if (!compiled.map.nested.output_node) {
            return;
        }

        auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
        const auto key = branches_.entries().key(entry);
        output.erase(Scalar<SCALAR>::from(key));
    }

    static void inner_graph_scheduled(void *receiver, RuntimeGraphOwnerCallbacks::token_type token) noexcept {
        auto &runtime = *static_cast<MeshRuntimeState *>(receiver);
        runtime.branches_.entries().mark_ready(static_cast<size_type>(token));
    }

    static void inner_graph_timed_schedule_changed(RuntimeGraphOwnerContext &context, void *receiver,
                                                   RuntimeGraphOwnerCallbacks::token_type token,
                                                   std::optional<EvaluationTime> time) {
        auto &runtime = *static_cast<MeshRuntimeState *>(receiver);
        runtime.branches_.entries().timed_schedule_changed(context, static_cast<size_type>(token), time);
    }

    static const RuntimeGraphOwnerCallbacks owner_callbacks;

    friend class MeshDependencyDefinition;

    allocator memory_;
    MapRuntime branches_;
    std::pmr::vector<DependencyEdge> edges_;
    std::uint32_t free_edge_{no_dependency};
    util::sparse_bunch<BranchState> branch_states_;
    util::occupancy_index branch_state_positions_;
    std::uint32_t first_pending_stop_{no_dependency};
    std::pmr::vector<EvaluationFrame> evaluation_stack_;
    util::index<> active_evaluation_;
    std::pmr::vector<CycleSearchFrame> cycle_stack_;
    util::index<> cycle_visited_;
    std::pmr::vector<size_type> selections_to_release_;
    const CompiledMeshGraph *active_compiled_{};
    size_type pending_dependency_{no_dependency};
    bool selection_release_pending_{};
    bool destroying_{};
};

const RuntimeGraphOwnerCallbacks MeshRuntimeState::owner_callbacks{
    .inner_scheduled = MeshRuntimeState::inner_graph_scheduled,
    .timed_schedule_changed = MeshRuntimeState::inner_graph_timed_schedule_changed,
};

bool MeshDependencyDefinition::evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
    const auto &section = MeshContextSection::find(invocation.context(), invocation.compiled_data().mesh_depth);
    auto &state = invocation.state();
    if (state.mesh != nullptr && state.mesh != section.mesh) {
        throw std::logic_error{"a mesh dependency changed its target mesh"};
    }
    state.mesh = section.mesh;

    const auto selected = section.mesh->select_dependency(section.branch, state.edge, dependency_key(invocation));
    if (selected.pending) {
        return false;
    }
    if (!selected.reference) {
        invocation.output().visit([](Out<REF<TS_T>> *output) { output->invalidate(); }, ddv::noop);
        return true;
    }

    invocation.output().visit([&](Out<REF<TS_T>> *output) { output->set_value(selected.reference); }, ddv::noop);
    return true;
}

void MeshDependencyDefinition::on_invalidation(NodeInvocation<compiled_data_type, state_type> invocation) {
    auto &state = invocation.state();
    if (state.mesh != nullptr) {
        state.mesh->clear_dependency(state.edge);
    }
    invocation.output().visit([](Out<REF<TS_T>> *output) { output->invalidate(); }, ddv::noop);
}

void MeshDependencyDefinition::stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
    auto &state = invocation.state();
    if (state.mesh != nullptr) {
        state.mesh->deactivate_dependency(state.edge);
    }
}

void MeshDependencyDefinition::destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
    auto &state = invocation.state();
    if (state.mesh != nullptr) {
        state.mesh->remove_dependency(state.edge);
        state = {};
    }
}

struct MeshNodeState {
    MeshRuntimeState *runtime{};
};

class MeshNodeDefinition : public MapNodeDefinition {
  public:
    using compiled_data_type = CompiledMeshGraph;
    using state_type = MeshNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_timed_scheduling = true;

    using MapNodeDefinition::MapNodeDefinition;

    compiled_data_type compile(const ResolvedSignature &signature, allocator &memory) const {
        return {
            .map = MapNodeDefinition::compile(signature, memory),
        };
    }

    static bool evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        if (state.runtime == nullptr) {
            auto memory = invocation.context().runtime_graph().allocation();
            state.runtime = memory.new_object<MeshRuntimeState>(memory);
        }
        return state.runtime->evaluate(invocation.compiled_data(), invocation);
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

WiringPoint wire_mesh_dependency(const WiringArgument &key, std::size_t mesh_depth, const TimeSeriesType &target_type) {
    const Type *key_type{};
    if (key.is_time_series()) {
        const auto &time_series = static_cast<const TimeSeriesType &>(key.type());
        const auto *value = type_cast<TimeSeriesValueType>(&time_series.target_type());
        if (value == nullptr) {
            throw WiringError{"mesh()[key] requires a scalar or scalar-value time series key"};
        }
        key_type = &time_series_value_type(value->value_type(), *value->guarantee().value());
    } else if (key.is_scalar()) {
        key_type = &key.type();
    } else {
        throw WiringError{"mesh()[key] requires a scalar or scalar-value time series key"};
    }

    auto node_template = std::make_unique<RuntimeSignatureNodeTemplate<MeshDependencyDefinition>>(
        MeshDependencyDefinition{*key_type, target_type, mesh_depth});
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    const auto result = owned(std::span<const WiringArgument>{&key, 1});
    if (!result) {
        throw WiringError{"a mesh dependency node produced no output"};
    }
    return *result;
}

std::unique_ptr<NodeTemplate> make_mesh_node(MapGraphWiring wiring, const ResolvedSignature &outer_signature) {
    return std::make_unique<RuntimeSignatureNodeTemplate<MeshNodeDefinition>>(
        MeshNodeDefinition{std::move(wiring), outer_signature});
}

} // namespace bg::details
