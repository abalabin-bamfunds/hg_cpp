#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstdint>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace bg {

class RuntimeGraph;
class TimeSeriesRuntimeType;

namespace details {

class ItemNotifications;
class ReferenceProvider;

using ItemChangeNotificationFunction = void (*)(OutputChangeContext &, void *, item_id_t, OutputChange);
using ItemChangePublicationFunction = void (*)(OutputChangeContext &, void *);

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

class ReferenceRepresentationFactory {
  public:
    using IsRequiredFunction = bool (*)(const TimeSeriesRuntimeType &, const TimeSeriesRuntimeType &) noexcept;
    using RepresentationRuntimeTypeFunction = const TimeSeriesRuntimeType &(*)(const TimeSeriesRuntimeType &,
                                                                                const TimeSeriesRuntimeType &);
    using CreateFunction = ReferenceRepresentation *(*)(ReferenceProvider &, Reference, const TimeSeriesRuntimeType &,
                                                        const TimeSeriesRuntimeType &, allocator &);

    explicit constexpr ReferenceRepresentationFactory(IsRequiredFunction is_required,
                                                      RepresentationRuntimeTypeFunction representation_runtime_type,
                                                      CreateFunction create) noexcept
        : is_required_{is_required}, representation_runtime_type_{representation_runtime_type}, create_{create} {}

    bool is_required(const TimeSeriesRuntimeType &provided, const TimeSeriesRuntimeType &requested) const noexcept {
        return is_required_(provided, requested);
    }

    const TimeSeriesRuntimeType &representation_runtime_type(const TimeSeriesRuntimeType &provided,
                                                             const TimeSeriesRuntimeType &requested) const {
        return representation_runtime_type_(provided, requested);
    }

    [[nodiscard]] ReferenceRepresentation *create(ReferenceProvider &provider, Reference reference,
                                                  const TimeSeriesRuntimeType &provided,
                                                  const TimeSeriesRuntimeType &requested, allocator &memory) const {
        return create_(provider, reference, provided, requested, memory);
    }

  private:
    IsRequiredFunction is_required_;
    RepresentationRuntimeTypeFunction representation_runtime_type_;
    CreateFunction create_;
};

class ReferenceProvider {
  public:
    ReferenceProvider(allocator memory, RootReference &root, DynamicSubscribers *root_subscribers);
    ~ReferenceProvider();

    ReferenceProvider(const ReferenceProvider &) = delete;
    ReferenceProvider &operator=(const ReferenceProvider &) = delete;

    // Returns the root's existing provider without creating one.
    static ReferenceProvider *get(Reference reference) noexcept;

    // Returns the root's provider, creating it when necessary.
    static ReferenceProvider &require(Reference reference);

    [[nodiscard]] static Reference representation(Reference reference,
                                                  const TimeSeriesRuntimeType &requested_runtime_type);
    static bool representation_required(const TimeSeriesRuntimeType &provided,
                                        const TimeSeriesRuntimeType &requested) noexcept;
    static const TimeSeriesRuntimeType &representation_runtime_type(const TimeSeriesRuntimeType &provided,
                                                                    const TimeSeriesRuntimeType &requested);

    static void add_subscriber(Reference reference, RuntimeGraph &graph, std::uint32_t input_offset,
                               std::size_t notification_data, std::uint32_t node_index, bool active, bool required,
                               InputNotificationFunction notification = nullptr);
    static void remove_subscriber(Reference reference, RuntimeGraph &graph, std::size_t input_offset) noexcept;
    static void notify(Reference reference, OutputChangeContext &context, OutputChange change);

    static void add_item_notification(Reference reference, RuntimeGraph &graph, void *destination_representation,
                                      item_id_t destination_item_id, ItemChangeNotificationFunction notify_item_change,
                                      ItemChangePublicationFunction publish_item_changes);
    static void remove_item_notification(Reference reference, RuntimeGraph &graph,
                                         const void *destination_representation, item_id_t destination_item_id,
                                         ItemChangeNotificationFunction notify_item_change) noexcept;

    static void destroy_root(RootReference &root, allocator &memory) noexcept;

    RuntimeGraph &owner() const noexcept { return *root_->owner_graph; }
    allocator &allocation() noexcept { return memory_; }
    const allocator &allocation() const noexcept { return memory_; }

    ResolvedTimeSeries resolve_representation(std::uint32_t index) const;
    void add_observer(Reference reference, RuntimeGraph &graph, void *receiver, RuntimeObserverFunction function);
    void remove_observer(Reference reference, RuntimeGraph &graph, const void *receiver) noexcept;
    void notify_subscriptions(const ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context,
                              OutputChange change);

    void begin_item_notification_batch() noexcept;
    void cancel_item_notification_batch() noexcept;
    void publish_item_notification_batch(OutputChangeContext &context);

    void add_cycle_callback(void *receiver, void (*function)(void *) noexcept);
    void remove_cycle_callback(const void *receiver) noexcept;

    bool item_reference_has_subscribers(const ItemReference &reference) const noexcept;
    bool release_item_reference(ItemReference &reference) noexcept;

    void add_component(ReferenceProviderComponent &component) noexcept;

    template <typename Component> Component *component() noexcept {
        static_assert(std::is_base_of_v<ReferenceProviderComponent, Component>);
        static_assert(std::is_final_v<Component>);
        for (auto *candidate = components_; candidate != nullptr; candidate = candidate->next_) {
            if (typeid(*candidate) == typeid(Component)) {
                return static_cast<Component *>(candidate);
            }
        }
        return nullptr;
    }

  private:
    static ReferenceSubscriptionDetail &subscriptions(Reference reference) noexcept;
    static Reference parent(Reference reference) noexcept;
    static RootReference &root(Reference reference) noexcept;
    static ReferenceProvider &require(RootReference &root);

    void add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t notification_data,
                             std::uint32_t node_index, bool active, bool required,
                             InputNotificationFunction notification);
    void add_root_observer(RuntimeGraph &graph, void *receiver, RuntimeObserverFunction function);
    void remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void remove_root_observer(RuntimeGraph &graph, const void *receiver) noexcept;
    void notify_root(OutputChangeContext &context, OutputChange change);
    void add_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, std::uint32_t input_offset,
                        std::size_t notification_data, std::uint32_t node_index, bool active, bool required,
                        InputNotificationFunction notification);
    void add_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, void *receiver,
                      RuntimeObserverFunction function);
    void remove_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                           std::size_t input_offset) noexcept;
    void remove_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                         const void *receiver) noexcept;
    void add_item_notification_record(Reference reference, RuntimeGraph &graph, void *destination_representation,
                                      item_id_t destination_item_id,
                                      ItemChangeNotificationFunction notify_item_change,
                                      ItemChangePublicationFunction publish_item_changes);
    void remove_item_notification_record(Reference reference, RuntimeGraph &graph,
                                         const void *destination_representation, item_id_t destination_item_id,
                                         ItemChangeNotificationFunction notify_item_change) noexcept;
    [[nodiscard]] Reference make_representation(Reference base_reference,
                                                const TimeSeriesRuntimeType &requested_runtime_type);

    allocator memory_;
    RootReference *root_;
    std::pmr::vector<ReferenceRepresentation *> representations_;
    std::pmr::vector<DynamicSubscribers *> subscriber_groups_;
    ReferenceProviderComponent *components_{};
    ItemNotifications *item_notifications_{};
};

} // namespace details
} // namespace bg
