#include <bg/graph/compiled_graph.hpp>

#include <bg/evaluation_time.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/service_binding_section.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_notification.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg {

void DefaultInputDescriptor::notify_value(OutputChangeContext &context, std::size_t node_index,
                                          std::size_t parameter_index, OutputChange change) {
    auto runtime_node = context.runtime_graph().node(node_index);
    auto &input = runtime_node.default_input(parameter_index);
    const auto notification = runtime_node.default_descriptor(parameter_index).notification;
    const auto initial_revalidation = change == OutputChange::revalidation && !input.last_updated.valid() &&
                                      context.evaluation_time() ==
                                          runtime_node.input_last_updated(parameter_index,
                                                                          context.runtime_graph_inputs())
                                              .time();
    input.last_updated.publish(context.evaluation_time());
    if (notification.function != nullptr) {
        notification.function(context, node_index, notification.data,
                              initial_revalidation ? OutputChange::revalidation : OutputChange::update);
    }
}

void DefaultInputDescriptor::notify_startup(OutputChangeContext &context, std::size_t node_index,
                                            std::size_t parameter_index, OutputChange) {
    auto runtime_node = context.runtime_graph().node(node_index);
    if (runtime_node.default_input(parameter_index).last_updated.valid()) {
        return;
    }

    const auto notification = runtime_node.default_descriptor(parameter_index).notification;
    if (notification.function != nullptr) {
        notification.function(context, node_index, notification.data, OutputChange::revalidation);
    }
}

namespace {

constexpr std::size_t max_packed_offset = (std::size_t{1} << 30) - 1;
constexpr std::size_t max_parameter_count = (std::size_t{1} << 10) - 1;
constexpr std::size_t max_subscriber_count = (std::size_t{1} << 20) - 1;
constexpr std::uint32_t no_wiring_path = std::numeric_limits<std::uint32_t>::max();

using ScheduleWord = details::ScheduleWord;
using SchedulingInfo = details::SchedulingInfo;

static_assert(std::is_trivially_copyable_v<EvaluationTime>);
static_assert(alignof(NodeFunction) >= alignof(std::uint32_t));
static_assert(alignof(const ScalarRuntimeType *) >= alignof(std::uint32_t));
static_assert(alignof(const TimeSeriesRuntimeType *) >= alignof(std::uint32_t));

[[nodiscard]] std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept {
    const auto remainder = offset % alignment;

    return remainder == 0 ? offset : offset + alignment - remainder;
}

[[nodiscard]] std::size_t runtime_image_data_offset(std::size_t scheduling_word_count,
                                                    std::size_t runtime_alignment) noexcept {
    return align_up(scheduling_word_count * sizeof(SchedulingInfo), runtime_alignment);
}

std::uint32_t packed_offset(std::size_t value, const char *description) {
    if (value > max_packed_offset) {
        throw std::length_error{description};
    }

    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t alignment_exponent(std::size_t alignment, const char *description) {
    if (!std::has_single_bit(alignment)) {
        throw std::logic_error{description};
    }

    const auto exponent = static_cast<std::uint32_t>(std::countr_zero(alignment));
    if (exponent >= (std::uint32_t{1} << 6)) {
        throw std::length_error{description};
    }

    return exponent;
}

template <typename T>
std::span<const T> stored_objects(const std::byte *storage, std::uint32_t offset, std::size_t count) noexcept {
    if (count == 0) {
        return {};
    }

    return {reinterpret_cast<const T *>(storage + offset), count};
}

struct StorageSection {
    std::size_t offset{};
    std::size_t count{};

    template <typename T> std::span<T> objects(std::byte *storage) const noexcept {
        return {reinterpret_cast<T *>(storage + offset), count};
    }

    template <typename T> std::span<const T> objects(const std::byte *storage) const noexcept {
        return {reinterpret_cast<const T *>(storage + offset), count};
    }
};

class StorageLayout {
  public:
    template <typename T> [[nodiscard]] StorageSection add(std::size_t count = 1) noexcept {
        return add_bytes(count * sizeof(T), alignof(T), count);
    }

    [[nodiscard]] StorageSection add_bytes(std::size_t size, std::size_t alignment, std::size_t count = 0) noexcept {
        alignment_ = std::max(alignment_, alignment);
        size_ = align_up(size_, alignment);
        const auto result = StorageSection{size_, count};
        size_ += size;

        return result;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t alignment() const noexcept { return alignment_; }

  private:
    std::size_t size_{};
    std::size_t alignment_{1};
};

struct FixedNodeLayout {
    std::size_t offset{};
    StorageSection compiled_node;
    StorageSection parameters;
    StorageSection input_node_indices;
    StorageSection controlled_inputs;
    StorageSection default_inputs;
    StorageSection subscribers;
    StorageSection output_notification;
    StorageSection item_subscription_root;
    StorageSection item_subscribers;
    StorageSection state_offset;
    StorageSection state_destruction_function;
    StorageSection state_metadata_counts;
    StorageSection state_field_offsets;
    StorageSection generic_state_runtime_types;
    StorageSection invalidation_function;
    StorageSection node_invocation_metadata;
    StorageSection runtime_type_metadata;
    StorageSection runtime_types;
    StorageSection all_valid_metadata;
    StorageSection all_valid_inputs;
    StorageSection node_invocation_parameters;
    StorageSection node_invocation_controlled_input_indices;
    StorageSection compiled_data;
    std::vector<std::size_t> scalar_offsets;
    std::size_t size{};
    std::size_t alignment{};
};

struct ReferenceInputSubscriberPlan {
    std::uint32_t node_index;
    std::uint32_t input_index;
    bool active;
    bool required;
    bool controlled_activity;
    bool sampled_reference;
    InputNotification notification;
};

struct InputNotificationSubscriberPlan {
    std::uint32_t node_index;
    std::size_t data;
    InputNotificationFunction function;
};

struct ControlledInputSubscriberPlan {
    std::uint32_t node_index;
    std::uint32_t input_index;
    bool required;
};

struct SubscriptionPlan {
    std::vector<Subscriber> nodes;
    std::vector<ControlledInputSubscriberPlan> controlled_inputs;
    std::vector<ReferenceInputSubscriberPlan> reference_inputs;
    std::vector<InputNotificationSubscriberPlan> input_subscribers;
};

struct ItemSubscriptionPlan {
    std::uint32_t item;
    std::size_t output;
    std::uint32_t sampled_last_updated_offset{};
    SubscriptionPlan subscriptions;
    std::vector<ItemSubscriptionPlan> items;
};

struct OutputLayout {
    std::uint32_t last_updated_offset{};
    std::uint32_t value_offset{};
    const TimeSeriesRuntimeType *runtime_type{};
};

struct StateLayout {
    std::uint32_t offset{};
    NodeStateLayout node;
    bool requires_destruction{};
};

struct TimeSeriesInputPlan {
    std::uint32_t parameter;
    WiredInput wired_input;
    WiredInput fallback;
    bool default_value;
    bool constant_binding;
    bool dereferences_reference;
    bool sampled_reference;
    bool dynamic_activity;
    std::uint32_t offset;
    std::uint32_t output_last_updated_offset;
    std::uint32_t default_input_offset;
    std::uint32_t activity_offset;
};

struct InheritedTimeSeriesPlan {
    SubscriptionPlan subscriptions;
    std::uint32_t last_updated_offset{};
    std::uint32_t value_offset{};
    StorageSection subscribers;
    StorageSection output_notification;
};

struct BorrowedInputPlan {
    const TimeSeriesRuntimeType *runtime_type{};
    SubscriptionPlan subscriptions;
    StorageSection subscribers;
    StorageSection output_notification;
};

struct RuntimeStorageLayout {
    std::size_t schedule_word_count{};
    std::optional<std::uint32_t> constant_start_time_offset;
    std::optional<std::uint32_t> service_bindings_offset;
    std::size_t size{};
    std::size_t alignment{};
};

struct NodePlan {
    SubscriptionPlan subscriptions;
    std::vector<ItemSubscriptionPlan> item_subscriptions;
    InvalidationBehavior invalidation;
    OutputLayout output;
    StateLayout state;
    CompiledNodeDataLayout compiled_data;
    std::vector<TimeSeriesInputPlan> time_series_inputs;
    std::vector<std::uint32_t> input_node_indices;
    FixedNodeLayout fixed;
};

struct ConstantPlan {
    std::size_t node;
    std::size_t output;
    std::size_t scalar;
    StorageSection storage;
};

struct DefaultInputPlan {
    WiredInput source;
    WiredInput fallback;
};

struct WiringPathLayout {
    std::vector<const WiringPath *> paths;
    std::vector<std::uint32_t> parents;
    std::vector<std::uint32_t> node_paths;
};

struct NodeFunctionLayout {
    void add(NodeFunction function, std::uint32_t node) {
        if (function == nullptr) {
            return;
        }

        functions.push_back(function);
        nodes.push_back(node);
    }

    std::vector<NodeFunction> functions;
    std::vector<std::uint32_t> nodes;
};

struct FixedStorageLayout {
    StorageSection header;
    StorageSection node_offsets;
    StorageSection node_paths;
    StorageSection constant_metadata;
    StorageSection start_functions;
    StorageSection start_nodes;
    StorageSection stop_functions;
    StorageSection stop_nodes;
    StorageSection wiring_paths;
    StorageSection scalar_destruction_runtime_types;
    StorageSection scalar_destruction_offsets;
    StorageSection output_destruction_runtime_types;
    StorageSection output_destruction_offsets;
    StorageSection variable_memory;
    StorageSection initial_runtime_image;
    StorageSection constant_notifications;
    StorageSection inherited_time_series_metadata;
    StorageSection inherited_time_series_bindings;
    StorageSection service_binding_metadata;
    StorageSection service_binding_provider_nodes;
    StorageSection borrowed_input_metadata;
    StorageSection borrowed_input_bindings;
    std::size_t size{};
    std::size_t alignment{};
};

struct CompilationPlan {
    std::span<const WiredNode> nodes;
    std::span<const WiredOutput> outputs;
    std::vector<NodePlan> node_plans;
    std::vector<std::size_t> compiled_node_indices;
    std::vector<std::size_t> wired_node_indices;
    std::vector<ConstantPlan> constants;
    std::vector<std::size_t> constant_outputs;
    std::vector<DefaultInputPlan> defaults;
    std::vector<std::size_t> default_outputs;
    WiringPathLayout wiring_paths;
    NodeFunctionLayout starts;
    NodeFunctionLayout stops;
    std::vector<InputNotificationSubscriberPlan> constant_notifications;
    std::vector<InheritedTimeSeriesPlan> inherited_time_series;
    std::vector<std::size_t> inherited_time_series_slots;
    std::vector<BorrowedInputPlan> borrowed_inputs;
    std::span<const GraphWiring::ServiceBindingRequirement> service_bindings;
    RuntimeStorageLayout runtime;
    FixedStorageLayout fixed;
    std::size_t scalar_destruction_count{};
    std::size_t output_destruction_count{};
    bool uses_variable_memory{};
    bool uses_timed_scheduling{};
    bool uses_cycle_callbacks{};
    bool uses_queue{};
    bool supports_borrowed_evaluation{};

    bool has_lifecycle() const noexcept { return !starts.nodes.empty() || !stops.nodes.empty(); }

    bool is_lowered_node(std::size_t node) const noexcept { return compiled_node_indices[node] == WiredOutput::npos; }

    std::size_t compiled_node_index(std::size_t node) const noexcept { return compiled_node_indices[node]; }

    const ConstantPlan *constant_for_output(std::size_t output) const noexcept {
        const auto index = constant_outputs[output];
        return index == WiredOutput::npos ? nullptr : &constants[index];
    }

    const DefaultInputPlan *default_for_output(std::size_t output) const noexcept {
        const auto index = default_outputs[output];
        return index == WiredOutput::npos ? nullptr : &defaults[index];
    }

    InheritedTimeSeriesPlan &inherited_time_series_at(std::size_t index) {
        return inherited_time_series[inherited_time_series_slots[index]];
    }

    const InheritedTimeSeriesPlan &inherited_time_series_at(std::size_t index) const {
        return inherited_time_series[inherited_time_series_slots[index]];
    }
};

struct ConstructedScalar {
    const ScalarRuntimeType *runtime_type;
    void *data;
};

struct ConstructedNodeData {
    CompiledNodeDataDestructionFunction destruction_function;
    void *data;
};

[[nodiscard]] WiringPathLayout make_wiring_path_layout(std::span<const WiredNode> nodes,
                                                       std::span<const std::size_t> wired_node_indices) {
    auto result = WiringPathLayout{};
    result.node_paths.reserve(wired_node_indices.size());

    auto indices = std::unordered_map<const WiringPath *, std::uint32_t>{};
    auto missing = std::vector<const WiringPath *>{};
    for (const auto node_index : wired_node_indices) {
        const auto &node = nodes[node_index];
        auto *path = &node.path();
        missing.clear();

        while (path != nullptr && !indices.contains(path)) {
            missing.push_back(path);
            path = path->parent;
        }

        for (auto entry = missing.rbegin(); entry != missing.rend(); ++entry) {
            const auto index = static_cast<std::uint32_t>(result.paths.size());
            const auto parent = (*entry)->parent == nullptr ? no_wiring_path : indices.at((*entry)->parent);
            indices.emplace(*entry, index);
            result.paths.push_back(*entry);
            result.parents.push_back(parent);
        }

        result.node_paths.push_back(indices.at(&node.path()));
    }

    return result;
}

std::size_t root_output_index(std::size_t output, std::span<const WiredOutput> outputs) {
    while (outputs[output].parent != WiredOutput::npos) {
        output = outputs[output].parent;
    }

    return output;
}

std::size_t root_output_node_index(std::size_t output, std::span<const WiredOutput> outputs) {
    return outputs[root_output_index(output, outputs)].node_index;
}

OutputLayout physical_output_layout(std::size_t output, std::span<const WiredOutput> outputs,
                                    std::span<const NodePlan> node_plans) {
    const auto &wired_output = outputs[output];
    if (wired_output.parent == WiredOutput::npos) {
        if (wired_output.node_index == WiredOutput::npos) {
            return {0, 0, wired_output.runtime_type};
        }
        return node_plans[wired_output.node_index].output;
    }

    const auto parent = physical_output_layout(wired_output.parent, outputs, node_plans);
    const auto item = parent.runtime_type->item_layout(wired_output.item);

    return {
        packed_offset(parent.value_offset + item.last_updated_offset, "runtime storage reaches 1 GiB"),
        packed_offset(parent.value_offset + item.value_offset, "runtime storage reaches 1 GiB"),
        &item.runtime_type,
    };
}

[[nodiscard]] const ItemSubscriptionPlan *find_item_subscription(std::span<const ItemSubscriptionPlan> items,
                                                                 std::size_t output) noexcept {
    for (const auto &item : items) {
        if (item.output == output) {
            return &item;
        }
        if (const auto *result = find_item_subscription(item.items, output)) {
            return result;
        }
    }

    return nullptr;
}

OutputLayout wired_output_layout(std::size_t output, std::span<const WiredOutput> outputs,
                                 std::span<const NodePlan> node_plans) {
    auto result = physical_output_layout(output, outputs, node_plans);
    if (outputs[output].parent == WiredOutput::npos ||
        outputs[root_output_index(output, outputs)].node_index == WiredOutput::npos) {
        return result;
    }

    const auto node_index = root_output_node_index(output, outputs);
    result.last_updated_offset =
        find_item_subscription(node_plans[node_index].item_subscriptions, output)->sampled_last_updated_offset;
    return result;
}

SubscriptionPlan &subscriber_list(std::size_t output, std::span<const WiredOutput> outputs,
                                  CompilationPlan &plan) {
    const auto root = root_output_index(output, outputs);
    const auto node_index = outputs[root].node_index;
    if (node_index == WiredOutput::npos) {
        throw std::logic_error{"a wired output has no owning node"};
    }
    if (outputs[output].parent == WiredOutput::npos) {
        return plan.node_plans[node_index].subscriptions;
    }

    auto path = std::vector<std::size_t>{};
    while (outputs[output].parent != WiredOutput::npos) {
        path.push_back(output);
        output = outputs[output].parent;
    }

    auto *items = &plan.node_plans[node_index].item_subscriptions;
    ItemSubscriptionPlan *leaf = nullptr;
    for (const auto output_index : path | std::views::reverse) {
        const auto item = outputs[output_index].item;
        auto entry = std::ranges::find(*items, item, &ItemSubscriptionPlan::item);
        if (entry == items->end()) {
            entry = items->insert(entry, ItemSubscriptionPlan{
                                             .item = static_cast<std::uint32_t>(item),
                                             .output = output_index,
                                             .sampled_last_updated_offset = 0,
                                             .subscriptions = {},
                                             .items = {},
                                         });
        }
        leaf = &*entry;
        items = &entry->items;
    }

    return leaf->subscriptions;
}

void plan_time_series_inputs(CompilationPlan &plan, const GraphWiring &wiring) {
    for (auto node_index = std::size_t{}; node_index < plan.nodes.size(); ++node_index) {
        if (plan.is_lowered_node(node_index)) {
            continue;
        }
        const auto &node = plan.nodes[node_index];
        auto input_index = std::size_t{};

        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            const auto &parameter = node.signature().parameters[parameter_index];
            if (!parameter.type->is_time_series()) {
                continue;
            }

            auto input = node.inputs()[input_index++];
            auto fallback = WiredInput::output(0);
            auto default_value = false;
            while (input.is_output()) {
                const auto *default_input = plan.default_for_output(input.index());
                if (default_input == nullptr) {
                    break;
                }

                input = default_input->source;
                fallback = default_input->fallback;
                default_value = true;
            }
            if (input.is_inherited()) {
                input = wiring.resolve_inherited_input(input);
            }
            const auto constant_binding =
                !default_value && input.is_output() && plan.constant_for_output(input.index()) != nullptr;
            plan.node_plans[node_index].time_series_inputs.push_back({
                .parameter = static_cast<std::uint32_t>(parameter_index),
                .wired_input = input,
                .fallback = fallback,
                .default_value = default_value,
                .constant_binding = constant_binding,
                .dereferences_reference = input.dereferences_reference(),
                .sampled_reference = input.dereferences_reference() &&
                                     (type_cast<TimeSeriesSetType>(parameter.type) != nullptr ||
                                      type_cast<TimeSeriesDictionaryType>(parameter.type) != nullptr),
                .dynamic_activity = parameter.dynamic_activity,
                .offset = 0,
                .output_last_updated_offset = 0,
                .default_input_offset = 0,
                .activity_offset = 0,
            });
        }
    }
}

std::vector<std::size_t> dependency_order(const CompilationPlan &plan, const GraphWiring &wiring) {
    enum class VisitState : std::uint8_t { unseen, visiting, emitted };
    struct PendingNode {
        std::size_t index;
        std::size_t next_input;
        std::size_t next_ordering_dependency;
    };

    auto states = std::vector<VisitState>(plan.nodes.size(), VisitState::unseen);
    auto pending = std::vector<PendingNode>{};
    pending.reserve(plan.nodes.size());
    auto result = std::vector<std::size_t>{};
    result.reserve(plan.nodes.size());

    auto ordering_edges = std::vector<std::pair<std::size_t, std::size_t>>{};
    for (const auto &constraint : wiring.ordering_constraints()) {
        const auto ordered_node = wiring.output_node_index(constraint.ordered_output);
        for (auto consumer = std::size_t{}; consumer < plan.nodes.size(); ++consumer) {
            const auto &inputs = plan.nodes[consumer].inputs();
            if (std::ranges::any_of(
                    inputs, [&](const auto input) { return wiring.input_consumes(input, constraint.consumed); })) {
                ordering_edges.emplace_back(ordered_node, consumer);
            }
        }
    }
    for (const auto &binding : wiring.service_binding_requirements()) {
        if (!binding.provider.has_output()) {
            continue;
        }

        const auto provider = wiring.output_node_index(binding.provider);
        for (const auto consumer : binding.consumers) {
            const auto consumer_node = wiring.node_index(consumer);
            if (binding.ordering == GraphWiring::ServiceBindingOrdering::provider_after_consumers) {
                ordering_edges.emplace_back(provider, consumer_node);
            } else {
                ordering_edges.emplace_back(consumer_node, provider);
            }
        }
    }
    std::ranges::sort(ordering_edges);
    ordering_edges.erase(std::unique(ordering_edges.begin(), ordering_edges.end()), ordering_edges.end());

    auto ordering_offsets = std::vector<std::size_t>{};
    if (!ordering_edges.empty()) {
        ordering_offsets.resize(plan.nodes.size() + 1);
        for (const auto &[producer, consumer] : ordering_edges) {
            (void)consumer;
            ++ordering_offsets[producer + 1];
        }
        std::partial_sum(ordering_offsets.begin(), ordering_offsets.end(), ordering_offsets.begin());
    }

    const auto throw_dependency_cycle = [&](std::size_t predecessor) -> void {
        const auto cycle_start = std::ranges::find(pending, predecessor, &PendingNode::index);
        auto message = std::string{"graph wiring contains a dependency cycle: "};
        for (auto node = cycle_start; node != pending.end(); ++node) {
            if (node != cycle_start) {
                message += " -> ";
            }
            message += plan.nodes[node->index].path().text();
        }
        message += " -> ";
        message += plan.nodes[predecessor].path().text();
        throw std::invalid_argument{message};
    };

    const auto visit_predecessor = [&](std::size_t predecessor) {
        if (states[predecessor] == VisitState::visiting) {
            throw_dependency_cycle(predecessor);
        }
        if (states[predecessor] == VisitState::emitted) {
            return false;
        }

        states[predecessor] = VisitState::visiting;
        const auto first_ordering_dependency = ordering_offsets.empty() ? std::size_t{} : ordering_offsets[predecessor];
        pending.push_back({predecessor, 0, first_ordering_dependency});
        return true;
    };

    for (auto root = std::size_t{}; root < plan.nodes.size(); ++root) {
        if (states[root] != VisitState::unseen) {
            continue;
        }

        states[root] = VisitState::visiting;
        const auto first_ordering_dependency = ordering_offsets.empty() ? std::size_t{} : ordering_offsets[root];
        pending.push_back({root, 0, first_ordering_dependency});
        while (!pending.empty()) {
            auto &current = pending.back();
            const auto &inputs = plan.node_plans[current.index].time_series_inputs;
            auto predecessor_found = false;
            while (current.next_input < inputs.size()) {
                const auto &input = inputs[current.next_input++];
                if (!input.wired_input.is_output()) {
                    continue;
                }

                const auto predecessor = plan.outputs[input.wired_input.index()].node_index;
                if (predecessor == WiredOutput::npos) {
                    continue;
                }
                if (visit_predecessor(predecessor)) {
                    predecessor_found = true;
                    break;
                }
            }

            if (!predecessor_found && !ordering_offsets.empty()) {
                const auto ordering_end = ordering_offsets[current.index + 1];
                while (current.next_ordering_dependency < ordering_end) {
                    const auto predecessor = ordering_edges[current.next_ordering_dependency++].second;
                    if (visit_predecessor(predecessor)) {
                        predecessor_found = true;
                        break;
                    }
                }
            }

            if (predecessor_found) {
                continue;
            }

            states[current.index] = VisitState::emitted;
            result.push_back(current.index);
            pending.pop_back();
        }
    }

    return result;
}

void plan_subscribers(std::span<const WiredNode> nodes, std::span<const WiredOutput> outputs,
                      std::span<NodePlan> node_plans, CompilationPlan &plan) {
    const auto add_subscriber = [](std::vector<Subscriber> &subscribers, std::uint32_t node_index, bool is_active,
                                   bool is_required, bool is_active_optional) {
        if (!subscribers.empty() && subscribers.back().node_index() == node_index) {
            const auto &existing = subscribers.back();
            subscribers.back() =
                Subscriber{node_index, existing.active() || is_active, existing.required() || is_required,
                           existing.active_optional() || is_active_optional};
            return;
        }

        subscribers.emplace_back(node_index, is_active, is_required, is_active_optional);
    };

    for (auto target_index = std::size_t{}; target_index < nodes.size(); ++target_index) {
        if (plan.is_lowered_node(target_index)) {
            continue;
        }
        const auto &target = nodes[target_index];
        const auto &inputs = node_plans[target_index].time_series_inputs;
        for (auto input_index = std::size_t{}; input_index < inputs.size(); ++input_index) {
            const auto &input = inputs[input_index];
            const auto &parameter = target.signature().parameters[input.parameter];
            const auto is_active = parameter.activity == active;
            const auto is_required = !input.default_value && parameter.validity != optional;
            if (input.sampled_reference) {
                plan.uses_cycle_callbacks = true;
            }
            const auto subscriber_index = static_cast<std::uint32_t>(plan.compiled_node_index(target_index));
            const auto original_notification = target.node_template().input_notification(input_index);
            const auto notification = input.default_value
                                          ? InputNotification{&DefaultInputDescriptor::notify_value, input.parameter}
                                          : original_notification;
            const auto add_default_startup_notification = [&] {
                if (input.default_value && original_notification.function != nullptr) {
                    plan.constant_notifications.push_back(
                        {subscriber_index, input.parameter, &DefaultInputDescriptor::notify_startup});
                }
            };
            if (input.wired_input.is_output() &&
                plan.constant_for_output(input.wired_input.index()) != nullptr) {
                if (notification.function != nullptr) {
                    plan.constant_notifications.push_back({subscriber_index, notification.data, notification.function});
                }
                add_default_startup_notification();
                continue;
            }
            auto &subscriptions = input.wired_input.is_inherited()
                                      ? plan.inherited_time_series_at(input.wired_input.index()).subscriptions
                                      : input.wired_input.is_borrowed()
                                            ? plan.borrowed_inputs[input.wired_input.index()].subscriptions
                                            : subscriber_list(input.wired_input.index(), outputs, plan);
            if (input.dereferences_reference) {
                subscriptions.reference_inputs.push_back({
                    .node_index = subscriber_index,
                    .input_index = static_cast<std::uint32_t>(input_index),
                    .active = is_active,
                    .required = is_required,
                    .controlled_activity = input.dynamic_activity,
                    .sampled_reference = input.sampled_reference,
                    .notification = notification,
                });
                if (input.dynamic_activity) {
                    subscriptions.controlled_inputs.push_back(
                        {subscriber_index, static_cast<std::uint32_t>(input_index), is_required});
                } else if (is_active) {
                    add_subscriber(subscriptions.nodes, subscriber_index, true, false, !is_required);
                }
            } else {
                if (notification.function != nullptr) {
                    subscriptions.input_subscribers.push_back({
                        .node_index = subscriber_index,
                        .data = notification.data,
                        .function = notification.function,
                    });
                }
                if (input.dynamic_activity) {
                    subscriptions.controlled_inputs.push_back(
                        {subscriber_index, static_cast<std::uint32_t>(input_index), is_required});
                    continue;
                }
                if (is_active || is_required) {
                    add_subscriber(subscriptions.nodes, subscriber_index, is_active, is_required,
                                   is_active && !is_required);
                }
            }
            add_default_startup_notification();
        }
    }
}

const TimeSeriesRuntimeType *input_runtime_type(const TimeSeriesInputPlan &input, const TimeSeriesRuntimeType *output,
                                                const TimeSeriesType &requested_type) {
    if (input.dereferences_reference) {
        return &resolve_time_series_runtime_type(requested_type);
    }

    return output;
}

std::vector<const ScalarRuntimeType *> parameter_runtime_types(const WiredNode &node, const NodePlan &node_plan,
                                                               std::span<const WiredOutput> outputs,
                                                               std::span<const NodePlan> node_plans,
                                                               const CompilationPlan &plan) {
    auto result = std::vector<const ScalarRuntimeType *>{};
    result.reserve(node.signature().parameters.size());
    auto input_index = std::size_t{};
    auto scalar_index = std::size_t{};
    for (const auto &parameter : node.signature().parameters) {
        if (parameter.type->is_time_series()) {
            const auto &input = node_plan.time_series_inputs[input_index++];
            if (input.wired_input.is_output()) {
                if (const auto *constant = plan.constant_for_output(input.wired_input.index())) {
                    const auto &scalar = plan.nodes[constant->node].scalar_values()[constant->scalar];
                    result.push_back(&scalar.runtime_type());
                    continue;
                }
            }
            const auto &requested_type = static_cast<const TimeSeriesType &>(*parameter.type);
            const auto *runtime_type =
                !input.wired_input.is_output()
                    ? &resolve_time_series_runtime_type(requested_type)
                    : input_runtime_type(
                          input, wired_output_layout(input.wired_input.index(), outputs, node_plans).runtime_type,
                          requested_type);
            const ScalarRuntimeType *scalar_runtime_type{};
            runtime_type->type().visit(
                [&](const TimeSeriesValueType *) {
                    scalar_runtime_type =
                        &static_cast<const TimeSeriesValueRuntimeType &>(*runtime_type).scalar_runtime_type();
                },
                ddv::noop);
            result.push_back(scalar_runtime_type);
        } else {
            const auto &scalar = node.scalar_values()[scalar_index++];
            result.push_back(scalar.has_value() ? &scalar.runtime_type() : nullptr);
        }
    }

    return result;
}

const TimeSeriesRuntimeType *find_output_runtime_type(const WiredNode &node, const NodePlan &node_plan) {
    if (node_plan.output.runtime_type != nullptr) {
        return node_plan.output.runtime_type;
    }

    return &resolve_time_series_runtime_type(*node.signature().output);
}

[[nodiscard]] OutputLayout add_output_layout(StorageLayout &storage, const WiredNode &node, const NodePlan &node_plan,
                                             std::span<const WiredOutput> outputs, std::span<const NodePlan> node_plans,
                                             const CompilationPlan &plan) {
    if (node.signature().output == nullptr) {
        return {};
    }

    const auto *runtime_type = find_output_runtime_type(node, node_plan);
    if (runtime_type == nullptr) {
        throw std::logic_error{"node output has no runtime type"};
    }
    if (!runtime_type->supports_byte_copy_initialization()) {
        throw std::logic_error{"output storage does not support byte-copy initialization"};
    }

    const auto last_updated = storage.add<TimeSeriesState>();
    const auto value = storage.add_bytes(runtime_type->value_size(), runtime_type->value_alignment());

    return {packed_offset(last_updated.offset, "runtime storage reaches 1 GiB"),
            packed_offset(value.offset, "runtime storage reaches 1 GiB"), runtime_type};
}

[[nodiscard]] StateLayout add_state_layout(StorageLayout &storage, const WiredNode &node, const NodePlan &node_plan,
                                           std::span<const WiredOutput> outputs, std::span<const NodePlan> node_plans,
                                           const CompilationPlan &plan) {
    auto layout = node.node_template().state_layout(
        node.signature(), parameter_runtime_types(node, node_plan, outputs, node_plans, plan), node.state_types());
    for ([[maybe_unused]] const auto *type : node.dynamic_reference_input_types()) {
        layout.size = align_up(layout.size, alignof(details::DynamicReferenceInputStorage));
        layout.field_offsets.push_back(layout.size);
        layout.size += sizeof(details::DynamicReferenceInputStorage);
        layout.alignment = std::max(layout.alignment, alignof(details::DynamicReferenceInputStorage));
    }
    if (layout.size == 0) {
        return {};
    }

    const auto state = storage.add_bytes(layout.size, layout.alignment);

    return {packed_offset(state.offset, "runtime storage reaches 1 GiB"), std::move(layout),
            node.node_template().state_destruction_function(node.signature()) != nullptr};
}

void plan_runtime_storage(CompilationPlan &plan) {
    auto storage = StorageLayout{};
    const auto schedule_word_count = (plan.wired_node_indices.size() + std::numeric_limits<ScheduleWord>::digits - 1) /
                                     std::numeric_limits<ScheduleWord>::digits;
    if (plan.uses_timed_scheduling) {
        (void)storage.add<void *>();
    }
    if (plan.uses_cycle_callbacks) {
        (void)storage.add<void *>();
    }
    if (plan.uses_queue) {
        (void)storage.add<void *>();
    }
    if (!plan.constants.empty()) {
        plan.runtime.constant_start_time_offset =
            packed_offset(storage.add<TimeSeriesState>().offset, "runtime storage reaches 1 GiB");
    }
    if (!plan.service_bindings.empty()) {
        plan.runtime.service_bindings_offset =
            packed_offset(storage.add<details::ServiceRequestBinding *>(plan.service_bindings.size()).offset,
                          "runtime storage reaches 1 GiB");
    }

    for (auto &requirement : plan.inherited_time_series) {
        requirement.last_updated_offset =
            packed_offset(storage.add<TimeSeriesState>().offset, "runtime storage reaches 1 GiB");
        requirement.value_offset = packed_offset(storage.add<Reference>().offset, "runtime storage reaches 1 GiB");
    }

    const auto add_item_samples = [&](this const auto &self, std::span<ItemSubscriptionPlan> items) -> void {
        for (auto &item : items) {
            item.sampled_last_updated_offset =
                packed_offset(storage.add<TimeSeriesState>().offset, "runtime storage reaches 1 GiB");
            self(item.items);
        }
    };

    for (const auto i : plan.wired_node_indices) {
        if (plan.is_lowered_node(i)) {
            continue;
        }
        auto &node_plan = plan.node_plans[i];
        node_plan.output = add_output_layout(storage, plan.nodes[i], node_plan, plan.outputs, plan.node_plans, plan);
        add_item_samples(node_plan.item_subscriptions);

        for (auto &input : node_plan.time_series_inputs) {
            if (input.default_value) {
                const auto slot = storage.add<DefaultInputState>();
                input.default_input_offset = packed_offset(slot.offset, "runtime storage reaches 1 GiB");
            }

            if (!input.dereferences_reference) {
                if (input.dynamic_activity) {
                    input.activity_offset = packed_offset(storage.add<bool>().offset, "runtime storage reaches 1 GiB");
                }
                continue;
            }

            const auto slot = storage.add<ReferenceInputStorage>();
            input.offset = packed_offset(slot.offset, "runtime storage reaches 1 GiB");
            if (input.dynamic_activity) {
                // Dynamic reference subscribers recover this adjacent byte without growing their records.
                input.activity_offset = packed_offset(storage.add<bool>().offset, "runtime storage reaches 1 GiB");
            }
            if (input.sampled_reference) {
                (void)storage.add<details::ReferenceInputSampleStorage>();
            }
            input.output_last_updated_offset =
                input.wired_input.is_inherited()
                    ? plan.inherited_time_series_at(input.wired_input.index()).last_updated_offset
                    : wired_output_layout(input.wired_input.index(), plan.outputs, plan.node_plans).last_updated_offset;
        }

        node_plan.state = add_state_layout(storage, plan.nodes[i], node_plan, plan.outputs, plan.node_plans, plan);
    }

    (void)packed_offset(storage.size(), "runtime storage reaches 1 GiB");

    plan.runtime.schedule_word_count = schedule_word_count;
    plan.runtime.size = storage.size();
    plan.runtime.alignment = storage.alignment();
}

std::size_t output_notification_size(const SubscriptionPlan &subscriptions) noexcept {
    const auto notifying_reference_count =
        std::ranges::count_if(subscriptions.reference_inputs,
                              [](const auto &subscriber) { return subscriber.notification.function != nullptr; });
    const auto reference_count = subscriptions.reference_inputs.size() - notifying_reference_count;
    const auto reference_bytes = reference_count * sizeof(details::ReferenceInputSubscriber);
    const auto notifying_reference_bytes =
        notifying_reference_count * sizeof(details::ReferenceInputNotificationSubscriber);
    const auto mixed_references = reference_count != 0 && notifying_reference_count != 0;
    auto reference_payload_bytes = reference_bytes + notifying_reference_bytes;
    if (mixed_references) {
        reference_payload_bytes += sizeof(details::ReferenceInputNotificationHeader);
    }

    auto result = std::size_t{};
    if (!subscriptions.controlled_inputs.empty()) {
        result += align_up(sizeof(OutputNotification) +
                               subscriptions.controlled_inputs.size() * sizeof(ControlledInputSubscriber),
                           alignof(OutputNotification));
    }
    if (reference_payload_bytes != 0) {
        result += align_up(sizeof(OutputNotification) + reference_payload_bytes, alignof(OutputNotification));
    }
    if (!subscriptions.input_subscribers.empty()) {
        result += align_up(sizeof(OutputNotification) +
                               subscriptions.input_subscribers.size() * sizeof(InputNotificationSubscriber),
                           alignof(OutputNotification));
    }

    return result;
}

std::size_t subscription_size(const SubscriptionPlan &subscriptions) noexcept {
    auto result = subscriptions.nodes.size() * sizeof(Subscriber);
    if (const auto notifications = output_notification_size(subscriptions); notifications != 0) {
        result = align_up(result, alignof(OutputNotification));
        result += notifications;
    }

    return result;
}

std::size_t item_subscriber_size(const ItemSubscriptionPlan &item);

std::size_t item_subscriber_size(std::span<const ItemSubscriptionPlan> items) {
    auto result = std::size_t{};
    for (const auto &item : items) {
        if (item.subscriptions.nodes.size() > max_subscriber_count ||
            item.subscriptions.controlled_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.subscriptions.reference_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.subscriptions.input_subscribers.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.items.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"a time series item has too many subscriptions"};
        }

        result += item_subscriber_size(item);
    }

    return result;
}

std::size_t item_subscriber_size(const ItemSubscriptionPlan &item) {
    auto result = sizeof(ItemSubscription) + subscription_size(item.subscriptions);
    result = align_up(result, alignof(ItemSubscription));
    result += item_subscriber_size(item.items);
    return align_up(result, alignof(ItemSubscription));
}

struct RuntimeTypeCounts {
    std::size_t parameter_slots{};
    std::size_t dynamic_reference_inputs{};
    bool output{};

    std::size_t dynamic_reference_begin() const noexcept {
        return parameter_slots + static_cast<std::size_t>(output);
    }

    std::size_t total() const noexcept {
        return dynamic_reference_begin() + dynamic_reference_inputs;
    }
};

RuntimeTypeCounts runtime_type_counts(const WiredNode &node) {
    auto result = RuntimeTypeCounts{.dynamic_reference_inputs = node.dynamic_reference_input_types().size(),
                                    .output = node.node_template().uses_output_runtime_type()};
    for (auto parameter = std::size_t{}; parameter < node.signature().parameters.size(); ++parameter) {
        if (node.node_template().uses_time_series_input_runtime_type(node.signature(), parameter) ||
            node.node_template().uses_scalar_input_runtime_type(node.signature(), parameter)) {
            result.parameter_slots = node.signature().parameters.size();
            break;
        }
    }
    return result;
}

[[nodiscard]] FixedNodeLayout make_node_layout(const WiredNode &node, const NodePlan &node_plan) {
    if (node.signature().parameters.size() > max_parameter_count) {
        throw std::length_error{"a node has more than 1023 parameters"};
    }
    if (node_plan.subscriptions.nodes.size() > max_subscriber_count) {
        throw std::length_error{"a node has too many subscribers"};
    }
    if (node_plan.subscriptions.controlled_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
        node_plan.subscriptions.reference_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
        node_plan.subscriptions.input_subscribers.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error{"a node has too many output notifications"};
    }

    auto storage = StorageLayout{};
    auto result = FixedNodeLayout{};
    const auto runtime_types = runtime_type_counts(node);
    const auto all_valid_input_count = std::ranges::count_if(
        node_plan.time_series_inputs, [&](const auto &input) {
            return node.signature().parameters[input.parameter].validity == InputValidity::all_valid &&
                   !node.signature().parameters[input.parameter].valid_items &&
                   !input.constant_binding;
        });
    if (all_valid_input_count != 0) {
        result.all_valid_inputs = storage.add<AllValidInputDescriptor>(all_valid_input_count);
        result.all_valid_metadata = storage.add_bytes(sizeof(const RuntimeType *), alignof(const RuntimeType *), 1);
    }
    result.runtime_types = storage.add<const RuntimeType *>(runtime_types.total());
    if (runtime_types.total() != 0 || all_valid_input_count != 0) {
        result.runtime_type_metadata = storage.add<RuntimeTypeMetadata>();
    }
    result.compiled_node = storage.add<CompiledNode>();

    result.parameters = storage.add<ParameterBinding>(node.signature().parameters.size());
    if (!node_plan.input_node_indices.empty()) {
        result.input_node_indices = storage.add<std::uint32_t>(node_plan.input_node_indices.size());
    }
    const auto controlled_input_count =
        std::ranges::count_if(node_plan.time_series_inputs, [](const auto &input) { return input.dynamic_activity; });
    if (controlled_input_count != 0) {
        result.controlled_inputs = storage.add<ControlledInput>(controlled_input_count);
    }
    const auto default_input_count =
        std::ranges::count(node_plan.time_series_inputs, true, &TimeSeriesInputPlan::default_value);
    result.subscribers = storage.add<Subscriber>(node_plan.subscriptions.nodes.size());
    if (!node_plan.subscriptions.controlled_inputs.empty() || !node_plan.subscriptions.reference_inputs.empty() ||
        !node_plan.subscriptions.input_subscribers.empty()) {
        result.output_notification =
            storage.add_bytes(output_notification_size(node_plan.subscriptions), alignof(OutputNotification));
    }
    if (!node_plan.item_subscriptions.empty()) {
        result.item_subscription_root = storage.add<ItemSubscriptionRoot>();
        result.item_subscribers =
            storage.add_bytes(item_subscriber_size(node_plan.item_subscriptions), alignof(ItemSubscription));
    }
    if (node_plan.state.node.size != 0) {
        result.state_offset = storage.add<std::uint32_t>();
        if (node_plan.state.requires_destruction) {
            result.state_destruction_function = storage.add<NodeFunction>();
        }
        if (!node_plan.state.node.generic_runtime_types.empty()) {
            result.state_metadata_counts = storage.add<StateMetadataCounts>();
            result.state_field_offsets = storage.add<std::uint32_t>(node_plan.state.node.field_offsets.size());
            result.generic_state_runtime_types =
                storage.add<const ScalarRuntimeType *>(node_plan.state.node.generic_runtime_types.size());
        }
    }
    if (node_plan.invalidation.mode == InvalidationMode::invoke) {
        result.invalidation_function = storage.add<NodeFunction>();
    }
    if (node.node_template().uses_node_invocation_parameters() || node_plan.compiled_data.size != 0) {
        result.node_invocation_metadata = storage.add<NodeInvocationMetadata>();
    }

    result.scalar_offsets.reserve(node.scalar_values().size());
    for (const auto &scalar : node.scalar_values()) {
        if (!scalar.has_value()) {
            result.scalar_offsets.push_back(0);
            continue;
        }

        const auto &runtime_type = scalar.runtime_type();
        const auto value = storage.add_bytes(runtime_type.size(), runtime_type.alignment());
        result.scalar_offsets.push_back(value.offset);
    }

    if (node.node_template().uses_node_invocation_parameters()) {
        if (!node.signature().parameters.empty()) {
            result.node_invocation_parameters =
                storage.add<NodeInvocationParameterMetadata>(node.signature().parameters.size());
        }
        if (std::ranges::any_of(node_plan.time_series_inputs,
                                [](const auto &input) { return input.dynamic_activity; })) {
            result.node_invocation_controlled_input_indices =
                storage.add<std::uint16_t>(node.signature().parameters.size());
        }
    }
    if (node_plan.compiled_data.size != 0) {
        result.compiled_data = storage.add_bytes(node_plan.compiled_data.size, node_plan.compiled_data.alignment);
    }

    if (default_input_count != 0) {
        result.default_inputs = storage.add<DefaultInputDescriptor>(default_input_count);
    }

    result.size = storage.size();
    result.alignment = storage.alignment();

    return result;
}

void plan_fixed_storage(CompilationPlan &plan) {
    auto storage = StorageLayout{};
    auto &layout = plan.fixed;
    layout.header = storage.add<details::CompiledStorageHeader>();
    layout.node_offsets = storage.add<std::uint32_t>(plan.wired_node_indices.size());
    layout.node_paths = storage.add<std::uint32_t>(plan.wired_node_indices.size());
    if (!plan.constants.empty()) {
        layout.constant_metadata = storage.add<details::ConstantMetadata>();
    }
    if (!plan.inherited_time_series.empty()) {
        layout.inherited_time_series_metadata = storage.add<details::InheritedTimeSeriesMetadata>();
        layout.inherited_time_series_bindings =
            storage.add<details::InheritedTimeSeriesBinding>(plan.inherited_time_series.size());
    }
    if (!plan.service_bindings.empty()) {
        layout.service_binding_metadata = storage.add<details::ServiceBindingMetadata>();
        layout.service_binding_provider_nodes = storage.add<std::uint32_t>(plan.service_bindings.size());
    }
    if (!plan.borrowed_inputs.empty()) {
        layout.borrowed_input_metadata = storage.add<details::BorrowedInputMetadata>();
        layout.borrowed_input_bindings = storage.add<details::BorrowedInputBinding>(plan.borrowed_inputs.size());
    }
    if (plan.has_lifecycle()) {
        layout.start_functions = storage.add<NodeFunction>(plan.starts.nodes.size());
        layout.start_nodes = storage.add<std::uint32_t>(plan.starts.nodes.size());
        layout.stop_functions = storage.add<NodeFunction>(plan.stops.nodes.size());
        layout.stop_nodes = storage.add<std::uint32_t>(plan.stops.nodes.size());
    }
    layout.wiring_paths = storage.add<WiringPath>(plan.wiring_paths.paths.size());
    layout.scalar_destruction_runtime_types = storage.add<const ScalarRuntimeType *>(plan.scalar_destruction_count);
    layout.scalar_destruction_offsets = storage.add<std::uint32_t>(plan.scalar_destruction_count);
    layout.output_destruction_runtime_types = storage.add<const TimeSeriesRuntimeType *>(plan.output_destruction_count);
    layout.output_destruction_offsets = storage.add<std::uint32_t>(plan.output_destruction_count);
    if (plan.uses_variable_memory) {
        layout.variable_memory = storage.add<std::pmr::unsynchronized_pool_resource>();
    }
    const auto runtime_offset = runtime_image_data_offset(plan.runtime.schedule_word_count, plan.runtime.alignment);
    const auto runtime_image_size = runtime_offset + plan.runtime.size;
    const auto runtime_image_alignment = std::max(alignof(SchedulingInfo), plan.runtime.alignment);
    layout.initial_runtime_image =
        storage.add_bytes(runtime_image_size, runtime_image_alignment, runtime_image_size);
    layout.constant_notifications = storage.add<InputNotificationSubscriber>(plan.constant_notifications.size());

    for (auto &requirement : plan.inherited_time_series) {
        requirement.subscribers = storage.add<Subscriber>(requirement.subscriptions.nodes.size());
        if (!requirement.subscriptions.controlled_inputs.empty() ||
            !requirement.subscriptions.reference_inputs.empty() ||
            !requirement.subscriptions.input_subscribers.empty()) {
            requirement.output_notification =
                storage.add_bytes(output_notification_size(requirement.subscriptions), alignof(OutputNotification),
                                  output_notification_size(requirement.subscriptions));
        }
    }

    for (auto &input : plan.borrowed_inputs) {
        if (input.subscriptions.nodes.size() > max_subscriber_count ||
            input.subscriptions.controlled_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
            input.subscriptions.reference_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
            input.subscriptions.input_subscribers.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"a borrowed input has too many subscribers"};
        }
        input.subscribers = storage.add<Subscriber>(input.subscriptions.nodes.size());
        if (!input.subscriptions.controlled_inputs.empty() || !input.subscriptions.reference_inputs.empty() ||
            !input.subscriptions.input_subscribers.empty()) {
            input.output_notification = storage.add_bytes(output_notification_size(input.subscriptions),
                                                         alignof(OutputNotification),
                                                         output_notification_size(input.subscriptions));
        }
    }

    for (auto node : plan.wired_node_indices) {
        auto &fixed = plan.node_plans[node].fixed;
        const auto section = storage.add_bytes(fixed.size, fixed.alignment);
        fixed.offset = section.offset;
    }
    for (auto &constant : plan.constants) {
        const auto &scalar = plan.nodes[constant.node].scalar_values()[constant.scalar];
        constant.storage = storage.add_bytes(scalar.runtime_type().size(), scalar.runtime_type().alignment());
    }

    layout.size = storage.size();
    layout.alignment = storage.alignment();
}

[[nodiscard]] CompilationPlan make_compilation_plan(const GraphWiring &wiring) {
    auto result = CompilationPlan{};
    result.nodes = wiring.nodes();
    result.outputs = wiring.outputs();
    result.service_bindings = wiring.service_binding_requirements();
    result.node_plans.resize(result.nodes.size());
    result.compiled_node_indices.assign(result.nodes.size(), WiredOutput::npos);
    result.constant_outputs.assign(result.outputs.size(), WiredOutput::npos);
    result.default_outputs.assign(result.outputs.size(), WiredOutput::npos);
    result.inherited_time_series_slots.assign(wiring.inherited_requirements().size(), WiredOutput::npos);
    const auto borrowed_input_types = wiring.borrowed_input_types();
    result.borrowed_inputs.resize(borrowed_input_types.size());
    for (auto index = std::size_t{}; index < borrowed_input_types.size(); ++index) {
        result.borrowed_inputs[index].runtime_type =
            &resolve_time_series_runtime_type(*borrowed_input_types[index]);
    }
    for (auto index = std::size_t{}; index < wiring.inherited_requirements().size(); ++index) {
        const auto &requirement = wiring.inherited_requirements()[index];
        if (!requirement.provider.has_output()) {
            throw std::invalid_argument{"an inherited time series requirement has no provider"};
        }
        if (&requirement.provider.graph() == &wiring) {
            continue;
        }

        result.inherited_time_series_slots[index] = result.inherited_time_series.size();
        result.inherited_time_series.emplace_back();
    }
    for (const auto &binding : result.service_bindings) {
        if (!binding.parent_requirement && !binding.provider.has_output()) {
            throw std::invalid_argument{"a root service binding has no provider"};
        }
        if (binding.parent_requirement && binding.provider.has_output()) {
            throw std::invalid_argument{"an inner service binding cannot have a local provider"};
        }
    }

    const auto input_for_parameter = [](const WiredNode &node, std::size_t parameter) {
        if (parameter >= node.signature().parameters.size() ||
            !node.signature().parameters[parameter].type->is_time_series()) {
            throw std::logic_error{"a default output parameter is not a time series"};
        }

        auto input = std::size_t{};
        for (auto index = std::size_t{}; index < parameter; ++index) {
            input += static_cast<std::size_t>(node.signature().parameters[index].type->is_time_series());
        }
        if (input >= node.inputs().size()) {
            throw std::logic_error{"a default output has no input"};
        }
        return input;
    };

    for (auto node_index = std::size_t{}; node_index < result.nodes.size(); ++node_index) {
        const auto &node = result.nodes[node_index];
        const auto constant = node.node_template().constant_output(node.signature());
        const auto output_index = std::ranges::find_if(result.outputs, [=](const auto &candidate) {
            return candidate.parent == WiredOutput::npos && candidate.node_index == node_index;
        });
        if (constant) {
            if (constant->scalar_parameter >= node.signature().parameters.size() ||
                node.signature().parameters[constant->scalar_parameter].type->is_time_series() ||
                node.signature().output == nullptr ||
                node.signature().output->guarantee().value() != TimeSeriesGuarantee::const_) {
                throw std::logic_error{"invalid constant output"};
            }
            const auto *output = type_cast<TimeSeriesValueType>(node.signature().output);
            if (output == nullptr ||
                &output->value_type() != node.signature().parameters[constant->scalar_parameter].type) {
                throw std::logic_error{"constant output does not match its scalar parameter"};
            }

            auto scalar = std::size_t{};
            for (auto parameter = std::size_t{}; parameter < constant->scalar_parameter; ++parameter) {
                scalar += static_cast<std::size_t>(!node.signature().parameters[parameter].type->is_time_series());
            }
            if (scalar >= node.scalar_values().size() || !node.scalar_values()[scalar].has_value()) {
                throw std::logic_error{"constant output requires one scalar value"};
            }
            if (output_index == result.outputs.end()) {
                throw std::logic_error{"constant node has no root output"};
            }

            const auto index = static_cast<std::size_t>(output_index - result.outputs.begin());
            result.constant_outputs[index] = result.constants.size();
            result.constants.push_back({node_index, index, scalar, {}});
            continue;
        }

        const auto default_input = node.node_template().default_output(node.signature());
        if (!default_input) {
            result.compiled_node_indices[node_index] = node_index;
            continue;
        }
        if (node.signature().output == nullptr || output_index == result.outputs.end()) {
            throw std::logic_error{"a default adapter has no root output"};
        }

        const auto source = input_for_parameter(node, default_input->source_parameter);
        const auto fallback = input_for_parameter(node, default_input->fallback_parameter);
        const auto *source_type =
            type_cast<TimeSeriesValueType>(node.signature().parameters[default_input->source_parameter].type);
        const auto *fallback_type =
            type_cast<TimeSeriesValueType>(node.signature().parameters[default_input->fallback_parameter].type);
        const auto *output_type = type_cast<TimeSeriesValueType>(node.signature().output);
        if (source_type == nullptr || fallback_type == nullptr || output_type == nullptr ||
            &source_type->value_type() != &output_type->value_type() ||
            &fallback_type->value_type() != &output_type->value_type() ||
            fallback_type->guarantee().value() != TimeSeriesGuarantee::const_ ||
            output_type->guarantee().value() != TimeSeriesGuarantee::ordinary) {
            throw std::logic_error{"invalid default output"};
        }
        const auto index = static_cast<std::size_t>(output_index - result.outputs.begin());
        result.default_outputs[index] = result.defaults.size();
        result.defaults.push_back({node.inputs()[source], node.inputs()[fallback]});
    }

    plan_time_series_inputs(result, wiring);
    const auto node_order = dependency_order(result, wiring);
    result.wired_node_indices.clear();
    for (const auto node_index : node_order) {
        if (result.is_lowered_node(node_index)) {
            continue;
        }

        result.compiled_node_indices[node_index] = result.wired_node_indices.size();
        result.wired_node_indices.push_back(node_index);
    }

    for (auto node_index = std::size_t{}; node_index < result.nodes.size(); ++node_index) {
        const auto &node = result.nodes[node_index];
        if (!node.node_template().uses_input_node_indices()) {
            continue;
        }

        auto &input_node_indices = result.node_plans[node_index].input_node_indices;
        input_node_indices.assign(node.signature().parameters.size(), CompiledNode::no_input_node_index);
        auto input_index = std::size_t{};
        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            if (!node.signature().parameters[parameter_index].type->is_time_series()) {
                continue;
            }

            const auto input = node.inputs()[input_index++];
            if (!input.is_output()) {
                continue;
            }

            const auto producer = root_output_node_index(input.index(), result.outputs);
            if (!result.is_lowered_node(producer)) {
                input_node_indices[parameter_index] =
                    static_cast<std::uint32_t>(result.compiled_node_index(producer));
            }
        }
        if (std::ranges::all_of(input_node_indices, [](auto index) {
                return index == CompiledNode::no_input_node_index;
            })) {
            input_node_indices.clear();
        }
    }

    std::erase_if(result.constants, [&](const auto &constant) {
        return !std::ranges::any_of(result.nodes, [&](const auto &node) {
            const auto node_index = static_cast<std::size_t>(&node - result.nodes.data());
            return !result.is_lowered_node(node_index) &&
                   std::ranges::any_of(result.node_plans[node_index].time_series_inputs, [&](const auto &input) {
                       return (input.wired_input.is_output() && input.wired_input.index() == constant.output) ||
                              (input.default_value && input.fallback.is_output() &&
                               input.fallback.index() == constant.output);
                   });
        });
    });
    std::fill(result.constant_outputs.begin(), result.constant_outputs.end(), WiredOutput::npos);
    for (auto index = std::size_t{}; index < result.constants.size(); ++index) {
        result.constant_outputs[result.constants[index].output] = index;
    }
    for (const auto &output : result.outputs) {
        if (output.parent == WiredOutput::npos && output.node_index != WiredOutput::npos &&
            output.runtime_type != nullptr) {
            result.node_plans[output.node_index].output.runtime_type = output.runtime_type;
        }
    }

    plan_subscribers(result.nodes, result.outputs, result.node_plans, result);
    result.wiring_paths = make_wiring_path_layout(result.nodes, result.wired_node_indices);

    for (const auto i : result.wired_node_indices) {
        const auto &node = result.nodes[i];
        result.node_plans[i].invalidation = node.node_template().invalidation_behavior(node.signature());
        const auto compiled_node = static_cast<std::uint32_t>(result.compiled_node_index(i));
        result.starts.add(node.node_template().start_function(node.signature()), compiled_node);
        result.stops.add(node.node_template().stop_function(node.signature()), compiled_node);
        result.uses_timed_scheduling |= node.node_template().uses_timed_scheduling();
        result.uses_cycle_callbacks |= node.node_template().uses_cycle_callbacks();
        result.uses_queue |= node.node_template().uses_queue();
        result.node_plans[i].compiled_data = node.node_template().compiled_data_layout(node.signature());
    }
    for (const auto &constant : result.constants) {
        const auto &runtime_type = result.nodes[constant.node].scalar_values()[constant.scalar].runtime_type();
        result.scalar_destruction_count += static_cast<std::size_t>(runtime_type.requires_destruction());
        result.uses_variable_memory |= runtime_type.uses_variable_memory();
    }

    plan_runtime_storage(result);
    for (auto i = std::size_t{}; i < result.nodes.size(); ++i) {
        if (result.is_lowered_node(i)) {
            continue;
        }
        auto &invalidation = result.node_plans[i].invalidation;
        const auto *output_type = result.nodes[i].signature().output;
        if (invalidation.mode == InvalidationMode::invalidate_output && output_type != nullptr &&
            !output_type->supports_invalidation()) {
            invalidation.mode = InvalidationMode::preserve_output;
        }
    }
    result.output_destruction_count =
        static_cast<std::size_t>(std::ranges::count_if(result.wired_node_indices, [&](const auto node_index) {
            const auto &output = result.node_plans[node_index].output;
            return output.runtime_type != nullptr && output.runtime_type->requires_destruction();
        }));

    for (auto i = std::size_t{}; i < result.nodes.size(); ++i) {
        if (result.is_lowered_node(i)) {
            continue;
        }
        const auto &node = result.nodes[i];
        auto &node_plan = result.node_plans[i];
        node_plan.fixed = make_node_layout(node, node_plan);

        for (const auto &scalar : node.scalar_values()) {
            if (!scalar.has_value()) {
                continue;
            }

            const auto &runtime_type = scalar.runtime_type();
            result.scalar_destruction_count += static_cast<std::size_t>(runtime_type.requires_destruction());
            result.uses_variable_memory |= runtime_type.uses_variable_memory();
        }
    }

    result.supports_borrowed_evaluation = !result.has_lifecycle() && !result.uses_timed_scheduling &&
                                          !result.uses_cycle_callbacks && !result.uses_queue && result.constants.empty() &&
                                          result.constant_notifications.empty() &&
                                          result.inherited_time_series.empty() && result.service_bindings.empty() &&
                                          result.output_destruction_count == 0;
    for (const auto &output : result.outputs) {
        if ((output.type != nullptr && output.type->is_time_series() &&
             static_cast<const TimeSeriesType &>(*output.type).contains_reference()) ||
            (output.runtime_type != nullptr && output.runtime_type->is_reference())) {
            result.supports_borrowed_evaluation = false;
            break;
        }
    }
    for (const auto node_index : result.wired_node_indices) {
        const auto &node = result.nodes[node_index];
        const auto &node_plan = result.node_plans[node_index];
        const auto has_reference_input = std::ranges::any_of(node_plan.time_series_inputs, [&](const auto &input) {
            const auto &type = *node.signature().parameters[input.parameter].type;
            return input.dereferences_reference || static_cast<const TimeSeriesType &>(type).contains_reference();
        });
        const auto has_reference_output =
            node.signature().output != nullptr && node.signature().output->contains_reference();
        if (node.node_template().may_pend_evaluation(node.signature()) || node_plan.state.node.size != 0 ||
            !node.dynamic_reference_input_types().empty() || has_reference_input || has_reference_output ||
            (node_plan.output.runtime_type != nullptr && node_plan.output.runtime_type->is_reference())) {
            result.supports_borrowed_evaluation = false;
        }
    }
    plan_fixed_storage(result);

    return result;
}

} // namespace

class CompiledGraphBuilder {
  public:
    static void build(CompiledGraph &graph, const GraphWiring &wiring, allocator memory,
                      std::span<std::size_t> node_indices = {}) {
        if (!wiring.is_finalized()) {
            throw std::invalid_argument{"a GraphWiring must be finalized before compilation"};
        }
        auto plan = make_compilation_plan(wiring);
        if (plan.wired_node_indices.size() > CompiledGraph::count_mask) {
            throw std::length_error{"a compiled graph has too many nodes"};
        }
        if (plan.scalar_destruction_count > CompiledGraph::count_mask) {
            throw std::length_error{"a compiled graph has too many scalars"};
        }
        for (auto &node_index : node_indices) {
            if (node_index >= plan.compiled_node_indices.size() || plan.is_lowered_node(node_index)) {
                throw std::logic_error{"an inner graph boundary cannot use a lowered node"};
            }
            node_index = plan.compiled_node_index(node_index);
        }

        set_graph_metadata(graph, plan);

        auto *storage = static_cast<std::byte *>(memory.allocate_bytes(plan.fixed.size, plan.fixed.alignment));
        graph.fixed_storage_ = storage;
        auto release = util::scope_exit{[&]() noexcept {
            memory.deallocate_bytes(storage, plan.fixed.size, plan.fixed.alignment);
            graph.fixed_storage_ = nullptr;
        }};
        construct_storage(plan, storage, memory);
        release.release();
    }

  private:
    static void set_graph_metadata(CompiledGraph &graph, const CompilationPlan &plan) {
        const auto fixed_size = packed_offset(plan.fixed.size, "compiled fixed storage reaches 1 GiB");
        graph.fixed_storage_size_and_flags_ =
            fixed_size | (plan.uses_variable_memory ? CompiledGraph::variable_memory_flag : std::uint32_t{}) |
            (plan.uses_timed_scheduling ? CompiledGraph::timed_scheduling_flag : std::uint32_t{});
        graph.runtime_storage_size_ = packed_offset(plan.runtime.size, "runtime storage reaches 1 GiB");
        graph.node_count_and_fixed_alignment_ =
            static_cast<std::uint32_t>(plan.wired_node_indices.size()) |
            (alignment_exponent(plan.fixed.alignment, "invalid compiled storage alignment")
             << CompiledGraph::alignment_shift);
        graph.scalar_count_and_runtime_alignment_ =
            static_cast<std::uint32_t>(plan.scalar_destruction_count) |
            (alignment_exponent(plan.runtime.alignment, "invalid runtime storage alignment")
             << CompiledGraph::alignment_shift);
    }

    static void construct_storage(const CompilationPlan &plan, std::byte *storage, allocator upstream_memory) {
        construct_header(plan, storage);
        construct_wiring_paths(plan, storage);
        if (plan.has_lifecycle()) {
            construct_starts(plan, storage);
            construct_stops(plan, storage);
        }

        auto constructed_scalars = std::vector<ConstructedScalar>{};
        constructed_scalars.reserve(plan.scalar_destruction_count);
        auto constructed_node_data = std::vector<ConstructedNodeData>{};
        constructed_node_data.reserve(plan.nodes.size());
        auto variable_memory = allocator{*std::pmr::null_memory_resource()};
        std::pmr::unsynchronized_pool_resource *variable_pool = nullptr;
        auto rollback = util::scope_exit{[&]() noexcept {
            for (auto data = constructed_node_data.rbegin(); data != constructed_node_data.rend(); ++data) {
                data->destruction_function(data->data);
            }
            for (auto scalar = constructed_scalars.rbegin(); scalar != constructed_scalars.rend(); ++scalar) {
                scalar->runtime_type->destroy(scalar->data, variable_memory);
            }

            if (variable_pool != nullptr) {
                std::destroy_at(variable_pool);
            }
        }};

        if (plan.uses_variable_memory) {
            auto *pool = plan.fixed.variable_memory.objects<std::pmr::unsynchronized_pool_resource>(storage).data();
            std::construct_at(pool, &upstream_memory.memory_resource());
            variable_pool = pool;
            variable_memory = allocator{*pool};
        }

        construct_initial_runtime_image(plan, storage);
        construct_output_destruction(plan, storage);
        auto scalar_destruction_index = std::size_t{};
        construct_constants(plan, storage, variable_memory, constructed_scalars, scalar_destruction_index);
        construct_nodes(plan, storage, upstream_memory, variable_memory, constructed_scalars, scalar_destruction_index,
                        constructed_node_data);
        construct_inherited_time_series(plan, storage);
        construct_borrowed_inputs(plan, storage);
        construct_service_bindings(plan, storage);
        rollback.release();
    }

    static void construct_header(const CompilationPlan &plan, std::byte *storage) {
        const auto offset = [](const StorageSection &section) {
            return packed_offset(section.offset, "compiled fixed storage reaches 1 GiB");
        };

        std::construct_at(
            plan.fixed.header.objects<details::CompiledStorageHeader>(storage).data(),
            details::CompiledStorageHeader{
                .wiring_paths_offset_and_flags =
                    offset(plan.fixed.wiring_paths) |
                    (plan.uses_queue ? details::CompiledStorageHeader::queue_flag : std::uint32_t{}) |
                    (plan.supports_borrowed_evaluation
                         ? details::CompiledStorageHeader::borrowed_evaluation_flag
                         : std::uint32_t{}),
                .wiring_path_count = static_cast<std::uint32_t>(plan.wiring_paths.paths.size()),
                .start_functions_offset = offset(plan.fixed.start_functions),
                .start_count = static_cast<std::uint32_t>(plan.starts.nodes.size()),
                .stop_functions_offset = offset(plan.fixed.stop_functions),
                .stop_count = static_cast<std::uint32_t>(plan.stops.nodes.size()),
                .scalar_destruction_runtime_types_offset = offset(plan.fixed.scalar_destruction_runtime_types),
                .output_destruction_count = static_cast<std::uint32_t>(plan.output_destruction_count),
                .variable_memory_offset =
                    offset(plan.fixed.variable_memory) |
                    (plan.uses_cycle_callbacks ? CompiledGraph::cycle_callbacks_flag : std::uint32_t{}) |
                    (!plan.constants.empty() ? CompiledGraph::constants_flag : std::uint32_t{}),
                .initial_runtime_image_offset =
                    offset(plan.fixed.initial_runtime_image) |
                    (!plan.inherited_time_series.empty() ? CompiledGraph::inherited_time_series_flag
                                                         : std::uint32_t{}) |
                    (!plan.service_bindings.empty() ? CompiledGraph::service_bindings_flag : std::uint32_t{}),
                .borrowed_input_metadata_offset =
                    plan.borrowed_inputs.empty() ? 0 : offset(plan.fixed.borrowed_input_metadata),
            });

        if (!plan.constants.empty()) {
            std::construct_at(plan.fixed.constant_metadata.objects<details::ConstantMetadata>(storage).data(),
                              details::ConstantMetadata{
                                  .start_time_offset = *plan.runtime.constant_start_time_offset,
                                  .notification_offset = offset(plan.fixed.constant_notifications),
                                  .notification_count = static_cast<std::uint32_t>(plan.constant_notifications.size()),
                              });
        }
        if (!plan.inherited_time_series.empty()) {
            std::construct_at(
                plan.fixed.inherited_time_series_metadata.objects<details::InheritedTimeSeriesMetadata>(storage).data(),
                details::InheritedTimeSeriesMetadata{
                    .requirements_offset = offset(plan.fixed.inherited_time_series_bindings),
                    .requirement_count = static_cast<std::uint32_t>(plan.inherited_time_series.size()),
                });
        }
        if (!plan.service_bindings.empty()) {
            std::construct_at(
                plan.fixed.service_binding_metadata.objects<details::ServiceBindingMetadata>(storage).data(),
                details::ServiceBindingMetadata{
                    .runtime_bindings_offset = *plan.runtime.service_bindings_offset,
                    .provider_nodes_offset = offset(plan.fixed.service_binding_provider_nodes),
                    .binding_count = static_cast<std::uint32_t>(plan.service_bindings.size()),
                });
        }
        if (!plan.borrowed_inputs.empty()) {
            std::construct_at(
                plan.fixed.borrowed_input_metadata.objects<details::BorrowedInputMetadata>(storage).data(),
                details::BorrowedInputMetadata{
                    .bindings_offset = offset(plan.fixed.borrowed_input_bindings),
                    .binding_count = static_cast<std::uint32_t>(plan.borrowed_inputs.size()),
                });
        }

        auto notifications = plan.fixed.constant_notifications.objects<InputNotificationSubscriber>(storage);
        for (auto index = std::size_t{}; index < notifications.size(); ++index) {
            const auto &source = plan.constant_notifications[index];
            std::construct_at(notifications.data() + index, source.function, source.data, source.node_index);
        }
    }

    static void construct_wiring_paths(const CompilationPlan &plan, std::byte *storage) {
        std::uninitialized_copy(plan.wiring_paths.node_paths.begin(), plan.wiring_paths.node_paths.end(),
                                plan.fixed.node_paths.objects<std::uint32_t>(storage).data());

        auto compiled_paths = plan.fixed.wiring_paths.objects<WiringPath>(storage);
        for (auto i = std::size_t{}; i < plan.wiring_paths.paths.size(); ++i) {
            const auto &source = *plan.wiring_paths.paths[i];
            const auto *parent = plan.wiring_paths.parents[i] == no_wiring_path
                                     ? nullptr
                                     : compiled_paths.data() + plan.wiring_paths.parents[i];
            std::construct_at(compiled_paths.data() + i, WiringPath{parent, source.name, source.label});
        }
    }

    static void construct_starts(const CompilationPlan &plan, std::byte *storage) {
        std::uninitialized_copy(plan.starts.functions.begin(), plan.starts.functions.end(),
                                plan.fixed.start_functions.objects<NodeFunction>(storage).data());
        std::uninitialized_copy(plan.starts.nodes.begin(), plan.starts.nodes.end(),
                                plan.fixed.start_nodes.objects<std::uint32_t>(storage).data());
    }

    static void construct_stops(const CompilationPlan &plan, std::byte *storage) {
        std::uninitialized_copy(plan.stops.functions.begin(), plan.stops.functions.end(),
                                plan.fixed.stop_functions.objects<NodeFunction>(storage).data());
        std::uninitialized_copy(plan.stops.nodes.begin(), plan.stops.nodes.end(),
                                plan.fixed.stop_nodes.objects<std::uint32_t>(storage).data());
    }

    static void construct_initial_runtime_image(const CompilationPlan &plan, std::byte *fixed_storage) {
        auto runtime_image = plan.fixed.initial_runtime_image.objects<std::byte>(fixed_storage);
        std::fill(runtime_image.begin(), runtime_image.end(), std::byte{});
        auto scheduling = std::span{reinterpret_cast<SchedulingInfo *>(runtime_image.data()),
                                    plan.runtime.schedule_word_count};
        std::uninitialized_value_construct(scheduling.begin(), scheduling.end());
        const auto runtime_offset = runtime_image_data_offset(scheduling.size(), plan.runtime.alignment);
        auto runtime_storage = runtime_image.subspan(runtime_offset, plan.runtime.size);

        if (plan.runtime.constant_start_time_offset) {
            auto *start_time =
                reinterpret_cast<TimeSeriesState *>(runtime_storage.data() + *plan.runtime.constant_start_time_offset);
            std::construct_at(start_time, MIN_DT);
        }

        for (const auto node_index : plan.wired_node_indices) {
            const auto &node = plan.nodes[node_index];
            const auto &inputs = plan.node_plans[node_index].time_series_inputs;
            for (const auto &input : inputs) {
                if (input.dynamic_activity) {
                    runtime_storage[input.activity_offset] =
                        std::byte{node.signature().parameters[input.parameter].activity == active};
                }
            }
            const auto required_inputs_are_constants = std::ranges::all_of(inputs, [&](const auto &input) {
                return input.default_value || node.signature().parameters[input.parameter].validity == optional ||
                       (input.wired_input.is_output() &&
                        plan.constant_for_output(input.wired_input.index()) != nullptr);
            });
            const auto compiled_node = plan.compiled_node_index(node_index);
            auto &info = scheduling[compiled_node / CompiledGraph::schedule_word_bits];
            const auto mask = ScheduleWord{1} << (compiled_node % CompiledGraph::schedule_word_bits);
            info.inputs_valid(mask, required_inputs_are_constants);
            if (std::ranges::any_of(inputs, [&](const auto &input) {
                    return node.signature().parameters[input.parameter].activity == active &&
                           (input.default_value || (input.wired_input.is_output() &&
                                                    plan.constant_for_output(input.wired_input.index()) != nullptr));
                })) {
                info.evaluation |= mask;
            }
            if (node.node_template().schedule_on_start(node.signature())) {
                info.evaluation |= mask;
            }
        }

        for (const auto node_index : plan.wired_node_indices) {
            const auto &node = plan.node_plans[node_index];
            const auto &output = node.output;
            if (output.runtime_type == nullptr) {
                continue;
            }

            std::construct_at(reinterpret_cast<TimeSeriesState *>(runtime_storage.data() + output.last_updated_offset));
            output.runtime_type->construct_initial_value(runtime_storage.data() + output.value_offset);
        }

        const auto initialize_item_samples = [&](this const auto &self,
                                                 std::span<const ItemSubscriptionPlan> items) -> void {
            for (const auto &item : items) {
                std::construct_at(
                    reinterpret_cast<TimeSeriesState *>(runtime_storage.data() + item.sampled_last_updated_offset));
                self(item.items);
            }
        };
        for (const auto node_index : plan.wired_node_indices) {
            const auto &node = plan.node_plans[node_index];
            initialize_item_samples(node.item_subscriptions);
            for (const auto &reference : node.time_series_inputs) {
                if (!reference.dereferences_reference) {
                    continue;
                }
                std::construct_at(reinterpret_cast<ReferenceInputStorage *>(runtime_storage.data() + reference.offset));
                if (reference.sampled_reference) {
                    const auto sample_offset = details::ReferenceInputSampleStorage::offset_for(
                        reference.offset, reference.dynamic_activity);
                    std::construct_at(reinterpret_cast<details::ReferenceInputSampleStorage *>(
                        runtime_storage.data() + sample_offset));
                }
            }
        }

        for (const auto &requirement : plan.inherited_time_series) {
            std::construct_at(
                reinterpret_cast<TimeSeriesState *>(runtime_storage.data() + requirement.last_updated_offset));
            std::construct_at(reinterpret_cast<Reference *>(runtime_storage.data() + requirement.value_offset));
        }

        for (const auto i : plan.wired_node_indices) {
            const auto &state = plan.node_plans[i].state;
            if (state.node.size == 0) {
                continue;
            }

            const auto &node = plan.nodes[i];
            node.node_template().construct_initial_state(state.node, runtime_storage.data() + state.offset);
        }
    }

    static void construct_output_destruction(const CompilationPlan &plan, std::byte *fixed_storage) {
        auto runtime_types =
            plan.fixed.output_destruction_runtime_types.objects<const TimeSeriesRuntimeType *>(fixed_storage).begin();
        auto offsets = plan.fixed.output_destruction_offsets.objects<std::uint32_t>(fixed_storage).begin();

        for (const auto node_index : plan.wired_node_indices) {
            const auto &output = plan.node_plans[node_index].output;
            if (output.runtime_type == nullptr || !output.runtime_type->requires_destruction()) {
                continue;
            }

            std::construct_at(std::to_address(runtime_types++), output.runtime_type);
            std::construct_at(std::to_address(offsets++), output.value_offset);
        }
    }

    static void construct_constants(const CompilationPlan &plan, std::byte *storage, allocator variable_memory,
                                    std::vector<ConstructedScalar> &constructed_scalars,
                                    std::size_t &scalar_destruction_index) {
        auto destruction_runtime_types =
            plan.fixed.scalar_destruction_runtime_types.objects<const ScalarRuntimeType *>(storage);
        auto destruction_offsets = plan.fixed.scalar_destruction_offsets.objects<std::uint32_t>(storage);
        for (const auto &constant : plan.constants) {
            const auto &scalar = plan.nodes[constant.node].scalar_values()[constant.scalar];
            auto *destination = storage + constant.storage.offset;
            scalar.runtime_type().copy_construct(destination, scalar.data(), variable_memory);
            if (!scalar.runtime_type().requires_destruction()) {
                continue;
            }

            constructed_scalars.push_back({&scalar.runtime_type(), destination});
            std::construct_at(destruction_runtime_types.data() + scalar_destruction_index, &scalar.runtime_type());
            std::construct_at(destruction_offsets.data() + scalar_destruction_index,
                              packed_offset(constant.storage.offset, "compiled fixed storage reaches 1 GiB"));
            ++scalar_destruction_index;
        }
    }

    static void construct_parameters(const CompilationPlan &plan, std::size_t node_index, std::byte *storage,
                                     allocator variable_memory, std::vector<ConstructedScalar> &constructed_scalars,
                                     std::size_t &scalar_destruction_index) {
        const auto &node = plan.nodes[node_index];
        const auto &layout = plan.node_plans[node_index].fixed;
        const auto &node_plan = plan.node_plans[node_index];
        auto *node_storage = storage + layout.offset;
        auto parameters = layout.parameters.objects<ParameterBinding>(node_storage);
        std::uninitialized_copy(node_plan.input_node_indices.begin(), node_plan.input_node_indices.end(),
                                layout.input_node_indices.objects<std::uint32_t>(node_storage).begin());
        auto controlled_inputs = layout.controlled_inputs.objects<ControlledInput>(node_storage);
        auto default_inputs = layout.default_inputs.objects<DefaultInputDescriptor>(node_storage);
        auto destruction_runtime_types =
            plan.fixed.scalar_destruction_runtime_types.objects<const ScalarRuntimeType *>(storage);
        auto destruction_offsets = plan.fixed.scalar_destruction_offsets.objects<std::uint32_t>(storage);

        auto input_index = std::size_t{};
        auto controlled_input_index = std::size_t{};
        auto scalar_index = std::size_t{};
        auto default_input_index = std::size_t{};
        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            const auto &parameter = node.signature().parameters[parameter_index];

            if (parameter.type->is_time_series()) {
                const auto &input = plan.node_plans[node_index].time_series_inputs[input_index++];
                if (input.dynamic_activity) {
                    std::construct_at(controlled_inputs.data() + controlled_input_index,
                                      ControlledInput{input.activity_offset});
                    ++controlled_input_index;
                }
                if (input.default_value) {
                    auto *descriptor = default_inputs.data() + default_input_index++;
                    auto source = ParameterBinding{};
                    if (input.wired_input.is_output()) {
                        if (const auto *constant = plan.constant_for_output(input.wired_input.index())) {
                            const auto source_offset =
                                static_cast<std::size_t>(reinterpret_cast<std::byte *>(&descriptor->source) - storage);
                            source = ParameterBinding::constant(
                                *plan.runtime.constant_start_time_offset,
                                packed_offset(constant->storage.offset - source_offset,
                                              "a constant is more than 1 GiB from its parameter"),
                                optional);
                        }
                    }
                    if (source.mode() == ParameterBindingMode::scalar && input.wired_input.is_borrowed()) {
                        source = ParameterBinding::borrowed(static_cast<std::uint32_t>(input.wired_input.index()),
                                                           optional);
                    }
                    if (source.mode() == ParameterBindingMode::scalar) {
                        if (input.dereferences_reference) {
                            source = ParameterBinding::reference(input.output_last_updated_offset, input.offset,
                                                                 optional, input.sampled_reference,
                                                                 input.dynamic_activity);
                        } else {
                            const auto resolved_output = input.wired_input.is_inherited()
                                                             ? OutputLayout{plan.inherited_time_series_at(
                                                                                input.wired_input.index())
                                                                                .last_updated_offset,
                                                                            plan.inherited_time_series_at(
                                                                                input.wired_input.index())
                                                                                .value_offset,
                                                                            nullptr}
                                                             : wired_output_layout(input.wired_input.index(),
                                                                                   plan.outputs, plan.node_plans);
                            source = ParameterBinding{resolved_output.last_updated_offset, resolved_output.value_offset,
                                                      optional};
                        }
                    }

                    const auto *fallback = plan.constant_for_output(input.fallback.index());
                    if (fallback == nullptr) {
                        throw std::logic_error{"a default value fallback is not constant"};
                    }

                    std::construct_at(descriptor,
                                      DefaultInputDescriptor{source, storage + fallback->storage.offset,
                                                             node.node_template().input_notification(input_index - 1),
                                                             *plan.runtime.constant_start_time_offset});
                    const auto descriptor_offset =
                        static_cast<std::size_t>(reinterpret_cast<std::byte *>(descriptor) -
                                                 reinterpret_cast<std::byte *>(parameters.data() + parameter_index));
                    std::construct_at(
                        parameters.data() + parameter_index,
                        ParameterBinding::default_value(
                            input.default_input_offset,
                            packed_offset(descriptor_offset, "a default input descriptor is more than 1 GiB away"),
                            parameter.validity));
                    continue;
                }
                if (input.wired_input.is_output()) {
                    if (const auto *constant = plan.constant_for_output(input.wired_input.index())) {
                        const auto parameter_offset =
                            layout.offset + layout.parameters.offset + parameter_index * sizeof(ParameterBinding);
                        std::construct_at(parameters.data() + parameter_index,
                                          ParameterBinding::constant(
                                              *plan.runtime.constant_start_time_offset,
                                              packed_offset(constant->storage.offset - parameter_offset,
                                                            "a constant is more than 1 GiB from its parameter"),
                                              parameter.validity));
                        continue;
                    }
                }
                if (input.wired_input.is_borrowed()) {
                    std::construct_at(parameters.data() + parameter_index,
                                      ParameterBinding::borrowed(static_cast<std::uint32_t>(input.wired_input.index()),
                                                                 parameter.validity));
                    continue;
                }
                if (input.dereferences_reference) {
                    std::construct_at(parameters.data() + parameter_index,
                                      ParameterBinding::reference(input.output_last_updated_offset, input.offset,
                                                                  parameter.validity, input.sampled_reference,
                                                                  input.dynamic_activity));
                } else {
                    const auto resolved_output =
                        input.wired_input.is_inherited()
                            ? OutputLayout{plan.inherited_time_series_at(input.wired_input.index()).last_updated_offset,
                                           plan.inherited_time_series_at(input.wired_input.index()).value_offset,
                                           nullptr}
                            : wired_output_layout(input.wired_input.index(), plan.outputs, plan.node_plans);
                    std::construct_at(parameters.data() + parameter_index,
                                      ParameterBinding{resolved_output.last_updated_offset,
                                                       resolved_output.value_offset, parameter.validity});
                }
                continue;
            }

            const auto current_scalar_index = scalar_index++;
            const auto &scalar = node.scalar_values()[current_scalar_index];
            if (!scalar.has_value()) {
                std::construct_at(parameters.data() + parameter_index, ParameterBinding{});
                continue;
            }

            const auto &runtime_type = scalar.runtime_type();
            const auto scalar_offset = layout.scalar_offsets[current_scalar_index];
            auto *scalar_data = node_storage + scalar_offset;
            runtime_type.copy_construct(scalar_data, scalar.data(), variable_memory);

            const auto parameter_offset = layout.parameters.offset + parameter_index * sizeof(ParameterBinding);
            std::construct_at(parameters.data() + parameter_index,
                              ParameterBinding{packed_offset(scalar_offset - parameter_offset,
                                                             "a scalar is more than 1 GiB from its parameter")});

            if (!runtime_type.requires_destruction()) {
                continue;
            }

            constructed_scalars.push_back({&runtime_type, scalar_data});
            std::construct_at(destruction_runtime_types.data() + scalar_destruction_index, &runtime_type);
            std::construct_at(destruction_offsets.data() + scalar_destruction_index,
                              packed_offset(static_cast<std::size_t>(static_cast<std::byte *>(scalar_data) - storage),
                                            "compiled fixed storage reaches 1 GiB"));
            ++scalar_destruction_index;
        }
    }

    static void construct_runtime_types(const CompilationPlan &plan, std::size_t node_index, std::byte *storage) {
        const auto &node = plan.nodes[node_index];
        const auto &layout = plan.node_plans[node_index].fixed;
        auto *node_storage = storage + layout.offset;
        auto runtime_types = layout.runtime_types.objects<const RuntimeType *>(node_storage);
        const auto counts = runtime_type_counts(node);
        if (layout.runtime_type_metadata.count != 0) {
            std::construct_at(layout.runtime_type_metadata.objects<RuntimeTypeMetadata>(node_storage).data(),
                              RuntimeTypeMetadata{static_cast<std::uint32_t>(layout.runtime_types.count),
                                                  static_cast<std::uint32_t>(counts.dynamic_reference_begin())});
        }
        std::ranges::fill(runtime_types, nullptr);
        const auto set_runtime_type = [&](std::size_t index, const RuntimeType *runtime_type) {
            runtime_types[runtime_types.size() - index - 1] = runtime_type;
        };
        auto all_valid_inputs = layout.all_valid_inputs.objects<AllValidInputDescriptor>(node_storage);
        auto all_valid_runtime_type_index = std::size_t{};
        auto input_index = std::size_t{};
        auto scalar_index = std::size_t{};

        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            if (!node.signature().parameters[parameter_index].type->is_time_series()) {
                const auto &scalar = node.scalar_values()[scalar_index++];
                if (node.node_template().uses_scalar_input_runtime_type(node.signature(), parameter_index)) {
                    const auto *runtime_type = [&] {
                        if (scalar.has_value()) {
                            return &scalar.runtime_type();
                        }

                        const auto &type =
                            static_cast<const ScalarType &>(*node.signature().parameters[parameter_index].type);
                        return &resolve_scalar_runtime_type(type, {});
                    }();
                    set_runtime_type(parameter_index, runtime_type);
                }
                continue;
            }

            const auto &input = plan.node_plans[node_index].time_series_inputs[input_index++];
            const auto &parameter =
                static_cast<const TimeSeriesType &>(*node.signature().parameters[parameter_index].type);

            const auto needs_all_valid_check =
                node.signature().parameters[parameter_index].validity == InputValidity::all_valid &&
                !node.signature().parameters[parameter_index].valid_items &&
                !input.constant_binding;
            const auto needs_runtime_type =
                node.node_template().uses_time_series_input_runtime_type(node.signature(), parameter_index);
            if (!needs_all_valid_check && !needs_runtime_type) {
                continue;
            }

            auto *runtime_type = [&] {
                if (input.wired_input.is_borrowed()) {
                    return &resolve_time_series_runtime_type(parameter);
                }
                if (input.wired_input.is_output() &&
                    plan.constant_for_output(input.wired_input.index()) != nullptr) {
                    return &resolve_time_series_runtime_type(parameter);
                }
                if (input.wired_input.is_inherited()) {
                    return &resolve_time_series_runtime_type(parameter);
                }
                const auto output = wired_output_layout(input.wired_input.index(), plan.outputs, plan.node_plans);
                return input_runtime_type(input, output.runtime_type, parameter);
            }();

            if (needs_all_valid_check) {
                if (runtime_type == nullptr) {
                    runtime_type = &resolve_time_series_runtime_type(parameter);
                }
                all_valid_inputs[all_valid_runtime_type_index++] = {
                    static_cast<std::uint32_t>(parameter_index),
                    runtime_type,
                    type_cast<TimeSeriesType>(node.signature().parameters[parameter_index].type),
                };
            }

            if (needs_runtime_type) {
                set_runtime_type(parameter_index, runtime_type);
            }
        }

        if (node.node_template().uses_output_runtime_type()) {
            const auto *runtime_type = plan.node_plans[node_index].output.runtime_type;
            set_runtime_type(counts.dynamic_reference_begin() - 1, runtime_type);
        }

        auto dynamic_reference_input_index = counts.dynamic_reference_begin();
        for (const auto *type : node.dynamic_reference_input_types()) {
            set_runtime_type(dynamic_reference_input_index++, &resolve_time_series_runtime_type(*type));
        }

        if (!layout.all_valid_metadata.objects<AllValidInputMetadata>(node_storage).empty()) {
            std::construct_at(layout.all_valid_metadata.objects<AllValidInputMetadata>(node_storage).data(),
                              AllValidInputMetadata{static_cast<std::uint32_t>(all_valid_runtime_type_index)});
        }
    }

    static const TimeSeriesRuntimeType &
    requested_reference_runtime_type(const CompilationPlan &plan, const ReferenceInputSubscriberPlan &subscriber) {
        const auto node_index = plan.wired_node_indices[subscriber.node_index];
        const auto &input = plan.node_plans[node_index].time_series_inputs[subscriber.input_index];
        const auto &parameter = *plan.nodes[node_index].signature().parameters[input.parameter].type;
        return resolve_time_series_runtime_type(type_cast<TimeSeriesType>(parameter));
    }

    static std::byte *construct_output_notifications(const CompilationPlan &plan, const SubscriptionPlan &subscribers,
                                                     std::byte *destination) {
        destination = reinterpret_cast<std::byte *>(
            align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(OutputNotification)));
        const auto notifying_reference_count =
            std::ranges::count_if(subscribers.reference_inputs,
                                  [](const auto &subscriber) { return subscriber.notification.function != nullptr; });
        const auto reference_count = subscribers.reference_inputs.size() - notifying_reference_count;
        const auto controlled_bytes = subscribers.controlled_inputs.size() * sizeof(ControlledInputSubscriber);
        const auto reference_bytes = reference_count * sizeof(details::ReferenceInputSubscriber);
        const auto notifying_reference_bytes =
            notifying_reference_count * sizeof(details::ReferenceInputNotificationSubscriber);
        const auto mixed_references = reference_count != 0 && notifying_reference_count != 0;
        auto reference_notification_bytes = reference_bytes + notifying_reference_bytes;
        if (mixed_references) {
            reference_notification_bytes += sizeof(details::ReferenceInputNotificationHeader);
        }
        const auto input_bytes = subscribers.input_subscribers.size() * sizeof(InputNotificationSubscriber);
        const auto reference_block = reference_notification_bytes == 0
                                         ? std::size_t{}
                                         : align_up(sizeof(OutputNotification) + reference_notification_bytes,
                                                    alignof(OutputNotification));
        const auto controlled_block =
            subscribers.controlled_inputs.empty()
                ? std::size_t{}
                : align_up(sizeof(OutputNotification) + controlled_bytes, alignof(OutputNotification));
        const auto input_block = subscribers.input_subscribers.empty()
                                     ? std::size_t{}
                                     : align_up(sizeof(OutputNotification) + input_bytes, alignof(OutputNotification));
        const auto total_bytes = controlled_block + reference_block + input_block;
        auto *block = destination;

        if (!subscribers.controlled_inputs.empty()) {
            const auto following = total_bytes - sizeof(OutputNotification) - controlled_bytes;
            auto *notification =
                std::construct_at(reinterpret_cast<OutputNotification *>(block), notify_controlled_inputs,
                                  static_cast<std::uint32_t>(controlled_bytes), static_cast<std::uint32_t>(following));
            auto *records = reinterpret_cast<ControlledInputSubscriber *>(notification + 1);
            for (const auto &subscriber : subscribers.controlled_inputs) {
                const auto node_index = plan.wired_node_indices[subscriber.node_index];
                const auto &input = plan.node_plans[node_index].time_series_inputs[subscriber.input_index];
                std::construct_at(records++, input.activity_offset, subscriber.node_index, subscriber.required);
            }
            block += controlled_block;
        }

        const auto construct_plain_reference_records = [&](auto *records) {
            for (const auto &subscriber : subscribers.reference_inputs) {
                if (subscriber.notification.function != nullptr) {
                    continue;
                }

                const auto node_index = plan.wired_node_indices[subscriber.node_index];
                const auto &input = plan.node_plans[node_index].time_series_inputs[subscriber.input_index];
                std::construct_at(records++, input.offset, subscriber.node_index, subscriber.active,
                                  subscriber.required, subscriber.controlled_activity,
                                  requested_reference_runtime_type(plan, subscriber), subscriber.sampled_reference);
            }
        };
        const auto construct_notifying_reference_records = [&](auto *records) {
            for (const auto &subscriber : subscribers.reference_inputs) {
                if (subscriber.notification.function == nullptr) {
                    continue;
                }

                const auto node_index = plan.wired_node_indices[subscriber.node_index];
                const auto &input = plan.node_plans[node_index].time_series_inputs[subscriber.input_index];
                std::construct_at(records++, input.offset, subscriber.notification.data, subscriber.node_index,
                                  subscriber.active, subscriber.required, subscriber.controlled_activity,
                                  requested_reference_runtime_type(plan, subscriber), subscriber.sampled_reference,
                                  subscriber.notification.function);
            }
        };

        if (reference_notification_bytes != 0) {
            const auto following = total_bytes - static_cast<std::size_t>(block - destination) -
                                   sizeof(OutputNotification) - reference_notification_bytes;
            if (mixed_references) {
                auto *notification = std::construct_at(
                    reinterpret_cast<OutputNotification *>(block), details::notify_mixed_reference_inputs,
                    static_cast<std::uint32_t>(reference_notification_bytes), static_cast<std::uint32_t>(following));
                auto *header = std::construct_at(reinterpret_cast<details::ReferenceInputNotificationHeader *>(
                                                      notification + 1),
                                                  details::ReferenceInputNotificationHeader{
                                                      static_cast<std::uint32_t>(reference_count),
                                                      static_cast<std::uint32_t>(notifying_reference_count)});
                auto *plain_records = reinterpret_cast<details::ReferenceInputSubscriber *>(header + 1);
                construct_plain_reference_records(plain_records);
                auto *notifying_records = reinterpret_cast<details::ReferenceInputNotificationSubscriber *>(
                    reinterpret_cast<std::byte *>(plain_records) + reference_bytes);
                construct_notifying_reference_records(notifying_records);
            } else if (reference_count != 0) {
                auto *notification = std::construct_at(
                    reinterpret_cast<OutputNotification *>(block), details::notify_reference_inputs,
                    static_cast<std::uint32_t>(reference_bytes), static_cast<std::uint32_t>(following));
                construct_plain_reference_records(reinterpret_cast<details::ReferenceInputSubscriber *>(
                    notification + 1));
            } else {
                auto *notification = std::construct_at(
                    reinterpret_cast<OutputNotification *>(block), details::notify_reference_inputs_with_notifications,
                    static_cast<std::uint32_t>(notifying_reference_bytes), static_cast<std::uint32_t>(following));
                construct_notifying_reference_records(reinterpret_cast<details::ReferenceInputNotificationSubscriber *>(
                    notification + 1));
            }
            block += reference_block;
        }

        if (!subscribers.input_subscribers.empty()) {
            auto *notification = std::construct_at(reinterpret_cast<OutputNotification *>(block),
                                                   notify_input_subscribers, static_cast<std::uint32_t>(input_bytes));
            auto *records = reinterpret_cast<InputNotificationSubscriber *>(notification + 1);
            for (const auto &subscriber : subscribers.input_subscribers) {
                std::construct_at(records++, subscriber.function, subscriber.data, subscriber.node_index);
            }
        }

        return destination + total_bytes;
    }

    static void construct_inherited_time_series(const CompilationPlan &plan, std::byte *storage) {
        auto bindings = plan.fixed.inherited_time_series_bindings.objects<details::InheritedTimeSeriesBinding>(storage);
        for (auto index = std::size_t{}; index < plan.inherited_time_series.size(); ++index) {
            const auto &requirement = plan.inherited_time_series[index];
            std::uninitialized_copy(requirement.subscriptions.nodes.begin(), requirement.subscriptions.nodes.end(),
                                    requirement.subscribers.objects<Subscriber>(storage).data());
            if (requirement.output_notification.count != 0) {
                construct_output_notifications(plan, requirement.subscriptions,
                                               storage + requirement.output_notification.offset);
            }

            std::construct_at(
                bindings.data() + index,
                details::InheritedTimeSeriesBinding{
                    .last_updated_offset = requirement.last_updated_offset,
                    .value_offset = requirement.value_offset,
                    .subscribers_offset =
                        packed_offset(requirement.subscribers.offset, "compiled fixed storage reaches 1 GiB"),
                    .subscriber_count = static_cast<std::uint32_t>(requirement.subscriptions.nodes.size()),
                    .notification_offset = requirement.output_notification.count == 0
                                               ? std::numeric_limits<std::uint32_t>::max()
                                               : packed_offset(requirement.output_notification.offset,
                                                               "compiled fixed storage reaches 1 GiB"),
                });
        }
    }

    static void construct_borrowed_inputs(const CompilationPlan &plan, std::byte *storage) {
        auto bindings = plan.fixed.borrowed_input_bindings.objects<details::BorrowedInputBinding>(storage);
        for (auto index = std::size_t{}; index < plan.borrowed_inputs.size(); ++index) {
            const auto &input = plan.borrowed_inputs[index];
            std::uninitialized_copy(input.subscriptions.nodes.begin(), input.subscriptions.nodes.end(),
                                    input.subscribers.objects<Subscriber>(storage).data());
            if (input.output_notification.count != 0) {
                construct_output_notifications(plan, input.subscriptions,
                                               storage + input.output_notification.offset);
            }
            std::construct_at(
                bindings.data() + index,
                details::BorrowedInputBinding{
                    .runtime_type = input.runtime_type,
                    .subscribers_offset =
                        packed_offset(input.subscribers.offset, "compiled fixed storage reaches 1 GiB"),
                    .subscriber_count = static_cast<std::uint32_t>(input.subscriptions.nodes.size()),
                    .notification_offset = input.output_notification.count == 0
                                               ? std::numeric_limits<std::uint32_t>::max()
                                               : packed_offset(input.output_notification.offset,
                                                               "compiled fixed storage reaches 1 GiB"),
                });
        }
    }

    static void construct_service_bindings(const CompilationPlan &plan, std::byte *storage) {
        auto nodes = plan.fixed.service_binding_provider_nodes.objects<std::uint32_t>(storage);
        for (auto index = std::size_t{}; index < plan.service_bindings.size(); ++index) {
            const auto &binding = plan.service_bindings[index];
            const auto node = binding.provider.has_output()
                                  ? plan.compiled_node_index(
                                        binding.provider.graph().output_node_index(binding.provider))
                                  : details::no_service_binding_provider_node;
            std::construct_at(nodes.data() + index, static_cast<std::uint32_t>(node));
        }
    }

    static std::byte *construct_item_subscribers(const CompilationPlan &plan,
                                                 std::span<const ItemSubscriptionPlan> items, std::byte *destination) {
        for (const auto &item : items) {
            destination = reinterpret_cast<std::byte *>(
                align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(ItemSubscription)));
            auto *compiled = std::construct_at(
                reinterpret_cast<ItemSubscription *>(destination), item.item,
                static_cast<std::uint32_t>(item.subscriptions.nodes.size()),
                static_cast<std::uint32_t>(item_subscriber_size(item)), item.sampled_last_updated_offset,
                !item.subscriptions.controlled_inputs.empty() || !item.subscriptions.reference_inputs.empty() ||
                    !item.subscriptions.input_subscribers.empty());
            auto *subscribers = reinterpret_cast<Subscriber *>(compiled + 1);
            std::uninitialized_copy(item.subscriptions.nodes.begin(), item.subscriptions.nodes.end(), subscribers);
            destination = reinterpret_cast<std::byte *>(subscribers + item.subscriptions.nodes.size());
            if (!item.subscriptions.controlled_inputs.empty() || !item.subscriptions.reference_inputs.empty() ||
                !item.subscriptions.input_subscribers.empty()) {
                destination = construct_output_notifications(plan, item.subscriptions, destination);
            }
            destination = reinterpret_cast<std::byte *>(
                align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(ItemSubscription)));
            destination = construct_item_subscribers(plan, item.items, destination);
            destination = reinterpret_cast<std::byte *>(
                align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(ItemSubscription)));
        }

        return destination;
    }

    static void construct_nodes(const CompilationPlan &plan, std::byte *storage, allocator compiled_memory,
                                allocator variable_memory, std::vector<ConstructedScalar> &constructed_scalars,
                                std::size_t &scalar_destruction_index,
                                std::vector<ConstructedNodeData> &constructed_node_data) {
        auto compiled_allocator = compiled_memory;
        auto node_offsets = plan.fixed.node_offsets.objects<std::uint32_t>(storage);

        for (auto compiled_node = std::size_t{}; compiled_node < plan.wired_node_indices.size(); ++compiled_node) {
            const auto i = plan.wired_node_indices[compiled_node];
            const auto &node = plan.nodes[i];
            const auto &node_plan = plan.node_plans[i];
            const auto &layout = node_plan.fixed;
            auto *node_storage = storage + layout.offset;

            construct_parameters(plan, i, storage, variable_memory, constructed_scalars, scalar_destruction_index);
            std::uninitialized_copy(node_plan.subscriptions.nodes.begin(), node_plan.subscriptions.nodes.end(),
                                    layout.subscribers.objects<Subscriber>(node_storage).data());
            if (!node_plan.subscriptions.controlled_inputs.empty() ||
                !node_plan.subscriptions.reference_inputs.empty() ||
                !node_plan.subscriptions.input_subscribers.empty()) {
                construct_output_notifications(plan, node_plan.subscriptions,
                                               node_storage + layout.output_notification.offset);
            }
            if (!node_plan.item_subscriptions.empty()) {
                std::construct_at(layout.item_subscription_root.objects<ItemSubscriptionRoot>(node_storage).data(),
                                  *node_plan.output.runtime_type,
                                  static_cast<std::uint32_t>(item_subscriber_size(node_plan.item_subscriptions)));
                construct_item_subscribers(plan, node_plan.item_subscriptions,
                                           node_storage + layout.item_subscribers.offset);
            }
            if (node_plan.state.node.size != 0) {
                const auto has_state_metadata = !node_plan.state.node.generic_runtime_types.empty();
                std::construct_at(layout.state_offset.objects<std::uint32_t>(node_storage).data(),
                                  node_plan.state.offset |
                                      (node_plan.state.requires_destruction ? CompiledNode::state_destruction_flag
                                                                            : std::uint32_t{}) |
                                      (has_state_metadata ? CompiledNode::state_metadata_flag : std::uint32_t{}));
                if (node_plan.state.requires_destruction) {
                    std::construct_at(layout.state_destruction_function.objects<NodeFunction>(node_storage).data(),
                                      node.node_template().state_destruction_function(node.signature()));
                }
                if (has_state_metadata) {
                    std::construct_at(
                        layout.state_metadata_counts.objects<StateMetadataCounts>(node_storage).data(),
                        StateMetadataCounts{
                            static_cast<std::uint16_t>(node_plan.state.node.field_offsets.size()),
                            static_cast<std::uint16_t>(node_plan.state.node.generic_runtime_types.size())});
                    std::ranges::transform(
                        node_plan.state.node.field_offsets,
                        layout.state_field_offsets.objects<std::uint32_t>(node_storage).begin(),
                        [](auto offset) { return packed_offset(offset, "node state reaches 1 GiB"); });
                    std::uninitialized_copy(
                        node_plan.state.node.generic_runtime_types.begin(),
                        node_plan.state.node.generic_runtime_types.end(),
                        layout.generic_state_runtime_types.objects<const ScalarRuntimeType *>(node_storage).data());
                }
            }
            if (node_plan.invalidation.mode == InvalidationMode::invoke) {
                std::construct_at(layout.invalidation_function.objects<NodeFunction>(node_storage).data(),
                                  node_plan.invalidation.function);
            }
            if (layout.node_invocation_metadata.count != 0) {
                const auto parameter_offset = layout.node_invocation_parameters.count == 0
                                                  ? std::uint32_t{}
                                                  : packed_offset(layout.node_invocation_parameters.offset -
                                                                      layout.compiled_node.offset,
                                                                  "compiled node metadata reaches 1 GiB");
                const auto compiled_data_offset =
                    node_plan.compiled_data.size == 0
                        ? std::uint32_t{}
                        : packed_offset(layout.compiled_data.offset - layout.compiled_node.offset,
                                        "compiled node data reaches 1 GiB");
                std::construct_at(layout.node_invocation_metadata.objects<NodeInvocationMetadata>(node_storage).data(),
                                  NodeInvocationMetadata{parameter_offset, compiled_data_offset,
                                                         node_plan.compiled_data.destruction_function});
            }

            if (node.node_template().uses_node_invocation_parameters()) {
                auto parameters =
                    layout.node_invocation_parameters.objects<NodeInvocationParameterMetadata>(node_storage);
                auto controlled_input_indices =
                    layout.node_invocation_controlled_input_indices.objects<std::uint16_t>(node_storage);
                if (!controlled_input_indices.empty()) {
                    std::uninitialized_value_construct(controlled_input_indices.begin(),
                                                       controlled_input_indices.end());
                }
                auto input_index = std::size_t{};
                auto controlled_input_index = std::uint16_t{};
                for (auto parameter = std::size_t{}; parameter < parameters.size(); ++parameter) {
                    const auto &source = node.signature().parameters[parameter];
                    auto controlled = ControlledInput::no_control;
                    if (source.type->is_time_series()) {
                        const auto &input = node_plan.time_series_inputs[input_index++];
                        if (input.dynamic_activity) {
                            controlled = controlled_input_index++;
                        }
                    }
                    std::construct_at(parameters.data() + parameter,
                                      NodeInvocationParameterMetadata{source.name, source.type});
                    if (!controlled_input_indices.empty()) {
                        controlled_input_indices[parameter] = static_cast<std::uint16_t>(controlled);
                    }
                }
            }

            if (node_plan.compiled_data.size != 0) {
                auto *data = node_storage + layout.compiled_data.offset;
                node.node_template().construct_compiled_data(node.signature(), data, compiled_allocator);
                if (node_plan.compiled_data.destruction_function != nullptr) {
                    constructed_node_data.push_back({node_plan.compiled_data.destruction_function, data});
                }
            }
            construct_runtime_types(plan, i, storage);

            std::construct_at(
                reinterpret_cast<CompiledNode *>(node_storage + layout.compiled_node.offset),
                CompiledNode{node.node_template().evaluation_function(node.signature()),
                             node_plan.output.last_updated_offset, node_plan.output.value_offset,
                             static_cast<std::uint32_t>(node.signature().parameters.size()),
                             static_cast<std::uint32_t>(node_plan.subscriptions.nodes.size()),
                             node.signature().output != nullptr, node_plan.state.node.size != 0,
                             !node_plan.subscriptions.controlled_inputs.empty() ||
                                 !node_plan.subscriptions.reference_inputs.empty() ||
                             !node_plan.subscriptions.input_subscribers.empty(),
                             !node_plan.item_subscriptions.empty(), layout.node_invocation_metadata.count != 0,
                             layout.all_valid_metadata.count != 0,
                             node.node_template().uses_output_runtime_type(),
                             static_cast<std::uint32_t>(layout.controlled_inputs.count),
                             !node_plan.input_node_indices.empty(), node_plan.invalidation.mode,
                             node.node_template().queue_mode()});
            std::construct_at(node_offsets.data() + compiled_node,
                              packed_offset(layout.offset + layout.compiled_node.offset,
                                            "compiled fixed storage reaches 1 GiB"));
        }
    }
};

std::shared_ptr<const CompiledGraph> details::compile_graph(const GraphWiring &wiring, allocator memory,
                                                            std::span<std::size_t> node_indices) {
    auto graph_allocator = std::pmr::polymorphic_allocator<CompiledGraph>{&memory.memory_resource()};
    return std::allocate_shared<CompiledGraph>(graph_allocator, CompiledGraph::CompilationTag{}, wiring, memory,
                                               node_indices);
}

CompiledGraph::CompiledGraph(const GraphWiring &wiring) : CompiledGraph(wiring, allocator{}) {}

CompiledGraph::CompiledGraph(const GraphWiring &wiring, allocator memory) : fixed_allocator_(memory) {
    CompiledGraphBuilder::build(*this, wiring, memory);
}

CompiledGraph::CompiledGraph(CompilationTag, const GraphWiring &wiring, allocator memory,
                             std::span<std::size_t> node_indices)
    : fixed_allocator_(memory) {
    CompiledGraphBuilder::build(*this, wiring, memory, node_indices);
}

const WiringPath &CompiledGraph::wiring_path(std::size_t node_index) const noexcept {
    const auto &header = storage_header();
    const auto path_index = header.node_paths(node_count())[node_index];

    return stored_objects<WiringPath>(fixed_storage_, header.wiring_paths_offset(), header.wiring_path_count)[path_index];
}

std::span<const details::InheritedTimeSeriesBinding> CompiledGraph::inherited_time_series() const noexcept {
    if (!has_inherited_time_series()) {
        return {};
    }
    const auto *metadata = storage_header().inherited_time_series_metadata(node_count(), has_constants());
    return stored_objects<details::InheritedTimeSeriesBinding>(fixed_storage_, metadata->requirements_offset,
                                                               metadata->requirement_count);
}

std::size_t CompiledGraph::inherited_time_series_count() const noexcept {
    return inherited_time_series().size();
}

const details::ServiceBindingMetadata *CompiledGraph::service_binding_metadata() const noexcept {
    if (!has_service_bindings()) {
        return nullptr;
    }
    return storage_header().service_binding_metadata(node_count(), has_constants(), has_inherited_time_series());
}

std::span<const std::uint32_t> CompiledGraph::service_binding_provider_nodes() const noexcept {
    const auto *metadata = service_binding_metadata();
    if (metadata == nullptr) {
        return {};
    }
    return stored_objects<std::uint32_t>(fixed_storage_, metadata->provider_nodes_offset, metadata->binding_count);
}

std::size_t CompiledGraph::service_binding_count() const noexcept {
    const auto *metadata = service_binding_metadata();
    return metadata == nullptr ? 0 : metadata->binding_count;
}

std::span<const Subscriber> CompiledGraph::inherited_time_series_subscribers(
    const details::InheritedTimeSeriesBinding &requirement) const noexcept {
    return stored_objects<Subscriber>(fixed_storage_, requirement.subscribers_offset, requirement.subscriber_count);
}

const OutputNotification *CompiledGraph::inherited_time_series_notification(
    const details::InheritedTimeSeriesBinding &requirement) const noexcept {
    if (requirement.notification_offset == std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    return reinterpret_cast<const OutputNotification *>(fixed_storage_ + requirement.notification_offset);
}

std::span<const details::BorrowedInputBinding> CompiledGraph::borrowed_input_bindings() const noexcept {
    const auto *metadata = borrowed_input_metadata();
    return metadata == nullptr
               ? std::span<const details::BorrowedInputBinding>{}
               : stored_objects<details::BorrowedInputBinding>(fixed_storage_, metadata->bindings_offset,
                                                               metadata->binding_count);
}

std::span<const Subscriber>
CompiledGraph::borrowed_input_subscribers(const details::BorrowedInputBinding &binding) const noexcept {
    return stored_objects<Subscriber>(fixed_storage_, binding.subscribers_offset, binding.subscriber_count);
}

const OutputNotification *
CompiledGraph::borrowed_input_notification(const details::BorrowedInputBinding &binding) const noexcept {
    if (binding.notification_offset == std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    return reinterpret_cast<const OutputNotification *>(fixed_storage_ + binding.notification_offset);
}

const std::byte *CompiledGraph::initial_runtime_storage() const noexcept {
    const auto *image = initial_runtime_image();
    const auto alignment = runtime_storage_alignment();
    const auto runtime_offset = runtime_image_data_offset(schedule_word_count(), alignment);
    return image + runtime_offset;
}

std::span<const CompiledGraph::SchedulingInfo> CompiledGraph::initial_scheduling_info() const noexcept {
    const auto *image = initial_runtime_image();
    return {reinterpret_cast<const SchedulingInfo *>(image), schedule_word_count()};
}

const std::byte *CompiledGraph::initial_runtime_image() const noexcept {
    return fixed_storage_ +
           (storage_header().initial_runtime_image_offset & initial_runtime_image_offset_mask);
}

CompiledGraph::NodeFunctions CompiledGraph::node_functions(std::uint32_t functions_offset,
                                                           std::uint32_t count) const noexcept {
    const auto functions = stored_objects<NodeFunction>(fixed_storage_, functions_offset, count);
    const auto nodes_offset = static_cast<std::uint32_t>(functions_offset + count * sizeof(NodeFunction));

    return {functions, stored_objects<std::uint32_t>(fixed_storage_, nodes_offset, count)};
}

CompiledGraph::NodeFunctions CompiledGraph::start_functions() const noexcept {
    const auto &header = storage_header();

    return node_functions(header.start_functions_offset, header.start_count);
}

CompiledGraph::NodeFunctions CompiledGraph::stop_functions() const noexcept {
    const auto &header = storage_header();

    return node_functions(header.stop_functions_offset, header.stop_count);
}

CompiledGraph::OutputDestruction CompiledGraph::output_destruction() const noexcept {
    const auto &header = storage_header();
    const auto runtime_types_offset = static_cast<std::uint32_t>(
        align_up(header.scalar_destruction_runtime_types_offset +
                     scalar_destruction_count() * (sizeof(const ScalarRuntimeType *) + sizeof(std::uint32_t)),
                 alignof(const TimeSeriesRuntimeType *)));
    const auto runtime_types = stored_objects<const TimeSeriesRuntimeType *>(fixed_storage_, runtime_types_offset,
                                                                             header.output_destruction_count);
    const auto offsets_offset = static_cast<std::uint32_t>(
        runtime_types_offset + header.output_destruction_count * sizeof(const TimeSeriesRuntimeType *));

    return {runtime_types,
            stored_objects<std::uint32_t>(fixed_storage_, offsets_offset, header.output_destruction_count)};
}

std::optional<std::size_t> CompiledGraph::constant_start_time_offset() const noexcept {
    if (const auto *metadata = constant_metadata()) {
        return metadata->start_time_offset;
    }

    return std::nullopt;
}

std::span<const InputNotificationSubscriber> CompiledGraph::constant_notifications() const noexcept {
    if (const auto *metadata = constant_metadata()) {
        return stored_objects<InputNotificationSubscriber>(fixed_storage_, metadata->notification_offset,
                                                           metadata->notification_count);
    }

    return {};
}

CompiledGraph::~CompiledGraph() {
    if (fixed_storage_ == nullptr) {
        return;
    }

    const auto &header = storage_header();

    std::pmr::unsynchronized_pool_resource *variable_pool = nullptr;
    auto variable_memory = allocator{*std::pmr::null_memory_resource()};
    if (has_variable_memory()) {
        variable_pool =
            reinterpret_cast<std::pmr::unsynchronized_pool_resource *>(fixed_storage_ + variable_memory_offset());
        variable_memory = allocator{*variable_pool};
    }

    for (auto node_index = node_count(); node_index != 0;) {
        const auto &compiled_node = node(--node_index);
        const auto *metadata = compiled_node.node_invocation_metadata();
        if (metadata == nullptr || metadata->destruction_function == nullptr) {
            continue;
        }

        metadata->destruction_function(const_cast<void *>(metadata->compiled_data(compiled_node)));
    }

    const auto count = scalar_destruction_count();
    const auto runtime_types = stored_objects<const ScalarRuntimeType *>(
        fixed_storage_, header.scalar_destruction_runtime_types_offset, count);
    const auto offsets_offset = static_cast<std::uint32_t>(header.scalar_destruction_runtime_types_offset +
                                                           count * sizeof(const ScalarRuntimeType *));
    const auto offsets = stored_objects<std::uint32_t>(fixed_storage_, offsets_offset, count);
    for (auto i = count; i > 0; --i) {
        runtime_types[i - 1]->destroy(fixed_storage_ + offsets[i - 1], variable_memory);
    }

    if (variable_pool != nullptr) {
        std::destroy_at(variable_pool);
    }

    fixed_allocator_.deallocate_bytes(fixed_storage_, fixed_storage_size(), fixed_storage_alignment());
}

} // namespace bg
