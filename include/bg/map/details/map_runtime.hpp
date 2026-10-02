#pragma once

#include <bg/graph/details/nested_graph_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/map/details/map_entries.hpp>
#include <bg/map/details/map_node_definition.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>

namespace bg::details {

// Applies mapped-input, pass-through-input, and key-set changes to map entries.
// The owning node decides when an entry starts or stops and how ready entries are evaluated.
class MapRuntime {
  public:
    explicit MapRuntime(allocator memory)
        : entries_{memory}, pending_presence_changes_{&memory.memory_resource()},
          boundary_updates_{&memory.memory_resource()}, nested_inputs_{&memory.memory_resource()} {}

    MapRuntime(const MapRuntime &) = delete;
    MapRuntime &operator=(const MapRuntime &) = delete;

    MapEntries &entries() noexcept { return entries_; }
    const MapEntries &entries() const noexcept { return entries_; }

    template <typename Invocation, typename PresenceChanged>
    void update(const CompiledMapGraph &compiled, Invocation invocation, PresenceChanged &&presence_changed) {
        entries_.activate_due(invocation.context());
        process_key_changes(compiled, invocation);

        for (auto *entry : pending_presence_changes_) {
            entry->presence_change_queued = false;
            std::invoke(presence_changed, *entry);
        }
        pending_presence_changes_.clear();

        update_boundaries(compiled, invocation);
        update_pass_through_inputs(compiled, invocation);
    }

    template <typename Invocation>
    void start(MapEntries::Entry &entry, const CompiledMapGraph &compiled, Invocation invocation) {
        start(entry, compiled, invocation, std::nullopt);
    }

    template <typename Invocation>
    void start(MapEntries::Entry &entry, const CompiledMapGraph &compiled, Invocation invocation,
               RuntimeGraphOwner owner) {
        start(entry, compiled, invocation, std::optional{owner}, invocation.context());
    }

    template <typename Invocation>
    void start(MapEntries::Entry &entry, const CompiledMapGraph &compiled, Invocation invocation,
               RuntimeGraphOwner owner, EvaluationContext &context) {
        start(entry, compiled, invocation, std::optional{owner}, context);
    }

    void stop(MapEntries::Entry &entry, EvaluationContext &context) { entries_.stop(entry, context); }

    void stop_all(EvaluationContext &context) noexcept { entries_.stop_all(context); }
    void destroy_all(EvaluationContext &context) noexcept { entries_.destroy_all(context); }

  private:
    struct PendingBoundaryUpdate {
        MapEntries::Entry *entry;
        std::uint32_t input_index;
    };

    template <typename Invocation> static In<TS_T> input(Invocation invocation, std::size_t parameter_index) {
        return invocation.template input<In<TS_T>>(parameter_index, parameter_index);
    }

    void queue_presence_change(MapEntries::Entry &entry) {
        if (entry.presence_change_queued) {
            return;
        }
        entry.presence_change_queued = true;
        pending_presence_changes_.push_back(&entry);
    }

    template <typename Key> void remove_key_presence(const Key &key) {
        auto *entry = entries_.find(key);
        if (entry == nullptr || entry->presence_count == 0) {
            return;
        }
        --entry->presence_count;
        queue_presence_change(*entry);
    }

    template <typename Key> void set_key_set_presence(const Key &key, bool present) {
        if (present) {
            auto &entry = entries_.require(key);
            entry.presence_count = 1;
            queue_presence_change(entry);
        } else if (auto *entry = entries_.find(key)) {
            entry->presence_count = 0;
            queue_presence_change(*entry);
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
        pending_presence_changes_.clear();
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
                            remove_key_presence(key);
                        }
                        if (auto *entry = entries_.find(key)) {
                            boundary_updates_.push_back({entry, static_cast<std::uint32_t>(input_index)});
                        }
                    });
                    dictionary->for_each_added([&](const auto &key) {
                        auto &entry = entries_.require(key);
                        if (!compiled.key_set_parameter) {
                            ++entry.presence_count;
                            queue_presence_change(entry);
                        }
                        boundary_updates_.push_back({&entry, static_cast<std::uint32_t>(input_index)});
                    });
                    dictionary->for_each_modified([&](const auto &key) {
                        if (auto *entry = entries_.find(key)) {
                            boundary_updates_.push_back({entry, static_cast<std::uint32_t>(input_index)});
                        }
                    });
                },
                ddv::noop);
        }
    }

    template <typename Invocation>
    void start(MapEntries::Entry &entry, const CompiledMapGraph &compiled, Invocation invocation,
               std::optional<RuntimeGraphOwner> owner) {
        start(entry, compiled, invocation, owner, invocation.context());
    }

    template <typename Invocation>
    void start(MapEntries::Entry &entry, const CompiledMapGraph &compiled, Invocation invocation,
               std::optional<RuntimeGraphOwner> owner, EvaluationContext &context) {
        nested_inputs_.clear();
        nested_inputs_.reserve(compiled.inputs.size());
        for (const auto &description : compiled.inputs) {
            const auto outer = input(invocation, description.parameter_index);
            nested_inputs_.push_back(description.kind == MapInputKind::mapped
                                         ? mapped_reference(outer, entries_.key(entry))
                                         : pass_through_reference(outer));
        }
        const auto inputs = std::span<const NestedGraphInput>{nested_inputs_};
        const auto inherited_count = compiled.nested.graph->inherited_time_series_count();
        auto startup_context = NestedGraphStartupContext{
            invocation, context, compiled.nested, invocation.parameter_count() - inherited_count, inherited_count};

        auto rollback = util::scope_exit{[&]() noexcept { entry.inner_graph.stop_and_destroy(invocation.context()); }};
        const auto constants = [&] {
            if (compiled.nested.constant_input_nodes.empty()) {
                return std::span<const NestedGraphConstantInput>{};
            }
            nested_constant_.emplace(entries_.key(entry));
            return std::span<const NestedGraphConstantInput>{std::addressof(*nested_constant_), 1};
        }();

        if (owner) {
            entries_.start(entry, compiled.nested, startup_context.context(), inputs, constants, *owner);
        } else {
            entries_.start(entry, compiled.nested, startup_context.context(), inputs, constants);
        }
        rollback.release();
        nested_constant_.reset();
    }

    static NestedGraphInput pass_through_reference(const In<TS_T> &input) {
        return *input.visit(
            [](const In<REF<TS_T>> *reference) {
                return NestedGraphInput{reference->valid() ? reference->value() : Reference{}};
            },
            ddv::noop);
    }

    static NestedGraphInput mapped_reference(const In<TS_T> &input, const Scalar<> &key) {
        return *input.visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
                const auto typed_key = Scalar<SCALAR>::from(key);
                if (!dictionary->contains(typed_key)) {
                    return NestedGraphInput{Reference{}};
                }

                const auto item = (*dictionary)[typed_key];
                return *item.visit(
                    [](const In<REF<TS_T>> *reference) {
                        return NestedGraphInput{reference->valid() ? reference->value() : Reference{}};
                    },
                    ddv::noop);
            },
            ddv::noop);
    }

    template <typename Invocation> void update_boundaries(const CompiledMapGraph &compiled, Invocation invocation) {
        for (const auto update : boundary_updates_) {
            auto &entry = *update.entry;
            if (!entry.inner_graph.running()) {
                continue;
            }

            const auto &description = compiled.inputs[update.input_index];
            const auto outer = input(invocation, description.parameter_index);
            entry.inner_graph.update_input(compiled.nested, description.inner_input_index,
                                           mapped_reference(outer, entries_.key(entry)), invocation.context());
            entries_.mark_ready(entry);
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
            for (auto [key, entry] : entries_.table()) {
                if (!entry.inner_graph.running()) {
                    continue;
                }
                entry.inner_graph.update_input(compiled.nested, description.inner_input_index, reference,
                                               invocation.context());
                entries_.mark_ready(entry);
            }
        }
    }

    MapEntries entries_;
    std::pmr::vector<MapEntries::Entry *> pending_presence_changes_;
    std::pmr::vector<PendingBoundaryUpdate> boundary_updates_;
    std::pmr::vector<NestedGraphInput> nested_inputs_;
    std::optional<NestedGraphConstantInput> nested_constant_;
};

} // namespace bg::details
