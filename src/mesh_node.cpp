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
    branch_id_t branch_id;

    MeshContextSection(MeshRuntimeState &mesh, branch_id_t branch_id) noexcept
        : EvaluationContextSection{nullptr, &mesh_context_section_type}, mesh{&mesh}, branch_id{branch_id} {}

    static const MeshContextSection &find(const EvaluationContext &context, std::size_t depth) {
        const auto *section = context.find_section(&mesh_context_section_type, depth);
        if (section == nullptr) {
            throw std::logic_error{"a mesh dependency has no matching evaluation context"};
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
    static Scalar<> dependency_key(NodeInvocation<compiled_data_type, state_type> invocation) {
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

struct MeshNodeState {
    MeshRuntimeState *runtime{};
};

using MeshInvocation = NodeInvocation<CompiledMeshGraph, MeshNodeState>;

class MeshRuntimeState {
    static constexpr auto no_dependency = std::numeric_limits<std::uint32_t>::max() >> 1;

    struct BranchState {
        std::uint32_t first_dependency{no_dependency};
        std::uint32_t next_queued{no_dependency};
        union {
            std::uint32_t first_dependent;
            std::uint32_t destruction_dependent_count;
        };
        std::uint32_t dependent_count : 31 {};
        std::uint32_t stop_queued : 1 {};

        BranchState() noexcept : first_dependent{no_dependency} {}
    };

    static_assert(sizeof(BranchState) == 4 * sizeof(std::uint32_t));

    struct DependencyEdge {
        union {
            std::uint32_t branch_id{};
            std::uint32_t free_next;
        };
        std::uint32_t dependency_id : 31 {no_dependency};
        std::uint32_t active : 1 {};
        std::uint32_t dependency_previous{no_dependency};
        std::uint32_t dependency_next{no_dependency};
        std::uint32_t dependent_previous{no_dependency};
        std::uint32_t dependent_next{no_dependency};
    };

    static_assert(sizeof(DependencyEdge) == 6 * sizeof(std::uint32_t));

    struct DependencyResult {
        Reference reference;
        bool pending{};
    };

    struct EvaluationFrame {
        branch_id_t branch_id;
        std::uint32_t dependency_cursor{no_dependency};
    };

    struct CycleSearchFrame {
        branch_id_t branch_id;
        std::uint32_t dependency_cursor;
    };

  public:
    explicit MeshRuntimeState(allocator memory)
        : memory_{memory}, branches_{memory}, edges_{&memory.memory_resource()},
          evaluation_stack_{&memory.memory_resource()}, cycle_stack_{&memory.memory_resource()},
          dependencies_to_unlink_{&memory.memory_resource()} {}

    MeshRuntimeState(const MeshRuntimeState &) = delete;
    MeshRuntimeState &operator=(const MeshRuntimeState &) = delete;

    ~MeshRuntimeState() {
        branch_states_.release_with(branch_state_positions_, memory_.memory_resource(),
                                    [&](BranchState *state) noexcept { std::destroy_at(state); });
        branch_state_positions_.release(memory_.memory_resource());
        active_evaluation_.release(memory_.memory_resource());
        cycle_visited_.release(memory_.memory_resource());
    }

    bool evaluate(const CompiledMeshGraph &compiled, MeshInvocation invocation) {
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
            auto &state = branch_state(entry.branch_id);
            const auto should_run = entry.presence_count != 0 || state.dependent_count != 0;
            if (should_run == entry.inner_graph.running()) {
                return;
            }
            if (should_run) {
                start(entry, compiled, invocation);
            } else {
                queue_stop(entry.branch_id);
            }
        });

        process_stops(compiled, invocation);
        if (!evaluate_ready(compiled, invocation)) {
            return false;
        }
        process_stops(compiled, invocation);
        return true;
    }

    void stop(EvaluationContext &context) noexcept {
        branches_.stop_all(context);
        evaluation_stack_.clear();
        active_evaluation_.clear();
        dependencies_to_unlink_.clear();
        pending_dependency_ = no_dependency;
        dependency_unlink_pending_ = false;
        first_pending_stop_ = no_dependency;
        for (const auto branch_id : branch_state_positions_) {
            auto &state = branch_states_[branch_id];
            state.stop_queued = false;
            state.next_queued = no_dependency;
        }
    }

    void destroy(EvaluationContext &context) noexcept {
        auto &entries = branches_.entries();
        branches_.stop_all(context);
        entries.clear_schedule_tracking(context);

        for (const auto item : entries.table()) {
            auto &state = existing_branch_state(item.value.branch_id);
            state.destruction_dependent_count = 0;
            state.next_queued = no_dependency;
        }
        for (const auto &edge : edges_) {
            if (edge.dependency_id != no_dependency) {
                ++existing_branch_state(edge.dependency_id).destruction_dependent_count;
            }
        }

        auto first = no_dependency;
        for (const auto item : entries.table()) {
            auto &state = existing_branch_state(item.value.branch_id);
            if (state.destruction_dependent_count == 0) {
                state.next_queued = first;
                first = item.value.branch_id;
            }
        }

        destroying_ = true;
        auto destroyed = std::size_t{};
        while (first != no_dependency) {
            const auto branch_id = first;
            auto &state = existing_branch_state(branch_id);
            first = state.next_queued;
            entries.table().value_at(branch_id).inner_graph.stop_and_destroy(context);

            auto edge_index = state.first_dependency;
            while (edge_index != no_dependency) {
                auto &edge = edges_[edge_index];
                const auto next = edge.dependency_next;
                if (edge.dependency_id != no_dependency) {
                    auto &dependency = existing_branch_state(edge.dependency_id);
                    --dependency.destruction_dependent_count;
                    if (dependency.destruction_dependent_count == 0) {
                        dependency.next_queued = first;
                        first = edge.dependency_id;
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

        dependencies_to_unlink_.clear();
        dependency_unlink_pending_ = false;

        auto memory = context.runtime_graph().allocation();
        memory.delete_object(this);
    }

  private:
    using Entry = MapEntries::Entry;
    BranchState &branch_state(branch_id_t branch_id) {
        if (auto *state = branch_states_.get(branch_id, branch_state_positions_)) {
            return *state;
        }

        branch_state_positions_.set(branch_id, memory_.memory_resource());
        auto reset_position = util::scope_exit{[&]() noexcept { branch_state_positions_.reset(branch_id); }};
        auto &result = branch_states_.emplace_at(branch_id, memory_.memory_resource());
        reset_position.release();
        return result;
    }

    BranchState &existing_branch_state(branch_id_t branch_id) noexcept {
        auto *state = branch_states_.get(branch_id, branch_state_positions_);
        assert(state != nullptr);
        return *state;
    }

    void start(Entry &entry, const CompiledMeshGraph &compiled, MeshInvocation invocation) {
        const auto owner = RuntimeGraphOwner{
            .receiver = this,
            .token = entry.branch_id,
            .callbacks = &owner_callbacks,
        };
        auto section = MeshContextSection{*this, entry.branch_id};
        auto branch_context = invocation.context().child(section);
        branches_.start(entry, compiled.map, invocation, owner, branch_context);
    }

    void queue_stop(branch_id_t branch_id) {
        auto &state = branch_state(branch_id);
        if (state.stop_queued) {
            return;
        }
        state.stop_queued = true;
        state.next_queued = first_pending_stop_;
        first_pending_stop_ = branch_id;
    }

    void release_dependency(branch_id_t dependency_id) {
        auto &dependency = branch_state(dependency_id);
        if (dependency.dependent_count == 0) {
            throw std::logic_error{"mesh dependency count underflow"};
        }

        --dependency.dependent_count;
        const auto &entry = branches_.entries().table().value_at(dependency_id);
        if (dependency.dependent_count == 0 && entry.presence_count == 0) {
            queue_stop(dependency_id);
        }
    }

    void queue_dependency_unlink(branch_id_t branch_id, EvaluationContext &context) {
        dependencies_to_unlink_.push_back(branch_id);
        if (dependency_unlink_pending_) {
            return;
        }

        context.add_cycle_callback(this, unlink_stopped_dependencies_callback);
        dependency_unlink_pending_ = true;
    }

    static void unlink_stopped_dependencies_callback(void *receiver) noexcept {
        static_cast<MeshRuntimeState *>(receiver)->unlink_stopped_dependencies();
    }

    void unlink_stopped_dependencies() noexcept {
        dependency_unlink_pending_ = false;
        for (const auto branch_id : dependencies_to_unlink_) {
            auto edge_index = existing_branch_state(branch_id).first_dependent;
            while (edge_index != no_dependency) {
                const auto next = edges_[edge_index].dependent_next;
                if (!edges_[edge_index].active) {
                    unlink_dependency(edge_index);
                }
                edge_index = next;
            }
        }
        dependencies_to_unlink_.clear();
    }

    void process_stops(const CompiledMeshGraph &compiled, MeshInvocation invocation) {
        auto stops = std::pmr::vector<branch_id_t>{&memory_.memory_resource()};
        while (first_pending_stop_ != no_dependency) {
            const auto branch_id = first_pending_stop_;
            auto &state = branch_state(branch_id);
            first_pending_stop_ = std::exchange(state.next_queued, no_dependency);
            state.stop_queued = false;

            auto &entry = branches_.entries().table().value_at(branch_id);
            if (entry.presence_count != 0 || state.dependent_count != 0 || !entry.inner_graph.running()) {
                continue;
            }

            for (auto edge_index = state.first_dependency; edge_index != no_dependency;) {
                auto &edge = edges_[edge_index];
                edge_index = edge.dependency_next;
                if (!edge.active) {
                    continue;
                }
                edge.active = false;
                release_dependency(edge.dependency_id);
            }

            stops.push_back(branch_id);
        }
        for (auto position = stops.size(); position != 0; --position) {
            auto &entry = branches_.entries().table().value_at(stops[position - 1]);
            branches_.stop(entry, invocation.context());
            queue_dependency_unlink(entry.branch_id, invocation.context());
            erase_output(compiled, invocation, entry);
        }
    }

    bool evaluate_ready(const CompiledMeshGraph &compiled, MeshInvocation invocation) {
        auto &ready = branches_.entries().ready();
        while (ready.begin() != ready.end()) {
            const auto branch_id = *ready.begin();
            ready.reset(branch_id);

            auto &entry = branches_.entries().table().value_at(branch_id);
            if (!entry.inner_graph.running()) {
                continue;
            }

            evaluation_stack_.clear();
            active_evaluation_.clear();
            push_evaluation(branch_id);
            if (!evaluate_branches(compiled, invocation)) {
                return false;
            }
            process_stops(compiled, invocation);
        }
        return true;
    }

    bool evaluate_branches(const CompiledMeshGraph &compiled, MeshInvocation invocation) {
        while (!evaluation_stack_.empty()) {
            auto &frame = evaluation_stack_.back();
            auto &state = branch_state(frame.branch_id);

            auto pushed_dependency = false;
            if (frame.dependency_cursor == no_dependency) {
                frame.dependency_cursor = state.first_dependency;
            }
            while (frame.dependency_cursor != no_dependency) {
                const auto edge_index =
                    std::exchange(frame.dependency_cursor, edges_[frame.dependency_cursor].dependency_next);
                const auto &edge = edges_[edge_index];
                const auto dependency = edge.dependency_id;
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

            auto &entry = branches_.entries().table().value_at(frame.branch_id);
            auto section = MeshContextSection{*this, frame.branch_id};
            auto branch_context = invocation.context().child(section);
            if (!entry.inner_graph.evaluate(branch_context)) {
                if (pending_dependency_ == no_dependency) {
                    return false;
                }
                const auto dependency_id = std::exchange(pending_dependency_, no_dependency);
                ensure_running(compiled, invocation, dependency_id);
                push_evaluation(dependency_id);
                continue;
            }

            publish_output(compiled, invocation, entry);
            branches_.entries().ready().reset(frame.branch_id);
            active_evaluation_.reset(frame.branch_id);
            evaluation_stack_.pop_back();
        }
        return true;
    }

    void ensure_running(const CompiledMeshGraph &compiled, MeshInvocation invocation, branch_id_t branch_id) {
        auto &entry = branches_.entries().table().value_at(branch_id);
        if (!entry.inner_graph.running()) {
            start(entry, compiled, invocation);
        }
    }

    void push_evaluation(branch_id_t branch_id) {
        if (active_evaluation_.contains(branch_id)) {
            throw std::runtime_error{"mesh branch dependency cycle"};
        }

        active_evaluation_.set(branch_id, memory_.memory_resource());
        branches_.entries().ready().reset(branch_id);
        evaluation_stack_.push_back({branch_id, no_dependency});
    }

    DependencyResult resolve_dependency(branch_id_t branch_id, std::uint32_t &edge_index, const Scalar<> &key) {
        if (active_compiled_ == nullptr) {
            throw std::logic_error{"a mesh dependency request is outside mesh evaluation"};
        }

        if (edge_index == MeshDependencyState::no_edge) {
            edge_index = add_edge(branch_id);
        } else if (edges_[edge_index].branch_id != branch_id) {
            throw std::logic_error{"a mesh dependency moved between branches"};
        }

        auto &dependency_entry = branches_.entries().require(key);
        if (dependency_entry.branch_id >= no_dependency) {
            throw std::length_error{"a mesh has too many branches for runtime dependencies"};
        }
        auto &dependency = branch_state(dependency_entry.branch_id);
        auto &edge = edges_[edge_index];
        if (edge.dependency_id != dependency_entry.branch_id || !edge.active) {
            require_acyclic(branch_id, dependency_entry.branch_id);

            if (edge.dependency_id != dependency_entry.branch_id) {
                if (edge.active) {
                    edge.active = false;
                    release_dependency(edge.dependency_id);
                }
                unlink_dependency(edge_index);
                link_dependency(edge_index, dependency_entry.branch_id);
            }

            ++dependency.dependent_count;
            edge.active = true;
        }

        const auto evaluation_required =
            !dependency_entry.inner_graph.running() || branches_.entries().ready().contains(dependency_entry.branch_id);
        if (evaluation_required) {
            pending_dependency_ = dependency_entry.branch_id;
            return {
                .reference = {},
                .pending = true,
            };
        }

        const auto reference = dependency_entry.inner_graph.output(active_compiled_->map.nested);
        return {.reference = reference};
    }

    void clear_dependency(std::uint32_t edge_index) {
        if (edge_index == MeshDependencyState::no_edge) {
            return;
        }
        auto &edge = edges_[edge_index];
        if (edge.active) {
            edge.active = false;
            release_dependency(edge.dependency_id);
        }
        unlink_dependency(edge_index);
    }

    void deactivate_dependency(std::uint32_t edge_index) {
        if (edge_index == MeshDependencyState::no_edge || !edges_[edge_index].active) {
            return;
        }
        auto &edge = edges_[edge_index];
        edge.active = false;
        release_dependency(edge.dependency_id);
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
            release_dependency(edge.dependency_id);
        }
        discard_edge(edge_index);
    }

    void link_dependency(std::uint32_t edge_index, branch_id_t dependency_id) {
        auto &dependency = branch_state(dependency_id);
        auto &edge = edges_[edge_index];
        edge.dependency_id = dependency_id;
        edge.dependent_previous = no_dependency;
        edge.dependent_next = dependency.first_dependent;
        if (edge.dependent_next != no_dependency) {
            edges_[edge.dependent_next].dependent_previous = edge_index;
        }
        dependency.first_dependent = edge_index;
    }

    void unlink_dependency(std::uint32_t edge_index) noexcept {
        auto &edge = edges_[edge_index];
        if (edge.dependency_id == no_dependency) {
            return;
        }

        auto &dependency = existing_branch_state(edge.dependency_id);
        if (edge.dependent_previous == no_dependency) {
            dependency.first_dependent = edge.dependent_next;
        } else {
            edges_[edge.dependent_previous].dependent_next = edge.dependent_next;
        }
        if (edge.dependent_next != no_dependency) {
            edges_[edge.dependent_next].dependent_previous = edge.dependent_previous;
        }

        edge.dependency_id = no_dependency;
        edge.dependent_previous = no_dependency;
        edge.dependent_next = no_dependency;
    }

    void discard_edge(std::uint32_t edge_index) noexcept {
        auto &edge = edges_[edge_index];
        if (!destroying_) {
            unlink_dependency(edge_index);
        }
        auto &branch = existing_branch_state(edge.branch_id);
        if (edge.dependency_previous == no_dependency) {
            branch.first_dependency = edge.dependency_next;
        } else {
            edges_[edge.dependency_previous].dependency_next = edge.dependency_next;
        }
        if (edge.dependency_next != no_dependency) {
            edges_[edge.dependency_next].dependency_previous = edge.dependency_previous;
        }

        edge.free_next = free_edge_;
        edge.dependency_id = no_dependency;
        edge.active = false;
        free_edge_ = edge_index;
    }

    std::uint32_t add_edge(branch_id_t branch_id) {
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
            free_edge_ = edges_[result].free_next;
            return result;
        }();

        auto &branch = branch_state(branch_id);
        auto &edge = edges_[index];
        edge = {
            .branch_id = branch_id,
            .dependency_id = no_dependency,
            .active = false,
            .dependency_previous = no_dependency,
            .dependency_next = branch.first_dependency,
            .dependent_previous = no_dependency,
            .dependent_next = no_dependency,
        };
        if (branch.first_dependency != no_dependency) {
            edges_[branch.first_dependency].dependency_previous = index;
        }
        branch.first_dependency = index;
        return index;
    }

    void require_acyclic(branch_id_t branch_id, branch_id_t dependency_id) {
        if (branch_id == dependency_id) {
            throw std::runtime_error{"mesh branch dependency cycle"};
        }

        cycle_stack_.clear();
        cycle_visited_.clear();
        cycle_visited_.set(dependency_id, memory_.memory_resource());
        cycle_stack_.push_back({dependency_id, branch_state(dependency_id).first_dependency});

        while (!cycle_stack_.empty()) {
            auto &frame = cycle_stack_.back();
            if (frame.dependency_cursor == no_dependency) {
                cycle_stack_.pop_back();
                continue;
            }

            const auto &edge = edges_[frame.dependency_cursor];
            frame.dependency_cursor = edge.dependency_next;
            const auto dependency = edge.dependency_id;
            if (!edge.active || cycle_visited_.contains(dependency)) {
                continue;
            }
            if (dependency == branch_id) {
                throw std::runtime_error{"mesh branch dependency cycle"};
            }

            cycle_visited_.set(dependency, memory_.memory_resource());
            cycle_stack_.push_back({dependency, branch_state(dependency).first_dependency});
        }
    }

    void publish_output(const CompiledMeshGraph &compiled, MeshInvocation invocation, Entry &entry) {
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

    void erase_output(const CompiledMeshGraph &compiled, MeshInvocation invocation, Entry &entry) {
        if (!compiled.map.nested.output_node) {
            return;
        }

        auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
        const auto key = branches_.entries().key(entry);
        output.erase(Scalar<SCALAR>::from(key));
    }

    static void inner_graph_scheduled(void *receiver, RuntimeGraphOwnerCallbacks::token_type token) noexcept {
        auto &runtime = *static_cast<MeshRuntimeState *>(receiver);
        runtime.branches_.entries().mark_ready(static_cast<branch_id_t>(token));
    }

    static void inner_graph_timed_schedule_changed(RuntimeGraphOwnerContext &context, void *receiver,
                                                   RuntimeGraphOwnerCallbacks::token_type token,
                                                   std::optional<EvaluationTime> time) {
        auto &runtime = *static_cast<MeshRuntimeState *>(receiver);
        runtime.branches_.entries().timed_schedule_changed(context, static_cast<branch_id_t>(token), time);
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
    std::pmr::vector<branch_id_t> dependencies_to_unlink_;
    const CompiledMeshGraph *active_compiled_{};
    branch_id_t pending_dependency_{no_dependency};
    bool dependency_unlink_pending_{};
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

    const auto dependency = section.mesh->resolve_dependency(section.branch_id, state.edge, dependency_key(invocation));
    if (dependency.pending) {
        return false;
    }
    if (!dependency.reference) {
        invocation.output().visit([](Out<REF<TS_T>> *output) { output->invalidate(); }, ddv::noop);
        return true;
    }

    invocation.output().visit([&](Out<REF<TS_T>> *output) { output->set_value(dependency.reference); }, ddv::noop);
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
