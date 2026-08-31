#include <bg/graph/compiled_graph.hpp>

#include <bg/evaluation_time.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/scalar/view.hpp>
#include <bg/time_series/ref/ref_notification.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg {
namespace {

constexpr std::size_t max_packed_offset = (std::size_t{1} << 30) - 1;
constexpr std::size_t max_parameter_count = (std::size_t{1} << 10) - 1;
constexpr std::size_t max_subscriber_count = (std::size_t{1} << 20) - 1;
constexpr std::uint32_t no_wiring_path = std::numeric_limits<std::uint32_t>::max();

using ScheduleWord = details::ScheduleWord;
using SchedulingInfo = details::SchedulingInfo;

static_assert(std::is_trivially_copyable_v<EvaluationTime>);
static_assert(alignof(EvaluationFunction) >= alignof(std::uint32_t));
static_assert(alignof(const ScalarStorageRepresentation *) >= alignof(std::uint32_t));
static_assert(alignof(const TimeSeriesStorageRepresentation *) >= alignof(std::uint32_t));

[[nodiscard]] std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept {
    const auto remainder = offset % alignment;

    return remainder == 0 ? offset : offset + alignment - remainder;
}

[[nodiscard]] std::uint32_t packed_offset(std::size_t value, const char *description) {
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
[[nodiscard]] std::span<const T> stored_objects(const std::byte *storage, std::uint32_t offset,
                                                std::size_t count) noexcept {
    if (count == 0) {
        return {};
    }

    return {reinterpret_cast<const T *>(storage + offset), count};
}

struct StorageSection {
    std::size_t offset{};
    std::size_t count{};

    template <typename T> [[nodiscard]] std::span<T> objects(std::byte *storage) const noexcept {
        return {reinterpret_cast<T *>(storage + offset), count};
    }

    template <typename T> [[nodiscard]] std::span<const T> objects(const std::byte *storage) const noexcept {
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

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }

  private:
    std::size_t size_{};
    std::size_t alignment_{1};
};

struct FixedNodeLayout {
    std::size_t offset{};
    StorageSection parameters;
    StorageSection subscribers;
    StorageSection output_notification;
    StorageSection item_subscription_root;
    StorageSection item_subscribers;
    StorageSection state_offset;
    StorageSection state_destruction_function;
    StorageSection state_metadata_counts;
    StorageSection state_field_offsets;
    StorageSection generic_state_storage_representations;
    StorageSection invalidation_function;
    StorageSection output_finalization_representation;
    StorageSection time_series_input_view_prototypes;
    StorageSection scalar_input_view_prototypes;
    StorageSection output_view_prototype;
    std::vector<std::size_t> scalar_offsets;
    std::size_t size{};
    std::size_t alignment{};
};

struct ReferenceInputSubscriberPlan {
    std::uint32_t node_index;
    std::uint32_t input_index;
    bool active;
    bool required;
};

struct SubscriptionPlan {
    std::vector<Subscriber> nodes;
    std::vector<ReferenceInputSubscriberPlan> reference_inputs;
};

struct ItemSubscriptionPlan {
    std::uint32_t item;
    std::size_t wiring_point;
    std::uint32_t sampled_last_updated_offset{};
    SubscriptionPlan subscriptions;
    std::vector<ItemSubscriptionPlan> items;
};

struct OutputLayout {
    std::uint32_t last_updated_offset{};
    std::uint32_t value_offset{};
    const TimeSeriesStorageRepresentation *storage_representation{};
};

struct StateLayout {
    std::uint32_t offset{};
    NodeStateLayout node;
    bool requires_destruction{};
};

struct TimeSeriesInputPlan {
    std::uint32_t parameter;
    std::size_t source;
    bool dereferences_reference;
    std::uint32_t offset;
    std::uint32_t source_last_updated_offset;
};

struct RuntimeStorageLayout {
    std::size_t schedule_word_count{};
    std::size_t size{};
    std::size_t alignment{};
};

struct NodePlan {
    SubscriptionPlan subscriptions;
    std::vector<ItemSubscriptionPlan> item_subscriptions;
    InvalidationBehavior invalidation;
    OutputLayout output;
    StateLayout state;
    std::vector<TimeSeriesInputPlan> time_series_inputs;
    FixedNodeLayout fixed;
};

struct WiringPathLayout {
    std::vector<const WiringPath *> paths;
    std::vector<std::uint32_t> parents;
    std::vector<std::uint32_t> node_paths;
};

struct NodeFunctionLayout {
    void add(EvaluationFunction function, std::uint32_t node) {
        if (function == nullptr) {
            return;
        }

        functions.push_back(function);
        nodes.push_back(node);
    }

    std::vector<EvaluationFunction> functions;
    std::vector<std::uint32_t> nodes;
};

struct FixedStorageLayout {
    StorageSection header;
    StorageSection node_offsets;
    StorageSection node_paths;
    StorageSection start_functions;
    StorageSection start_nodes;
    StorageSection stop_functions;
    StorageSection stop_nodes;
    StorageSection wiring_paths;
    StorageSection scalar_destruction_representations;
    StorageSection scalar_destruction_offsets;
    StorageSection output_destruction_representations;
    StorageSection output_destruction_offsets;
    StorageSection variable_memory;
    StorageSection initial_runtime_storage;
    std::size_t size{};
    std::size_t alignment{};
};

struct DynamicSubscribersPlan {
    std::size_t source;
    std::size_t node;
    std::size_t state_offset;
};

struct CompilationPlan {
    std::span<const WiredNode> nodes;
    std::span<const WiringPointRecord> wiring_points;
    std::vector<NodePlan> node_plans;
    WiringPathLayout wiring_paths;
    NodeFunctionLayout starts;
    NodeFunctionLayout stops;
    std::vector<DynamicSubscribersPlan> dynamic_subscribers;
    RuntimeStorageLayout runtime;
    FixedStorageLayout fixed;
    std::size_t scalar_destruction_count{};
    std::size_t output_destruction_count{};
    bool uses_variable_memory{};
    bool uses_future_scheduling{};

    [[nodiscard]] bool has_lifecycle() const noexcept { return !starts.nodes.empty() || !stops.nodes.empty(); }
};

struct ConstructedScalar {
    const ScalarStorageRepresentation *storage_representation;
    void *data;
};

[[nodiscard]] WiringPathLayout make_wiring_path_layout(std::span<const WiredNode> nodes) {
    auto result = WiringPathLayout{};
    result.node_paths.reserve(nodes.size());

    auto indices = std::unordered_map<const WiringPath *, std::uint32_t>{};
    auto missing = std::vector<const WiringPath *>{};
    for (const auto &node : nodes) {
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

[[nodiscard]] std::size_t root_source_node(std::size_t source, std::span<const WiringPointRecord> wiring_points) {
    while (wiring_points[source].parent != WiringPointRecord::npos) {
        source = wiring_points[source].parent;
    }

    return wiring_points[source].node_index;
}

[[nodiscard]] OutputLayout physical_source_output(std::size_t source, std::span<const WiringPointRecord> wiring_points,
                                                  std::span<const NodePlan> node_plans) {
    const auto &record = wiring_points[source];
    if (record.parent == WiringPointRecord::npos) {
        return node_plans[record.node_index].output;
    }

    const auto parent = physical_source_output(record.parent, wiring_points, node_plans);
    const auto item = parent.storage_representation->fixed_item(record.item);

    return {
        packed_offset(parent.value_offset + item.last_updated_offset, "runtime storage reaches 1 GiB"),
        packed_offset(parent.value_offset + item.value_offset, "runtime storage reaches 1 GiB"),
        &item.representation,
    };
}

[[nodiscard]] const ItemSubscriptionPlan *find_item_subscription(std::span<const ItemSubscriptionPlan> items,
                                                                 std::size_t wiring_point) noexcept {
    for (const auto &item : items) {
        if (item.wiring_point == wiring_point) {
            return &item;
        }
        if (const auto *result = find_item_subscription(item.items, wiring_point)) {
            return result;
        }
    }

    return nullptr;
}

[[nodiscard]] OutputLayout source_output(std::size_t source, std::span<const WiringPointRecord> wiring_points,
                                         std::span<const NodePlan> node_plans) {
    auto result = physical_source_output(source, wiring_points, node_plans);
    if (wiring_points[source].parent == WiringPointRecord::npos) {
        return result;
    }

    const auto source_node = root_source_node(source, wiring_points);
    result.last_updated_offset =
        find_item_subscription(node_plans[source_node].item_subscriptions, source)->sampled_last_updated_offset;
    return result;
}

[[nodiscard]] SubscriptionPlan &subscriber_list(std::size_t source, std::span<const WiringPointRecord> wiring_points,
                                                std::span<NodePlan> node_plans) {
    const auto source_index = root_source_node(source, wiring_points);
    if (wiring_points[source].parent == WiringPointRecord::npos) {
        return node_plans[source_index].subscriptions;
    }

    auto path = std::vector<std::size_t>{};
    while (wiring_points[source].parent != WiringPointRecord::npos) {
        path.push_back(source);
        source = wiring_points[source].parent;
    }

    auto *items = &node_plans[source_index].item_subscriptions;
    ItemSubscriptionPlan *leaf = nullptr;
    for (const auto wiring_point : path | std::views::reverse) {
        const auto item = wiring_points[wiring_point].item;
        auto entry = std::ranges::find(*items, item, &ItemSubscriptionPlan::item);
        if (entry == items->end()) {
            entry = items->insert(entry, ItemSubscriptionPlan{
                                             .item = static_cast<std::uint32_t>(item),
                                             .wiring_point = wiring_point,
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

void plan_time_series_inputs(CompilationPlan &plan) {
    for (auto node_index = std::size_t{}; node_index < plan.nodes.size(); ++node_index) {
        const auto &node = plan.nodes[node_index];
        auto source_index = std::size_t{};

        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            const auto &parameter = node.signature().parameters[parameter_index];
            if (!parameter.type->is_time_series()) {
                continue;
            }

            const auto source = node.input_sources()[source_index++];
            const auto source_is_reference =
                dynamic_cast<const TimeSeriesReferenceType *>(plan.wiring_points[source].type) != nullptr;
            const auto input_is_reference = dynamic_cast<const TimeSeriesReferenceType *>(parameter.type) != nullptr;
            plan.node_plans[node_index].time_series_inputs.push_back({
                .parameter = static_cast<std::uint32_t>(parameter_index),
                .source = source,
                .dereferences_reference = source_is_reference && !input_is_reference,
                .offset = 0,
                .source_last_updated_offset = 0,
            });
        }
    }
}

void plan_subscribers(std::span<const WiredNode> nodes, std::span<const WiringPointRecord> wiring_points,
                      std::span<NodePlan> node_plans) {
    const auto add_subscriber = [](std::vector<Subscriber> &subscribers, std::uint32_t node_index, bool is_active,
                                   bool is_required, bool is_active_optional) {
        if (!subscribers.empty() && !subscribers.back().has_dynamic_subscribers() &&
            subscribers.back().node_index() == node_index) {
            const auto &existing = subscribers.back();
            subscribers.back() =
                Subscriber{node_index, existing.active() || is_active, existing.required() || is_required,
                           existing.active_optional() || is_active_optional};
            return;
        }

        subscribers.emplace_back(node_index, is_active, is_required, is_active_optional);
    };

    for (auto target_index = std::size_t{}; target_index < nodes.size(); ++target_index) {
        const auto &target = nodes[target_index];
        const auto &inputs = node_plans[target_index].time_series_inputs;
        for (auto input_index = std::size_t{}; input_index < inputs.size(); ++input_index) {
            const auto &input = inputs[input_index];
            const auto &parameter = target.signature().parameters[input.parameter];
            const auto is_active = parameter.activity == active;
            const auto is_required = parameter.validity == required;
            const auto subscriber_index = static_cast<std::uint32_t>(target_index);
            auto &subscriptions = subscriber_list(input.source, wiring_points, node_plans);
            if (input.dereferences_reference) {
                subscriptions.reference_inputs.push_back({
                    .node_index = subscriber_index,
                    .input_index = static_cast<std::uint32_t>(input_index),
                    .active = is_active,
                    .required = is_required,
                });
                if (is_active) {
                    add_subscriber(subscriptions.nodes, subscriber_index, true, false, !is_required);
                }
            } else if (is_active || is_required) {
                add_subscriber(subscriptions.nodes, subscriber_index, is_active, is_required,
                               is_active && !is_required);
            }
        }
    }
}

void plan_dynamic_subscribers(CompilationPlan &plan) {
    for (auto node_index = std::size_t{}; node_index < plan.nodes.size(); ++node_index) {
        const auto &node = plan.nodes[node_index];
        const auto layout = node.node_template().dynamic_subscribers_layout(node.signature());
        if (!layout) {
            continue;
        }
        if (layout->source_input >= node.input_sources().size()) {
            throw std::logic_error{"dynamic subscriber source input is out of range"};
        }

        const auto source = plan.node_plans[node_index].time_series_inputs[layout->source_input].source;
        (void)subscriber_list(source, plan.wiring_points, plan.node_plans);
        plan.dynamic_subscribers.push_back({source, node_index, layout->state_offset});
    }
}

void resolve_dynamic_subscribers(CompilationPlan &plan) {
    for (const auto &dynamic : plan.dynamic_subscribers) {
        const auto &state = plan.node_plans[dynamic.node].state;
        if (dynamic.state_offset + sizeof(details::DynamicSubscribers *) > state.node.size) {
            throw std::logic_error{"dynamic subscribers are outside node state"};
        }

        const auto offset = state.offset + dynamic.state_offset;
        if (offset % alignof(details::DynamicSubscribers *) != 0) {
            throw std::logic_error{"dynamic subscribers are not pointer-aligned"};
        }

        subscriber_list(dynamic.source, plan.wiring_points, plan.node_plans)
            .nodes.push_back(Subscriber::dynamic_subscribers(packed_offset(offset, "runtime storage reaches 1 GiB")));
    }
}

[[nodiscard]] const TimeSeriesStorageRepresentation *
input_storage_representation(const TimeSeriesInputPlan &input, const TimeSeriesStorageRepresentation *source) {
    if (input.dereferences_reference) {
        return source->reference_target_representation();
    }

    return source;
}

[[nodiscard]] std::vector<const ScalarStorageRepresentation *>
parameter_storage_representations(const WiredNode &node, const NodePlan &node_plan,
                                  std::span<const WiringPointRecord> wiring_points,
                                  std::span<const NodePlan> node_plans) {
    auto parameter_storage_representations = std::vector<const ScalarStorageRepresentation *>{};
    parameter_storage_representations.reserve(node.signature().parameters.size());
    auto input_index = std::size_t{};
    auto scalar_index = std::size_t{};
    for (const auto &parameter : node.signature().parameters) {
        if (parameter.type->is_time_series()) {
            const auto &input = node_plan.time_series_inputs[input_index++];
            const auto output = source_output(input.source, wiring_points, node_plans);
            const auto *representation = input_storage_representation(input, output.storage_representation);
            parameter_storage_representations.push_back(representation->scalar_storage_representation());
        } else {
            const auto &scalar = node.scalar_values()[scalar_index++];
            parameter_storage_representations.push_back(scalar.has_value() ? &scalar.storage_representation()
                                                                           : nullptr);
        }
    }

    return parameter_storage_representations;
}

[[nodiscard]] const TimeSeriesStorageRepresentation *
find_output_storage_representation(const WiredNode &node, const NodePlan &node_plan,
                                   std::span<const WiringPointRecord> wiring_points,
                                   std::span<const NodePlan> node_plans) {
    const auto parameter_representations =
        parameter_storage_representations(node, node_plan, wiring_points, node_plans);
    auto input_representations = std::vector<const TimeSeriesStorageRepresentation *>{};
    input_representations.reserve(node.input_sources().size());
    for (const auto &input : node_plan.time_series_inputs) {
        const auto source = source_output(input.source, wiring_points, node_plans);
        const auto *representation = input_storage_representation(input, source.storage_representation);
        input_representations.push_back(representation);
    }

    return node.node_template().output_storage_representation(node.signature(), parameter_representations,
                                                              input_representations);
}

[[nodiscard]] OutputLayout add_output_layout(StorageLayout &storage, const WiredNode &node, const NodePlan &node_plan,
                                             std::span<const WiringPointRecord> wiring_points,
                                             std::span<const NodePlan> node_plans) {
    if (node.signature().output == nullptr) {
        return {};
    }

    const auto *representation = find_output_storage_representation(node, node_plan, wiring_points, node_plans);
    if (representation == nullptr) {
        throw std::logic_error{"node output has no storage representation"};
    }
    if (!representation->supports_byte_copy_initialization()) {
        throw std::logic_error{"output storage does not support byte-copy initialization"};
    }

    const auto last_updated = storage.add<EvaluationTime>();
    const auto value = storage.add_bytes(representation->value_size(), representation->value_alignment());

    return {packed_offset(last_updated.offset, "runtime storage reaches 1 GiB"),
            packed_offset(value.offset, "runtime storage reaches 1 GiB"), representation};
}

[[nodiscard]] StateLayout add_state_layout(StorageLayout &storage, const WiredNode &node, const NodePlan &node_plan,
                                           std::span<const WiringPointRecord> wiring_points,
                                           std::span<const NodePlan> node_plans) {
    auto layout = node.node_template().state_layout(
        node.signature(), parameter_storage_representations(node, node_plan, wiring_points, node_plans),
        node.state_types());
    if (layout.size == 0) {
        return {};
    }

    const auto state = storage.add_bytes(layout.size, layout.alignment);

    return {packed_offset(state.offset, "runtime storage reaches 1 GiB"), std::move(layout),
            node.node_template().state_destruction_function(node.signature()) != nullptr};
}

void plan_runtime_storage(CompilationPlan &plan, std::size_t schedule_word_count) {
    auto storage = StorageLayout{};
    (void)storage.add<SchedulingInfo>(schedule_word_count);
    if (plan.uses_future_scheduling) {
        (void)storage.add<void *>();
    }

    const auto add_item_samples = [&](this const auto &self, std::span<ItemSubscriptionPlan> items) -> void {
        for (auto &item : items) {
            item.sampled_last_updated_offset =
                packed_offset(storage.add<EvaluationTime>().offset, "runtime storage reaches 1 GiB");
            self(item.items);
        }
    };

    for (auto i = std::size_t{}; i < plan.nodes.size(); ++i) {
        auto &node_plan = plan.node_plans[i];
        node_plan.output = add_output_layout(storage, plan.nodes[i], node_plan, plan.wiring_points, plan.node_plans);
        add_item_samples(node_plan.item_subscriptions);

        for (auto &input : node_plan.time_series_inputs) {
            if (!input.dereferences_reference) {
                continue;
            }

            const auto source_layout = source_output(input.source, plan.wiring_points, plan.node_plans);
            const auto slot = storage.add<details::ReferenceInputStorage>();
            input.offset = packed_offset(slot.offset, "runtime storage reaches 1 GiB");
            input.source_last_updated_offset = source_layout.last_updated_offset;
        }

        node_plan.state = add_state_layout(storage, plan.nodes[i], node_plan, plan.wiring_points, plan.node_plans);
    }

    (void)packed_offset(storage.size(), "runtime storage reaches 1 GiB");

    plan.runtime = {schedule_word_count, storage.size(), storage.alignment()};
}

[[nodiscard]] std::size_t subscription_size(const SubscriptionPlan &subscriptions) noexcept {
    auto result = subscriptions.nodes.size() * sizeof(Subscriber);
    if (!subscriptions.reference_inputs.empty()) {
        result = align_up(result, alignof(OutputNotification));
        result += sizeof(OutputNotification) +
                  subscriptions.reference_inputs.size() * sizeof(details::ReferenceInputSubscriber);
    }

    return result;
}

[[nodiscard]] std::size_t item_subscriber_size(const ItemSubscriptionPlan &item);

[[nodiscard]] std::size_t item_subscriber_size(std::span<const ItemSubscriptionPlan> items) {
    auto result = std::size_t{};
    for (const auto &item : items) {
        if (item.subscriptions.nodes.size() > max_subscriber_count ||
            item.subscriptions.reference_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.items.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"a time series item has too many subscriptions"};
        }

        result += item_subscriber_size(item);
    }

    return result;
}

[[nodiscard]] std::size_t item_subscriber_size(const ItemSubscriptionPlan &item) {
    auto result = sizeof(ItemSubscribers) + subscription_size(item.subscriptions);
    result = align_up(result, alignof(ItemSubscribers));
    result += item_subscriber_size(item.items);
    return align_up(result, alignof(ItemSubscribers));
}

[[nodiscard]] FixedNodeLayout make_node_layout(const WiredNode &node, const NodePlan &node_plan) {
    if (node.signature().parameters.size() > max_parameter_count) {
        throw std::length_error{"a node has more than 1023 parameters"};
    }
    if (node_plan.subscriptions.nodes.size() > max_subscriber_count) {
        throw std::length_error{"a node has too many subscribers"};
    }
    if (node_plan.subscriptions.reference_inputs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error{"a node has too many reference input subscribers"};
    }

    auto storage = StorageLayout{};
    (void)storage.add<CompiledNode>();

    auto result = FixedNodeLayout{};
    result.parameters = storage.add<ParameterBinding>(node.signature().parameters.size());
    result.subscribers = storage.add<Subscriber>(node_plan.subscriptions.nodes.size());
    if (!node_plan.subscriptions.reference_inputs.empty()) {
        result.output_notification = storage.add<OutputNotification>();
        (void)storage.add<details::ReferenceInputSubscriber>(node_plan.subscriptions.reference_inputs.size());
    }
    if (!node_plan.item_subscriptions.empty()) {
        result.item_subscription_root = storage.add<ItemSubscriptionRoot>();
        result.item_subscribers =
            storage.add_bytes(item_subscriber_size(node_plan.item_subscriptions), alignof(ItemSubscribers));
    }
    if (node_plan.state.node.size != 0) {
        result.state_offset = storage.add<std::uint32_t>();
        if (node_plan.state.requires_destruction) {
            result.state_destruction_function = storage.add<EvaluationFunction>();
        }
        if (!node_plan.state.node.generic_storage_representations.empty()) {
            result.state_metadata_counts = storage.add<StateMetadataCounts>();
            result.state_field_offsets = storage.add<std::uint32_t>(node_plan.state.node.field_offsets.size());
            result.generic_state_storage_representations = storage.add<const ScalarStorageRepresentation *>(
                node_plan.state.node.generic_storage_representations.size());
        }
    }
    if (node_plan.invalidation.mode == InvalidationMode::invoke) {
        result.invalidation_function = storage.add<EvaluationFunction>();
    }
    if (node_plan.item_subscriptions.empty() && node_plan.output.storage_representation != nullptr &&
        node_plan.output.storage_representation->requires_modified_item_finalization()) {
        result.output_finalization_representation = storage.add<const TimeSeriesStorageRepresentation *>();
    }

    auto generic_time_series_input_count = std::size_t{};
    auto generic_scalar_input_count = std::size_t{};
    for (auto i = std::size_t{}; i < node.signature().parameters.size(); ++i) {
        generic_time_series_input_count +=
            static_cast<std::size_t>(node.node_template().uses_generic_time_series_input_view(i));
        generic_scalar_input_count += static_cast<std::size_t>(node.node_template().uses_generic_scalar_input_view(i));
    }
    if (generic_time_series_input_count != 0) {
        result.time_series_input_view_prototypes =
            storage.add<TimeSeriesInputViewPrototype>(generic_time_series_input_count);
    }
    if (generic_scalar_input_count != 0) {
        result.scalar_input_view_prototypes = storage.add<ScalarInputViewPrototype>(generic_scalar_input_count);
    }
    if (node.node_template().uses_generic_output_view()) {
        result.output_view_prototype = storage.add<OutputViewPrototype>();
    }

    result.scalar_offsets.reserve(node.scalar_values().size());
    for (const auto &scalar : node.scalar_values()) {
        if (!scalar.has_value()) {
            result.scalar_offsets.push_back(0);
            continue;
        }

        const auto &representation = scalar.storage_representation();
        const auto value = storage.add_bytes(representation.size(), representation.alignment());
        result.scalar_offsets.push_back(value.offset);
    }

    result.size = storage.size();
    result.alignment = storage.alignment();

    return result;
}

void plan_fixed_storage(CompilationPlan &plan) {
    auto storage = StorageLayout{};
    auto &layout = plan.fixed;
    layout.header = storage.add<details::CompiledStorageHeader>();
    layout.node_offsets = storage.add<std::uint32_t>(plan.nodes.size());
    layout.node_paths = storage.add<std::uint32_t>(plan.nodes.size());
    if (plan.has_lifecycle()) {
        layout.start_functions = storage.add<EvaluationFunction>(plan.starts.nodes.size());
        layout.start_nodes = storage.add<std::uint32_t>(plan.starts.nodes.size());
        layout.stop_functions = storage.add<EvaluationFunction>(plan.stops.nodes.size());
        layout.stop_nodes = storage.add<std::uint32_t>(plan.stops.nodes.size());
    }
    layout.wiring_paths = storage.add<WiringPath>(plan.wiring_paths.paths.size());
    layout.scalar_destruction_representations =
        storage.add<const ScalarStorageRepresentation *>(plan.scalar_destruction_count);
    layout.scalar_destruction_offsets = storage.add<std::uint32_t>(plan.scalar_destruction_count);
    layout.output_destruction_representations =
        storage.add<const TimeSeriesStorageRepresentation *>(plan.output_destruction_count);
    layout.output_destruction_offsets = storage.add<std::uint32_t>(plan.output_destruction_count);
    if (plan.uses_variable_memory) {
        layout.variable_memory = storage.add<std::pmr::unsynchronized_pool_resource>();
    }
    layout.initial_runtime_storage = storage.add_bytes(plan.runtime.size, plan.runtime.alignment, plan.runtime.size);

    for (auto &node : plan.node_plans) {
        const auto section = storage.add_bytes(node.fixed.size, node.fixed.alignment);
        node.fixed.offset = section.offset;
    }

    layout.size = storage.size();
    layout.alignment = storage.alignment();
}

[[nodiscard]] CompilationPlan make_compilation_plan(const GraphWiring &wiring, std::size_t schedule_word_count) {
    auto result = CompilationPlan{};
    result.nodes = wiring.nodes();
    result.wiring_points = wiring.wiring_points();
    result.node_plans.resize(result.nodes.size());
    plan_time_series_inputs(result);
    plan_subscribers(result.nodes, result.wiring_points, result.node_plans);
    plan_dynamic_subscribers(result);
    result.wiring_paths = make_wiring_path_layout(result.nodes);

    for (auto i = std::size_t{}; i < result.nodes.size(); ++i) {
        const auto &node = result.nodes[i];
        result.node_plans[i].invalidation = node.node_template().invalidation_behavior(node.signature());
        result.starts.add(node.node_template().start_function(node.signature()), static_cast<std::uint32_t>(i));
        result.stops.add(node.node_template().stop_function(node.signature()), static_cast<std::uint32_t>(i));
        result.uses_future_scheduling |= node.node_template().uses_future_scheduling();
    }

    plan_runtime_storage(result, schedule_word_count);
    for (auto i = std::size_t{}; i < result.nodes.size(); ++i) {
        auto &invalidation = result.node_plans[i].invalidation;
        const auto *output_type = result.nodes[i].signature().output;
        if (invalidation.mode == InvalidationMode::invalidate_output && output_type != nullptr &&
            !output_type->supports_invalidation()) {
            invalidation.mode = InvalidationMode::preserve_output;
        }
    }
    resolve_dynamic_subscribers(result);

    result.output_destruction_count =
        static_cast<std::size_t>(std::ranges::count_if(result.node_plans, [](const auto &node) {
            return node.output.storage_representation != nullptr &&
                   node.output.storage_representation->requires_destruction();
        }));

    for (auto i = std::size_t{}; i < result.nodes.size(); ++i) {
        const auto &node = result.nodes[i];
        auto &node_plan = result.node_plans[i];
        node_plan.fixed = make_node_layout(node, node_plan);

        for (const auto &scalar : node.scalar_values()) {
            if (!scalar.has_value()) {
                continue;
            }

            const auto &representation = scalar.storage_representation();
            result.scalar_destruction_count += static_cast<std::size_t>(representation.requires_destruction());
            result.uses_variable_memory |= representation.uses_variable_memory();
        }
    }

    plan_fixed_storage(result);

    return result;
}

} // namespace

class CompiledGraphBuilder {
  public:
    static void build(CompiledGraph &graph, const GraphWiring &wiring, std::pmr::memory_resource &memory) {
        const auto nodes = wiring.nodes();
        if (nodes.size() > CompiledGraph::count_mask) {
            throw std::length_error{"a compiled graph has too many nodes"};
        }

        auto plan = make_compilation_plan(wiring, CompiledGraph::schedule_word_count(nodes.size()));
        if (plan.scalar_destruction_count > CompiledGraph::count_mask) {
            throw std::length_error{"a compiled graph has too many scalars"};
        }

        set_graph_metadata(graph, plan);

        auto *storage = static_cast<std::byte *>(memory.allocate(plan.fixed.size, plan.fixed.alignment));
        graph.fixed_storage_ = storage;
        auto release = util::scope_exit{[&]() noexcept {
            memory.deallocate(storage, plan.fixed.size, plan.fixed.alignment);
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
            (plan.uses_future_scheduling ? CompiledGraph::future_scheduling_flag : std::uint32_t{});
        graph.runtime_storage_size_ = packed_offset(plan.runtime.size, "runtime storage reaches 1 GiB");
        graph.node_count_and_fixed_alignment_ =
            static_cast<std::uint32_t>(plan.nodes.size()) |
            (alignment_exponent(plan.fixed.alignment, "invalid compiled storage alignment")
             << CompiledGraph::alignment_shift);
        graph.scalar_count_and_runtime_alignment_ =
            static_cast<std::uint32_t>(plan.scalar_destruction_count) |
            (alignment_exponent(plan.runtime.alignment, "invalid runtime storage alignment")
             << CompiledGraph::alignment_shift);
    }

    static void construct_storage(const CompilationPlan &plan, std::byte *storage,
                                  std::pmr::memory_resource &upstream_memory) {
        construct_header(plan, storage);
        construct_wiring_paths(plan, storage);
        if (plan.has_lifecycle()) {
            construct_starts(plan, storage);
            construct_stops(plan, storage);
        }

        auto constructed_scalars = std::vector<ConstructedScalar>{};
        constructed_scalars.reserve(plan.scalar_destruction_count);
        auto *variable_memory = std::pmr::null_memory_resource();
        std::pmr::unsynchronized_pool_resource *variable_pool = nullptr;
        auto rollback = util::scope_exit{[&]() noexcept {
            for (auto scalar = constructed_scalars.rbegin(); scalar != constructed_scalars.rend(); ++scalar) {
                scalar->storage_representation->destroy(scalar->data, *variable_memory);
            }

            if (variable_pool != nullptr) {
                std::destroy_at(variable_pool);
            }
        }};

        if (plan.uses_variable_memory) {
            auto *pool = plan.fixed.variable_memory.objects<std::pmr::unsynchronized_pool_resource>(storage).data();
            std::construct_at(pool, &upstream_memory);
            variable_pool = pool;
            variable_memory = pool;
        }

        construct_initial_runtime_storage(plan, storage);
        construct_output_destruction(plan, storage);
        construct_nodes(plan, storage, *variable_memory, constructed_scalars);
        rollback.release();
    }

    static void construct_header(const CompilationPlan &plan, std::byte *storage) {
        const auto offset = [](const StorageSection &section) {
            return packed_offset(section.offset, "compiled fixed storage reaches 1 GiB");
        };

        std::construct_at(
            plan.fixed.header.objects<details::CompiledStorageHeader>(storage).data(),
            details::CompiledStorageHeader{
                .wiring_paths_offset = offset(plan.fixed.wiring_paths),
                .wiring_path_count = static_cast<std::uint32_t>(plan.wiring_paths.paths.size()),
                .start_functions_offset = offset(plan.fixed.start_functions),
                .start_count = static_cast<std::uint32_t>(plan.starts.nodes.size()),
                .stop_functions_offset = offset(plan.fixed.stop_functions),
                .stop_count = static_cast<std::uint32_t>(plan.stops.nodes.size()),
                .scalar_destruction_representations_offset = offset(plan.fixed.scalar_destruction_representations),
                .output_destruction_count = static_cast<std::uint32_t>(plan.output_destruction_count),
                .variable_memory_offset = offset(plan.fixed.variable_memory),
                .initial_runtime_storage_offset = offset(plan.fixed.initial_runtime_storage),
            });
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
                                plan.fixed.start_functions.objects<EvaluationFunction>(storage).data());
        std::uninitialized_copy(plan.starts.nodes.begin(), plan.starts.nodes.end(),
                                plan.fixed.start_nodes.objects<std::uint32_t>(storage).data());
    }

    static void construct_stops(const CompilationPlan &plan, std::byte *storage) {
        std::uninitialized_copy(plan.stops.functions.begin(), plan.stops.functions.end(),
                                plan.fixed.stop_functions.objects<EvaluationFunction>(storage).data());
        std::uninitialized_copy(plan.stops.nodes.begin(), plan.stops.nodes.end(),
                                plan.fixed.stop_nodes.objects<std::uint32_t>(storage).data());
    }

    static void construct_initial_runtime_storage(const CompilationPlan &plan, std::byte *fixed_storage) {
        auto runtime_storage = plan.fixed.initial_runtime_storage.objects<std::byte>(fixed_storage);
        std::fill(runtime_storage.begin(), runtime_storage.end(), std::byte{});
        auto scheduling =
            std::span{reinterpret_cast<SchedulingInfo *>(runtime_storage.data()), plan.runtime.schedule_word_count};
        std::uninitialized_value_construct(scheduling.begin(), scheduling.end());

        for (auto node_index = std::size_t{}; node_index < plan.nodes.size(); ++node_index) {
            const auto &parameters = plan.nodes[node_index].signature().parameters;
            const auto has_required_input = std::ranges::any_of(parameters, [](const auto &parameter) {
                return parameter.type->is_time_series() && parameter.validity == required;
            });
            if (!has_required_input) {
                auto &info = scheduling[node_index / CompiledGraph::schedule_word_bits];
                info.inputs_valid(ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits), true);
            }
        }

        for (const auto &node : plan.node_plans) {
            const auto &output = node.output;
            if (output.storage_representation == nullptr) {
                continue;
            }

            std::construct_at(reinterpret_cast<EvaluationTime *>(runtime_storage.data() + output.last_updated_offset),
                              MIN_DT);
            output.storage_representation->construct_initial_value(runtime_storage.data() + output.value_offset);
        }

        const auto initialize_item_samples = [&](this const auto &self,
                                                 std::span<const ItemSubscriptionPlan> items) -> void {
            for (const auto &item : items) {
                std::construct_at(
                    reinterpret_cast<EvaluationTime *>(runtime_storage.data() + item.sampled_last_updated_offset),
                    MIN_DT);
                self(item.items);
            }
        };
        for (const auto &node : plan.node_plans) {
            initialize_item_samples(node.item_subscriptions);
            for (const auto &reference : node.time_series_inputs) {
                if (!reference.dereferences_reference) {
                    continue;
                }
                std::construct_at(
                    reinterpret_cast<details::ReferenceInputStorage *>(runtime_storage.data() + reference.offset));
            }
        }

        for (auto i = std::size_t{}; i < plan.nodes.size(); ++i) {
            const auto &state = plan.node_plans[i].state;
            if (state.node.size == 0) {
                continue;
            }

            const auto &node = plan.nodes[i];
            node.node_template().construct_initial_state(state.node, runtime_storage.data() + state.offset);
        }
    }

    static void construct_output_destruction(const CompilationPlan &plan, std::byte *fixed_storage) {
        auto representations = plan.fixed.output_destruction_representations
                                   .objects<const TimeSeriesStorageRepresentation *>(fixed_storage)
                                   .begin();
        auto offsets = plan.fixed.output_destruction_offsets.objects<std::uint32_t>(fixed_storage).begin();

        for (const auto &node : plan.node_plans) {
            const auto &output = node.output;
            if (output.storage_representation == nullptr || !output.storage_representation->requires_destruction()) {
                continue;
            }

            std::construct_at(std::to_address(representations++), output.storage_representation);
            std::construct_at(std::to_address(offsets++), output.value_offset);
        }
    }

    static void construct_parameters(const CompilationPlan &plan, std::size_t node_index, std::byte *storage,
                                     std::pmr::memory_resource &variable_memory,
                                     std::vector<ConstructedScalar> &constructed_scalars,
                                     std::size_t &scalar_destruction_index) {
        const auto &node = plan.nodes[node_index];
        const auto &layout = plan.node_plans[node_index].fixed;
        auto *node_storage = storage + layout.offset;
        auto parameters = layout.parameters.objects<ParameterBinding>(node_storage);
        auto destruction_representations =
            plan.fixed.scalar_destruction_representations.objects<const ScalarStorageRepresentation *>(storage);
        auto destruction_offsets = plan.fixed.scalar_destruction_offsets.objects<std::uint32_t>(storage);

        auto input_index = std::size_t{};
        auto scalar_index = std::size_t{};
        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            const auto &parameter = node.signature().parameters[parameter_index];

            if (parameter.type->is_time_series()) {
                const auto &input = plan.node_plans[node_index].time_series_inputs[input_index++];
                const auto resolved_output = source_output(input.source, plan.wiring_points, plan.node_plans);
                if (input.dereferences_reference) {
                    std::construct_at(parameters.data() + parameter_index,
                                      ParameterBinding{input.source_last_updated_offset, input.offset,
                                                       parameter.activity, parameter.validity});
                } else {
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

            const auto &representation = scalar.storage_representation();
            const auto scalar_offset = layout.scalar_offsets[current_scalar_index];
            auto *scalar_data = node_storage + scalar_offset;
            representation.copy_construct(scalar_data, scalar.data(), variable_memory);

            const auto parameter_offset = layout.parameters.offset + parameter_index * sizeof(ParameterBinding);
            std::construct_at(parameters.data() + parameter_index,
                              ParameterBinding{packed_offset(scalar_offset - parameter_offset,
                                                             "a scalar is more than 1 GiB from its parameter")});

            if (!representation.requires_destruction()) {
                continue;
            }

            constructed_scalars.push_back({&representation, scalar_data});
            std::construct_at(destruction_representations.data() + scalar_destruction_index, &representation);
            std::construct_at(destruction_offsets.data() + scalar_destruction_index,
                              packed_offset(static_cast<std::size_t>(static_cast<std::byte *>(scalar_data) - storage),
                                            "compiled fixed storage reaches 1 GiB"));
            ++scalar_destruction_index;
        }
    }

    static void construct_view_prototypes(const CompilationPlan &plan, std::size_t node_index, std::byte *storage) {
        const auto &node = plan.nodes[node_index];
        const auto &layout = plan.node_plans[node_index].fixed;
        auto *node_storage = storage + layout.offset;
        auto time_series_input_views =
            layout.time_series_input_view_prototypes.objects<TimeSeriesInputViewPrototype>(node_storage);
        auto scalar_input_views = layout.scalar_input_view_prototypes.objects<ScalarInputViewPrototype>(node_storage);
        auto generic_time_series_input_index = std::size_t{};
        auto generic_scalar_input_index = std::size_t{};
        auto input_index = std::size_t{};
        auto scalar_index = std::size_t{};

        for (auto parameter_index = std::size_t{}; parameter_index < node.signature().parameters.size();
             ++parameter_index) {
            if (!node.signature().parameters[parameter_index].type->is_time_series()) {
                const auto &scalar = node.scalar_values()[scalar_index++];
                if (node.node_template().uses_generic_scalar_input_view(parameter_index)) {
                    const auto *prototype = scalar.storage_representation().scalar_input_view_prototype();
                    std::construct_at(scalar_input_views.data() + generic_scalar_input_index,
                                      ScalarInputViewPrototype{prototype});
                    ++generic_scalar_input_index;
                }
                continue;
            }

            const auto &input = plan.node_plans[node_index].time_series_inputs[input_index++];
            if (!node.node_template().uses_generic_time_series_input_view(parameter_index)) {
                continue;
            }

            const auto source = source_output(input.source, plan.wiring_points, plan.node_plans);
            const auto *representation = input_storage_representation(input, source.storage_representation);
            std::construct_at(time_series_input_views.data() + generic_time_series_input_index,
                              TimeSeriesInputViewPrototype{&representation->input_view_prototype()});
            ++generic_time_series_input_index;
        }

        if (node.node_template().uses_generic_output_view()) {
            const auto *representation = plan.node_plans[node_index].output.storage_representation;
            std::construct_at(layout.output_view_prototype.objects<OutputViewPrototype>(node_storage).data(),
                              OutputViewPrototype{&representation->output_view_prototype()});
        }
    }

    static std::byte *construct_output_notification(const CompilationPlan &plan,
                                                    std::span<const ReferenceInputSubscriberPlan> subscribers,
                                                    std::byte *destination) {
        auto address = align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(OutputNotification));
        destination = reinterpret_cast<std::byte *>(address);
        const auto record_bytes = subscribers.size() * sizeof(details::ReferenceInputSubscriber);
        auto *notification =
            std::construct_at(reinterpret_cast<OutputNotification *>(destination), details::notify_reference_inputs,
                              static_cast<std::uint32_t>(record_bytes));
        auto *records = reinterpret_cast<details::ReferenceInputSubscriber *>(notification + 1);

        for (const auto &subscriber : subscribers) {
            const auto &input = plan.node_plans[subscriber.node_index].time_series_inputs[subscriber.input_index];
            std::construct_at(records++, input.offset, subscriber.node_index, subscriber.active, subscriber.required);
        }

        return reinterpret_cast<std::byte *>(records);
    }

    static std::byte *construct_item_subscribers(const CompilationPlan &plan,
                                                 std::span<const ItemSubscriptionPlan> items, std::byte *destination) {
        for (const auto &item : items) {
            destination = reinterpret_cast<std::byte *>(
                align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(ItemSubscribers)));
            auto *compiled =
                std::construct_at(reinterpret_cast<ItemSubscribers *>(destination), item.item,
                                  static_cast<std::uint32_t>(item.subscriptions.nodes.size()),
                                  static_cast<std::uint32_t>(item_subscriber_size(item)),
                                  item.sampled_last_updated_offset, !item.subscriptions.reference_inputs.empty());
            auto *subscribers = reinterpret_cast<Subscriber *>(compiled + 1);
            std::uninitialized_copy(item.subscriptions.nodes.begin(), item.subscriptions.nodes.end(), subscribers);
            destination = reinterpret_cast<std::byte *>(subscribers + item.subscriptions.nodes.size());
            if (!item.subscriptions.reference_inputs.empty()) {
                destination = construct_output_notification(plan, item.subscriptions.reference_inputs, destination);
            }
            destination = reinterpret_cast<std::byte *>(
                align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(ItemSubscribers)));
            destination = construct_item_subscribers(plan, item.items, destination);
            destination = reinterpret_cast<std::byte *>(
                align_up(reinterpret_cast<std::uintptr_t>(destination), alignof(ItemSubscribers)));
        }

        return destination;
    }

    static void construct_nodes(const CompilationPlan &plan, std::byte *storage,
                                std::pmr::memory_resource &variable_memory,
                                std::vector<ConstructedScalar> &constructed_scalars) {
        auto node_offsets = plan.fixed.node_offsets.objects<std::uint32_t>(storage);
        auto scalar_destruction_index = std::size_t{};

        for (auto i = std::size_t{}; i < plan.nodes.size(); ++i) {
            const auto &node = plan.nodes[i];
            const auto &node_plan = plan.node_plans[i];
            const auto &layout = node_plan.fixed;
            auto *node_storage = storage + layout.offset;

            construct_parameters(plan, i, storage, variable_memory, constructed_scalars, scalar_destruction_index);
            std::uninitialized_copy(node_plan.subscriptions.nodes.begin(), node_plan.subscriptions.nodes.end(),
                                    layout.subscribers.objects<Subscriber>(node_storage).data());
            if (!node_plan.subscriptions.reference_inputs.empty()) {
                construct_output_notification(plan, node_plan.subscriptions.reference_inputs,
                                              node_storage + layout.output_notification.offset);
            }
            if (!node_plan.item_subscriptions.empty()) {
                std::construct_at(layout.item_subscription_root.objects<ItemSubscriptionRoot>(node_storage).data(),
                                  *node_plan.output.storage_representation,
                                  static_cast<std::uint32_t>(item_subscriber_size(node_plan.item_subscriptions)));
                construct_item_subscribers(plan, node_plan.item_subscriptions,
                                           node_storage + layout.item_subscribers.offset);
            }
            if (node_plan.state.node.size != 0) {
                const auto has_state_metadata = !node_plan.state.node.generic_storage_representations.empty();
                std::construct_at(layout.state_offset.objects<std::uint32_t>(node_storage).data(),
                                  node_plan.state.offset |
                                      (node_plan.state.requires_destruction ? CompiledNode::state_destruction_flag
                                                                            : std::uint32_t{}) |
                                      (has_state_metadata ? CompiledNode::state_metadata_flag : std::uint32_t{}));
                if (node_plan.state.requires_destruction) {
                    std::construct_at(
                        layout.state_destruction_function.objects<EvaluationFunction>(node_storage).data(),
                        node.node_template().state_destruction_function(node.signature()));
                }
                if (has_state_metadata) {
                    std::construct_at(
                        layout.state_metadata_counts.objects<StateMetadataCounts>(node_storage).data(),
                        StateMetadataCounts{
                            static_cast<std::uint16_t>(node_plan.state.node.field_offsets.size()),
                            static_cast<std::uint16_t>(node_plan.state.node.generic_storage_representations.size())});
                    std::ranges::transform(
                        node_plan.state.node.field_offsets,
                        layout.state_field_offsets.objects<std::uint32_t>(node_storage).begin(),
                        [](auto offset) { return packed_offset(offset, "node state reaches 1 GiB"); });
                    std::uninitialized_copy(node_plan.state.node.generic_storage_representations.begin(),
                                            node_plan.state.node.generic_storage_representations.end(),
                                            layout.generic_state_storage_representations
                                                .objects<const ScalarStorageRepresentation *>(node_storage)
                                                .data());
                }
            }
            if (node_plan.invalidation.mode == InvalidationMode::invoke) {
                std::construct_at(layout.invalidation_function.objects<EvaluationFunction>(node_storage).data(),
                                  node_plan.invalidation.function);
            }
            const auto stores_output_finalization_representation =
                node_plan.item_subscriptions.empty() && node_plan.output.storage_representation != nullptr &&
                node_plan.output.storage_representation->requires_modified_item_finalization();
            if (stores_output_finalization_representation) {
                std::construct_at(layout.output_finalization_representation
                                      .objects<const TimeSeriesStorageRepresentation *>(node_storage)
                                      .data(),
                                  node_plan.output.storage_representation);
            }
            construct_view_prototypes(plan, i, storage);

            std::construct_at(reinterpret_cast<CompiledNode *>(node_storage),
                              CompiledNode{node.node_template().evaluation_function(node.signature()),
                                           node_plan.output.last_updated_offset, node_plan.output.value_offset,
                                           static_cast<std::uint32_t>(node.signature().parameters.size()),
                                           static_cast<std::uint32_t>(node_plan.subscriptions.nodes.size()),
                                           node.signature().output != nullptr, node_plan.state.node.size != 0,
                                           !node_plan.subscriptions.reference_inputs.empty(),
                                           !node_plan.item_subscriptions.empty(),
                                           stores_output_finalization_representation, node_plan.invalidation.mode});
            std::construct_at(node_offsets.data() + i,
                              packed_offset(layout.offset, "compiled fixed storage reaches 1 GiB"));
        }
    }
};

CompiledGraph::CompiledGraph(const GraphWiring &wiring) : CompiledGraph(wiring, *std::pmr::get_default_resource()) {}

CompiledGraph::CompiledGraph(const GraphWiring &wiring, std::pmr::memory_resource &memory) : fixed_memory_(&memory) {
    CompiledGraphBuilder::build(*this, wiring, memory);
}

const WiringPath &CompiledGraph::wiring_path(std::size_t node_index) const noexcept {
    const auto &header = storage_header();
    const auto path_index = header.node_paths(node_count())[node_index];

    return stored_objects<WiringPath>(fixed_storage_, header.wiring_paths_offset, header.wiring_path_count)[path_index];
}

const std::byte *CompiledGraph::initial_runtime_storage() const noexcept {
    return fixed_storage_ + storage_header().initial_runtime_storage_offset;
}

CompiledGraph::NodeFunctions CompiledGraph::node_functions(std::uint32_t functions_offset,
                                                           std::uint32_t count) const noexcept {
    const auto functions = stored_objects<EvaluationFunction>(fixed_storage_, functions_offset, count);
    const auto nodes_offset = static_cast<std::uint32_t>(functions_offset + count * sizeof(EvaluationFunction));

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
    const auto representations_offset = static_cast<std::uint32_t>(
        align_up(header.scalar_destruction_representations_offset +
                     scalar_destruction_count() * (sizeof(const ScalarStorageRepresentation *) + sizeof(std::uint32_t)),
                 alignof(const TimeSeriesStorageRepresentation *)));
    const auto representations = stored_objects<const TimeSeriesStorageRepresentation *>(
        fixed_storage_, representations_offset, header.output_destruction_count);
    const auto offsets_offset = static_cast<std::uint32_t>(
        representations_offset + header.output_destruction_count * sizeof(const TimeSeriesStorageRepresentation *));

    return {representations,
            stored_objects<std::uint32_t>(fixed_storage_, offsets_offset, header.output_destruction_count)};
}

CompiledGraph::~CompiledGraph() {
    if (fixed_storage_ == nullptr) {
        return;
    }

    const auto &header = storage_header();

    std::pmr::unsynchronized_pool_resource *variable_pool = nullptr;
    auto *variable_memory = std::pmr::null_memory_resource();
    if (has_variable_memory()) {
        variable_pool =
            reinterpret_cast<std::pmr::unsynchronized_pool_resource *>(fixed_storage_ + header.variable_memory_offset);
        variable_memory = variable_pool;
    }

    const auto count = scalar_destruction_count();
    const auto representations = stored_objects<const ScalarStorageRepresentation *>(
        fixed_storage_, header.scalar_destruction_representations_offset, count);
    const auto offsets_offset = static_cast<std::uint32_t>(header.scalar_destruction_representations_offset +
                                                           count * sizeof(const ScalarStorageRepresentation *));
    const auto offsets = stored_objects<std::uint32_t>(fixed_storage_, offsets_offset, count);
    for (auto i = count; i > 0; --i) {
        representations[i - 1]->destroy(fixed_storage_ + offsets[i - 1], *variable_memory);
    }

    if (variable_pool != nullptr) {
        std::destroy_at(variable_pool);
    }

    fixed_memory_->deallocate(fixed_storage_, fixed_storage_size(), fixed_storage_alignment());
}

} // namespace bg
