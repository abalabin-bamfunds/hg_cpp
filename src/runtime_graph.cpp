#include <bg/graph/runtime_graph.hpp>

#include <bg/graph/compiled_node.hpp>
#include <bg/scalar/details/allocation_context.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace bg {

class RuntimeGraph::FutureSchedule {
    struct Record {
        EvaluationTime time;
        std::uint64_t id;
        std::uint32_t node_index;
    };

    struct Later {
        [[nodiscard]] bool operator()(const Record &left, const Record &right) const noexcept {
            return left.time > right.time;
        }
    };

  public:
    explicit FutureSchedule(std::pmr::memory_resource &memory) : records_{&memory} {}

    [[nodiscard]] std::uint64_t add(EvaluationTime time, std::size_t node_index) {
        const auto id = next_id_++;
        records_.push_back(Record{time, id, static_cast<std::uint32_t>(node_index)});
        std::ranges::push_heap(records_, Later{});

        return id;
    }

    void cancel(std::size_t node_index, std::uint64_t id) noexcept {
        const auto record = std::ranges::find_if(
            records_, [=](const auto &entry) { return entry.id == id && entry.node_index == node_index; });
        if (record != records_.end()) {
            records_.erase(record);
            std::ranges::make_heap(records_, Later{});
        }
    }

    void activate(RuntimeGraph &graph, EvaluationTime time) noexcept {
        while (!records_.empty() && records_.front().time <= time) {
            std::ranges::pop_heap(records_, Later{});
            graph.schedule(records_.back().node_index);
            records_.pop_back();
        }
    }

    [[nodiscard]] std::optional<EvaluationTime> next_time() const noexcept {
        if (records_.empty()) {
            return std::nullopt;
        }

        return records_.front().time;
    }

    void clear() noexcept { records_.clear(); }

  private:
    std::pmr::vector<Record> records_;
    std::uint64_t next_id_{1};
};

class RuntimeGraph::ScheduledNodes {
    // Allows iteration over scheduled nodes taking into account evaluation schedule and validity check.
    // Returns index and whether evaluation and/or validity check is requested for that node.
    // Allows for changes in the schedule during iteration

  public:
    struct ScheduledNode {
        std::size_t node_index;
        bool evaluation_requested;
        bool validity_check_requested;
    };

    class Iterator {
      public:
        explicit Iterator(ScheduledNodes &nodes) noexcept : nodes_(&nodes) { advance(); }

        [[nodiscard]] auto operator*() const noexcept -> ScheduledNode { return current_; }

        auto operator++() noexcept -> Iterator & {
            advance();
            return *this;
        }

        [[nodiscard]] auto operator==(std::default_sentinel_t) const noexcept -> bool {
            return word_index_ == nodes_->word_count();
        }

      private:
        void advance() noexcept {
            while (word_index_ < nodes_->word_count()) {
                const auto evaluation = nodes_->evaluation(word_index_);
                const auto validity_check = nodes_->validity_check(word_index_);
                const auto pending = (evaluation | validity_check) & ~processed_;

                if (pending == 0) {
                    nodes_->clear(word_index_);
                    ++word_index_;
                    processed_ = ScheduleWord{};
                    continue;
                }

                const auto bit = static_cast<std::size_t>(std::countr_zero(pending));
                const auto mask = ScheduleWord{1} << bit;

                current_ = {
                    .node_index = word_index_ * CompiledGraph::schedule_word_bits + bit,
                    .evaluation_requested = (evaluation & mask) != 0,
                    .validity_check_requested = (validity_check & mask) != 0,
                };
                processed_ |= mask;
                return;
            }
        }

        ScheduledNodes *nodes_;
        std::size_t word_index_{};
        ScheduleWord processed_{};
        ScheduledNode current_{};
    };

    explicit ScheduledNodes(std::span<SchedulingInfo> scheduling) noexcept : scheduling_(scheduling) {}

    [[nodiscard]] auto begin() noexcept -> Iterator { return Iterator{*this}; }
    [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t { return {}; }

  private:
    [[nodiscard]] std::size_t word_count() const noexcept { return scheduling_.size(); }

    [[nodiscard]] ScheduleWord evaluation(std::size_t index) const noexcept { return scheduling_[index].evaluation; }

    [[nodiscard]] ScheduleWord validity_check(std::size_t index) const noexcept {
        return scheduling_[index].validity_check;
    }

    void clear(std::size_t index) noexcept {
        scheduling_[index].evaluation = ScheduleWord{};
        scheduling_[index].validity_check = ScheduleWord{};
    }

    std::span<SchedulingInfo> scheduling_;
};

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph)
    : RuntimeGraph(compiled_graph, *std::pmr::get_default_resource()) {}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory)
    : compiled_graph_(&compiled_graph), memory_(&memory), owned_global_state_(std::make_unique<GlobalState>()),
      global_state_(owned_global_state_.get()) {
    initialize_storage();
}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state)
    : RuntimeGraph(compiled_graph, *std::pmr::get_default_resource(), global_state) {}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory,
                           GlobalState &global_state)
    : compiled_graph_(&compiled_graph), memory_(&memory), global_state_(&global_state) {
    initialize_storage();
}

void RuntimeGraph::initialize_storage() {
    if (compiled_graph_->runtime_storage_size() == 0) {
        return;
    }

    storage_ = static_cast<std::byte *>(
        memory_->allocate(compiled_graph_->runtime_storage_size(), compiled_graph_->runtime_storage_alignment()));
    std::memcpy(storage_, compiled_graph_->initial_runtime_storage(), compiled_graph_->runtime_storage_size());
}

RuntimeGraph::~RuntimeGraph() {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr) {
        std::destroy_at(future_schedule());
        memory_->deallocate(future_schedule(), sizeof(FutureSchedule), alignof(FutureSchedule));
    }

    if (storage_ != nullptr) {
        memory_->deallocate(storage_, compiled_graph_->runtime_storage_size(),
                            compiled_graph_->runtime_storage_alignment());
    }
}

auto RuntimeGraph::scheduling_info() noexcept -> std::span<SchedulingInfo> {
    return {reinterpret_cast<SchedulingInfo *>(storage_), compiled_graph_->schedule_word_count()};
}

void RuntimeGraph::schedule(std::size_t node_index) noexcept {
    const auto word_index = node_index / CompiledGraph::schedule_word_bits;
    auto scheduling = scheduling_info();
    auto &word = scheduling[word_index].evaluation;
    word |= ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);
}

void RuntimeGraph::schedule_validity_check(std::size_t node_index) noexcept {
    auto scheduling = scheduling_info();
    auto &word = scheduling[node_index / CompiledGraph::schedule_word_bits].validity_check;
    word |= ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);
}

ScheduleHandle RuntimeGraph::schedule_at(std::size_t node_index, EvaluationTime time) {
    if (time <= evaluation_time_) {
        throw std::invalid_argument{"a scheduled evaluation time must be in the future"};
    }

    if (!compiled_graph_->has_future_scheduling()) {
        throw std::logic_error{"the graph does not declare future scheduling"};
    }

    if (future_schedule() == nullptr) {
        auto *storage = memory_->allocate(sizeof(FutureSchedule), alignof(FutureSchedule));
        try {
            future_schedule() = std::construct_at(static_cast<FutureSchedule *>(storage), *memory_);
        } catch (...) {
            memory_->deallocate(storage, sizeof(FutureSchedule), alignof(FutureSchedule));
            throw;
        }
    }

    return ScheduleHandle{future_schedule()->add(time, node_index)};
}

void RuntimeGraph::cancel(std::size_t node_index, ScheduleHandle handle) noexcept {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr && handle.id_ != 0) {
        future_schedule()->cancel(node_index, handle.id_);
    }
}

RuntimeGraph::FutureSchedule *&RuntimeGraph::future_schedule() noexcept {
    return *reinterpret_cast<FutureSchedule **>(storage_ + compiled_graph_->future_schedule_offset());
}

RuntimeGraph::FutureSchedule *RuntimeGraph::future_schedule() const noexcept {
    return *reinterpret_cast<FutureSchedule *const *>(storage_ + compiled_graph_->future_schedule_offset());
}

void RuntimeGraph::activate_scheduled(EvaluationTime evaluation_time) noexcept {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr) {
        future_schedule()->activate(*this, evaluation_time);
    }
}

void RuntimeGraph::clear_future_schedule() noexcept {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr) {
        future_schedule()->clear();
    }
}

std::optional<EvaluationTime> RuntimeGraph::next_scheduled_time() const noexcept {
    if (!compiled_graph_->has_future_scheduling() || future_schedule() == nullptr) {
        return std::nullopt;
    }

    return future_schedule()->next_time();
}

void RuntimeGraph::schedule_subscribers(const CompiledNode &node, OutputChange change) noexcept {
    for (const auto &subscriber : node.subscribers()) {
        if ((change != OutputChange::invalidation && subscriber.active()) ||
            (change == OutputChange::invalidation && subscriber.active_optional())) {
            schedule(subscriber.node_index());
        }

        if (change == OutputChange::invalidation && subscriber.required()) {
            schedule_validity_check(subscriber.node_index());
        }
    }
}

void RuntimeGraph::schedule_output_change(const CompiledNode &node, bool now_valid) noexcept {
    schedule_subscribers(node, now_valid ? OutputChange::update : OutputChange::invalidation);
}

void RuntimeGraph::apply_invalidation(const CompiledNode &node, std::size_t node_index) {
    switch (node.invalidation_mode()) {
    case InvalidationMode::invalidate_output:
        if (node.has_output()) {
            auto &last_updated = node.output_last_updated(storage_);
            if (last_updated > MIN_DT) {
                last_updated = MIN_DT;
                schedule_output_change(node, false);
            }
        }
        return;

    case InvalidationMode::preserve_output:
        return;

    case InvalidationMode::invoke:
        break;
    }

    const auto output_was_valid = node.has_output() && node.output_last_updated(storage_) > MIN_DT;
    EvaluationContext context{evaluation_time_, *this, node_index};
    const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
    node.invalidation_function()(node, storage_, &context);

    if (!node.has_output()) {
        return;
    }

    const auto &last_updated = node.output_last_updated(storage_);
    const auto now_valid = last_updated > MIN_DT;
    if (last_updated == evaluation_time_ || output_was_valid != now_valid) {
        schedule_output_change(node, now_valid);
    }
}

void RuntimeGraph::start(EvaluationTime evaluation_time) {
    evaluation_time_ = evaluation_time;
    const auto starts = compiled_graph_->start_functions();
    auto started_node_count = std::size_t{};
    auto function = starts.functions.begin();

    try {
        for (const auto node_index : starts.nodes) {
            started_node_count = node_index;
            const auto &node = compiled_graph_->node(node_index);
            const auto output_was_valid = node.has_output() && node.output_last_updated(storage_) > MIN_DT;

            EvaluationContext context{evaluation_time, *this, node_index};
            const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
            (*function)(node, storage_, &context);
            ++function;
            started_node_count = node_index + 1;

            if (node.has_output()) {
                const auto &last_updated = node.output_last_updated(storage_);
                const auto now_valid = last_updated > MIN_DT;
                if (last_updated == evaluation_time || output_was_valid != now_valid) {
                    schedule_output_change(node, now_valid);
                }
            }
        }
    } catch (...) {
        stop_nodes(started_node_count);
        throw;
    }
}

void RuntimeGraph::stop_nodes(std::size_t node_count) noexcept {
    const auto stops = compiled_graph_->stop_functions();
    auto i = static_cast<std::size_t>(std::ranges::lower_bound(stops.nodes, node_count) - stops.nodes.begin());

    while (i != 0) {
        --i;
        const auto node_index = stops.nodes[i];

        const auto &node = compiled_graph_->node(node_index);
        EvaluationContext context{evaluation_time_, *this, node_index};
        const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
        stops.functions[i](node, storage_, &context);
    }

    clear_future_schedule();
}

void RuntimeGraph::stop() noexcept {
    stop_nodes(compiled_graph_->node_count());
}

bool RuntimeGraph::inputs_are_valid(const CompiledNode &node) const noexcept {
    for (const auto &parameter : node.parameters()) {
        if (parameter.mode() != ParameterBindingMode::time_series || parameter.validity() != required) {
            continue;
        }

        if (parameter.last_updated(storage_) <= MIN_DT) {
            return false;
        }
    }

    return true;
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time) {
    if (evaluation_time <= last_evaluation_time_) {
        throw std::invalid_argument{"evaluation time must increase between outer cycles"};
    }

    last_evaluation_time_ = evaluation_time;
    evaluation_time_ = evaluation_time;
    activate_scheduled(evaluation_time);

    auto scheduling = scheduling_info();
    auto scheduled = ScheduledNodes{scheduling};

    for (const auto [node_index, evaluation_requested, validity_check_requested] : scheduled) {
        const auto word_index = node_index / CompiledGraph::schedule_word_bits;
        const auto mask = ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);
        const auto &node = compiled_graph_->node(node_index);
        const auto has_output = node.has_output();
        const auto inputs_were_valid = scheduling[word_index].inputs_valid(mask);
        auto inputs_valid = inputs_were_valid;

        if (validity_check_requested || (evaluation_requested && !inputs_valid)) {
            inputs_valid = inputs_are_valid(node);
            scheduling[word_index].inputs_valid(mask, inputs_valid);
        }

        if (inputs_were_valid && !inputs_valid) {
            try {
                apply_invalidation(node, node_index);
            } catch (...) {
                stop_nodes(compiled_graph_->node_count());
                throw;
            }
        }

        if (!inputs_valid) {
            continue;
        }

        if (!evaluation_requested) {
            continue;
        }

        const auto output_was_valid = has_output && node.output_last_updated(storage_) > MIN_DT;
        EvaluationContext context{evaluation_time, *this, node_index};
        const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};

        try {
            node.evaluation_function()(node, storage_, &context);
        } catch (...) {
            stop_nodes(compiled_graph_->node_count());
            throw;
        }

        if (has_output) {
            const auto &last_updated = node.output_last_updated(storage_);
            const auto now_valid = last_updated > MIN_DT;
            if (last_updated == evaluation_time || output_was_valid != now_valid) {
                schedule_output_change(node, now_valid);
            }
        }
    }
}

void EvaluationContext::schedule_now() const noexcept {
    runtime_graph_->schedule(node_index_);
}

ScheduleHandle EvaluationContext::schedule_at(EvaluationTime time) const {
    return runtime_graph_->schedule_at(node_index_, time);
}

void EvaluationContext::cancel(ScheduleHandle handle) const noexcept {
    runtime_graph_->cancel(node_index_, handle);
}

GlobalState &EvaluationContext::global_state() const noexcept {
    return runtime_graph_->global_state();
}

} // namespace bg
