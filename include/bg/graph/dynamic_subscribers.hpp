#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/util/adaptive_set.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

namespace bg {

class RuntimeGraph;

namespace details {

struct DynamicNodeSubscriber {
    DynamicNodeSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset, std::uint32_t node_index, bool active,
                          bool required) noexcept
        : graph{&target_graph}, input_offset{input_offset},
          node_and_flags{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                         (active && !required ? active_optional_flag : 0)} {}

    std::size_t node_index() const noexcept { return node_and_flags & node_index_mask; }
    bool active() const noexcept { return (node_and_flags & active_flag) != 0; }
    bool required() const noexcept { return (node_and_flags & required_flag) != 0; }
    bool active_optional() const noexcept { return (node_and_flags & active_optional_flag) != 0; }

    struct Identity {
        RuntimeGraph *graph;
        std::uint32_t input_offset;
    };

    Identity identity() const noexcept { return {graph, input_offset}; }

    RuntimeGraph *graph;
    std::uint32_t input_offset;
    std::uint32_t node_and_flags;

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t active_optional_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t node_index_mask = active_optional_flag - 1;
};

struct DynamicNodeSubscriberHash {
    std::size_t operator()(const DynamicNodeSubscriber &subscriber) const noexcept {
        return operator()(subscriber.identity());
    }

    std::size_t operator()(DynamicNodeSubscriber::Identity subscriber) const noexcept {
        // Mix aligned graph addresses because linear_hash_table selects buckets from the low hash bits.
        auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(subscriber.graph)) ^
                     (static_cast<std::uint64_t>(subscriber.input_offset) << 32);
        value ^= value >> 30;
        value *= std::uint64_t{0xbf58476d1ce4e5b9};
        value ^= value >> 27;
        value *= std::uint64_t{0x94d049bb133111eb};
        return static_cast<std::size_t>(value ^ (value >> 31));
    }
};

struct DynamicNodeSubscriberEqual {
    bool operator()(const DynamicNodeSubscriber &left, const DynamicNodeSubscriber &right) const noexcept {
        return operator()(left, right.identity());
    }

    bool operator()(const DynamicNodeSubscriber &left, DynamicNodeSubscriber::Identity right) const noexcept {
        return left.graph == right.graph && left.input_offset == right.input_offset;
    }
};

struct DynamicInputSubscriber {
    DynamicInputSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset, std::size_t data,
                           std::uint32_t node_index, InputNotificationFunction function) noexcept
        : graph{&target_graph}, function{function}, data{data}, input_offset{input_offset}, node_index{node_index} {}

    RuntimeGraph *graph;
    InputNotificationFunction function;
    std::size_t data;
    std::uint32_t input_offset;
    std::uint32_t node_index;
};

struct RuntimeObserver {
    RuntimeObserver(RuntimeGraph &receiving_graph, void *receiver, RuntimeObserverFunction function) noexcept
        : graph{&receiving_graph}, receiver{receiver}, function{function} {}

    RuntimeGraph *graph;
    void *receiver;
    RuntimeObserverFunction function;
};

struct DynamicSubscribers {
    explicit DynamicSubscribers(allocator memory) : memory_{memory} {}

    [[nodiscard]] static DynamicSubscribers &ensure(DynamicSubscribers *&subscribers, allocator memory) {
        if (subscribers != nullptr) {
            return *subscribers;
        }

        subscribers = memory.new_object<DynamicSubscribers>(memory);
        return *subscribers;
    }

    static void release(DynamicSubscribers *subscribers) noexcept {
        if (subscribers == nullptr) {
            return;
        }

        auto allocation = subscribers->memory_;
        if (subscribers->input_subscribers_ != nullptr) {
            allocation.delete_object(subscribers->input_subscribers_);
        }
        if (subscribers->observers_ != nullptr) {
            allocation.delete_object(subscribers->observers_);
        }
        subscribers->node_subscribers_.release(allocation);
        allocation.delete_object(subscribers);
    }

    void add(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data, std::uint32_t node_index, bool active,
             bool required, InputNotificationFunction input_function = nullptr) {
        if (active || required) {
            node_subscribers_.insert(DynamicNodeSubscriber{graph, input_offset, node_index, active, required}, memory_);
        }
        if (input_function != nullptr) {
            input_subscribers().records.emplace_back(graph, input_offset, data, node_index, input_function);
        }
    }

    void remove(RuntimeGraph &graph, std::size_t input_offset) noexcept {
        node_subscribers_.erase(DynamicNodeSubscriber::Identity{&graph, static_cast<std::uint32_t>(input_offset)},
                                memory_);

        if (input_subscribers_ == nullptr) {
            return;
        }
        const auto input_subscriber = std::ranges::find_if(input_subscribers_->records, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.input_offset == input_offset;
        });
        if (input_subscriber != input_subscribers_->records.end()) {
            *input_subscriber = input_subscribers_->records.back();
            input_subscribers_->records.pop_back();
        }
    }

    void add_observer(RuntimeGraph &graph, void *receiver, RuntimeObserverFunction function) {
        observers().records.emplace_back(graph, receiver, function);
    }

    void remove_observer(RuntimeGraph &graph, const void *receiver) noexcept {
        if (observers_ == nullptr) {
            return;
        }

        const auto record = std::ranges::find_if(observers_->records, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.receiver == receiver;
        });
        if (record != observers_->records.end()) {
            *record = observers_->records.back();
            observers_->records.pop_back();
        }
    }

    template <typename Function> void for_each_node_subscriber(Function &&function) const {
        for (const auto &record : node_subscribers_) {
            function(record);
        }
    }

    template <typename Function> void for_each_input_subscriber(Function &&function) const {
        if (input_subscribers_ == nullptr) {
            return;
        }
        for (const auto &record : input_subscribers_->records) {
            function(record);
        }
    }

    template <typename Function> void for_each_observer(Function &&function) const {
        if (observers_ == nullptr) {
            return;
        }
        for (const auto &record : observers_->records) {
            function(record);
        }
    }

    bool empty() const noexcept {
        return node_subscribers_.empty() && (input_subscribers_ == nullptr || input_subscribers_->records.empty()) &&
               (observers_ == nullptr || observers_->records.empty());
    }

  private:
    struct InputSubscribers {
        explicit InputSubscribers(allocator memory) : records{&memory.memory_resource()} {}
        std::pmr::vector<DynamicInputSubscriber> records;
    };

    struct Observers {
        explicit Observers(allocator memory) : records{&memory.memory_resource()} {}
        std::pmr::vector<RuntimeObserver> records;
    };

    InputSubscribers &input_subscribers() {
        if (input_subscribers_ == nullptr) {
            input_subscribers_ = memory_.new_object<InputSubscribers>(memory_);
        }
        return *input_subscribers_;
    }

    Observers &observers() {
        if (observers_ == nullptr) {
            observers_ = memory_.new_object<Observers>(memory_);
        }
        return *observers_;
    }

    static constexpr auto node_subscriber_index_threshold = std::uint32_t{32};

    allocator memory_;
    util::adaptive_set<DynamicNodeSubscriber, node_subscriber_index_threshold, DynamicNodeSubscriberHash,
                       DynamicNodeSubscriberEqual>
        node_subscribers_;
    InputSubscribers *input_subscribers_{};
    Observers *observers_{};
};

static_assert(sizeof(DynamicNodeSubscriber) == 2 * sizeof(void *));
static_assert(sizeof(DynamicInputSubscriber) == 4 * sizeof(void *));
static_assert(sizeof(RuntimeObserver) == 3 * sizeof(void *));

} // namespace details
} // namespace bg
