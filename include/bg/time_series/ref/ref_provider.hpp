#pragma once

#include <bg/graph/output_notification.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstdint>
#include <memory_resource>
#include <vector>

namespace bg {

class RuntimeGraph;
class TimeSeriesType;

namespace details {

class ScalarData;
class DictionaryObservers;
class ReferenceItemSubscribers;

using ReferenceItemChangeFunction = void (*)(OutputChangeContext &, void *, TimeSeriesItemId, OutputChange);
using ReferenceItemPublishFunction = void (*)(OutputChangeContext &, void *);

class DictionaryKeySubscription {
  public:
    DictionaryKeySubscription() = default;

    explicit operator bool() const noexcept { return dictionary_ != nullptr; }

  private:
    friend class DictionaryObservers;
    friend bool reference_dictionary_key_subscription_matches(DictionaryKeySubscription, ReferenceAddress,
                                                               const ScalarData &);
    friend ReferenceAddress reference_dictionary_key_subscription_target(DictionaryKeySubscription);
    friend void remove_reference_dictionary_key_subscriber(DictionaryKeySubscription, RuntimeGraph &,
                                                           std::uint32_t) noexcept;

    DictionaryKeySubscription(void *dictionary, TimeSeriesItemId item) noexcept
        : dictionary_{dictionary}, item_{item} {}

    void *dictionary_{};
    TimeSeriesItemId item_{TimeSeriesItemId(-1)};
};

static_assert(sizeof(DictionaryKeySubscription) == 2 * sizeof(void *));

class ReferenceRepresentation {
  public:
    explicit ReferenceRepresentation(ReferenceAddress input) noexcept : input_{input} {}
    virtual ~ReferenceRepresentation();

    ReferenceAddress input() const noexcept { return input_; }
    ReferenceAddress address() noexcept { return &address_; }
    virtual ReferenceTarget target() const = 0;
    virtual void notify(OutputChangeContext &context, OutputChange change) = 0;
    virtual void destroy(std::pmr::memory_resource &memory) noexcept = 0;

  private:
    friend class ReferenceProvider;

    void bind(std::uint32_t index) noexcept { address_.bind_representation(input_, index); }

    ReferenceAddress input_;
    ReferenceRoute address_;
};

class ReferenceProvider {
  public:
    ReferenceProvider(std::pmr::memory_resource &memory, ReferenceRoot &root, DynamicSubscribers *root_subscribers);
    ~ReferenceProvider();

    ReferenceProvider(const ReferenceProvider &) = delete;
    ReferenceProvider &operator=(const ReferenceProvider &) = delete;

    RuntimeGraph &owner() const noexcept { return *root_->graph; }
    std::pmr::memory_resource &memory() const noexcept { return *memory_; }
    ReferenceTarget representation_target(std::uint32_t index) const;
    void add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data,
                             std::uint32_t node_index, bool active, bool required,
                             InputChangeFunction callback = nullptr);
    void add_root_observer(RuntimeGraph &graph, void *target, DynamicObserverFunction function);
    void remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void remove_root_observer(RuntimeGraph &graph, const void *target) noexcept;
    void notify_root(OutputChangeContext &context, OutputChange change);
    void add_subscriber(ReferenceRoute &route, RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data,
                        std::uint32_t node_index, bool active, bool required, InputChangeFunction callback = nullptr);
    void add_observer(ReferenceRoute &route, RuntimeGraph &graph, void *target, DynamicObserverFunction function);
    void remove_subscriber(ReferenceRoute &route, RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void remove_observer(ReferenceRoute &route, RuntimeGraph &graph, const void *target) noexcept;
    void notify(ReferenceRoute &route, OutputChangeContext &context, OutputChange change);
    void begin_item_notifications() noexcept;
    void cancel_item_notifications() noexcept;
    void publish_item_notifications(OutputChangeContext &context);
    void add_item_subscriber(ReferenceRoute *route, RuntimeGraph &graph, void *target, TimeSeriesItemId item,
                             ReferenceItemChangeFunction change_function,
                             ReferenceItemPublishFunction publish_function);
    void remove_item_subscriber(ReferenceRoute *route, RuntimeGraph &graph, const void *target,
                                TimeSeriesItemId item, ReferenceItemChangeFunction change_function) noexcept;
    void add_cycle_callback(void *target, void (*function)(void *) noexcept);
    void remove_cycle_callback(const void *target) noexcept;
    bool release_route(ReferenceRoute &route) noexcept;
    [[nodiscard]] ReferenceAddress representation(ReferenceAddress input, const TimeSeriesType &requested_type);
    [[nodiscard]] DictionaryKeySubscription add_dictionary_key_subscriber(ReferenceAddress dictionary,
                                                                          const ScalarData &key, RuntimeGraph &graph,
                                                                          std::uint32_t node_index);

  private:
    std::pmr::memory_resource *memory_;
    ReferenceRoot *root_;
    std::pmr::vector<ReferenceRepresentation *> representations_;
    std::pmr::vector<DynamicSubscribers *> subscriber_groups_;
    DictionaryObservers *dictionary_observers_{};
    ReferenceItemSubscribers *item_subscribers_{};
};

[[nodiscard]] ReferenceAddress reference_representation(ReferenceAddress input, const TimeSeriesType &requested_type);
void add_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::uint32_t input_offset,
                              std::size_t data, std::uint32_t node_index, bool active, bool required,
                              InputChangeFunction callback = nullptr);
void add_reference_observer(ReferenceAddress address, RuntimeGraph &graph, void *target,
                            DynamicObserverFunction function);
void remove_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::size_t input_offset) noexcept;
void remove_reference_observer(ReferenceAddress address, RuntimeGraph &graph, const void *target) noexcept;
void notify_reference(ReferenceAddress address, OutputChangeContext &context, OutputChange change);
void add_reference_item_subscriber(ReferenceAddress address, RuntimeGraph &graph, void *target,
                                   TimeSeriesItemId item, ReferenceItemChangeFunction change_function,
                                   ReferenceItemPublishFunction publish_function);
void remove_reference_item_subscriber(ReferenceAddress address, RuntimeGraph &graph, const void *target,
                                      TimeSeriesItemId item,
                                      ReferenceItemChangeFunction change_function) noexcept;
[[nodiscard]] DictionaryKeySubscription add_reference_dictionary_key_subscriber(ReferenceAddress address,
                                                                                const ScalarData &key,
                                                                                RuntimeGraph &graph,
                                                                                std::uint32_t node_index);
bool reference_dictionary_key_subscription_matches(DictionaryKeySubscription subscription, ReferenceAddress dictionary,
                                                   const ScalarData &key);
ReferenceAddress reference_dictionary_key_subscription_target(DictionaryKeySubscription subscription);
void remove_reference_dictionary_key_subscriber(DictionaryKeySubscription subscription, RuntimeGraph &graph,
                                                std::uint32_t node_index) noexcept;
void destroy_reference_root(ReferenceRoot &root, std::pmr::memory_resource &memory) noexcept;

} // namespace details
} // namespace bg
