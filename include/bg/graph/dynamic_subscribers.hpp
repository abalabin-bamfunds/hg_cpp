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

struct DynamicSubscriber {
    DynamicSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset, std::uint32_t node_index, bool active,
                      bool required) noexcept
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

struct DynamicCallbackSubscriber {
    DynamicCallbackSubscriber(RuntimeGraph &target_graph, std::uint32_t input_offset, std::size_t data,
                              std::uint32_t node_index, InputChangeFunction callback) noexcept
        : graph{&target_graph}, callback{callback}, data{data}, input_offset{input_offset}, node_index{node_index} {}

    RuntimeGraph *graph;
    InputChangeFunction callback;
    std::size_t data;
    std::uint32_t input_offset;
    std::uint32_t node_index;
};

struct DynamicObserver {
    DynamicObserver(RuntimeGraph &receiving_graph, void *receiver, DynamicObserverFunction function) noexcept
        : graph{&receiving_graph}, receiver{receiver}, function{function} {}

    RuntimeGraph *graph;
    void *receiver;
    DynamicObserverFunction function;
};

struct DynamicSubscribers {
    explicit DynamicSubscribers(std::pmr::memory_resource &memory) : records_{&memory} {}

    [[nodiscard]] static DynamicSubscribers &ensure(DynamicSubscribers *&subscribers,
                                                    std::pmr::memory_resource &memory) {
        if (subscribers != nullptr) {
            return *subscribers;
        }

        subscribers = allocator{memory}.new_object<DynamicSubscribers>(memory);
        return *subscribers;
    }

    static void release(DynamicSubscribers *subscribers) noexcept {
        if (subscribers == nullptr) {
            return;
        }

        auto *memory = subscribers->records_.get_allocator().resource();
        auto allocation = allocator{*memory};
        if (subscribers->callback_records_ != nullptr) {
            allocation.delete_object(subscribers->callback_records_);
        }
        if (subscribers->observer_records_ != nullptr) {
            allocation.delete_object(subscribers->observer_records_);
        }
        allocation.delete_object(subscribers);
    }

    void add(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data, std::uint32_t node_index, bool active,
             bool required, InputChangeFunction callback = nullptr) {
        if (active || required) {
            records_.emplace_back(graph, input_offset, node_index, active, required);
        }
        if (callback != nullptr) {
            callback_records().records.emplace_back(graph, input_offset, data, node_index, callback);
        }
    }

    void remove(RuntimeGraph &graph, std::size_t input_offset) noexcept {
        const auto record = std::ranges::find_if(records_, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.input_offset == input_offset;
        });
        if (record != records_.end()) {
            *record = records_.back();
            records_.pop_back();
        }

        if (callback_records_ == nullptr) {
            return;
        }
        const auto callback_record = std::ranges::find_if(callback_records_->records, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.input_offset == input_offset;
        });
        if (callback_record != callback_records_->records.end()) {
            *callback_record = callback_records_->records.back();
            callback_records_->records.pop_back();
        }
    }

    void add_observer(RuntimeGraph &graph, void *receiver, DynamicObserverFunction function) {
        observer_records().records.emplace_back(graph, receiver, function);
    }

    void remove_observer(RuntimeGraph &graph, const void *receiver) noexcept {
        if (observer_records_ == nullptr) {
            return;
        }

        const auto record = std::ranges::find_if(observer_records_->records, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.receiver == receiver;
        });
        if (record != observer_records_->records.end()) {
            *record = observer_records_->records.back();
            observer_records_->records.pop_back();
        }
    }

    template <typename Function> void for_each(Function &&function) const {
        for (const auto &record : records_) {
            function(record);
        }
    }

    template <typename Function> void for_each_callback(Function &&function) const {
        if (callback_records_ == nullptr) {
            return;
        }
        for (const auto &record : callback_records_->records) {
            function(record);
        }
    }

    template <typename Function> void for_each_observer(Function &&function) const {
        if (observer_records_ == nullptr) {
            return;
        }
        for (const auto &record : observer_records_->records) {
            function(record);
        }
    }

    bool empty() const noexcept {
        return records_.empty() && (callback_records_ == nullptr || callback_records_->records.empty()) &&
               (observer_records_ == nullptr || observer_records_->records.empty());
    }

  private:
    struct CallbackRecords {
        explicit CallbackRecords(std::pmr::memory_resource &memory) : records{&memory} {}
        std::pmr::vector<DynamicCallbackSubscriber> records;
    };

    struct ObserverRecords {
        explicit ObserverRecords(std::pmr::memory_resource &memory) : records{&memory} {}
        std::pmr::vector<DynamicObserver> records;
    };

    CallbackRecords &callback_records() {
        if (callback_records_ == nullptr) {
            auto *memory = records_.get_allocator().resource();
            callback_records_ = allocator{*memory}.new_object<CallbackRecords>(*memory);
        }
        return *callback_records_;
    }

    ObserverRecords &observer_records() {
        if (observer_records_ == nullptr) {
            auto *memory = records_.get_allocator().resource();
            observer_records_ = allocator{*memory}.new_object<ObserverRecords>(*memory);
        }
        return *observer_records_;
    }

    std::pmr::vector<DynamicSubscriber> records_;
    CallbackRecords *callback_records_{};
    ObserverRecords *observer_records_{};
};

static_assert(sizeof(DynamicSubscriber) == 2 * sizeof(void *));
static_assert(sizeof(DynamicCallbackSubscriber) == 4 * sizeof(void *));
static_assert(sizeof(DynamicObserver) == 3 * sizeof(void *));

} // namespace details
} // namespace bg
