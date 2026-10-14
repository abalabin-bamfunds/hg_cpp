#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/graph/graph_instance_id.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace bg {

class RuntimeGraph;
class RuntimeGraphEvaluator;
class CompiledGraph;

enum class EvaluationProfileClock : std::uint32_t { timestamp_counter, steady_clock_nanoseconds };

struct EvaluationProfileNode {
    GraphInstanceId graph_id;
    std::uint32_t node_index;
    std::uint64_t checkpoint;
};

struct EvaluationProfileCycle {
    enum class Boundary : std::uint8_t { start, end };

    GraphInstanceId graph_id;
    Boundary boundary;
    EvaluationTime evaluation_time;
    EvaluationTime wall_clock_time;
    std::uint64_t checkpoint;
    std::int64_t steady_clock_nanoseconds;
};

struct EvaluationProfileGraph {
    GraphInstanceId graph_id;
    const CompiledGraph *compiled_graph;
    GraphInstanceId parent_graph_id;
    std::uint32_t owner_node_index;
    std::optional<ScalarValue<>> label;
};

struct EvaluationProfileGraphStop {
    GraphInstanceId graph_id;
};

struct EvaluationProfileGraphEntry {
    GraphInstanceId graph_id;
    std::uint64_t checkpoint;
};

struct EvaluationProfileGraphExit {
    GraphInstanceId graph_id;
    std::uint64_t checkpoint;
};

using EvaluationProfileRecord =
    std::variant<EvaluationProfileNode, EvaluationProfileCycle, EvaluationProfileGraph,
                 EvaluationProfileGraphStop, EvaluationProfileGraphEntry, EvaluationProfileGraphExit>;

class EvaluationProfileReader {
  public:
    explicit EvaluationProfileReader(util::buffer recording);
    ~EvaluationProfileReader();

    EvaluationProfileReader(const EvaluationProfileReader &) = delete;
    EvaluationProfileReader &operator=(const EvaluationProfileReader &) = delete;

    std::optional<EvaluationProfileRecord> read();
    EvaluationProfileClock clock() const noexcept { return clock_; }

  private:
    util::buffer_reader reader_;
    std::vector<GraphInstanceId> active_graphs_;
    GraphInstanceId cycle_graph_id_{};
    EvaluationTime cycle_evaluation_time_{};
    EvaluationProfileClock clock_{};
};

// Records evaluation checkpoints into a caller-owned single-writer buffer chain.
// The caller controls the recording lifetime and can poll a reader on another thread.
class EvaluationProfile {
  public:
    explicit EvaluationProfile(std::size_t block_capacity = 1 << 20, allocator memory = {});
    ~EvaluationProfile() noexcept;

    EvaluationProfile(const EvaluationProfile &) = delete;
    EvaluationProfile &operator=(const EvaluationProfile &) = delete;
    EvaluationProfile(EvaluationProfile &&) = delete;
    EvaluationProfile &operator=(EvaluationProfile &&) = delete;

    util::buffer take_recording();
    std::size_t recorded_bytes() const noexcept { return recorded_bytes_; }
    bool failed() const noexcept { return failed_; }
    void finish() noexcept;

    // Engine API used by RuntimeGraph and RuntimeGraphEvaluator to record graph execution.
    void start_graph(GraphInstanceId graph_id, const CompiledGraph &compiled_graph, GraphInstanceId parent_graph_id,
                     std::uint32_t owner_node_index, std::optional<Scalar<>> label = std::nullopt) noexcept;
    void stop_graph(GraphInstanceId graph_id) noexcept;
    void enter_graph(GraphInstanceId graph_id) noexcept;
    void exit_graph(GraphInstanceId graph_id) noexcept;
    void begin_cycle(GraphInstanceId graph_id, EvaluationTime evaluation_time,
                     EvaluationTime wall_clock_time) noexcept;
    void record_node(std::size_t node_index) noexcept;
    void end_cycle(EvaluationTime wall_clock_time) noexcept;
    void abort_cycle() noexcept;

  private:
    void write_graph_stop(GraphInstanceId graph_id);
    void write_cycle_start(GraphInstanceId graph_id, EvaluationTime evaluation_time, EvaluationTime wall_clock_time);
    void write_cycle_end(EvaluationTime wall_clock_time);
    void destroy_uncommitted_labels() noexcept;
    void commit() noexcept;
    void fail() noexcept;

    allocator memory_;
    util::buffer recording_;
    util::buffer_writer writer_;
    std::vector<ScalarValue<> *> labels_;
    std::size_t committed_label_count_{};
    std::size_t recorded_bytes_{};
    std::size_t committed_bytes_{};
    bool cycle_open_{};
    bool failed_{};
    bool finished_{};
    bool recording_taken_{};
};

} // namespace bg
