#include <bg/graph/details/map_node.hpp>

#include <bg/allocator.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/nested_graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/index.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {

namespace {

struct CompiledMapInput {
    MapInputKind kind;
    std::uint32_t parameter_index;
    std::uint32_t inner_input_index;
};

struct CompiledMapGraph {
    CompiledNestedGraph nested;
    std::pmr::vector<CompiledMapInput> inputs;
    std::optional<std::uint32_t> key_set_parameter{};
};

struct MapNodeIdentity {
    GraphWiringStructure graph;
    std::vector<std::size_t> input_metadata;
    std::optional<std::size_t> key_set_parameter;

    std::size_t hash() const noexcept {
        auto result = graph.hash();
        const auto combine = [&](std::size_t value) {
            result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        };
        for (const auto value : input_metadata) {
            combine(value);
        }
        combine(key_set_parameter.value_or(std::numeric_limits<std::size_t>::max()));
        return result;
    }

    bool cacheable() const noexcept { return graph.cacheable(); }

    bool operator==(const MapNodeIdentity &) const = default;
};

struct ScalarValueHash {
    using is_transparent = void;

    std::size_t operator()(const ScalarValue<> &value) const noexcept { return value.hash(); }

    template <typename T> std::size_t operator()(const Scalar<T> &value) const noexcept {
        return Scalar<>::from(value).hash();
    }
};

struct ScalarValueEqual {
    using is_transparent = void;

    template <typename Left, typename Right>
    bool operator()(const Left &left, const Right &right) const noexcept {
        return Scalar<>::from(left).equals(Scalar<>::from(right));
    }
};

struct MapEntry {
    static constexpr auto not_in_schedule_heap = std::numeric_limits<std::size_t>::max();

    NestedGraph graph;
    EvaluationTime scheduled_time{MIN_DT};
    std::size_t schedule_heap_index{not_in_schedule_heap};
    std::uint32_t position{};
    std::uint32_t presence_count : 31 {};
    std::uint32_t touched : 1 {};
};

struct PendingBoundaryUpdate {
    MapEntry *entry;
    std::uint32_t input_index;
};

class MapRuntimeState {
  public:
    explicit MapRuntimeState(allocator memory)
        : memory_{memory}, schedule_heap_{&memory.memory_resource()}, touched_{&memory.memory_resource()},
          boundary_updates_{&memory.memory_resource()}, nested_inputs_{&memory.memory_resource()},
          stopped_entries_{&memory.memory_resource()} {}

    MapRuntimeState(const MapRuntimeState &) = delete;
    MapRuntimeState &operator=(const MapRuntimeState &) = delete;

    ~MapRuntimeState() {
        ready_.release(memory_.memory_resource());
        entries_.release(memory_.memory_resource());
    }

    template <typename Invocation> void evaluate(const CompiledMapGraph &compiled, Invocation invocation) {
        activate_due(invocation.context());

        process_key_changes(compiled, invocation);
        reconcile_keys(compiled, invocation);
        update_boundaries(compiled, invocation);
        update_pass_through_inputs(compiled, invocation);
        evaluate_ready(compiled, invocation);
    }

    template <typename Invocation> void stop(const CompiledMapGraph &, Invocation invocation) noexcept {
        clear_schedule_tracking(invocation.context());

        for (auto [key, entry] : entries_) {
            if (entry.graph.running()) {
                entry.graph.stop(invocation.context());
                ready_.reset(entry.position);
            }
        }
    }

    template <typename Invocation> void destroy(Invocation invocation) noexcept {
        clear_schedule_tracking(invocation.context());
        for (auto [key, entry] : entries_) {
            entry.graph.stop_and_destroy(invocation.context());
        }

        auto memory = invocation.context().runtime_graph().allocation();
        memory.delete_object(this);
    }

    static void inner_graph_scheduled(void *receiver, RuntimeGraphOwnerCallbacks::token_type token) noexcept {
        auto &state = *static_cast<MapRuntimeState *>(receiver);
        state.mark_ready(static_cast<Entries::size_type>(token));
    }

    static void inner_graph_timed_schedule_changed(RuntimeGraphOwnerContext &context, void *receiver,
                                                   RuntimeGraphOwnerCallbacks::token_type token,
                                                   std::optional<EvaluationTime> time) {
        auto &state = *static_cast<MapRuntimeState *>(receiver);
        state.set_scheduled_time(context, state.entries_.value_at(static_cast<Entries::size_type>(token)), time);
    }

    static constexpr RuntimeGraphOwnerCallbacks owner_callbacks{
        .inner_scheduled = inner_graph_scheduled,
        .timed_schedule_changed = inner_graph_timed_schedule_changed,
    };

  private:
    using Entries = util::hash_dictionary<ScalarValue<>, MapEntry, ScalarValueHash, ScalarValueEqual>;

    template <typename Invocation> static In<TS_T> input(Invocation invocation, std::size_t parameter_index) {
        return node_invocation_access::input<In<TS_T>>(invocation, parameter_index, parameter_index);
    }

    template <typename Key> MapEntry &require_entry(const Key &key) {
        if (auto *entry = entries_.find(key)) {
            return *entry;
        }

        auto stored_key = ScalarValue<>{Scalar<>::from(key), memory_};
        auto [entry, inserted] = entries_.try_emplace(std::move(stored_key), memory_.memory_resource());
        (void)inserted;
        entry->position = entries_.find_position(key);
        // Owner scheduling callbacks are noexcept, so allocate the ready bit before the graph can invoke them.
        ready_.grow(entry->position + 1, memory_.memory_resource());
        return *entry;
    }

    template <typename Key> MapEntry *find_entry(const Key &key) { return entries_.find(key); }

    void touch(MapEntry &entry) {
        if (entry.touched) {
            return;
        }
        entry.touched = true;
        touched_.push_back(&entry);
    }

    template <typename Key> void remove_presence(const Key &key) {
        auto *entry = find_entry(key);
        if (entry == nullptr || entry->presence_count == 0) {
            return;
        }
        --entry->presence_count;
        touch(*entry);
    }

    template <typename Key> void set_key_set_presence(const Key &key, bool present) {
        if (present) {
            auto &entry = require_entry(key);
            entry.presence_count = 1;
            touch(entry);
        } else if (auto *entry = find_entry(key)) {
            entry->presence_count = 0;
            touch(*entry);
        }
    }

    template <typename Function> static void visit_added_keys(const In<TS_T> &input, Function &&function) {
        input.visit([&](const In<TSS<SCALAR>> *set) { set->for_each_added(function); },
                    [&](const In<TSD<SCALAR, TS_T>> *dictionary) { dictionary->for_each_added(function); }, ddv::noop);
    }

    template <typename Function> static void visit_removed_keys(const In<TS_T> &input, Function &&function) {
        input.visit([&](const In<TSS<SCALAR>> *set) { set->for_each_removed(function); },
                    [&](const In<TSD<SCALAR, TS_T>> *dictionary) { dictionary->for_each_removed(function); },
                    ddv::noop);
    }

    template <typename Invocation> void process_key_changes(const CompiledMapGraph &compiled, Invocation invocation) {
        touched_.clear();
        boundary_updates_.clear();

        if (compiled.key_set_parameter) {
            const auto key_set = input(invocation, *compiled.key_set_parameter);
            if (key_set.modified()) {
                visit_removed_keys(key_set, [&](const auto &key) { set_key_set_presence(key, false); });
                visit_added_keys(key_set, [&](const auto &key) { set_key_set_presence(key, true); });
            }
        }

        for (auto input_index = std::size_t{}; input_index < compiled.inputs.size(); ++input_index) {
            const auto &description = compiled.inputs[input_index];
            if (description.kind != MapInputKind::mapped) {
                continue;
            }

            const auto value = input(invocation, description.parameter_index);
            if (!value.modified()) {
                continue;
            }

            value.visit(
                [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
                    dictionary->for_each_removed([&](const auto &key) {
                        if (!compiled.key_set_parameter) {
                            remove_presence(key);
                        }
                        if (auto *entry = find_entry(key)) {
                            boundary_updates_.push_back({entry, static_cast<std::uint32_t>(input_index)});
                        }
                    });
                    dictionary->for_each_added([&](const auto &key) {
                        auto &entry = require_entry(key);
                        if (!compiled.key_set_parameter) {
                            ++entry.presence_count;
                            touch(entry);
                        }
                        boundary_updates_.push_back({&entry, static_cast<std::uint32_t>(input_index)});
                    });
                    dictionary->for_each_modified([&](const auto &key) {
                        if (auto *entry = find_entry(key)) {
                            boundary_updates_.push_back({entry, static_cast<std::uint32_t>(input_index)});
                        }
                    });
                },
                ddv::noop);
        }
    }

    template <typename Invocation> void reconcile_keys(const CompiledMapGraph &compiled, Invocation invocation) {
        for (auto *entry : touched_) {
            entry->touched = false;
            const auto should_run = entry->presence_count != 0;
            if (should_run == entry->graph.running()) {
                continue;
            }

            if (should_run) {
                start_inner_graph(compiled, invocation, *entry);
            } else {
                stop_inner_graph(compiled, invocation, *entry);
            }
        }
        touched_.clear();
    }

    template <typename Invocation>
    void start_inner_graph(const CompiledMapGraph &compiled, Invocation invocation, MapEntry &entry) {
        nested_inputs_.clear();
        nested_inputs_.reserve(compiled.inputs.size());
        for (const auto &description : compiled.inputs) {
            const auto outer = input(invocation, description.parameter_index);
            nested_inputs_.push_back(description.kind == MapInputKind::mapped
                                         ? mapped_reference(outer, entries_.key_at(entry.position).view())
                                         : pass_through_reference(outer));
        }

        auto rollback = util::scope_exit{[&]() noexcept { entry.graph.stop_and_destroy(invocation.context()); }};
        const auto owner = RuntimeGraphOwner{
            .receiver = this,
            .token = entry.position,
            .callbacks = &owner_callbacks,
        };
        if (compiled.nested.constant_input_nodes.empty()) {
            entry.graph.start(compiled.nested, invocation.context(), nested_inputs_, owner);
        } else {
            const auto constant = NestedGraphConstantInput{entries_.key_at(entry.position).view()};
            entry.graph.start(compiled.nested, invocation.context(), nested_inputs_,
                              std::span<const NestedGraphConstantInput>{&constant, 1}, owner);
        }
        mark_ready(entry.position);
        rollback.release();
    }

    template <typename Invocation>
    void stop_inner_graph(const CompiledMapGraph &compiled, Invocation invocation, MapEntry &entry) {
        stop_inner_graph(invocation.context(), entry);

        if (compiled.nested.output_node) {
            auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
            const auto key = entries_.key_at(entry.position).view();
            output.erase(Scalar<SCALAR>::from(key));
        }
    }

    void stop_inner_graph(EvaluationContext &context, MapEntry &entry) {
        if (!stopped_entry_cleanup_pending_) {
            context.add_cycle_callback(this, destroy_stopped_entries_callback);
            stopped_entry_cleanup_pending_ = true;
        }

        entry.graph.stop(context);
        ready_.reset(entry.position);
        stopped_entries_.push_back(entry.position);
    }

    static NestedGraphInput pass_through_reference(const In<TS_T> &input) {
        return *input.visit(
            [](const In<REF<TS_T>> *reference) {
                return NestedGraphInput{reference->valid() ? reference->value() : Reference{},
                                        reference->valid() ? reference->last_updated() : MIN_DT};
            },
            ddv::noop);
    }

    static NestedGraphInput mapped_reference(const In<TS_T> &input, const Scalar<> &key) {
        return *input.visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
                const auto typed_key = Scalar<SCALAR>::from(key);
                if (!dictionary->contains(typed_key)) {
                    return NestedGraphInput{Reference{}, MIN_DT};
                }

                const auto item = (*dictionary)[typed_key];
                return *item.visit(
                    [](const In<REF<TS_T>> *reference) {
                        return NestedGraphInput{reference->valid() ? reference->value() : Reference{},
                                                reference->valid() ? reference->last_updated() : MIN_DT};
                    },
                    ddv::noop);
            },
            ddv::noop);
    }

    template <typename Invocation> void update_boundaries(const CompiledMapGraph &compiled, Invocation invocation) {
        for (const auto update : boundary_updates_) {
            auto &entry = *update.entry;
            if (!entry.graph.running()) {
                continue;
            }

            const auto &description = compiled.inputs[update.input_index];
            const auto outer = input(invocation, description.parameter_index);
            entry.graph.update_input(compiled.nested, description.inner_input_index,
                                     mapped_reference(outer, entries_.key_at(entry.position).view()));
            mark_ready(entry.position);
        }
    }

    template <typename Invocation>
    void update_pass_through_inputs(const CompiledMapGraph &compiled, Invocation invocation) {
        for (const auto &description : compiled.inputs) {
            if (description.kind != MapInputKind::pass_through) {
                continue;
            }

            const auto outer = input(invocation, description.parameter_index);
            if (!outer.modified()) {
                continue;
            }
            const auto reference = pass_through_reference(outer);
            for (auto [key, entry] : entries_) {
                if (!entry.graph.running()) {
                    continue;
                }
                entry.graph.update_input(compiled.nested, description.inner_input_index, reference);
                mark_ready(entry.position);
            }
        }
    }

    void mark_ready(Entries::size_type position) noexcept {
        auto &entry = entries_.value_at(position);
        if (!entry.graph.running()) {
            return;
        }
        ready_.set(position, memory_.memory_resource());
    }

    template <typename Invocation> void evaluate_ready(const CompiledMapGraph &compiled, Invocation invocation) {
        for (auto position = ready_.begin(); position != ready_.end();) {
            const auto current = *position;
            ++position;
            ready_.reset(current);

            auto &entry = entries_.value_at(current);
            if (!entry.graph.running()) {
                continue;
            }

            entry.graph.evaluate(invocation.context());
            if (!compiled.nested.output_node) {
                continue;
            }

            auto output = node_invocation_access::output<TSD<SCALAR, REF<TS_T>>>(invocation);
            const auto key = entries_.key_at(current).view();
            auto item = output[Scalar<SCALAR>::from(key)];
            const auto result = entry.graph.output(compiled.nested);
            if (result) {
                item.set_value(result);
            } else {
                item.invalidate();
            }
        }
    }

    static void destroy_stopped_entries_callback(void *receiver) noexcept {
        static_cast<MapRuntimeState *>(receiver)->destroy_stopped_entries();
    }

    void destroy_stopped_entries() noexcept {
        stopped_entry_cleanup_pending_ = false;
        for (const auto position : stopped_entries_) {
            entries_.erase(entries_.key_at(position).view(), memory_.memory_resource());
        }
        stopped_entries_.clear();
    }

    void clear_schedule_tracking(EvaluationContext &context) noexcept {
        if (outer_scheduled_time_) {
            context.cancel(outer_schedule_);
            outer_schedule_ = {};
            outer_scheduled_time_.reset();
        }
        for (auto *entry : schedule_heap_) {
            entry->schedule_heap_index = MapEntry::not_in_schedule_heap;
            entry->scheduled_time = MIN_DT;
        }
        schedule_heap_.clear();
    }

    static bool scheduled_before(const MapEntry *left, const MapEntry *right) noexcept {
        return left->scheduled_time < right->scheduled_time;
    }

    void swap_schedule_heap_items(std::size_t left, std::size_t right) noexcept {
        std::swap(schedule_heap_[left], schedule_heap_[right]);
        schedule_heap_[left]->schedule_heap_index = left;
        schedule_heap_[right]->schedule_heap_index = right;
    }

    void move_up_schedule_heap(std::size_t index) noexcept {
        while (index != 0) {
            const auto parent = (index - 1) / 2;
            if (!scheduled_before(schedule_heap_[index], schedule_heap_[parent])) {
                break;
            }
            swap_schedule_heap_items(index, parent);
            index = parent;
        }
    }

    void move_down_schedule_heap(std::size_t index) noexcept {
        while (true) {
            const auto left = index * 2 + 1;
            if (left >= schedule_heap_.size()) {
                return;
            }
            const auto right = left + 1;
            const auto first =
                right < schedule_heap_.size() && scheduled_before(schedule_heap_[right], schedule_heap_[left]) ? right
                                                                                                               : left;
            if (!scheduled_before(schedule_heap_[first], schedule_heap_[index])) {
                return;
            }
            swap_schedule_heap_items(index, first);
            index = first;
        }
    }

    void remove_from_schedule_heap(MapEntry &entry) noexcept {
        if (entry.schedule_heap_index == MapEntry::not_in_schedule_heap) {
            return;
        }
        const auto index = entry.schedule_heap_index;
        entry.schedule_heap_index = MapEntry::not_in_schedule_heap;
        if (index == schedule_heap_.size() - 1) {
            schedule_heap_.pop_back();
            return;
        }

        schedule_heap_[index] = schedule_heap_.back();
        schedule_heap_.pop_back();
        schedule_heap_[index]->schedule_heap_index = index;
        move_up_schedule_heap(index);
        move_down_schedule_heap(schedule_heap_[index]->schedule_heap_index);
    }

    void set_scheduled_time(RuntimeGraphOwnerContext &context, MapEntry &entry, std::optional<EvaluationTime> time) {
        const auto previous_first = schedule_heap_.empty()
                                        ? std::optional<EvaluationTime>{}
                                        : std::optional<EvaluationTime>{schedule_heap_.front()->scheduled_time};
        remove_from_schedule_heap(entry);
        entry.scheduled_time = time.value_or(MIN_DT);
        if (time) {
            entry.schedule_heap_index = schedule_heap_.size();
            schedule_heap_.push_back(&entry);
            move_up_schedule_heap(entry.schedule_heap_index);
        }

        const auto next_first = schedule_heap_.empty()
                                    ? std::optional<EvaluationTime>{}
                                    : std::optional<EvaluationTime>{schedule_heap_.front()->scheduled_time};
        if (next_first == previous_first) {
            return;
        }
        if (outer_scheduled_time_) {
            context.cancel(outer_schedule_);
            outer_schedule_ = {};
        }
        outer_scheduled_time_ = next_first;
        if (next_first) {
            outer_schedule_ = context.schedule_at(*next_first);
        }
    }

    void activate_due(EvaluationContext &context) {
        const auto evaluation_time = context.evaluation_time();
        if (outer_scheduled_time_ && *outer_scheduled_time_ <= evaluation_time) {
            outer_schedule_ = {};
            outer_scheduled_time_.reset();
        }
        while (!schedule_heap_.empty() && schedule_heap_.front()->scheduled_time <= evaluation_time) {
            auto *entry = schedule_heap_.front();
            remove_from_schedule_heap(*entry);
            entry->scheduled_time = MIN_DT;
            mark_ready(entry->position);
        }
        if (!outer_scheduled_time_ && !schedule_heap_.empty()) {
            outer_scheduled_time_ = schedule_heap_.front()->scheduled_time;
            outer_schedule_ = context.schedule_at(*outer_scheduled_time_);
        }
    }

    allocator memory_;
    Entries entries_;
    std::pmr::vector<MapEntry *> schedule_heap_;
    std::pmr::vector<MapEntry *> touched_;
    std::pmr::vector<PendingBoundaryUpdate> boundary_updates_;
    std::pmr::vector<NestedGraphInput> nested_inputs_;
    util::index<> ready_;
    std::pmr::vector<Entries::size_type> stopped_entries_;
    ScheduleHandle outer_schedule_{};
    std::optional<EvaluationTime> outer_scheduled_time_;
    bool stopped_entry_cleanup_pending_{};
};

static_assert(sizeof(MapEntry) <= 5 * sizeof(void *));

struct MapNodeState {
    MapRuntimeState *runtime{};
};

class MapNodeDefinition {
  public:
    using compiled_data_type = CompiledMapGraph;
    using state_type = MapNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_timed_scheduling = true;

    MapNodeDefinition(MapGraphWiring wiring, const ResolvedSignature &outer_signature)
        : wiring_{std::move(wiring)} {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(outer_signature.parameters.size());
        for (const auto &parameter : outer_signature.parameters) {
            if (!parameter.type->is_time_series()) {
                throw std::invalid_argument{"a map runtime node accepts only time-series parameters"};
            }
            parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                                  parameter.accepts_absent_scalar});
        }
        if (outer_signature.output == nullptr) {
            signature_ = std::make_unique<Signature>(outer_signature.name, std::move(parameters));
        } else {
            signature_ =
                std::make_unique<Signature>(outer_signature.name, std::move(parameters), *outer_signature.output);
        }
        validate();
    }

    const Signature &signature() const noexcept { return *signature_; }

    NodeTemplateIdentity definition_identity() const {
        auto boundaries = std::vector<std::size_t>{};
        boundaries.reserve(wiring_.inputs.size() + static_cast<std::size_t>(wiring_.key_input_node.has_value()));
        for (const auto &input : wiring_.inputs) {
            boundaries.push_back(input.input_node);
        }
        if (wiring_.key_input_node) {
            boundaries.push_back(*wiring_.key_input_node);
        }
        auto input_metadata = std::vector<std::size_t>{};
        input_metadata.reserve(wiring_.inputs.size());
        for (const auto &input : wiring_.inputs) {
            input_metadata.push_back(input.parameter_index * 2 + static_cast<std::size_t>(input.kind));
        }
        return NodeTemplateIdentity::from_value(MapNodeIdentity{
            .graph = graph_wiring_structure(*wiring_.wiring, boundaries, wiring_.output_node),
            .input_metadata = std::move(input_metadata),
            .key_set_parameter = wiring_.key_set_parameter,
        });
    }

    compiled_data_type compile(const ResolvedSignature &, allocator &memory) const {
        auto node_indices = std::vector<std::size_t>{};
        node_indices.reserve(wiring_.inputs.size() + static_cast<std::size_t>(wiring_.key_input_node.has_value()) +
                             static_cast<std::size_t>(wiring_.output_node.has_value()));
        for (const auto &input : wiring_.inputs) {
            node_indices.push_back(input.input_node);
        }
        const auto key_position = node_indices.size();
        if (wiring_.key_input_node) {
            node_indices.push_back(*wiring_.key_input_node);
        }
        const auto output_position = node_indices.size();
        if (wiring_.output_node) {
            node_indices.push_back(*wiring_.output_node);
        }

        auto graph = compile_graph(*wiring_.wiring, memory, node_indices);
        auto result = compiled_data_type{
            .nested = {.graph = std::move(graph),
                       .input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
                       .constant_input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
                       .output_node = wiring_.output_node
                                          ? std::optional{node_indices[output_position]}
                                          : std::nullopt},
            .inputs = std::pmr::vector<CompiledMapInput>{&memory.memory_resource()},
        };
        result.nested.input_nodes.reserve(wiring_.inputs.size());
        result.inputs.reserve(wiring_.inputs.size());

        for (auto i = std::size_t{}; i < wiring_.inputs.size(); ++i) {
            const auto &input = wiring_.inputs[i];
            result.nested.input_nodes.push_back(node_indices[i]);
            result.inputs.push_back({input.kind, static_cast<std::uint32_t>(input.parameter_index),
                                     static_cast<std::uint32_t>(i)});
        }
        if (wiring_.key_input_node) {
            result.nested.constant_input_nodes.push_back(node_indices[key_position]);
        }
        if (wiring_.key_set_parameter) {
            result.key_set_parameter = static_cast<std::uint32_t>(*wiring_.key_set_parameter);
        }
        return result;
    }

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
            invocation.state().runtime->stop(invocation.compiled_data(), invocation);
        }
    }

    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        if (invocation.state().runtime != nullptr) {
            invocation.state().runtime->destroy(invocation);
            invocation.state().runtime = nullptr;
        }
    }

  private:
    void validate() const {
        if (wiring_.wiring == nullptr) {
            throw std::invalid_argument{"a map runtime node requires inner graph wiring"};
        }
        if (wiring_.inputs.empty() && !wiring_.key_set_parameter) {
            throw std::invalid_argument{"a map runtime node requires a mapped input or an explicit key set"};
        }
        if (signature_->parameter_count() > std::numeric_limits<std::uint32_t>::max() ||
            wiring_.inputs.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument{"a map runtime node has too many parameters"};
        }

        const auto nodes = wiring_.wiring->nodes();
        const auto boundary_output = [&](std::size_t node_index) -> const TimeSeriesType & {
            if (node_index >= nodes.size() || nodes[node_index].signature().output == nullptr) {
                throw std::invalid_argument{"a map boundary node must have an output"};
            }
            return *nodes[node_index].signature().output;
        };
        const auto *key_type = static_cast<const ScalarType *>(nullptr);
        const auto require_key_type = [&](const ScalarType &candidate) {
            if (key_type != nullptr && key_type != &candidate) {
                throw std::invalid_argument{"map key types must match"};
            }
            key_type = &candidate;
        };

        for (const auto &input : wiring_.inputs) {
            if (input.parameter_index >= signature_->parameter_count()) {
                throw std::invalid_argument{"a map input parameter index is out of range"};
            }
            const auto &type = *signature_->parameter(input.parameter_index).type;
            const auto *boundary_reference = type_cast<TimeSeriesReferenceType>(&boundary_output(input.input_node));
            if (input.kind == MapInputKind::mapped) {
                const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type);
                const auto *item_reference = dictionary == nullptr
                                                 ? nullptr
                                                 : type_cast<TimeSeriesReferenceType>(&dictionary->value_type());
                if (item_reference == nullptr) {
                    throw std::invalid_argument{"a mapped input must be a TSD with reference items"};
                }
                if (boundary_reference == nullptr ||
                    !boundary_reference->target_type().accepts(item_reference->target_type())) {
                    throw std::invalid_argument{"a mapped boundary type must accept its TSD item target"};
                }
                require_key_type(dictionary->key_type());
                continue;
            }

            const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
            if (reference == nullptr) {
                throw std::invalid_argument{"a pass-through input must be a reference"};
            }
            if (boundary_reference == nullptr ||
                !boundary_reference->target_type().accepts(reference->target_type())) {
                throw std::invalid_argument{"a pass-through boundary type must accept its reference target"};
            }
        }
        if (wiring_.key_set_parameter) {
            if (*wiring_.key_set_parameter >= signature_->parameter_count()) {
                throw std::invalid_argument{"the map key-set parameter index is out of range"};
            }
            const auto &type = *signature_->parameter(*wiring_.key_set_parameter).type;
            if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
                require_key_type(set->item_type());
            } else if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
                require_key_type(dictionary->key_type());
            } else {
                throw std::invalid_argument{"a map key-set parameter must be a TSS or TSD"};
            }
        }
        if (wiring_.key_input_node && !wiring_.key_set_parameter && wiring_.inputs.empty()) {
            throw std::invalid_argument{"a keyed map requires a source of keys"};
        }
        if (wiring_.output_node.has_value() != signature_->has_output()) {
            throw std::invalid_argument{"the map boundary output does not match its signature"};
        }
        if (wiring_.output_node) {
            const auto *dictionary = type_cast<TimeSeriesDictionaryType>(signature_->output());
            const auto *item_reference = dictionary == nullptr
                                             ? nullptr
                                             : type_cast<TimeSeriesReferenceType>(&dictionary->value_type());
            const auto *boundary_reference =
                type_cast<TimeSeriesReferenceType>(&boundary_output(*wiring_.output_node));
            if (item_reference == nullptr) {
                throw std::invalid_argument{"a map output must be a TSD with reference items"};
            }
            if (boundary_reference == nullptr ||
                !item_reference->target_type().accepts(boundary_reference->target_type())) {
                throw std::invalid_argument{"the map output item type must accept its boundary target"};
            }
            require_key_type(dictionary->key_type());
        }
        if (wiring_.key_input_node) {
            const auto *key = type_cast<TimeSeriesValueType>(&boundary_output(*wiring_.key_input_node));
            if (key == nullptr || key->guarantee().value() != TimeSeriesGuarantee::const_) {
                throw std::invalid_argument{"a map key boundary must be a const time-series value"};
            }
            require_key_type(key->value_type());
        }
    }

    MapGraphWiring wiring_;
    std::unique_ptr<Signature> signature_;
};

} // namespace

std::unique_ptr<NodeTemplate> make_map_node(MapGraphWiring wiring, const ResolvedSignature &outer_signature) {
    return std::make_unique<RuntimeSignatureNodeTemplate<MapNodeDefinition>>(
        MapNodeDefinition{std::move(wiring), outer_signature});
}

} // namespace bg::details
