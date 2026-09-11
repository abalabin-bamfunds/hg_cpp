#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstdint>
#include <type_traits>
#include <vector>

namespace bg {

class RuntimeGraph;
class TimeSeriesType;

namespace details {

class ReferenceItemNotificationLinks;

using ReferenceItemNotifyFunction = void (*)(OutputChangeContext &, void *, item_id_t, OutputChange);
using ReferenceItemPublishFunction = void (*)(OutputChangeContext &, void *);

class ReferenceProviderComponent {
  public:
    virtual ~ReferenceProviderComponent();

    ReferenceProviderComponent(const ReferenceProviderComponent &) = delete;
    ReferenceProviderComponent &operator=(const ReferenceProviderComponent &) = delete;

    virtual void detach() noexcept = 0;
    virtual void destroy(allocator &memory) noexcept = 0;

  protected:
    ReferenceProviderComponent() = default;

  private:
    friend class ReferenceProvider;

    ReferenceProviderComponent *next_{};
};

static_assert(sizeof(ReferenceProviderComponent) == 2 * sizeof(void *));

class ReferenceRepresentation {
  public:
    explicit ReferenceRepresentation(Reference base_reference) noexcept : base_reference_{base_reference} {}
    virtual ~ReferenceRepresentation();

    Reference base_reference() const noexcept { return base_reference_; }
    Reference reference() noexcept { return &reference_; }
    virtual ResolvedTimeSeries resolve() const = 0;
    virtual void notify(OutputChangeContext &context, OutputChange change) = 0;
    virtual void destroy(allocator &memory) noexcept = 0;

  private:
    friend class ReferenceProvider;

    void bind(std::uint32_t index) noexcept { reference_ = RepresentationReference{base_reference_, index}; }

    Reference base_reference_;
    RepresentationReference reference_;
};

class ReferenceProvider {
  public:
    ReferenceProvider(allocator memory, RootReference &root, DynamicSubscribers *root_subscribers);
    ~ReferenceProvider();

    ReferenceProvider(const ReferenceProvider &) = delete;
    ReferenceProvider &operator=(const ReferenceProvider &) = delete;

    RuntimeGraph &owner() const noexcept { return *root_->owner_graph; }
    allocator &allocation() noexcept { return memory_; }
    const allocator &allocation() const noexcept { return memory_; }
    ResolvedTimeSeries resolve_representation(std::uint32_t index) const;
    void add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t callback_data,
                             std::uint32_t node_index, bool active, bool required,
                             InputChangeFunction callback = nullptr);
    void add_root_observer(RuntimeGraph &graph, void *receiver, DynamicObserverFunction function);
    void remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void remove_root_observer(RuntimeGraph &graph, const void *receiver) noexcept;
    void notify_root(OutputChangeContext &context, OutputChange change);
    void add_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, std::uint32_t input_offset,
                        std::size_t callback_data, std::uint32_t node_index, bool active, bool required,
                        InputChangeFunction callback = nullptr);
    void add_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, void *receiver,
                      DynamicObserverFunction function);
    void remove_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                           std::size_t input_offset) noexcept;
    void remove_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                         const void *receiver) noexcept;
    void notify(ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context, OutputChange change);
    void begin_item_notification_batch() noexcept;
    void cancel_item_notification_batch() noexcept;
    void publish_item_notification_batch(OutputChangeContext &context);
    void add_item_notification_link(Reference reference, RuntimeGraph &graph, void *receiver,
                                    item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function,
                                    ReferenceItemPublishFunction publish_function);
    void remove_item_notification_link(Reference reference, RuntimeGraph &graph, const void *receiver,
                                       item_id_t receiver_item_id,
                                       ReferenceItemNotifyFunction notify_function) noexcept;
    void add_cycle_callback(void *receiver, void (*function)(void *) noexcept);
    void remove_cycle_callback(const void *receiver) noexcept;
    bool release_item_reference(ItemReference &reference) noexcept;
    [[nodiscard]] Reference representation(Reference base_reference, const TimeSeriesType &requested_type);
    void add_component(ReferenceProviderComponent &component) noexcept;

    template <typename Component> Component *component() noexcept {
        static_assert(std::is_base_of_v<ReferenceProviderComponent, Component>);
        for (auto *candidate = components_; candidate != nullptr; candidate = candidate->next_) {
            if (auto *result = dynamic_cast<Component *>(candidate)) {
                return result;
            }
        }
        return nullptr;
    }

  private:
    allocator memory_;
    RootReference *root_;
    std::pmr::vector<ReferenceRepresentation *> representations_;
    std::pmr::vector<DynamicSubscribers *> subscriber_groups_;
    ReferenceProviderComponent *components_{};
    ReferenceItemNotificationLinks *item_notification_links_{};
};

[[nodiscard]] Reference reference_representation(Reference reference, const TimeSeriesType &requested_type);
ReferenceProvider &require_reference_provider(Reference reference);
void add_reference_subscriber(Reference reference, RuntimeGraph &graph, std::uint32_t input_offset,
                              std::size_t callback_data, std::uint32_t node_index, bool active, bool required,
                              InputChangeFunction callback = nullptr);
void add_reference_observer(Reference reference, RuntimeGraph &graph, void *receiver, DynamicObserverFunction function);
void remove_reference_subscriber(Reference reference, RuntimeGraph &graph, std::size_t input_offset) noexcept;
void remove_reference_observer(Reference reference, RuntimeGraph &graph, const void *receiver) noexcept;
void notify_reference(Reference reference, OutputChangeContext &context, OutputChange change);
void add_reference_item_notification_link(Reference reference, RuntimeGraph &graph, void *receiver,
                                          item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function,
                                          ReferenceItemPublishFunction publish_function);
void remove_reference_item_notification_link(Reference reference, RuntimeGraph &graph, const void *receiver,
                                             item_id_t receiver_item_id,
                                             ReferenceItemNotifyFunction notify_function) noexcept;
void destroy_root_reference(RootReference &root, allocator &memory) noexcept;

} // namespace details
} // namespace bg
