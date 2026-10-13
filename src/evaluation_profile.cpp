#include <bg/evaluation_profile.hpp>

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/runtime_graph.hpp>

#include <chrono>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace bg {
namespace {

constexpr auto record_tag_shift = 30;
constexpr auto record_tag_mask = std::uint32_t{3} << record_tag_shift;
constexpr auto record_value_mask = ~record_tag_mask;
constexpr auto node_tag = std::uint32_t{0} << record_tag_shift;
constexpr auto cycle_tag = std::uint32_t{1} << record_tag_shift;
constexpr auto graph_tag = std::uint32_t{2} << record_tag_shift;
constexpr auto definition_tag = std::uint32_t{3} << record_tag_shift;
constexpr auto cycle_start_event = cycle_tag | 1;
constexpr auto cycle_end_event = cycle_tag | 2;
constexpr auto graph_start_event = graph_tag | 1;
constexpr auto graph_stop_event = graph_tag | 2;
constexpr auto definition_start_event = definition_tag | 1;
constexpr auto node_metadata_event = definition_tag | 2;
constexpr auto stream_magic = std::uint64_t{0x424750524f46494c};
constexpr auto stream_version = std::uint32_t{1};
constexpr auto no_owner_node = std::numeric_limits<std::uint32_t>::max();

struct StreamHeader {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t clock;
};

struct RecordHeader {
    std::uint32_t graph_id;
    std::uint32_t event;
    std::uint64_t checkpoint;
};

struct CycleMetadata {
    std::int64_t evaluation_time_microseconds;
    std::int64_t wall_clock_time_microseconds;
    std::int64_t steady_clock_nanoseconds;
};

struct GraphStartMetadata {
    std::uint32_t parent_graph_id;
    std::uint32_t owner_node_index;
};

static_assert(sizeof(StreamHeader) == 16);
static_assert(sizeof(RecordHeader) == 16);
static_assert(sizeof(CycleMetadata) == 24);
static_assert(sizeof(GraphStartMetadata) == 8);
static_assert(std::is_trivially_copyable_v<StreamHeader>);
static_assert(std::is_trivially_copyable_v<RecordHeader>);
static_assert(std::is_trivially_copyable_v<CycleMetadata>);
static_assert(std::is_trivially_copyable_v<GraphStartMetadata>);

constexpr auto checkpoint_clock = [] {
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
    return EvaluationProfileClock::timestamp_counter;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return EvaluationProfileClock::timestamp_counter;
#else
    return EvaluationProfileClock::steady_clock_nanoseconds;
#endif
}();

std::uint64_t read_checkpoint() noexcept {
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
    auto low = std::uint32_t{};
    auto high = std::uint32_t{};
    asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<std::uint64_t>(high) << 32) | low;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _ReadWriteBarrier();
    const auto result = __rdtsc();
    _ReadWriteBarrier();
    return result;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

std::int64_t steady_clock_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename T> void write_object(util::buffer_writer &writer, const T &value) {
    writer.write_bytes(std::as_bytes(std::span{&value, std::size_t{1}}));
}

template <typename T> T read_object(util::buffer_reader &reader) {
    auto value = T{};
    reader.read_bytes(std::as_writable_bytes(std::span{&value, std::size_t{1}}));
    return value;
}

} // namespace

EvaluationProfileReader::EvaluationProfileReader(util::buffer recording) : reader_{std::move(recording)} {
    if (reader_.readable().empty()) {
        throw std::runtime_error{"evaluation profile stream has no header"};
    }

    const auto header = read_object<StreamHeader>(reader_);
    if (header.magic != stream_magic || header.version != stream_version ||
        header.clock > static_cast<std::uint32_t>(EvaluationProfileClock::steady_clock_nanoseconds)) {
        throw std::runtime_error{"invalid evaluation profile stream header"};
    }
    clock_ = static_cast<EvaluationProfileClock>(header.clock);
}

std::optional<EvaluationProfileRecord> EvaluationProfileReader::read() {
    if (reader_.readable().empty()) {
        return std::nullopt;
    }

    const auto record = read_object<RecordHeader>(reader_);
    const auto tag = record.event & record_tag_mask;
    const auto value = record.event & record_value_mask;
    if (tag == node_tag) {
        return EvaluationProfileNode{record.graph_id, value, record.checkpoint};
    }
    if (tag == cycle_tag) {
        if (value != 1 && value != 2) {
            throw std::runtime_error{"invalid evaluation profile cycle event"};
        }
        const auto metadata = read_object<CycleMetadata>(reader_);
        return EvaluationProfileCycle{record.graph_id,
                                      value == 1 ? EvaluationProfileCycle::Boundary::start
                                                 : EvaluationProfileCycle::Boundary::end,
                                      EvaluationTime{std::chrono::microseconds{metadata.evaluation_time_microseconds}},
                                      EvaluationTime{std::chrono::microseconds{metadata.wall_clock_time_microseconds}},
                                      record.checkpoint,
                                      metadata.steady_clock_nanoseconds};
    }
    if (tag == graph_tag) {
        if (value == 1) {
            if (record.checkpoint == 0 || record.checkpoint > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error{"invalid evaluation profile definition id"};
            }
            const auto metadata = read_object<GraphStartMetadata>(reader_);
            return EvaluationProfileGraph{record.graph_id, static_cast<std::uint32_t>(record.checkpoint),
                                          metadata.parent_graph_id, metadata.owner_node_index};
        }
        if (value == 2) {
            return EvaluationProfileGraphStop{record.graph_id};
        }
        throw std::runtime_error{"invalid evaluation profile graph event"};
    }
    if (tag == definition_tag) {
        if (value == 1) {
            if (record.checkpoint > record_value_mask) {
                throw std::runtime_error{"invalid evaluation profile node count"};
            }
            return EvaluationProfileDefinition{record.graph_id, static_cast<std::uint32_t>(record.checkpoint)};
        }
        if (value == 2) {
            if (record.checkpoint > record_value_mask) {
                throw std::runtime_error{"invalid evaluation profile node index"};
            }
            const auto path_size = read_object<std::uint32_t>(reader_);
            auto path = std::string(path_size, '\0');
            reader_.read_bytes(std::as_writable_bytes(std::span{path.data(), path.size()}));
            return EvaluationProfileNodeMetadata{record.graph_id, static_cast<std::uint32_t>(record.checkpoint),
                                                 std::move(path)};
        }
        throw std::runtime_error{"invalid evaluation profile definition event"};
    }

    throw std::runtime_error{"unknown evaluation profile event"};
}

EvaluationProfile::EvaluationProfile(std::size_t block_capacity, allocator memory)
    : recording_{util::buffer::chain(block_capacity, memory)}, writer_{recording_} {
    const auto header = StreamHeader{stream_magic, stream_version, static_cast<std::uint32_t>(checkpoint_clock)};
    write_object(writer_, header);
    writer_.commit();
    recorded_bytes_ = sizeof(header);
    committed_bytes_ = sizeof(header);
}

util::buffer EvaluationProfile::take_recording() {
    if (recording_taken_) {
        throw std::logic_error{"evaluation profile recording has already been taken"};
    }
    recording_taken_ = true;
    return std::move(recording_);
}

std::uint32_t EvaluationProfile::definition_id(const CompiledGraph &compiled_graph) {
    if (const auto found = definitions_.find(&compiled_graph); found != definitions_.end()) {
        return found->second.definition_id;
    }
    if (next_definition_id_ == 0 || compiled_graph.node_count() > record_value_mask) {
        throw std::length_error{"evaluation profile definition id or node count limit exceeded"};
    }

    const auto id = next_definition_id_++;
    definitions_.emplace(&compiled_graph, GraphDefinition{id, 0});
    write_record(id, definition_start_event, compiled_graph.node_count());
    for (auto node_index = std::size_t{}; node_index < compiled_graph.node_count(); ++node_index) {
        const auto path = compiled_graph.wiring_path(node_index).text();
        if (path.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"evaluation profile node path is too long"};
        }

        const auto node = RecordHeader{id, node_metadata_event, node_index};
        const auto path_size = static_cast<std::uint32_t>(path.size());
        write_object(writer_, node);
        write_object(writer_, path_size);
        writer_.write_bytes(std::as_bytes(std::span{path.data(), path.size()}));
        recorded_bytes_ += sizeof(node) + sizeof(path_size) + path.size();
    }
    return id;
}

std::uint32_t EvaluationProfile::start_graph(RuntimeGraph &graph, const CompiledGraph &compiled_graph,
                                             std::uint32_t parent_graph_id,
                                             std::uint32_t owner_node_index) noexcept {
    if (failed_ || finished_) {
        return 0;
    }
    if (graph_ids_.contains(&graph) || next_graph_id_ == 0 ||
        (owner_node_index != no_owner_node && owner_node_index > record_value_mask)) {
        fail();
        return 0;
    }

    const auto graph_id = next_graph_id_++;
    try {
        const auto instance = graph_ids_.emplace(&graph, GraphInstance{graph_id, nullptr}).first;
        const auto graph_definition_id = definition_id(compiled_graph);
        write_record(graph_id, graph_start_event, graph_definition_id);
        write_object(writer_, GraphStartMetadata{parent_graph_id, owner_node_index});
        recorded_bytes_ += sizeof(GraphStartMetadata);
        instance->second.compiled_graph = &compiled_graph;
        ++definitions_.at(&compiled_graph).active_graph_count;
    } catch (...) {
        fail();
        return 0;
    }
    return graph_id;
}

void EvaluationProfile::stop_graph(RuntimeGraph &graph) noexcept {
    const auto found = graph_ids_.find(&graph);
    if (found == graph_ids_.end()) {
        return;
    }

    const auto [graph_id, compiled_graph] = found->second;
    graph_ids_.erase(found);
    if (compiled_graph != nullptr) {
        if (auto definition = definitions_.find(compiled_graph);
            definition != definitions_.end() && --definition->second.active_graph_count == 0) {
            definitions_.erase(compiled_graph);
        }
    }
    if (failed_ || finished_) {
        return;
    }
    try {
        write_record(graph_id, graph_stop_event, 0);
    } catch (...) {
        fail();
    }
}

std::uint32_t EvaluationProfile::graph_id(const RuntimeGraph &graph) const noexcept {
    const auto found = graph_ids_.find(&graph);
    return found == graph_ids_.end() ? 0 : found->second.graph_id;
}

void EvaluationProfile::begin_cycle(std::uint32_t graph_id, EvaluationTime evaluation_time,
                                     EvaluationTime wall_clock_time) noexcept {
    if (failed_ || finished_) {
        return;
    }
    if (cycle_open_) {
        fail();
        return;
    }
    cycle_open_ = true;
    try {
        write_cycle(graph_id, cycle_start_event, evaluation_time, wall_clock_time);
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::record_node(std::uint32_t graph_id, std::size_t node_index) noexcept {
    if (failed_ || finished_ || graph_id == 0) {
        return;
    }
    if (node_index > record_value_mask) {
        fail();
        return;
    }
    try {
        write_record(graph_id, node_tag | static_cast<std::uint32_t>(node_index), read_checkpoint());
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::end_cycle(std::uint32_t graph_id, EvaluationTime evaluation_time,
                                  EvaluationTime wall_clock_time) noexcept {
    if (failed_ || finished_ || !cycle_open_) {
        return;
    }
    try {
        write_cycle(graph_id, cycle_end_event, evaluation_time, wall_clock_time);
        commit();
        cycle_open_ = false;
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::abort_cycle() noexcept {
    if (!cycle_open_) {
        return;
    }

    writer_.rollback();
    recorded_bytes_ = committed_bytes_;
    cycle_open_ = false;
    failed_ = true;
    graph_ids_.clear();
    definitions_.clear();
}

void EvaluationProfile::write_record(std::uint32_t graph_id, std::uint32_t event, std::uint64_t checkpoint) {
    write_object(writer_, RecordHeader{graph_id, event, checkpoint});
    recorded_bytes_ += sizeof(RecordHeader);
}

void EvaluationProfile::write_cycle(std::uint32_t graph_id, std::uint32_t boundary, EvaluationTime evaluation_time,
                                    EvaluationTime wall_clock_time) {
    const auto record = RecordHeader{graph_id, boundary, read_checkpoint()};
    const auto metadata = CycleMetadata{
        std::chrono::duration_cast<std::chrono::microseconds>(evaluation_time.time_since_epoch()).count(),
        std::chrono::duration_cast<std::chrono::microseconds>(wall_clock_time.time_since_epoch()).count(),
        steady_clock_nanoseconds()};
    write_object(writer_, record);
    write_object(writer_, metadata);
    recorded_bytes_ += sizeof(record) + sizeof(metadata);
}

void EvaluationProfile::commit() noexcept {
    writer_.commit();
    committed_bytes_ = recorded_bytes_;
}

void EvaluationProfile::fail() noexcept {
    if (failed_) {
        return;
    }
    failed_ = true;
    writer_.rollback();
    recorded_bytes_ = committed_bytes_;
    cycle_open_ = false;
    graph_ids_.clear();
    definitions_.clear();
}

void EvaluationProfile::finish() noexcept {
    if (finished_) {
        return;
    }
    if (cycle_open_) {
        writer_.rollback();
        recorded_bytes_ = committed_bytes_;
        cycle_open_ = false;
        failed_ = true;
        graph_ids_.clear();
        definitions_.clear();
    } else if (!failed_) {
        writer_.commit();
        committed_bytes_ = recorded_bytes_;
    }
    finished_ = true;
}

} // namespace bg
