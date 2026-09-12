#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/output_notification.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

namespace bg {

class RuntimeGraph;

namespace details {

struct DynamicNodeSubscriber {
    DynamicNodeSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset, std::uint32_t node_index,
                          bool active, bool required) noexcept
        : graph{&target_graph}, input_offset{input_offset},
          node_and_flags{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                         (active && !required ? active_optional_flag : 0)} {}

    std::size_t node_index() const noexcept { return node_and_flags & node_index_mask; }
    bool active() const noexcept { return (node_and_flags & active_flag) != 0; }
    bool required() const noexcept { return (node_and_flags & required_flag) != 0; }
    bool active_optional() const noexcept { return (node_and_flags & active_optional_flag) != 0; }

    RuntimeGraph *graph;
    std::uint32_t input_offset;
    std::uint32_t node_and_flags;

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t active_optional_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t node_index_mask = active_optional_flag - 1;
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
    explicit DynamicSubscribers(allocator memory) : node_subscribers_{&memory.memory_resource()} {}

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

        auto *memory = subscribers->node_subscribers_.get_allocator().resource();
        auto allocation = allocator{*memory};
        if (subscribers->input_subscribers_ != nullptr) {
            allocation.delete_object(subscribers->input_subscribers_);
        }
        if (subscribers->observers_ != nullptr) {
            allocation.delete_object(subscribers->observers_);
        }
        allocation.delete_object(subscribers);
    }

    void add(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data, std::uint32_t node_index, bool active,
             bool required, InputNotificationFunction input_function = nullptr) {
        if (active || required) {
            node_subscribers_.emplace_back(graph, input_offset, node_index, active, required);
        }
        if (input_function != nullptr) {
            input_subscribers().records.emplace_back(graph, input_offset, data, node_index, input_function);
        }
    }

    void remove(RuntimeGraph &graph, std::size_t input_offset) noexcept {
        const auto record = std::ranges::find_if(node_subscribers_, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.input_offset == input_offset;
        });
        if (record != node_subscribers_.end()) {
            *record = node_subscribers_.back();
            node_subscribers_.pop_back();
        }

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
            auto *memory = node_subscribers_.get_allocator().resource();
            input_subscribers_ = allocator{*memory}.new_object<InputSubscribers>(allocator{*memory});
        }
        return *input_subscribers_;
    }

    Observers &observers() {
        if (observers_ == nullptr) {
            auto *memory = node_subscribers_.get_allocator().resource();
            observers_ = allocator{*memory}.new_object<Observers>(allocator{*memory});
        }
        return *observers_;
    }

    std::pmr::vector<DynamicNodeSubscriber> node_subscribers_;
    InputSubscribers *input_subscribers_{};
    Observers *observers_{};
};

static_assert(sizeof(DynamicNodeSubscriber) == 2 * sizeof(void *));
static_assert(sizeof(DynamicInputSubscriber) == 4 * sizeof(void *));
static_assert(sizeof(RuntimeObserver) == 3 * sizeof(void *));

} // namespace details
} // namespace bg
