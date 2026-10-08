#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/evaluation_function.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>

namespace bg {

class CompiledGraphBuilder;
class CompiledNode;
class RuntimeType;
class ScalarRuntimeType;
class TimeSeriesRuntimeType;
class TimeSeriesType;
class ItemSubscriptionRange;
class Type;

struct NodeInvocationParameterMetadata {
    util::symbol name;
    const Type *type;
};

struct NodeInvocationMetadata {
    std::uint32_t parameters_offset;
    std::uint32_t compiled_data_offset;
    CompiledNodeDataDestructionFunction destruction_function;

    std::span<const NodeInvocationParameterMetadata> parameters(const CompiledNode &node) const noexcept;
    std::span<const std::uint16_t> controlled_input_indices(const CompiledNode &node) const noexcept;
    const void *compiled_data(const CompiledNode &node) const noexcept;
};

struct StateMetadataCounts {
    std::uint16_t fields;
    std::uint16_t generic_fields;
};

struct AllValidInputMetadata {
    std::uint32_t input_count;
};

struct AllValidInputDescriptor {
    std::uint32_t parameter_index;
    const TimeSeriesRuntimeType *runtime_type;
    const TimeSeriesType *declared_type;
};

class ControlledInput {
  public:
    explicit ControlledInput(std::uint32_t activity_offset) noexcept : activity_offset_{activity_offset} {}

    std::size_t activity_offset() const noexcept { return activity_offset_; }

    static constexpr std::uint32_t no_control = std::numeric_limits<std::uint32_t>::max();

  private:
    std::uint32_t activity_offset_;
};

enum class ParameterBindingMode : std::uint8_t {
    time_series,
    reference,
    scalar,
    constant,
    default_value,
};

class Subscriber {
  public:
    Subscriber(std::uint32_t node_index, bool active, bool required, bool active_optional) noexcept
        : node_index_and_flags_{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                                (active_optional ? active_optional_flag : 0)} {}

    std::size_t node_index() const noexcept { return node_index_and_flags_ & node_index_mask; }

    bool active() const noexcept { return (node_index_and_flags_ & active_flag) != 0; }

    bool required() const noexcept { return (node_index_and_flags_ & required_flag) != 0; }

    bool active_optional() const noexcept { return (node_index_and_flags_ & active_optional_flag) != 0; }

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t active_optional_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t node_index_mask = active_optional_flag - 1;

    std::uint32_t node_index_and_flags_;
};

class alignas(void *) ItemSubscription {
  public:
    ItemSubscription(std::uint32_t item, std::uint32_t subscriber_count, std::uint32_t subtree_size,
                     std::uint32_t sampled_last_updated_offset, bool has_output_notification) noexcept
        : item_{item},
          subscriber_count_{subscriber_count | (has_output_notification ? output_notification_flag : std::uint32_t{})},
          subtree_size_{subtree_size}, sampled_last_updated_offset_{sampled_last_updated_offset} {}

    std::size_t item() const noexcept { return item_; }

    std::span<const Subscriber> subscribers() const noexcept {
        return {reinterpret_cast<const Subscriber *>(this + 1), subscriber_count()};
    }

    const OutputNotification *output_notification() const noexcept {
        if ((subscriber_count_ & output_notification_flag) == 0) {
            return nullptr;
        }

        const auto subscribers = this->subscribers();
        auto address = reinterpret_cast<std::uintptr_t>(subscribers.data() + subscribers.size());
        address = align(address, alignof(OutputNotification));
        return reinterpret_cast<const OutputNotification *>(address);
    }

    std::size_t sampled_last_updated_offset() const noexcept { return sampled_last_updated_offset_; }

    ItemSubscriptionRange children() const noexcept;

  private:
    friend ItemSubscriptionRange;

    const ItemSubscription *first_child() const noexcept {
        auto address = output_notification() != nullptr
                           ? reinterpret_cast<std::uintptr_t>(output_notification()->end())
                           : reinterpret_cast<std::uintptr_t>(subscribers().data() + subscribers().size());
        address = align(address, alignof(ItemSubscription));
        return reinterpret_cast<const ItemSubscription *>(address);
    }

    const ItemSubscription *next() const noexcept {
        const auto *address = reinterpret_cast<const std::byte *>(this);
        return reinterpret_cast<const ItemSubscription *>(address + subtree_size_);
    }

    static std::uintptr_t align(std::uintptr_t address, std::size_t alignment) noexcept {
        return (address + alignment - 1) & ~(alignment - 1);
    }

    static constexpr std::uint32_t output_notification_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t subscriber_count_mask = output_notification_flag - 1;

    std::size_t subscriber_count() const noexcept { return subscriber_count_ & subscriber_count_mask; }

    std::uint32_t item_;
    std::uint32_t subscriber_count_;
    std::uint32_t subtree_size_;
    std::uint32_t sampled_last_updated_offset_;
};

class ItemSubscriptionRange {
  public:
    const ItemSubscription *find(std::size_t item) const noexcept {
        auto *candidate = first_;
        while (candidate != last_) {
            if (candidate->item() == item) {
                return candidate;
            }

            candidate = candidate->next();
        }

        return nullptr;
    }

  private:
    friend ItemSubscription;
    friend struct ItemSubscriptionRoot;

    ItemSubscriptionRange(const ItemSubscription *first, const ItemSubscription *last) noexcept
        : first_{first}, last_{last} {}

    const ItemSubscription *first_;
    const ItemSubscription *last_;
};

inline ItemSubscriptionRange ItemSubscription::children() const noexcept {
    return {first_child(), next()};
}

struct ItemSubscriptionRoot {
    const TimeSeriesRuntimeType *runtime_type;
    std::uint32_t byte_size;

    ItemSubscriptionRoot(const TimeSeriesRuntimeType &runtime_type, std::uint32_t byte_size) noexcept
        : runtime_type{&runtime_type}, byte_size{byte_size} {}

    const ItemSubscription *first_item() const noexcept { return reinterpret_cast<const ItemSubscription *>(this + 1); }

    const std::byte *end() const noexcept { return reinterpret_cast<const std::byte *>(first_item()) + byte_size; }

    ItemSubscriptionRange children() const noexcept {
        return {first_item(), reinterpret_cast<const ItemSubscription *>(end())};
    }
};

// Locates a time series in runtime storage or a scalar in the packed node record.
class ParameterBinding {
  public:
    ParameterBindingMode mode() const noexcept {
        if ((location_and_flags_ & scalar_flag) != 0) {
            if ((value_offset_ & default_flag) != 0) {
                return ParameterBindingMode::default_value;
            }
            return (value_offset_ & constant_flag) == 0 ? ParameterBindingMode::scalar : ParameterBindingMode::constant;
        }
        return (value_offset_ & reference_flag) == 0 ? ParameterBindingMode::time_series
                                                     : ParameterBindingMode::reference;
    }

    std::size_t last_updated_offset() const noexcept {
        return location_and_flags_ & location_mask;
    }

    std::size_t value_offset() const noexcept {
        const auto offset = value_offset_ & value_location_mask;
        return mode() == ParameterBindingMode::reference ? offset & ~reference_dynamic_activity_flag : offset;
    }

    // Packed-record offsets are relative to this binding; call through the stored record entry.
    const void *scalar_data() const noexcept {
        if ((location_and_flags_ & optional_flag) != 0) {
            return nullptr;
        }

        const auto *address = reinterpret_cast<const std::byte *>(this);

        return address + (location_and_flags_ & location_mask);
    }

    const void *constant_data() const noexcept {
        const auto *address = reinterpret_cast<const std::byte *>(this);
        return address + (value_offset_ & value_location_mask);
    }

    InputValidity validity() const noexcept { return (location_and_flags_ & optional_flag) == 0 ? required : optional; }
    bool has_reference_sample() const noexcept {
        return mode() == ParameterBindingMode::reference && (value_offset_ & reference_sample_flag) != 0;
    }
    bool reference_dynamic_activity() const noexcept {
        return mode() == ParameterBindingMode::reference &&
               (value_offset_ & reference_dynamic_activity_flag) != 0;
    }

  private:
    friend class CompiledGraphBuilder;

    static constexpr std::uint32_t scalar_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t optional_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t location_mask = optional_flag - 1;
    static constexpr std::uint32_t reference_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t constant_flag = reference_flag;
    static constexpr std::uint32_t default_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t reference_sample_flag = default_flag;
    // Reference input slots are aligned, so their low bit records whether the activity byte follows the slot.
    static constexpr std::uint32_t reference_dynamic_activity_flag = 1;
    static constexpr std::uint32_t value_location_mask = default_flag - 1;

    ParameterBinding(std::uint32_t last_updated_offset, std::uint32_t value_offset, InputValidity validity) noexcept
        : location_and_flags_{last_updated_offset | (validity == InputValidity::optional ? optional_flag : 0)},
          value_offset_{value_offset} {}

    explicit ParameterBinding(std::uint32_t scalar_offset) noexcept
        : location_and_flags_{scalar_offset | scalar_flag}, value_offset_{} {}

    static ParameterBinding constant(std::uint32_t start_time_offset, std::uint32_t scalar_offset,
                                     InputValidity validity) noexcept {
        return ParameterBinding{constant_binding, start_time_offset, scalar_offset, validity};
    }

    static ParameterBinding default_value(std::uint32_t last_updated_offset, std::uint32_t input_offset,
                                          InputValidity validity) noexcept {
        return ParameterBinding{default_binding, last_updated_offset, input_offset, validity};
    }

    static ParameterBinding reference(std::uint32_t last_updated_offset, std::uint32_t input_offset,
                                     InputValidity validity, bool sampled = false,
                                     bool dynamic_activity = false) noexcept {
        return ParameterBinding{last_updated_offset,
                                input_offset | reference_flag | (sampled ? reference_sample_flag : std::uint32_t{}) |
                                    (dynamic_activity ? reference_dynamic_activity_flag : std::uint32_t{}),
                                validity};
    }

    struct constant_binding_t {};
    static constexpr constant_binding_t constant_binding{};

    struct default_binding_t {};
    static constexpr default_binding_t default_binding{};

    ParameterBinding(constant_binding_t, std::uint32_t start_time_offset, std::uint32_t scalar_offset,
                     InputValidity validity) noexcept
        : location_and_flags_{start_time_offset | scalar_flag |
                              (validity == InputValidity::optional ? optional_flag : std::uint32_t{})},
          value_offset_{scalar_offset | constant_flag} {}

    ParameterBinding(default_binding_t, std::uint32_t last_updated_offset, std::uint32_t input_offset,
                     InputValidity validity) noexcept
        : location_and_flags_{last_updated_offset | scalar_flag |
                              (validity == InputValidity::optional ? optional_flag : std::uint32_t{})},
          value_offset_{input_offset | default_flag} {}

    ParameterBinding() noexcept : location_and_flags_{scalar_flag | optional_flag}, value_offset_{} {}

    std::uint32_t location_and_flags_;
    std::uint32_t value_offset_;
};

static_assert(sizeof(ParameterBinding) == 8);
static_assert(sizeof(ControlledInput) == 4);

struct DefaultInputState {
    TimeSeriesState last_updated;
};

struct DefaultInputDescriptor {
    ParameterBinding source;
    const void *fallback;
    InputNotification notification;
    std::uint32_t start_time_offset;

    static void notify_value(OutputChangeContext &context, std::size_t node_index, std::size_t parameter_index,
                             OutputChange change);
    static void notify_startup(OutputChangeContext &context, std::size_t node_index, std::size_t parameter_index,
                               OutputChange change);
};

// Begins a packed node record. Parameters, optional input node indices, subscribers, and other metadata follow.
class CompiledNode {
  public:
    static constexpr std::uint32_t max_all_valid_runtime_type_count = (std::uint32_t{1} << 10) - 2;
    static constexpr std::uint32_t no_input_node_index = std::numeric_limits<std::uint32_t>::max();

    bool has_output() const noexcept { return (output_last_updated_offset_ & no_output_flag) == 0; }

    EvaluationFunction evaluation_function() const noexcept { return evaluation_function_; }

    bool has_state() const noexcept { return (output_last_updated_offset_ & state_flag) != 0; }

    std::size_t output_last_updated_offset() const noexcept { return output_last_updated_offset_ & output_offset_mask; }

    std::size_t output_value_offset() const noexcept { return output_value_offset_ & output_offset_mask; }

    InvalidationMode invalidation_mode() const noexcept {
        return static_cast<InvalidationMode>(output_value_offset_ >> invalidation_mode_shift);
    }

    std::span<const ParameterBinding> parameters() const noexcept {
        const auto *first = std::launder(reinterpret_cast<const ParameterBinding *>(this + 1));

        return {first, parameter_count()};
    }

    // Returns the compiled producer for a direct local input, when the node template requested producer indices.
    std::optional<std::size_t> input_node_index(std::size_t parameter_index) const noexcept {
        if (!has_input_node_indices() || parameter_index >= parameter_count()) {
            return std::nullopt;
        }

        const auto *indices = reinterpret_cast<const std::uint32_t *>(parameters().data() + parameter_count());
        const auto index = indices[parameter_index];
        if (index == no_input_node_index) {
            return std::nullopt;
        }

        return index;
    }

    std::span<const ControlledInput> controlled_inputs() const noexcept {
        const auto parameters = this->parameters();
        auto *first = reinterpret_cast<const std::byte *>(parameters.data() + parameters.size());
        if (has_input_node_indices()) {
            first += parameters.size() * sizeof(std::uint32_t);
        }
        return {reinterpret_cast<const ControlledInput *>(first), controlled_input_count()};
    }

    std::span<const Subscriber> subscribers() const noexcept {
        const auto inputs = controlled_inputs();
        const auto *first = reinterpret_cast<const Subscriber *>(inputs.data() + inputs.size());

        return {first, subscriber_count()};
    }

    const OutputNotification *output_notification() const noexcept {
        if ((counts_ & output_notification_flag) == 0) {
            return nullptr;
        }

        const auto subscribers = this->subscribers();
        auto address = reinterpret_cast<std::uintptr_t>(subscribers.data() + subscribers.size());
        address = align(address, alignof(OutputNotification));
        return reinterpret_cast<const OutputNotification *>(address);
    }

    const ItemSubscriptionRoot *item_subscriptions() const noexcept {
        if ((counts_ & item_subscriptions_flag) == 0) {
            return nullptr;
        }

        auto address = output_notification_end();
        address = align(address, alignof(ItemSubscriptionRoot));
        return reinterpret_cast<const ItemSubscriptionRoot *>(address);
    }

    std::size_t state_offset() const noexcept {
        const auto *offset = reinterpret_cast<const std::uint32_t *>(state_address());

        return *offset & state_offset_mask;
    }

    NodeFunction state_destruction_function() const noexcept {
        if (!state_requires_destruction()) {
            return nullptr;
        }

        auto address = state_address();
        address = align(address + sizeof(std::uint32_t), alignof(NodeFunction));

        return *reinterpret_cast<const NodeFunction *>(address);
    }

    std::size_t state_field_offset(std::size_t index) const noexcept { return state_field_offsets_address()[index]; }

    const ScalarRuntimeType &generic_state_runtime_type(std::size_t index) const noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(state_field_offsets_address() + state_metadata_counts().fields);
        address = align(address, alignof(const ScalarRuntimeType *));

        return **(reinterpret_cast<const ScalarRuntimeType *const *>(address) + index);
    }

    NodeFunction invalidation_function() const noexcept {
        const auto address = align(optional_metadata_address(), alignof(NodeFunction));

        return *reinterpret_cast<const NodeFunction *>(address);
    }

    const TimeSeriesRuntimeType &time_series_input_runtime_type(std::size_t index) const noexcept {
        return static_cast<const TimeSeriesRuntimeType &>(runtime_type(index));
    }

    std::span<const AllValidInputDescriptor> all_valid_input_descriptors() const noexcept {
        if (!has_all_valid_input_metadata()) {
            return {};
        }

        const auto &metadata = all_valid_input_metadata();
        auto address = align(reinterpret_cast<std::uintptr_t>(&metadata + 1), alignof(AllValidInputDescriptor));
        return {reinterpret_cast<const AllValidInputDescriptor *>(address),
                metadata.input_count};
    }

    bool has_all_valid_input_metadata() const noexcept {
        return (flags_ & all_valid_runtime_type_count_mask) != 0;
    }

    const ScalarRuntimeType &scalar_input_runtime_type(std::size_t time_series_input_count,
                                                       std::size_t index) const noexcept {
        return static_cast<const ScalarRuntimeType &>(runtime_type(time_series_input_count + index));
    }

    const TimeSeriesRuntimeType &output_runtime_type(std::size_t time_series_input_count,
                                                     std::size_t scalar_input_count) const noexcept {
        return static_cast<const TimeSeriesRuntimeType &>(runtime_type(time_series_input_count + scalar_input_count));
    }

    const TimeSeriesRuntimeType &dynamic_reference_input_runtime_type(std::size_t time_series_input_count,
                                                                      std::size_t scalar_input_count,
                                                                      bool uses_output_runtime_type,
                                                                      std::size_t index) const noexcept {
        const auto output_count = static_cast<std::size_t>(uses_output_runtime_type);
        return static_cast<const TimeSeriesRuntimeType &>(
            runtime_type(time_series_input_count + scalar_input_count + output_count + index));
    }

    const TimeSeriesRuntimeType &node_invocation_output_runtime_type() const noexcept {
        const auto time_series_inputs = node_invocation_time_series_input_count();
        return output_runtime_type(time_series_inputs, parameters().size() - time_series_inputs);
    }

    std::size_t node_invocation_time_series_input_count() const noexcept {
        return (flags_ & node_invocation_time_series_input_count_mask) >> node_invocation_time_series_input_count_shift;
    }

    const NodeInvocationMetadata *node_invocation_metadata() const noexcept {
        if ((flags_ & node_invocation_flag) == 0) {
            return nullptr;
        }

        auto address = after_invalidation_metadata_address();
        address = align(address, alignof(NodeInvocationMetadata));
        return reinterpret_cast<const NodeInvocationMetadata *>(address);
    }

  private:
    friend class CompiledGraphBuilder;

    static constexpr std::uint32_t parameter_count_bits = 10;
    static constexpr std::uint32_t parameter_count_mask = (std::uint32_t{1} << parameter_count_bits) - 1;
    static constexpr std::uint32_t item_subscriptions_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t output_notification_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t subscriber_count_mask = output_notification_flag - 1;
    static constexpr std::uint32_t no_output_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t state_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t output_offset_mask = (std::uint32_t{1} << 30) - 1;
    static constexpr std::uint32_t invalidation_mode_shift = 30;
    static constexpr std::uint32_t state_destruction_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t state_metadata_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t state_offset_mask = state_destruction_flag - 1;
    static constexpr std::uint32_t node_invocation_flag = std::uint32_t{1};
    static constexpr std::uint32_t node_invocation_time_series_input_count_shift = 1;
    static constexpr std::uint32_t node_invocation_time_series_input_count_mask =
        parameter_count_mask << node_invocation_time_series_input_count_shift;
    static constexpr std::uint32_t controlled_input_count_shift = 11;
    static constexpr std::uint32_t controlled_input_count_mask = parameter_count_mask << controlled_input_count_shift;
    // Zero means no metadata; otherwise store the existing runtime-type pointer count plus one.
    static constexpr std::uint32_t all_valid_runtime_type_count_shift = 21;
    static constexpr std::uint32_t all_valid_runtime_type_count_mask =
        ((std::uint32_t{1} << 10) - 1) << all_valid_runtime_type_count_shift;
    static constexpr std::uint32_t input_node_indices_flag = std::uint32_t{1} << 31;

    CompiledNode(EvaluationFunction evaluation_function, std::uint32_t output_last_updated_offset,
                 std::uint32_t output_value_offset, std::uint32_t parameter_count, std::uint32_t subscriber_count,
                 bool has_output, bool has_state, bool has_output_notification, bool has_item_subscriptions,
                 bool uses_node_invocation, bool has_all_valid_input_metadata, std::uint32_t runtime_type_count,
                 std::uint32_t time_series_input_count, std::uint32_t controlled_input_count,
                 bool has_input_node_indices, InvalidationMode invalidation_mode) noexcept
        : evaluation_function_(evaluation_function),
          output_last_updated_offset_(output_last_updated_offset | (has_output ? std::uint32_t{} : no_output_flag) |
                                      (has_state ? state_flag : std::uint32_t{})),
          output_value_offset_(output_value_offset |
                               (static_cast<std::uint32_t>(invalidation_mode) << invalidation_mode_shift)),
          counts_(parameter_count | (subscriber_count << parameter_count_bits) |
                  (has_output_notification ? output_notification_flag : std::uint32_t{}) |
                  (has_item_subscriptions ? item_subscriptions_flag : std::uint32_t{})),
          flags_((uses_node_invocation ? node_invocation_flag : std::uint32_t{}) |
                 (uses_node_invocation ? time_series_input_count << node_invocation_time_series_input_count_shift
                                       : std::uint32_t{}) |
                 (controlled_input_count << controlled_input_count_shift) |
                 ((has_all_valid_input_metadata ? runtime_type_count + 1 : std::uint32_t{})
                  << all_valid_runtime_type_count_shift) |
                 (has_input_node_indices ? input_node_indices_flag : std::uint32_t{})) {}

    bool has_input_node_indices() const noexcept { return (flags_ & input_node_indices_flag) != 0; }

    std::size_t controlled_input_count() const noexcept {
        return (flags_ & controlled_input_count_mask) >> controlled_input_count_shift;
    }

    std::uint32_t parameter_count() const noexcept { return counts_ & parameter_count_mask; }

    std::uint32_t subscriber_count() const noexcept {
        return (counts_ & subscriber_count_mask) >> parameter_count_bits;
    }

    std::uintptr_t output_notification_end() const noexcept {
        if (const auto *notification = output_notification()) {
            return reinterpret_cast<std::uintptr_t>(notification->end());
        }

        const auto node_subscribers = subscribers();
        return reinterpret_cast<std::uintptr_t>(node_subscribers.data() + node_subscribers.size());
    }

    static std::uintptr_t align(std::uintptr_t address, std::size_t alignment) noexcept {
        return (address + alignment - 1) & ~(alignment - 1);
    }

    std::uintptr_t optional_metadata_address() const noexcept {
        auto address = state_address();

        if (has_state()) {
            address += sizeof(std::uint32_t);
            if (state_requires_destruction()) {
                address = align(address, alignof(NodeFunction)) + sizeof(NodeFunction);
            }
            if (has_state_metadata()) {
                address += sizeof(StateMetadataCounts);
                address = align(address, alignof(std::uint32_t));
                address += state_metadata_counts().fields * sizeof(std::uint32_t);
                address = align(address, alignof(const ScalarRuntimeType *));
                address += state_metadata_counts().generic_fields * sizeof(const ScalarRuntimeType *);
            }
        }

        return address;
    }

    bool state_requires_destruction() const noexcept {
        const auto *offset = reinterpret_cast<const std::uint32_t *>(state_address());

        return (*offset & state_destruction_flag) != 0;
    }

    bool has_state_metadata() const noexcept {
        const auto *offset = reinterpret_cast<const std::uint32_t *>(state_address());

        return (*offset & state_metadata_flag) != 0;
    }

    const StateMetadataCounts &state_metadata_counts() const noexcept {
        auto address = state_address();
        address += sizeof(std::uint32_t);
        if (state_requires_destruction()) {
            address = align(address, alignof(NodeFunction)) + sizeof(NodeFunction);
        }

        return *reinterpret_cast<const StateMetadataCounts *>(address);
    }

    const std::uint32_t *state_field_offsets_address() const noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(&state_metadata_counts() + 1);
        address = align(address, alignof(std::uint32_t));

        return reinterpret_cast<const std::uint32_t *>(address);
    }

    std::uintptr_t state_address() const noexcept {
        auto address = output_notification_end();
        if (const auto *root = item_subscriptions()) {
            address = reinterpret_cast<std::uintptr_t>(root->end());
        }

        return address;
    }

    const RuntimeType &runtime_type(std::size_t index) const noexcept {
        return *reinterpret_cast<const RuntimeType *const *>(runtime_types_address())[index];
    }

    std::uintptr_t runtime_types_address() const noexcept {
        auto address = after_invalidation_metadata_address();
        if ((flags_ & node_invocation_flag) != 0) {
            address = align(address, alignof(NodeInvocationMetadata)) + sizeof(NodeInvocationMetadata);
        }

        return align(address, alignof(const RuntimeType *));
    }

    const AllValidInputMetadata &all_valid_input_metadata() const noexcept {
        const auto runtime_type_count =
            ((flags_ & all_valid_runtime_type_count_mask) >> all_valid_runtime_type_count_shift) - 1;
        auto address = runtime_types_address() + runtime_type_count * sizeof(const RuntimeType *);
        address = align(address, alignof(AllValidInputMetadata));
        return *reinterpret_cast<const AllValidInputMetadata *>(address);
    }

    std::uintptr_t after_invalidation_metadata_address() const noexcept {
        auto address = optional_metadata_address();
        if (invalidation_mode() == InvalidationMode::invoke) {
            address = align(address, alignof(NodeFunction)) + sizeof(NodeFunction);
        }

        return address;
    }

    EvaluationFunction evaluation_function_;
    std::uint32_t output_last_updated_offset_;
    std::uint32_t output_value_offset_;
    std::uint32_t counts_;
    std::uint32_t flags_;
};

static_assert(sizeof(ParameterBinding) == 8);
static_assert(sizeof(Subscriber) == 4);
static_assert(sizeof(ItemSubscription) == 16);
static_assert(sizeof(ItemSubscriptionRange) == 2 * sizeof(void *));
static_assert(sizeof(ItemSubscriptionRoot) == 2 * sizeof(void *));
static_assert(sizeof(StateMetadataCounts) == sizeof(std::uint32_t));
static_assert(sizeof(NodeInvocationParameterMetadata) == 2 * sizeof(void *));
static_assert(sizeof(NodeInvocationMetadata) == 2 * sizeof(void *));
static_assert(sizeof(CompiledNode) == 24);
static_assert(static_cast<std::uint32_t>(InvalidationMode::invoke) < (std::uint32_t{1} << 2));
static_assert(std::is_trivially_destructible_v<ParameterBinding>);
static_assert(std::is_trivially_destructible_v<CompiledNode>);
static_assert(sizeof(CompiledNode) % alignof(ParameterBinding) == 0);
static_assert(sizeof(ParameterBinding) % alignof(Subscriber) == 0);

inline std::span<const NodeInvocationParameterMetadata>
NodeInvocationMetadata::parameters(const CompiledNode &node) const noexcept {
    const auto *storage = reinterpret_cast<const std::byte *>(&node);
    return {reinterpret_cast<const NodeInvocationParameterMetadata *>(storage + parameters_offset),
            node.parameters().size()};
}

inline std::span<const std::uint16_t>
NodeInvocationMetadata::controlled_input_indices(const CompiledNode &node) const noexcept {
    const auto parameters = this->parameters(node);
    return {reinterpret_cast<const std::uint16_t *>(parameters.data() + parameters.size()), parameters.size()};
}

inline const void *NodeInvocationMetadata::compiled_data(const CompiledNode &node) const noexcept {
    if (compiled_data_offset == 0) {
        return nullptr;
    }

    return reinterpret_cast<const std::byte *>(&node) + compiled_data_offset;
}

} // namespace bg
