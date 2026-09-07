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

class ReferenceRepresentation {
  public:
    explicit ReferenceRepresentation(ReferenceAddress source) noexcept : source_{source} {}
    virtual ~ReferenceRepresentation();

    ReferenceAddress source() const noexcept { return source_; }
    ReferenceAddress reference() noexcept { return &reference_; }
    virtual ReferenceTarget target() const = 0;
    virtual void source_changed(OutputNotificationContext &context, OutputChange change) = 0;
    virtual void destroy(std::pmr::memory_resource &memory) noexcept = 0;

  private:
    friend class ReferenceProvider;

    void bind(std::uint32_t index) noexcept { reference_.bind_representation(source_, index); }

    ReferenceAddress source_;
    ReferenceRoute reference_;
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
                             InputNotificationFunction callback = nullptr);
    void add_root_observer(RuntimeGraph &graph, void *target, DynamicObserverFunction function);
    void remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void remove_root_observer(RuntimeGraph &graph, const void *target) noexcept;
    void notify_root(OutputNotificationContext &context, OutputChange change);
    void add_subscriber(ReferenceRoute &route, RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data,
                        std::uint32_t node_index, bool active, bool required,
                        InputNotificationFunction callback = nullptr);
    void add_observer(ReferenceRoute &route, RuntimeGraph &graph, void *target, DynamicObserverFunction function);
    void remove_subscriber(ReferenceRoute &route, RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void remove_observer(ReferenceRoute &route, RuntimeGraph &graph, const void *target) noexcept;
    void notify(ReferenceRoute &route, OutputNotificationContext &context, OutputChange change);
    [[nodiscard]] ReferenceAddress representation(ReferenceAddress source, const TimeSeriesType &requested_type);
    void add_dictionary_key_subscriber(ReferenceAddress source, const ScalarData &key, RuntimeGraph &graph,
                                       std::uint32_t input_offset, std::uint32_t node_index);
    void remove_dictionary_key_subscriber(ReferenceAddress source, const ScalarData &key, RuntimeGraph &graph,
                                          std::uint32_t input_offset) noexcept;
    void source_changed(OutputNotificationContext &context, OutputChange change);

  private:
    std::pmr::memory_resource *memory_;
    ReferenceRoot *root_;
    std::pmr::vector<ReferenceRepresentation *> representations_;
    std::pmr::vector<DynamicSubscribers *> subscriber_groups_;
    DictionaryObservers *dictionary_observers_{};
};

[[nodiscard]] ReferenceAddress reference_representation(ReferenceAddress source, const TimeSeriesType &requested_type);
void add_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::uint32_t input_offset,
                              std::size_t data, std::uint32_t node_index, bool active, bool required,
                              InputNotificationFunction callback = nullptr);
void add_reference_observer(ReferenceAddress address, RuntimeGraph &graph, void *target,
                            DynamicObserverFunction function);
void remove_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::size_t input_offset) noexcept;
void remove_reference_observer(ReferenceAddress address, RuntimeGraph &graph, const void *target) noexcept;
void notify_reference(ReferenceAddress address, OutputNotificationContext &context, OutputChange change);
void add_reference_dictionary_key_subscriber(ReferenceAddress address, const ScalarData &key, RuntimeGraph &graph,
                                             std::uint32_t input_offset, std::uint32_t node_index);
void remove_reference_dictionary_key_subscriber(ReferenceAddress address, const ScalarData &key, RuntimeGraph &graph,
                                                std::uint32_t input_offset) noexcept;
void notify_reference_source(ReferenceRoot &root, OutputNotificationContext &context, OutputChange change);
void destroy_reference_root(ReferenceRoot &root, std::pmr::memory_resource &memory) noexcept;

} // namespace details
} // namespace bg
