#include <bg/evaluation_profiler.hpp>

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/realtime_evaluation_engine.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg {

struct EvaluationProfiler::State {
    struct OuterHistory {
        struct Bucket {
            std::optional<std::int64_t> period;
            bool closed{};
            EvaluationProfilerCycleStats timing;
        };

        OuterHistory(std::chrono::microseconds duration, std::size_t retention)
            : duration{duration} {
            if (duration <= std::chrono::microseconds::zero() || retention == 0 ||
                static_cast<std::uint64_t>(retention) >=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::invalid_argument{"evaluation profiler requires a positive outer period and retention count"};
            }
            buckets.resize(retention + 1);
        }

        std::int64_t period_of(EvaluationTime time) const {
            const auto elapsed = time.time_since_epoch().count();
            const auto width = duration.count();
            const auto quotient = elapsed / width;
            return quotient - (elapsed % width < 0);
        }

        std::size_t bucket_index(std::int64_t period) const {
            const auto count = static_cast<std::int64_t>(buckets.size());
            const auto remainder = period % count;
            return static_cast<std::size_t>(remainder < 0 ? remainder + count : remainder);
        }

        void advance_to(EvaluationTime time) {
            const auto period = period_of(time);
            if (!current_period) {
                current_period = period;
                buckets[bucket_index(period)].period = period;
                return;
            }
            if (period < *current_period) {
                throw std::runtime_error{"evaluation profile wall clock moved backwards"};
            }
            if (period == *current_period) {
                return;
            }

            buckets[bucket_index(*current_period)].closed = true;
            if (static_cast<std::uint64_t>(period) - static_cast<std::uint64_t>(*current_period) >= buckets.size()) {
                std::fill(buckets.begin(), buckets.end(), Bucket{});
            } else {
                auto next = *current_period;
                while (next < period) {
                    ++next;
                    buckets[bucket_index(next)] = {};
                }
            }
            current_period = period;
            buckets[bucket_index(period)].period = period;
        }

        void record(std::uint64_t nanoseconds) {
            auto &timing = buckets[bucket_index(*current_period)].timing;
            if (timing.cycles == 0) {
                timing.min_nanoseconds = nanoseconds;
            } else {
                timing.min_nanoseconds = std::min(timing.min_nanoseconds, nanoseconds);
            }
            timing.max_nanoseconds = std::max(timing.max_nanoseconds, nanoseconds);
            timing.total_nanoseconds += nanoseconds;
            ++timing.cycles;

            if (totals.cycles == 0) {
                totals.min_nanoseconds = nanoseconds;
            } else {
                totals.min_nanoseconds = std::min(totals.min_nanoseconds, nanoseconds);
            }
            totals.max_nanoseconds = std::max(totals.max_nanoseconds, nanoseconds);
            totals.total_nanoseconds += nanoseconds;
            ++totals.cycles;
        }

        void seal() {
            if (current_period) {
                buckets[bucket_index(*current_period)].closed = true;
            }
        }

        const Bucket *period_bucket(EvaluationTime start) const {
            const auto period = period_of(start);
            const auto &bucket = buckets[bucket_index(period)];
            return bucket.closed && bucket.period == period && bucket.timing.cycles != 0 ? &bucket : nullptr;
        }

        std::vector<EvaluationProfilerCyclePeriod> periods() const {
            auto result = std::vector<EvaluationProfilerCyclePeriod>{};
            for (const auto &bucket : buckets) {
                if (!bucket.closed || !bucket.period || bucket.timing.cycles == 0) {
                    continue;
                }
                result.push_back({EvaluationTime{std::chrono::microseconds{*bucket.period * duration.count()}},
                                  bucket.timing});
            }
            std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
                return left.start < right.start;
            });
            return result;
        }

        std::chrono::microseconds duration;
        std::optional<std::int64_t> current_period;
        EvaluationProfilerCycleStats totals;
        std::vector<Bucket> buckets;
    };

    struct NodePeriodRecord {
        std::uint32_t node_index;
        std::uint32_t evaluations;
        std::uint64_t checkpoint_ticks;
    };
    static_assert(sizeof(NodePeriodRecord) == 16);

    struct NodeBaseline {
        std::uint32_t node_index;
        std::uint64_t evaluations;
        std::uint64_t checkpoint_ticks;
    };

    struct CountOverflow {
        std::size_t record_index;
        std::uint64_t evaluations;
    };

    struct GraphPeriodRange {
        GraphInstanceId graph_id;
        std::size_t first_record;
        std::size_t record_count;
        EvaluationProfilerStats totals;
    };

    struct PeriodBucket {
        std::optional<std::int64_t> period;
        bool closed{};
        std::vector<NodePeriodRecord> nodes;
        std::vector<CountOverflow> count_overflows;
        std::vector<GraphPeriodRange> graphs;

        std::uint64_t evaluation_count(std::size_t record_index) const {
            const auto count = nodes[record_index].evaluations;
            if (count != 0) {
                return count;
            }
            const auto overflow = std::lower_bound(count_overflows.begin(), count_overflows.end(), record_index,
                                                   [](const auto &entry, auto index) {
                                                       return entry.record_index < index;
                                                   });
            if (overflow == count_overflows.end() || overflow->record_index != record_index) {
                throw std::runtime_error{"evaluation profile period count is missing its overflow"};
            }
            return overflow->evaluations;
        }
    };

    struct GraphData {
        EvaluationProfilerGraph info;
        std::vector<EvaluationProfilerStats> nodes;
        std::vector<std::uint64_t> touched_bits;
        std::vector<NodeBaseline> touched_nodes;
        EvaluationProfilerStats period_totals;
        std::int64_t stopped_period{};
        bool period_touched{};
    };

    struct Frame {
        GraphData *graph;
        std::uint64_t started_at;
        std::uint64_t previous_checkpoint;
        std::uint64_t pending_ticks{};
    };

    State(EvaluationProfilerConfig config, allocator memory)
        : memory{memory}, period_duration{config.node_period}, retained_periods{config.retained_node_periods},
          outer_history{config.outer_period, config.retained_outer_periods} {
        if (period_duration <= std::chrono::microseconds::zero() || retained_periods == 0 ||
            static_cast<std::uint64_t>(retained_periods) >=
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument{"evaluation profiler requires a positive period and retention count"};
        }
        periods.resize(retained_periods + 1);
    }

    std::int64_t period_of(EvaluationTime time) const {
        const auto elapsed = time.time_since_epoch().count();
        const auto duration = period_duration.count();
        const auto quotient = elapsed / duration;
        return quotient - (elapsed % duration < 0);
    }

    std::size_t bucket_index(std::int64_t period) const {
        const auto count = static_cast<std::int64_t>(periods.size());
        const auto remainder = period % count;
        return static_cast<std::size_t>(remainder < 0 ? remainder + count : remainder);
    }

    static std::uint64_t elapsed(std::uint64_t start, std::uint64_t end) {
        if (end < start) {
            throw std::runtime_error{"evaluation profile checkpoint moved backwards"};
        }
        return end - start;
    }

    GraphData &active_graph(GraphInstanceId graph_id) {
        const auto found = graphs.find(graph_id.value);
        if (found == graphs.end() || !found->second.info.active) {
            throw std::runtime_error{"evaluation profile refers to an inactive graph"};
        }
        return found->second;
    }

    void touch(GraphData &graph) {
        if (graph.period_touched) {
            return;
        }
        if (!current_period) {
            throw std::runtime_error{"evaluation profile has no current period"};
        }
        graph.period_touched = true;
        touched_graphs.push_back(graph.info.graph_id);
    }

    void add_graph_evaluation(GraphData &graph, std::uint64_t ticks) {
        touch(graph);
        ++graph.info.totals.evaluations;
        graph.info.totals.checkpoint_ticks += ticks;
        ++graph.period_totals.evaluations;
        graph.period_totals.checkpoint_ticks += ticks;
        graph.info.last_updated = cycle_wall_clock_time;
    }

    void flush(GraphData &graph) {
        if (!graph.period_touched) {
            return;
        }

        auto &bucket = periods[bucket_index(*current_period)];
        std::sort(graph.touched_nodes.begin(), graph.touched_nodes.end(),
                  [](const auto &left, const auto &right) { return left.node_index < right.node_index; });

        const auto first_record = bucket.nodes.size();
        for (const auto &baseline : graph.touched_nodes) {
            const auto &current = graph.nodes[baseline.node_index];
            const auto count = current.evaluations - baseline.evaluations;
            const auto record_index = bucket.nodes.size();
            bucket.nodes.push_back({baseline.node_index,
                                    count <= std::numeric_limits<std::uint32_t>::max()
                                        ? static_cast<std::uint32_t>(count)
                                        : 0,
                                    current.checkpoint_ticks - baseline.checkpoint_ticks});
            if (count > std::numeric_limits<std::uint32_t>::max()) {
                bucket.count_overflows.push_back({record_index, count});
            }
        }
        bucket.graphs.push_back({graph.info.graph_id, first_record, graph.touched_nodes.size(),
                                 graph.period_totals});

        graph.touched_nodes.clear();
        std::fill(graph.touched_bits.begin(), graph.touched_bits.end(), 0);
        graph.period_totals = {};
        graph.period_touched = false;
    }

    void close_node_period() {
        auto &closing = periods[bucket_index(*current_period)];
        for (const auto graph_id : touched_graphs) {
            flush(graphs.at(graph_id.value));
        }
        touched_graphs.clear();
        std::sort(closing.graphs.begin(), closing.graphs.end(),
                  [](const auto &left, const auto &right) { return left.graph_id.value < right.graph_id.value; });
        closing.closed = true;
    }

    void advance_to(EvaluationTime time) {
        const auto period = period_of(time);
        if (!frames.empty()) {
            throw std::runtime_error{"evaluation profile cannot change period inside a cycle"};
        }
        if (current_period && period < *current_period) {
            throw std::runtime_error{"evaluation profile wall clock moved backwards"};
        }
        outer_history.advance_to(time);
        if (!current_period) {
            current_period = period;
            periods[bucket_index(period)].period = period;
            return;
        }
        if (period == *current_period) {
            return;
        }

        close_node_period();

        if (static_cast<std::uint64_t>(period) - static_cast<std::uint64_t>(*current_period) >= periods.size()) {
            for (auto &bucket : periods) {
                bucket = {};
            }
        } else {
            auto next = *current_period;
            while (next < period) {
                ++next;
                periods[bucket_index(next)] = {};
            }
        }
        current_period = period;
        periods[bucket_index(period)].period = period;

        while (!stopped.empty() &&
               static_cast<std::uint64_t>(period) - static_cast<std::uint64_t>(stopped.front().first) >
                   retained_periods) {
            const auto [stopped_period, graph_id] = stopped.front();
            const auto found = graphs.find(graph_id.value);
            if (found != graphs.end() && !found->second.info.active &&
                found->second.stopped_period == stopped_period) {
                graphs.erase(found);
            }
            stopped.pop_front();
        }
    }

    void start_graph(EvaluationProfileGraph record) {
        if (record.graph_id.value == 0 || record.compiled_graph == nullptr) {
            throw std::runtime_error{"invalid evaluation profile graph start"};
        }
        const auto found = graphs.find(record.graph_id.value);
        if (found != graphs.end() && found->second.info.active) {
            throw std::runtime_error{"invalid evaluation profile graph start"};
        }
        if (record.parent_graph_id.value != 0) {
            (void)active_graph(record.parent_graph_id);
        } else {
            root_graph_id = record.graph_id;
        }

        auto graph = GraphData{};
        graph.info.graph_id = record.graph_id;
        graph.info.parent_graph_id = record.parent_graph_id;
        graph.info.owner_node_index = record.owner_node_index;
        graph.info.compiled_graph = record.compiled_graph;
        graph.info.node_count = record.compiled_graph->node_count();
        graph.info.active = true;
        if (record.label) {
            graph.info.label.emplace(record.label->view(), memory);
        }
        graph.nodes.resize(graph.info.node_count);

        if (found == graphs.end()) {
            graphs.emplace(record.graph_id.value, std::move(graph));
            return;
        }
        graph.info.totals = found->second.info.totals;
        graph.info.last_updated = found->second.info.last_updated;
        found->second = std::move(graph);
    }

    void stop_graph(EvaluationProfileGraphStop record) {
        auto &graph = active_graph(record.graph_id);
        flush(graph);
        graph.info.active = false;
        graph.info.compiled_graph = nullptr;
        graph.nodes = {};
        graph.touched_bits = {};
        graph.touched_nodes = {};
        if (current_period) {
            graph.stopped_period = *current_period;
            stopped.emplace_back(*current_period, record.graph_id);
        } else {
            graphs.erase(record.graph_id.value);
        }
    }

    void finish_run() {
        frames.clear();
        for (auto &entry : graphs) {
            auto &graph = entry.second;
            if (!graph.info.active) {
                continue;
            }
            if (graph.period_touched) {
                flush(graph);
            }
            graph.info.active = false;
            graph.info.compiled_graph = nullptr;
            graph.nodes = {};
            graph.touched_bits = {};
            graph.touched_nodes = {};
            if (current_period) {
                graph.stopped_period = *current_period;
                stopped.emplace_back(*current_period, graph.info.graph_id);
            }
        }
    }

    void seal() {
        if (current_period) {
            close_node_period();
        }
        outer_history.seal();
    }

    void cycle(EvaluationProfileCycle record) {
        if (record.boundary == EvaluationProfileCycle::Boundary::start) {
            if (!frames.empty()) {
                throw std::runtime_error{"evaluation profile cycle start is nested"};
            }
            advance_to(record.wall_clock_time);
            cycle_wall_clock_time = record.wall_clock_time;
            cycle_started_steady_nanoseconds = record.steady_clock_nanoseconds;
            frames.push_back({&active_graph(record.graph_id), record.checkpoint, record.checkpoint});
            return;
        }

        if (frames.size() != 1 || frames.back().graph->info.graph_id != record.graph_id) {
            throw std::runtime_error{"evaluation profile cycle end has no matching start"};
        }
        if (record.steady_clock_nanoseconds < cycle_started_steady_nanoseconds) {
            throw std::runtime_error{"evaluation profile steady clock moved backwards"};
        }
        auto &frame = frames.back();
        add_graph_evaluation(*frame.graph, elapsed(frame.started_at, record.checkpoint));
        frame.graph->info.last_updated = record.wall_clock_time;
        outer_history.record(static_cast<std::uint64_t>(record.steady_clock_nanoseconds -
                                                        cycle_started_steady_nanoseconds));
        frames.pop_back();
    }

    void enter_graph(EvaluationProfileGraphEntry record) {
        if (frames.empty()) {
            throw std::runtime_error{"evaluation profile graph entry is outside a cycle"};
        }
        auto &parent = frames.back();
        parent.pending_ticks += elapsed(parent.previous_checkpoint, record.checkpoint);
        frames.push_back({&active_graph(record.graph_id), record.checkpoint, record.checkpoint});
    }

    void exit_graph(EvaluationProfileGraphExit record) {
        if (frames.size() < 2 || frames.back().graph->info.graph_id != record.graph_id) {
            throw std::runtime_error{"evaluation profile graph exit has no matching entry"};
        }
        const auto frame = frames.back();
        add_graph_evaluation(*frame.graph, elapsed(frame.started_at, record.checkpoint));
        frames.pop_back();
        frames.back().previous_checkpoint = record.checkpoint;
    }

    void node(EvaluationProfileNode record) {
        if (frames.empty() || frames.back().graph->info.graph_id != record.graph_id) {
            throw std::runtime_error{"evaluation profile node is outside its graph evaluation"};
        }
        auto &frame = frames.back();
        auto &graph = *frame.graph;
        if (record.node_index >= graph.nodes.size()) {
            throw std::runtime_error{"evaluation profile node index is outside its compiled graph"};
        }

        touch(graph);
        if (graph.touched_bits.empty()) {
            graph.touched_bits.resize((graph.nodes.size() + 63) / 64);
        }
        const auto word = record.node_index / 64;
        const auto mask = std::uint64_t{1} << (record.node_index % 64);
        auto &totals = graph.nodes[record.node_index];
        if ((graph.touched_bits[word] & mask) == 0) {
            graph.touched_bits[word] |= mask;
            graph.touched_nodes.push_back({record.node_index, totals.evaluations, totals.checkpoint_ticks});
        }

        ++totals.evaluations;
        totals.checkpoint_ticks += elapsed(frame.previous_checkpoint, record.checkpoint) + frame.pending_ticks;
        frame.previous_checkpoint = record.checkpoint;
        frame.pending_ticks = 0;
        graph.info.last_updated = cycle_wall_clock_time;
    }

    void consume(EvaluationProfileRecord record) {
        if (auto *value = std::get_if<EvaluationProfileGraph>(&record)) {
            start_graph(std::move(*value));
        } else if (auto *value = std::get_if<EvaluationProfileGraphStop>(&record)) {
            stop_graph(*value);
        } else if (auto *value = std::get_if<EvaluationProfileCycle>(&record)) {
            cycle(*value);
        } else if (auto *value = std::get_if<EvaluationProfileGraphEntry>(&record)) {
            enter_graph(*value);
        } else if (auto *value = std::get_if<EvaluationProfileGraphExit>(&record)) {
            exit_graph(*value);
        } else {
            node(std::get<EvaluationProfileNode>(record));
        }
    }

    const PeriodBucket *period_bucket(EvaluationTime period_start) const {
        const auto period = period_of(period_start);
        const auto &bucket = periods[bucket_index(period)];
        if (!bucket.closed || bucket.period != period) {
            return nullptr;
        }
        return &bucket;
    }

    allocator memory;
    std::chrono::microseconds period_duration;
    std::size_t retained_periods;
    std::optional<std::int64_t> current_period;
    std::optional<GraphInstanceId> root_graph_id;
    EvaluationTime cycle_wall_clock_time{};
    std::int64_t cycle_started_steady_nanoseconds{};
    bool run_started{};
    std::unordered_map<std::uint64_t, GraphData> graphs;
    std::vector<Frame> frames;
    std::vector<GraphInstanceId> touched_graphs;
    std::deque<std::pair<std::int64_t, GraphInstanceId>> stopped;
    std::vector<PeriodBucket> periods;
    OuterHistory outer_history;
};

EvaluationProfiler::EvaluationProfiler(EvaluationProfilerConfig config, allocator memory)
    : state_{std::make_unique<State>(config, memory)} {}
EvaluationProfiler::~EvaluationProfiler() = default;

void EvaluationProfiler::Recorder::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                       EvaluationEngine engine, std::stop_token stop_token,
                                       const EvaluationInterruption &interruption) const {
    auto &profiler = profiler_;
    if (profiler.state_->run_started) {
        throw std::logic_error{"an evaluation profiler records one graph run"};
    }
    profiler.state_->run_started = true;

    auto profile = EvaluationProfile{};
    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto wait_mutex = std::mutex{};
    auto wait_condition = std::condition_variable{};
    auto finished = false;
    auto reader_error = std::exception_ptr{};
    auto reader_thread = std::jthread{[&] {
        try {
            auto lock = std::unique_lock{wait_mutex};
            while (!finished) {
                lock.unlock();
                profiler.poll(reader);
                lock.lock();
                wait_condition.wait_for(lock, std::chrono::seconds{1}, [&] { return finished; });
            }
            lock.unlock();
            profiler.poll(reader);
        } catch (...) {
            reader_error = std::current_exception();
        }
    }};

    auto graph_error = std::exception_ptr{};
    try {
        if (engine == EvaluationEngine::realtime) {
            RealtimeEvaluationEngine{}.run(runtime_graph, start_time, profile, stop_token, interruption);
        } else {
            SimulationEvaluationEngine{}.run(runtime_graph, start_time, profile, interruption);
        }
    } catch (...) {
        graph_error = std::current_exception();
    }

    profile.finish();
    {
        auto lock = std::lock_guard{wait_mutex};
        finished = true;
    }
    wait_condition.notify_one();
    reader_thread.join();

    profiler.state_->finish_run();
    if (!reader_error) {
        profiler.state_->seal();
    }

    if (graph_error) {
        std::rethrow_exception(graph_error);
    }
    if (reader_error) {
        std::rethrow_exception(reader_error);
    }
    if (profile.failed()) {
        throw std::runtime_error{"evaluation profile recording failed"};
    }
}

std::optional<GraphInstanceId> EvaluationProfiler::root_graph_id() const noexcept {
    return state_->root_graph_id;
}

std::size_t EvaluationProfiler::poll(EvaluationProfileReader &reader) {
    auto count = std::size_t{};
    while (auto record = reader.read()) {
        state_->consume(std::move(*record));
        ++count;
    }
    return count;
}

void EvaluationProfiler::consume(EvaluationProfileRecord record) { state_->consume(std::move(record)); }
void EvaluationProfiler::advance_to(EvaluationTime time) { state_->advance_to(time); }

const EvaluationProfilerGraph *EvaluationProfiler::graph(GraphInstanceId graph_id) const {
    const auto found = state_->graphs.find(graph_id.value);
    return found == state_->graphs.end() ? nullptr : &found->second.info;
}

std::vector<GraphInstanceId> EvaluationProfiler::graph_ids() const {
    auto result = std::vector<GraphInstanceId>{};
    result.reserve(state_->graphs.size());
    for (const auto &entry : state_->graphs) {
        result.push_back(entry.second.info.graph_id);
    }
    std::sort(result.begin(), result.end(), [](auto left, auto right) { return left.value < right.value; });
    return result;
}

std::optional<EvaluationProfilerStats> EvaluationProfiler::node_totals(GraphInstanceId graph_id,
                                                                         std::uint32_t node_index) const {
    const auto found = state_->graphs.find(graph_id.value);
    if (found == state_->graphs.end() || !found->second.info.active || node_index >= found->second.nodes.size()) {
        return std::nullopt;
    }
    return found->second.nodes[node_index];
}

std::optional<EvaluationProfilerStats> EvaluationProfiler::graph_period(GraphInstanceId graph_id,
                                                                          EvaluationTime period_start) const {
    const auto *bucket = state_->period_bucket(period_start);
    if (bucket == nullptr) {
        return std::nullopt;
    }
    auto range = std::lower_bound(bucket->graphs.begin(), bucket->graphs.end(), graph_id.value,
                                  [](const auto &entry, auto id) { return entry.graph_id.value < id; });
    if (range == bucket->graphs.end() || range->graph_id != graph_id) {
        return std::nullopt;
    }

    auto totals = EvaluationProfilerStats{};
    for (; range != bucket->graphs.end() && range->graph_id == graph_id; ++range) {
        totals.evaluations += range->totals.evaluations;
        totals.checkpoint_ticks += range->totals.checkpoint_ticks;
    }
    return totals;
}

std::optional<EvaluationProfilerStats> EvaluationProfiler::node_period(GraphInstanceId graph_id,
                                                                         std::uint32_t node_index,
                                                                         EvaluationTime period_start) const {
    const auto *bucket = state_->period_bucket(period_start);
    if (bucket == nullptr) {
        return std::nullopt;
    }
    auto range = std::lower_bound(bucket->graphs.begin(), bucket->graphs.end(), graph_id.value,
                                  [](const auto &entry, auto id) { return entry.graph_id.value < id; });
    auto totals = EvaluationProfilerStats{};
    auto found_node = false;
    for (; range != bucket->graphs.end() && range->graph_id == graph_id; ++range) {
        const auto begin = bucket->nodes.begin() + static_cast<std::ptrdiff_t>(range->first_record);
        const auto end = begin + static_cast<std::ptrdiff_t>(range->record_count);
        const auto found = std::lower_bound(begin, end, node_index,
                                            [](const auto &entry, auto index) { return entry.node_index < index; });
        if (found == end || found->node_index != node_index) {
            continue;
        }

        totals.evaluations += bucket->evaluation_count(static_cast<std::size_t>(found - bucket->nodes.begin()));
        totals.checkpoint_ticks += found->checkpoint_ticks;
        found_node = true;
    }
    return found_node ? std::optional{totals} : std::nullopt;
}

std::vector<EvaluationTime> EvaluationProfiler::node_period_starts() const {
    auto result = std::vector<EvaluationTime>{};
    for (const auto &bucket : state_->periods) {
        if (!bucket.closed || !bucket.period) {
            continue;
        }
        result.push_back(EvaluationTime{
            std::chrono::microseconds{*bucket.period * state_->period_duration.count()}});
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<EvaluationProfilerNodePeriod> EvaluationProfiler::node_periods(GraphInstanceId graph_id,
                                                                          EvaluationTime period_start) const {
    const auto *bucket = state_->period_bucket(period_start);
    if (bucket == nullptr) {
        return {};
    }

    auto result = std::vector<EvaluationProfilerNodePeriod>{};
    auto range = std::lower_bound(bucket->graphs.begin(), bucket->graphs.end(), graph_id.value,
                                  [](const auto &entry, auto id) { return entry.graph_id.value < id; });
    for (; range != bucket->graphs.end() && range->graph_id == graph_id; ++range) {
        for (auto index = range->first_record; index < range->first_record + range->record_count; ++index) {
            const auto &node = bucket->nodes[index];
            result.push_back({node.node_index, {bucket->evaluation_count(index), node.checkpoint_ticks}});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.node_index < right.node_index;
    });
    auto unique_count = std::size_t{};
    for (const auto &entry : result) {
        if (unique_count != 0 && result[unique_count - 1].node_index == entry.node_index) {
            result[unique_count - 1].totals.evaluations += entry.totals.evaluations;
            result[unique_count - 1].totals.checkpoint_ticks += entry.totals.checkpoint_ticks;
        } else {
            result[unique_count++] = entry;
        }
    }
    result.resize(unique_count);
    return result;
}

EvaluationProfilerCycleStats EvaluationProfiler::outer_totals() const { return state_->outer_history.totals; }

std::optional<EvaluationProfilerCycleStats> EvaluationProfiler::outer_period(EvaluationTime period_start) const {
    const auto *bucket = state_->outer_history.period_bucket(period_start);
    return bucket == nullptr ? std::nullopt : std::optional{bucket->timing};
}

std::vector<EvaluationProfilerCyclePeriod> EvaluationProfiler::outer_periods() const {
    return state_->outer_history.periods();
}

std::string EvaluationProfiler::report(std::size_t recent_outer_periods) const {
    auto out = std::ostringstream{};
    if (!state_->root_graph_id) {
        out << "No outer graph was recorded\n";
        return out.str();
    }

    const auto timing = outer_totals();
    out << "Outer graph: " << timing.cycles
        << (timing.cycles == 1 ? " cycle" : " cycles");
    if (timing.cycles != 0) {
        out << ", average " << timing.total_nanoseconds / timing.cycles << " ns"
            << ", min " << timing.min_nanoseconds << " ns"
            << ", max " << timing.max_nanoseconds << " ns";
    }
    out << '\n';

    const auto periods = outer_periods();
    const auto first = periods.size() > recent_outer_periods ? periods.size() - recent_outer_periods : 0;
    for (auto index = first; index < periods.size(); ++index) {
        const auto &period = periods[index];
        const auto day = std::chrono::floor<std::chrono::days>(period.start);
        const auto date = std::chrono::year_month_day{day};
        const auto time = std::chrono::hh_mm_ss{period.start - day};
        out << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-'
            << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
            << std::setw(2) << static_cast<unsigned>(date.day()) << 'T'
            << std::setw(2) << time.hours().count() << ':'
            << std::setw(2) << time.minutes().count() << ':'
            << std::setw(2) << time.seconds().count();
        if (time.subseconds().count() != 0) {
            out << '.' << std::setw(6) << time.subseconds().count();
        }
        out << "Z: " << period.timing.cycles
            << (period.timing.cycles == 1 ? " cycle" : " cycles") << ", average "
            << period.timing.total_nanoseconds / period.timing.cycles << " ns, max "
            << period.timing.max_nanoseconds << " ns\n";
    }
    return out.str();
}

} // namespace bg
