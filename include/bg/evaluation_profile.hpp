#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/util/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace bg {

class RuntimeGraph;
class RuntimeGraphEvaluator;
class CompiledGraph;

enum class EvaluationProfileClock : std::uint32_t { timestamp_counter, steady_clock_nanoseconds };

struct EvaluationProfileNode {
    std::uint32_t graph_id;
    std::uint32_t node_index;
    std::uint64_t checkpoint;
};

struct EvaluationProfileCycle {
    enum class Boundary : std::uint8_t { start, end };

    std::uint32_t graph_id;
    Boundary boundary;
    EvaluationTime evaluation_time;
    EvaluationTime wall_clock_time;
    std::uint64_t checkpoint;
    std::int64_t steady_clock_nanoseconds;
};

struct EvaluationProfileGraph {
    std::uint32_t graph_id;
    std::uint32_t definition_id;
    std::uint32_t parent_graph_id;
    std::uint32_t owner_node_index;
};

struct EvaluationProfileGraphStop {
    std::uint32_t graph_id;
};

struct EvaluationProfileDefinition {
    std::uint32_t definition_id;
    std::uint32_t node_count;
};

struct EvaluationProfileNodeMetadata {
    std::uint32_t definition_id;
    std::uint32_t node_index;
    std::string path;
};

using EvaluationProfileRecord =
    std::variant<EvaluationProfileNode, EvaluationProfileCycle, EvaluationProfileGraph,
                 EvaluationProfileGraphStop, EvaluationProfileDefinition, EvaluationProfileNodeMetadata>;

class EvaluationProfileReader {
  public:
    explicit EvaluationProfileReader(util::buffer recording);

    std::optional<EvaluationProfileRecord> read();
    EvaluationProfileClock clock() const noexcept { return clock_; }

  private:
    util::buffer_reader reader_;
    EvaluationProfileClock clock_{};
};

// Records evaluation checkpoints into a caller-owned single-writer buffer chain.
// The caller controls the recording lifetime and can poll a reader on another thread.
class EvaluationProfile {
  public:
    explicit EvaluationProfile(std::size_t block_capacity = 1 << 20, allocator memory = {});

    EvaluationProfile(const EvaluationProfile &) = delete;
    EvaluationProfile &operator=(const EvaluationProfile &) = delete;
    EvaluationProfile(EvaluationProfile &&) = delete;
    EvaluationProfile &operator=(EvaluationProfile &&) = delete;

    util::buffer take_recording();
    std::size_t recorded_bytes() const noexcept { return recorded_bytes_; }
    bool failed() const noexcept { return failed_; }
    void finish() noexcept;

    // Engine API used by RuntimeGraph and RuntimeGraphEvaluator to record graph execution.
    std::uint32_t start_graph(RuntimeGraph &graph, const CompiledGraph &compiled_graph,
                              std::uint32_t parent_graph_id, std::uint32_t owner_node_index) noexcept;
    void stop_graph(RuntimeGraph &graph) noexcept;
    std::uint32_t graph_id(const RuntimeGraph &graph) const noexcept;
    void begin_cycle(std::uint32_t graph_id, EvaluationTime evaluation_time, EvaluationTime wall_clock_time) noexcept;
    void record_node(std::uint32_t graph_id, std::size_t node_index) noexcept;
    void end_cycle(std::uint32_t graph_id, EvaluationTime evaluation_time,
                   EvaluationTime wall_clock_time) noexcept;
    void abort_cycle() noexcept;

  private:
    struct GraphInstance {
        std::uint32_t graph_id;
        const CompiledGraph *compiled_graph;
    };

    struct GraphDefinition {
        std::uint32_t definition_id;
        std::size_t active_graph_count;
    };

    std::uint32_t definition_id(const CompiledGraph &compiled_graph);
    void write_record(std::uint32_t graph_id, std::uint32_t event, std::uint64_t checkpoint);
    void write_cycle(std::uint32_t graph_id, std::uint32_t boundary, EvaluationTime evaluation_time,
                     EvaluationTime wall_clock_time);
    void commit() noexcept;
    void fail() noexcept;

    util::buffer recording_;
    util::buffer_writer writer_;
    std::unordered_map<const RuntimeGraph *, GraphInstance> graph_ids_;
    std::unordered_map<const CompiledGraph *, GraphDefinition> definitions_;
    std::uint32_t next_graph_id_{1};
    std::uint32_t next_definition_id_{1};
    std::size_t recorded_bytes_{};
    std::size_t committed_bytes_{};
    bool cycle_open_{};
    bool failed_{};
    bool finished_{};
    bool recording_taken_{};
};

} // namespace bg
