#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/evaluation_function.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/time_series_output_view.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>

namespace bg {

class CompiledGraphBuilder;
class ScalarStorageRepresentation;
class ScalarInputView;
class TimeSeriesInputView;
class TimeSeriesValueInputView;
class TimeSeriesOutputView;
class ItemSubscriptionRange;

struct TimeSeriesInputViewPrototype {
    const TimeSeriesInputView *view;
};

struct ScalarInputViewPrototype {
    const ScalarInputView *view;
};

struct OutputViewPrototype {
    const TimeSeriesOutputView *view;
};

struct StateMetadataCounts {
    std::uint16_t fields;
    std::uint16_t generic_fields;
};

enum class ParameterBindingMode : std::uint8_t {
    time_series,
    reference,
    scalar,
};

class Subscriber {
  public:
    Subscriber(std::uint32_t node_index, bool active, bool required, bool active_optional) noexcept
        : node_index_and_flags_{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                                (active_optional ? active_optional_flag : 0)} {}

    // A dynamic entry stores a pointer-aligned runtime-storage offset instead of a node index.
    [[nodiscard]] static Subscriber dynamic_subscribers(std::uint32_t offset) noexcept {
        return Subscriber{dynamic_subscribers_flag |
                          static_cast<std::uint32_t>(offset / dynamic_subscribers_alignment)};
    }

    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_and_flags_ & node_index_mask; }

    [[nodiscard]] bool active() const noexcept { return (node_index_and_flags_ & active_flag) != 0; }

    [[nodiscard]] bool required() const noexcept { return (node_index_and_flags_ & required_flag) != 0; }

    [[nodiscard]] bool active_optional() const noexcept { return (node_index_and_flags_ & active_optional_flag) != 0; }

    [[nodiscard]] bool has_dynamic_subscribers() const noexcept {
        return (node_index_and_flags_ & dynamic_subscribers_flag) != 0;
    }

    [[nodiscard]] std::size_t dynamic_subscribers_offset() const noexcept {
        return (node_index_and_flags_ & dynamic_subscribers_offset_mask) * dynamic_subscribers_alignment;
    }

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t active_optional_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t dynamic_subscribers_flag = std::uint32_t{1} << 28;
    static constexpr std::uint32_t node_index_mask = dynamic_subscribers_flag - 1;
    static constexpr std::uint32_t dynamic_subscribers_offset_mask = dynamic_subscribers_flag - 1;
    static constexpr std::size_t dynamic_subscribers_alignment = alignof(void *);

    explicit Subscriber(std::uint32_t value) noexcept : node_index_and_flags_{value} {}

    std::uint32_t node_index_and_flags_;
};

class alignas(void *) ItemSubscribers {
  public:
    ItemSubscribers(std::uint32_t item, std::uint32_t subscriber_count, std::uint32_t subtree_size,
                    std::uint32_t sampled_last_updated_offset, bool has_output_notification) noexcept
        : item_{item},
          subscriber_count_{subscriber_count | (has_output_notification ? output_notification_flag : std::uint32_t{})},
          subtree_size_{subtree_size}, sampled_last_updated_offset_{sampled_last_updated_offset} {}

    [[nodiscard]] std::size_t item() const noexcept { return item_; }

    [[nodiscard]] std::span<const Subscriber> subscribers() const noexcept {
        return {reinterpret_cast<const Subscriber *>(this + 1), subscriber_count()};
    }

    [[nodiscard]] const OutputNotification *output_notification() const noexcept {
        if ((subscriber_count_ & output_notification_flag) == 0) {
            return nullptr;
        }

        const auto subscribers = this->subscribers();
        auto address = reinterpret_cast<std::uintptr_t>(subscribers.data() + subscribers.size());
        address = align(address, alignof(OutputNotification));
        return reinterpret_cast<const OutputNotification *>(address);
    }

    [[nodiscard]] std::size_t sampled_last_updated_offset() const noexcept { return sampled_last_updated_offset_; }

    [[nodiscard]] ItemSubscriptionRange children() const noexcept;

  private:
    friend ItemSubscriptionRange;

    [[nodiscard]] const ItemSubscribers *first_child() const noexcept {
        auto address = output_notification() != nullptr
                           ? reinterpret_cast<std::uintptr_t>(output_notification()->end())
                           : reinterpret_cast<std::uintptr_t>(subscribers().data() + subscribers().size());
        address = align(address, alignof(ItemSubscribers));
        return reinterpret_cast<const ItemSubscribers *>(address);
    }

    [[nodiscard]] const ItemSubscribers *next() const noexcept {
        const auto *address = reinterpret_cast<const std::byte *>(this);
        return reinterpret_cast<const ItemSubscribers *>(address + subtree_size_);
    }
    [[nodiscard]] static std::uintptr_t align(std::uintptr_t address, std::size_t alignment) noexcept {
        return (address + alignment - 1) & ~(alignment - 1);
    }

    static constexpr std::uint32_t output_notification_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t subscriber_count_mask = output_notification_flag - 1;

    [[nodiscard]] std::size_t subscriber_count() const noexcept { return subscriber_count_ & subscriber_count_mask; }

    std::uint32_t item_;
    std::uint32_t subscriber_count_;
    std::uint32_t subtree_size_;
    std::uint32_t sampled_last_updated_offset_;
};

class ItemSubscriptionRange {
  public:
    [[nodiscard]] const ItemSubscribers *find(std::size_t item) const noexcept {
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
    friend ItemSubscribers;
    friend struct ItemSubscriptionRoot;

    ItemSubscriptionRange(const ItemSubscribers *first, const ItemSubscribers *last) noexcept
        : first_{first}, last_{last} {}

    const ItemSubscribers *first_;
    const ItemSubscribers *last_;
};

inline ItemSubscriptionRange ItemSubscribers::children() const noexcept {
    return {first_child(), next()};
}

struct ItemSubscriptionRoot {
    const TimeSeriesStorageRepresentation *representation;
    std::uint32_t byte_size;

    ItemSubscriptionRoot(const TimeSeriesStorageRepresentation &representation, std::uint32_t byte_size) noexcept
        : representation{&representation}, byte_size{byte_size} {}

    [[nodiscard]] const ItemSubscribers *first_item() const noexcept {
        return reinterpret_cast<const ItemSubscribers *>(this + 1);
    }

    [[nodiscard]] const std::byte *end() const noexcept {
        return reinterpret_cast<const std::byte *>(first_item()) + byte_size;
    }

    [[nodiscard]] ItemSubscriptionRange children() const noexcept {
        return {first_item(), reinterpret_cast<const ItemSubscribers *>(end())};
    }
};

// Locates a time series in runtime storage or a scalar in the packed node record.
class ParameterBinding {
  public:
    [[nodiscard]] ParameterBindingMode mode() const noexcept {
        if ((location_and_flags_ & scalar_flag) != 0) {
            return ParameterBindingMode::scalar;
        }
        return (value_offset_ & reference_flag) == 0 ? ParameterBindingMode::time_series
                                                     : ParameterBindingMode::reference;
    }

    [[nodiscard]] std::size_t last_updated_offset() const noexcept { return location_and_flags_ & location_mask; }

    [[nodiscard]] auto last_updated(const void *runtime_storage) const noexcept -> const EvaluationTime & {
        const auto *storage = static_cast<const std::byte *>(runtime_storage);
        const auto &source_last_updated = *reinterpret_cast<const EvaluationTime *>(storage + last_updated_offset());
        if (mode() != ParameterBindingMode::reference) {
            return source_last_updated;
        }

        const auto *target = reference_input(runtime_storage).target;
        if (target == nullptr) {
            return MIN_DT;
        }

        const auto &target_last_updated = *target->target_last_updated;
        if (target_last_updated <= MIN_DT) {
            return target_last_updated;
        }

        return source_last_updated > target_last_updated ? source_last_updated : target_last_updated;
    }

    [[nodiscard]] std::size_t value_offset() const noexcept { return value_offset_ & value_location_mask; }

    [[nodiscard]] const void *value(const void *runtime_storage) const noexcept {
        const auto *storage = static_cast<const std::byte *>(runtime_storage);
        if (mode() == ParameterBindingMode::reference) {
            const auto *target = reference_input(runtime_storage).target;
            return target == nullptr ? nullptr : target->target_value;
        }
        return storage + value_offset();
    }

    [[nodiscard]] const details::ReferenceInputStorage &reference_input(const void *runtime_storage) const noexcept {
        const auto *storage = static_cast<const std::byte *>(runtime_storage);
        return *reinterpret_cast<const details::ReferenceInputStorage *>(storage + value_offset());
    }

    [[nodiscard]] details::ReferenceInputStorage &reference_input(void *runtime_storage) const noexcept {
        auto *storage = static_cast<std::byte *>(runtime_storage);
        return *reinterpret_cast<details::ReferenceInputStorage *>(storage + value_offset());
    }

    [[nodiscard]] bool active() const noexcept { return (value_offset_ & active_flag) != 0; }

    [[nodiscard]] const void *scalar_data() const noexcept {
        if ((location_and_flags_ & optional_flag) != 0) {
            return nullptr;
        }

        const auto *address = reinterpret_cast<const std::byte *>(this);

        return address + (location_and_flags_ & location_mask);
    }

    [[nodiscard]] InputValidity validity() const noexcept {
        return (location_and_flags_ & optional_flag) == 0 ? required : optional;
    }

  private:
    friend class CompiledGraphBuilder;

    static constexpr std::uint32_t scalar_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t optional_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t location_mask = optional_flag - 1;
    static constexpr std::uint32_t reference_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t value_location_mask = active_flag - 1;

    ParameterBinding(std::uint32_t last_updated_offset, std::uint32_t value_offset, InputValidity validity) noexcept
        : location_and_flags_{last_updated_offset | (validity == optional ? optional_flag : std::uint32_t{})},
          value_offset_{value_offset} {}

    ParameterBinding(std::uint32_t reference_last_updated_offset, std::uint32_t input_offset, InputActivity activity,
                     InputValidity validity) noexcept
        : location_and_flags_{reference_last_updated_offset | (validity == optional ? optional_flag : std::uint32_t{})},
          value_offset_{input_offset | reference_flag | (activity == bg::active ? active_flag : std::uint32_t{})} {}

    explicit ParameterBinding(std::uint32_t scalar_offset) noexcept
        : location_and_flags_{scalar_offset | scalar_flag}, value_offset_{} {}

    ParameterBinding() noexcept : location_and_flags_{scalar_flag | optional_flag}, value_offset_{} {}

    std::uint32_t location_and_flags_;
    std::uint32_t value_offset_;
};

// Begins a packed node record. Parameters, subscribers, optional metadata, and scalar values follow the fixed header.
class CompiledNode {
  public:
    [[nodiscard]] bool has_output() const noexcept { return (output_last_updated_offset_ & no_output_flag) == 0; }

    [[nodiscard]] EvaluationFunction evaluation_function() const noexcept { return evaluation_function_; }

    [[nodiscard]] bool has_state() const noexcept { return (output_last_updated_offset_ & state_flag) != 0; }

    [[nodiscard]] std::size_t output_last_updated_offset() const noexcept {
        return output_last_updated_offset_ & output_offset_mask;
    }

    [[nodiscard]] auto output_last_updated(void *runtime_storage) const noexcept -> EvaluationTime & {
        auto *storage = static_cast<std::byte *>(runtime_storage);
        return *reinterpret_cast<EvaluationTime *>(storage + output_last_updated_offset());
    }

    [[nodiscard]] std::size_t output_value_offset() const noexcept { return output_value_offset_ & output_offset_mask; }

    [[nodiscard]] InvalidationMode invalidation_mode() const noexcept {
        return static_cast<InvalidationMode>(output_value_offset_ >> invalidation_mode_shift);
    }

    [[nodiscard]] std::span<const ParameterBinding> parameters() const noexcept {
        const auto *first = std::launder(reinterpret_cast<const ParameterBinding *>(this + 1));

        return {first, parameter_count()};
    }

    [[nodiscard]] std::span<const Subscriber> subscribers() const noexcept {
        const auto parameter_span = parameters();
        const auto *first = reinterpret_cast<const Subscriber *>(parameter_span.data() + parameter_span.size());

        return {first, subscriber_count()};
    }

    [[nodiscard]] const OutputNotification *output_notification() const noexcept {
        if ((counts_ & output_notification_flag) == 0) {
            return nullptr;
        }

        const auto subscribers = this->subscribers();
        auto address = reinterpret_cast<std::uintptr_t>(subscribers.data() + subscribers.size());
        address = align(address, alignof(OutputNotification));
        return reinterpret_cast<const OutputNotification *>(address);
    }

    [[nodiscard]] const ItemSubscriptionRoot *item_subscriptions() const noexcept {
        if ((counts_ & item_subscriptions_flag) == 0) {
            return nullptr;
        }

        auto address = output_notification_end();
        address = align(address, alignof(ItemSubscriptionRoot));
        return reinterpret_cast<const ItemSubscriptionRoot *>(address);
    }

    [[nodiscard]] std::size_t state_offset() const noexcept {
        const auto *offset = reinterpret_cast<const std::uint32_t *>(state_address());

        return *offset & state_offset_mask;
    }

    [[nodiscard]] EvaluationFunction state_destruction_function() const noexcept {
        if (!state_requires_destruction()) {
            return nullptr;
        }

        auto address = state_address();
        address = align(address + sizeof(std::uint32_t), alignof(EvaluationFunction));

        return *reinterpret_cast<const EvaluationFunction *>(address);
    }

    [[nodiscard]] std::size_t state_field_offset(std::size_t index) const noexcept {
        return state_field_offsets_address()[index];
    }

    [[nodiscard]] const ScalarStorageRepresentation &
    generic_state_storage_representation(std::size_t index) const noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(state_field_offsets_address() + state_metadata_counts().fields);
        address = align(address, alignof(const ScalarStorageRepresentation *));

        return **(reinterpret_cast<const ScalarStorageRepresentation *const *>(address) + index);
    }

    [[nodiscard]] EvaluationFunction invalidation_function() const noexcept {
        const auto address = align(optional_metadata_address(), alignof(EvaluationFunction));

        return *reinterpret_cast<const EvaluationFunction *>(address);
    }

    [[nodiscard]] const TimeSeriesStorageRepresentation *output_finalization_representation() const noexcept {
        if (const auto *subscriptions = item_subscriptions()) {
            return subscriptions->representation;
        }
        if ((flags_ & output_finalization_flag) == 0) {
            return nullptr;
        }

        auto address = after_invalidation_metadata_address();
        address = align(address, alignof(const TimeSeriesStorageRepresentation *));
        return *reinterpret_cast<const TimeSeriesStorageRepresentation *const *>(address);
    }

    [[nodiscard]] const TimeSeriesInputView &time_series_input_view_prototype(std::size_t index) const noexcept {
        return *reinterpret_cast<const TimeSeriesInputViewPrototype *>(input_view_prototypes_address())[index].view;
    }

    [[nodiscard]] const TimeSeriesValueInputView &
    time_series_value_input_view_prototype(std::size_t index) const noexcept {
        return static_cast<const TimeSeriesValueInputView &>(time_series_input_view_prototype(index));
    }

    [[nodiscard]] const ScalarInputView &scalar_input_view_prototype(std::size_t time_series_input_count,
                                                                     std::size_t index) const noexcept {
        auto address = input_view_prototypes_address() + time_series_input_count * sizeof(TimeSeriesInputViewPrototype);
        address = align(address, alignof(ScalarInputViewPrototype));

        return *reinterpret_cast<const ScalarInputViewPrototype *>(address)[index].view;
    }

    [[nodiscard]] const TimeSeriesOutputView &output_view_prototype(std::size_t time_series_input_count,
                                                                    std::size_t scalar_input_count) const noexcept {
        auto address = input_view_prototypes_address() + time_series_input_count * sizeof(TimeSeriesInputViewPrototype);
        address = align(address, alignof(ScalarInputViewPrototype));
        address += scalar_input_count * sizeof(ScalarInputViewPrototype);
        address = align(address, alignof(OutputViewPrototype));

        return *reinterpret_cast<const OutputViewPrototype *>(address)->view;
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
    static constexpr std::uint32_t output_finalization_flag = std::uint32_t{1};

    CompiledNode(EvaluationFunction evaluation_function, std::uint32_t output_last_updated_offset,
                 std::uint32_t output_value_offset, std::uint32_t parameter_count, std::uint32_t subscriber_count,
                 bool has_output, bool has_state, bool has_output_notification, bool has_item_subscriptions,
                 bool requires_output_finalization, InvalidationMode invalidation_mode) noexcept
        : evaluation_function_(evaluation_function),
          output_last_updated_offset_(output_last_updated_offset | (has_output ? std::uint32_t{} : no_output_flag) |
                                      (has_state ? state_flag : std::uint32_t{})),
          output_value_offset_(output_value_offset |
                               (static_cast<std::uint32_t>(invalidation_mode) << invalidation_mode_shift)),
          counts_(parameter_count | (subscriber_count << parameter_count_bits) |
                  (has_output_notification ? output_notification_flag : std::uint32_t{}) |
                  (has_item_subscriptions ? item_subscriptions_flag : std::uint32_t{})),
          flags_(requires_output_finalization ? output_finalization_flag : std::uint32_t{}) {}

    [[nodiscard]] std::uint32_t parameter_count() const noexcept { return counts_ & parameter_count_mask; }

    [[nodiscard]] std::uint32_t subscriber_count() const noexcept {
        return (counts_ & subscriber_count_mask) >> parameter_count_bits;
    }

    [[nodiscard]] std::uintptr_t output_notification_end() const noexcept {
        if (const auto *notification = output_notification()) {
            return reinterpret_cast<std::uintptr_t>(notification->end());
        }

        const auto node_subscribers = subscribers();
        return reinterpret_cast<std::uintptr_t>(node_subscribers.data() + node_subscribers.size());
    }

    [[nodiscard]] static std::uintptr_t align(std::uintptr_t address, std::size_t alignment) noexcept {
        return (address + alignment - 1) & ~(alignment - 1);
    }

    [[nodiscard]] std::uintptr_t optional_metadata_address() const noexcept {
        auto address = state_address();

        if (has_state()) {
            address += sizeof(std::uint32_t);
            if (state_requires_destruction()) {
                address = align(address, alignof(EvaluationFunction)) + sizeof(EvaluationFunction);
            }
            if (has_state_metadata()) {
                address += sizeof(StateMetadataCounts);
                address = align(address, alignof(std::uint32_t));
                address += state_metadata_counts().fields * sizeof(std::uint32_t);
                address = align(address, alignof(const ScalarStorageRepresentation *));
                address += state_metadata_counts().generic_fields * sizeof(const ScalarStorageRepresentation *);
            }
        }

        return address;
    }

    [[nodiscard]] bool state_requires_destruction() const noexcept {
        const auto *offset = reinterpret_cast<const std::uint32_t *>(state_address());

        return (*offset & state_destruction_flag) != 0;
    }

    [[nodiscard]] bool has_state_metadata() const noexcept {
        const auto *offset = reinterpret_cast<const std::uint32_t *>(state_address());

        return (*offset & state_metadata_flag) != 0;
    }

    [[nodiscard]] const StateMetadataCounts &state_metadata_counts() const noexcept {
        auto address = state_address();
        address += sizeof(std::uint32_t);
        if (state_requires_destruction()) {
            address = align(address, alignof(EvaluationFunction)) + sizeof(EvaluationFunction);
        }

        return *reinterpret_cast<const StateMetadataCounts *>(address);
    }

    [[nodiscard]] const std::uint32_t *state_field_offsets_address() const noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(&state_metadata_counts() + 1);
        address = align(address, alignof(std::uint32_t));

        return reinterpret_cast<const std::uint32_t *>(address);
    }

    [[nodiscard]] std::uintptr_t state_address() const noexcept {
        auto address = output_notification_end();
        if (const auto *root = item_subscriptions()) {
            address = reinterpret_cast<std::uintptr_t>(root->end());
        }

        return address;
    }

    [[nodiscard]] std::uintptr_t input_view_prototypes_address() const noexcept {
        auto address = after_invalidation_metadata_address();
        if ((flags_ & output_finalization_flag) != 0) {
            address = align(address, alignof(const TimeSeriesStorageRepresentation *)) +
                      sizeof(const TimeSeriesStorageRepresentation *);
        }

        return align(address, alignof(TimeSeriesInputViewPrototype));
    }

    [[nodiscard]] std::uintptr_t after_invalidation_metadata_address() const noexcept {
        auto address = optional_metadata_address();
        if (invalidation_mode() == InvalidationMode::invoke) {
            address = align(address, alignof(EvaluationFunction)) + sizeof(EvaluationFunction);
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
static_assert(sizeof(ItemSubscribers) == 16);
static_assert(sizeof(ItemSubscriptionRange) == 2 * sizeof(void *));
static_assert(sizeof(ItemSubscriptionRoot) == 2 * sizeof(void *));
static_assert(sizeof(TimeSeriesInputViewPrototype) == sizeof(void *));
static_assert(sizeof(ScalarInputViewPrototype) == sizeof(void *));
static_assert(sizeof(OutputViewPrototype) == sizeof(void *));
static_assert(sizeof(StateMetadataCounts) == sizeof(std::uint32_t));
static_assert(sizeof(CompiledNode) == 24);
static_assert(static_cast<std::uint32_t>(InvalidationMode::invoke) < (std::uint32_t{1} << 2));
static_assert(std::is_trivially_destructible_v<ParameterBinding>);
static_assert(std::is_trivially_destructible_v<CompiledNode>);
static_assert(sizeof(CompiledNode) % alignof(ParameterBinding) == 0);
static_assert(sizeof(ParameterBinding) % alignof(Subscriber) == 0);

} // namespace bg
